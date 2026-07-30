#pragma once

#include <boost/asio/spawn.hpp>
#include <iostream>
#include <map>

#include "../util/compat.h"
#include "../util/debug.h"
#include "../util/scheduler.h"
#include "../util/watch_dog.h"
#include "../util/async_queue.h"
#include "../util/wait_condition.h"
#include "contact.h"
#include "debug_ctx.h"

namespace ouinet::bittorrent {

template<class CandidateSet, class Evaluate>
void collect(
    DebugCtx dbg,
    CandidateSet first_candidates,
    Evaluate&& evaluate,
    Async yield
) {
    using namespace std;

    enum Progress { unused, used };

    using Candidates = std::map< Contact
                               , Progress
                               , typename CandidateSet::key_compare>;

    auto comp = first_candidates.key_comp();
    Candidates candidates(comp);

    if (dbg) cerr << dbg << "first candidates:" << "\n";

    for (auto& c : first_candidates) {
        if (dbg) cerr << dbg << "     " << c << "\n";
        candidates.insert(candidates.end(), { c, unused });
    }

    WaitCondition all_done(yield.get_executor());
    util::AsyncQueue<NodeContact> new_candidates(yield.get_executor());
    Scheduler scheduler(yield.get_executor(), 8);

    auto pick_candidate = [&] {
        // Pick the closest untried candidate...
        for (auto it = candidates.begin(); it != candidates.end(); ++it) {
            if (it->second != unused) continue;
            it->second = used;
            return it;
        }
        return candidates.end();
    };

    std::set<size_t> active_jobs;
    size_t next_job_id = 0;

    Async local_yield(yield);

    while (true) {
        if (dbg) cerr << dbg << "Start waiting for job (current count:" << scheduler.slot_count() << ")\n";

        auto slot = compat([&](Cancel cancel, asio::yield_context yield) {
            return scheduler.wait_for_slot(cancel, yield);
        })(local_yield);

        if (dbg) cerr << dbg << " Done waiting for job (job count:" << scheduler.slot_count() << ")\n";

        if (!slot) break;

        auto candidate_i = pick_candidate();

        std::queue<NodeContact> cs;

        while (candidate_i == candidates.end()) {
            if (active_jobs.empty() && new_candidates.size() == 0) {
                break;
            }

            if (dbg) cerr << dbg << " Start waiting for candidate (active jobs:"
                          << active_jobs.size() << " new_candidates:" << new_candidates.size() << ")\n";

            auto result = new_candidates.async_pop_one_or_more(cs, yield);

            if (dbg) cerr << dbg << " End waiting for candidate "
                          << debug(result) << " " << cs.size() << "\n";

            if (result == std::unexpected(asio::error::eof)) {
                continue;
            }

            if (!result) break;

            while (!cs.empty()) {
                auto c = std::move(cs.front());
                cs.pop();
                bool added = candidates.insert({ c, unused }).second;
                if (dbg && added) cerr << dbg << "     + " << c << "\n";
            }

            candidate_i = pick_candidate();
        }

        if (candidate_i == candidates.end()) break;

        auto job_id = next_job_id++;
        active_jobs.insert(job_id);

        local_yield.spawn([ &
                          , candidate = candidate_i->first
                          , job_id
                          , lock = all_done.lock()
                          , slot = std::move(slot)
                          ] (auto yield) mutable {
            bool on_finish_called = false;

            auto on_finish = [&] () mutable {
                if (on_finish_called) return;
                on_finish_called = true;


                active_jobs.erase(job_id);
                slot = Scheduler::Slot();

                // Make sure we don't get stuck waiting for candidates when
                // there is no more work and this candidate has not returned
                // any new ones.
                new_candidates.push_back(NodeContact(), asio::error::eof);
            };

            bool is_first_round = first_candidates.count(candidate);

            if (is_first_round) {
                WatchDog wd(yield.get_executor(), std::chrono::seconds(5), [&] () mutable {
                    if (dbg) cerr << dbg << "dismiss " << candidate << "\n";
                    on_finish();
                });

                WatchDog dummy_wd;

                evaluate( candidate
                        , dummy_wd
                        , new_candidates
                        , yield);
            } else {
                WatchDog wd(yield.get_executor(), std::chrono::milliseconds(200), [&] () mutable {
                    if (dbg) cerr << dbg << "dismiss " << candidate << "\n";
                    on_finish();
                });

                evaluate( candidate
                        , wd
                        , new_candidates
                        , yield);
            }

            on_finish();
        });
    }

    local_yield.cancel();

    if (dbg) cerr << dbg << " >>>>>>>>>>>>>>>>>>> DONE <<<<<<<<<<<<<<<<<<<<\n";

    all_done.wait(yield);
}

} // namespaces
