#pragma once

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

    const int THREADS = 64;
    WaitCondition all_done(ios);
    ConditionVariable candidate_available(ios);
    bool cancelled = false;
    auto cancel_slot = cancel_signal.connect([&] {
        cancelled = true;
        candidate_available.notify();
    });

    // If set, every contact higher than *end will be ignored.
    boost::optional<Contact> end;

    for (int thread = 0; thread < THREADS; thread++) {
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
                    if (end && comp(*end, it->first)) break;
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
                    if (!end || comp(candidate_i->first, *end)) {
                        end = candidate_i->first;
                    }
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

}} // namespaces
