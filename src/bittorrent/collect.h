#pragma once

#include <iostream>
#include "../util/scheduler.h"
#include "../util/watch_dog.h"
#include "../util/async_queue.h"

namespace ouinet { namespace bittorrent {

template<class CandidateSet, class Evaluate>
void collect(
    asio::io_service& ios,
    CandidateSet candidates_,
    Evaluate&& evaluate,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
    enum Progress { unused, used };

    using Candidates = std::map< Contact
                               , Progress
                               , typename CandidateSet::key_compare>;

    auto comp = candidates_.key_comp();
    Candidates candidates(comp);

    int in_progress_endpoints = 0;

    for (auto& c : candidates_) {
        candidates.insert(candidates.end(), { c, unused });
    }

    const unsigned THREADS = 64;
    WaitCondition all_done(ios);
    ConditionVariable candidate_available(ios);

    auto cancelled = cancel_signal.connect([&] {
        candidate_available.notify();
    });

    for (unsigned thread = 0; thread < THREADS; thread++) {
        asio::spawn(ios, [&, lock = all_done.lock()] (asio::yield_context yield) {
            while (true) {
                if (cancelled) {
                    break;
                }

                typename Candidates::iterator candidate_i = candidates.end();

                /*
                 * Try the closest untried candidate...
                 */
                for (auto it = candidates.begin(); it != candidates.end(); ++it) {
                    if (it->second != unused) continue;
                    it->second = used;
                    candidate_i = it;
                    break;
                }

                if (candidate_i == candidates.end()) {
                    if (in_progress_endpoints == 0) {
                        return;
                    }
                    candidate_available.wait(yield);
                    continue;
                }

                in_progress_endpoints++;
                sys::error_code ec;
                auto opt_new_candidates = evaluate(candidate_i->first, yield[ec], cancel_signal);
                in_progress_endpoints--;

                if (cancelled) {
                    break;
                }

                if (!opt_new_candidates) {
                    continue;
                }

                for (auto c : *opt_new_candidates) {
                    candidates.insert({ c, unused });
                }

                candidate_available.notify();
            }
        });
    }

    all_done.wait(yield);

    if (cancelled) {
        or_throw(yield, asio::error::operation_aborted);
    }
}

template<class CandidateSet, class Evaluate>
void collect2(
    DebugCtx dbg,
    asio::io_service& ios,
    CandidateSet first_candidates,
    Evaluate&& evaluate,
    asio::yield_context yield,
    Signal<void()>& cancel_signal
) {
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

    WaitCondition all_done(ios);
    util::AsyncQueue<dht::NodeContact> new_candidates(ios);

    Scheduler scheduler(ios, 16);

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

    // Default timeout per each `evaluate` call.
    auto default_timeout = std::chrono::seconds(30);

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

        asio::spawn(ios, [ &
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

            WatchDog wd(ios, default_timeout, [&] () mutable {
                    if (dbg) cerr << dbg << "dismiss " << candidate << "\n";
                    on_finish();
                });

            evaluate( candidate
                    , wd
                    , new_candidates
                    , yield[ec]
                    , local_cancel);

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
