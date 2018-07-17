#pragma once

namespace ouinet { namespace bittorrent {

template<class CandidateSet, class Evaluate>
void collect( asio::io_service& ios
            , CandidateSet candidates_
            , Evaluate&& evaluate
            , asio::yield_context yield)
{
    enum Progress { unused, used };

    using Candidates = std::map< Contact
                               , Progress
                               , typename CandidateSet::key_compare>;

    Candidates candidates(candidates_.key_comp());

    int in_progress_endpoints = 0;

    for (auto& c : candidates_) {
        candidates.insert(candidates.end(), { c, unused });
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
                auto opt_new_candidates = evaluate(candidate_i->first, yield);
                in_progress_endpoints--;

                if (!opt_new_candidates) {
                    return;
                }

                for (auto c : *opt_new_candidates) {
                    candidates.insert({ c, unused });
                }

                candidate_available.notify();
            }
        });
    }

    all_done.wait(yield);
}

}} // namespaces
