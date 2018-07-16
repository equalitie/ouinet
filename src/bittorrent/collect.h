#pragma once

namespace ouinet { namespace bittorrent {

template<class CandidateSet, class Evaluate>
void collect( asio::io_service& ios
            , CandidateSet candidates_
            , Evaluate&& evaluate
            , asio::yield_context yield)
{
    enum Progress { pending, in_progress, done };

    using Candidates = std::map< Contact
                               , Progress
                               , typename CandidateSet::key_compare>;

    Candidates candidates(candidates_.key_comp());

    int in_progress_endpoints = 0;

    for (auto& c : candidates_) {
        candidates[c] = pending;
    }

    const int THREADS = 64;
    WaitCondition all_done(ios);
    ConditionVariable candidate_available(ios);

    for (int thread = 0; thread < THREADS; thread++) {
        asio::spawn(ios, [&, lock = all_done.lock()] (asio::yield_context yield) {
            while (true) {
                typename Candidates::iterator candidate_i = candidates.end();

                /*
                 * Try the closest untried candidate...
                 */
                for (auto it = candidates.begin(); it != candidates.end(); ++it) {
                    if (it->second != pending) continue;
                    it->second = in_progress;
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
                auto opt_new_candidates = evaluate(candidate_i->first, yield);
                candidate_i->second = done;
                in_progress_endpoints--;

                if (!opt_new_candidates) {
                    return;
                }

                for (auto c : *opt_new_candidates) {
                    candidates.insert(std::make_pair(c, pending));
                }

                candidate_available.notify();
            }
        });
    }

    all_done.wait(yield);
}

}} // namespaces
