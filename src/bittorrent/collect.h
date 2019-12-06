#pragma once

#include <iostream>
#include "../util/scheduler.h"
#include "../util/watch_dog.h"
#include "../util/async_queue.h"

namespace ouinet { namespace bittorrent {

template<class CandidateSet, class Evaluate>
void collect(
    DebugCtx dbg,
    asio::executor& exec,
    CandidateSet first_candidates,
    Evaluate&& evaluate,
    Cancel& cancel_signal_,
    asio::yield_context yield
) {
    Cancel cancel_signal(cancel_signal_);

    using namespace std;
    using dht::NodeContact;

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

    WaitCondition all_done(exec);
    util::AsyncQueue<dht::NodeContact> new_candidates(exec);

    Scheduler scheduler(exec, 8);

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

    Cancel local_cancel(cancel_signal);

    while (true) {
        sys::error_code ec;

        if (dbg) cerr << dbg << "Start waiting for job (current count:" << scheduler.slot_count() << ")\n";

        auto slot = scheduler.wait_for_slot(local_cancel, yield[ec]);

        if (dbg) cerr << dbg << " Done waiting for job (job count:" << scheduler.slot_count() << ")\n";

        assert(!local_cancel || ec == asio::error::operation_aborted);
        if (ec) break;

        auto candidate_i = pick_candidate();

        std::queue<NodeContact> cs;

        while (candidate_i == candidates.end()) {
            sys::error_code ec2;

            if (active_jobs.empty() && new_candidates.size() == 0) {
                break;
            }

            if (dbg) cerr << dbg << " Start waiting for candidate (active jobs:"
                          << active_jobs.size() << " new_candidates:" << new_candidates.size() << ")\n";

            assert(!local_cancel);
            new_candidates.async_flush(cs, local_cancel, yield[ec2]);

            if (dbg) cerr << dbg << " End waiting for candidate "
                          << ec2.message() << " " << cs.size() << "\n";

            assert(!local_cancel || ec2 == asio::error::operation_aborted);

            if (ec2 == asio::error::eof) {
                continue;
            }

            if (ec2 || local_cancel) break;

            while (!cs.empty()) {
                auto c = std::move(cs.front());
                cs.pop();
                bool added = candidates.insert({ c, unused }).second;
                if (dbg && added) cerr << dbg << "     + " << c << "\n";
            }

            candidate_i = pick_candidate();
        }

        if (candidate_i == candidates.end()) break;

        assert(!local_cancel);

        auto job_id = next_job_id++;
        active_jobs.insert(job_id);

        asio::spawn(exec, [ &
                          , candidate = candidate_i->first
                          , job_id
                          , lock = all_done.lock()
                          , slot = std::move(slot)
                          ] (asio::yield_context yield) mutable {
            sys::error_code ec;

            bool on_finish_called = false;

            auto on_finish = [&] () mutable {
                if (on_finish_called) return;
                on_finish_called = true;


                active_jobs.erase(job_id);
                slot = Scheduler::Slot();

                // Make sure we don't get stuck waiting for candidates when
                // there is no more work and this candidate has not returned
                // any new ones.
                new_candidates.async_push( dht::NodeContact()
                                         , asio::error::eof
                                         , local_cancel
                                         , yield);
            };

            bool is_first_round = first_candidates.count(candidate);

            if (is_first_round) {
                WatchDog wd(exec, std::chrono::seconds(5), [&] () mutable {
                        if (dbg) cerr << dbg << "dismiss " << candidate << "\n";
                        on_finish();
                    });

                WatchDog dummy_wd;

                evaluate( candidate
                        , dummy_wd
                        , new_candidates
                        , local_cancel
                        , yield[ec]);
            } else {
                WatchDog wd(exec, std::chrono::milliseconds(200), [&] () mutable {
                        if (dbg) cerr << dbg << "dismiss " << candidate << "\n";
                        on_finish();
                    });

                evaluate( candidate
                        , wd
                        , new_candidates
                        , local_cancel
                        , yield[ec]);
            }

            on_finish();
        });
    }

    local_cancel();

    if (dbg) cerr << dbg << " >>>>>>>>>>>>>>>>>>> DONE <<<<<<<<<<<<<<<<<<<<\n";

    all_done.wait(yield);

    if (cancel_signal) {
        or_throw(yield, asio::error::operation_aborted);
    }
}

}} // namespaces
