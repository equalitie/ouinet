#pragma once

#include <memory>
#include <asio_utp.hpp>
#include "../namespaces.h"
#include "signal.h"

namespace ouinet { namespace util {

/*
 * For a given UdpMultiplexer, determines whether we ever receive a datagram
 * from an address we did not recently send any. If so, this udp socket can
 * presumably function as a world-reachable server: either there is no NAT
 * or firewalling in place to block us, exceptions have successfully been
 * made, or the outgoing traffic opened a hole for incoming traffic from
 * arbitrary sources.
 */
class UdpServerReachabilityAnalysis {
public:
    /*
     * Judgement of reachability so far:
     * - Unreachable means that we have seen no indication of being reachable,
     *   *so far*.
     * - ConfirmedReachable means a high-confidence judgement of being
     *   reachable.
     * - UnconfirmedReachable means a low-confidence judgement of being
     *   reachable. This will normally be either upgraded or downgraded soon.
     */
    enum class Reachability {
        Unreachable,
        ConfirmedReachable,
        UnconfirmedReachable
    };

    /*
     * Period after last communication with a peer after which we are confident
     * any firewall connection tracking entries have expired. Incoming data
     * after this period is considered unsolicited.
     */
    static constexpr long connectionTrackingExpiracyTime = 60 * 60 * 1000;

    /*
     * Period after startup during which connection tracking entries from
     * previous runs may still be in force. Incoming data in this period
     * is not considered unsolicited.
     *
     * This period may be shorter than connectionTrackingExpiracyTime. Traffic
     * in this gap will set the judgement to UnconfirmedReachable.
     */
    static constexpr long lingeringConnectionTrackingTime = 10 * 60 * 1000;

    /*
     * If no unsolicited traffic arrives for this long while reachability
     * is still unconfirmed, conclude that it was a fluke and downgrade
     * to Unreachable.
     */
    static constexpr long unconfirmedReachabilityExpiracyTime = 3 * lingeringConnectionTrackingTime;

    /*
     * If no unsolicited traffic arrives for this long while reachability
     * is already confirmed, conclude that something has likely changed in
     * networking conditions, and downgrade to UnconfirmedReachable.
     */
    static constexpr long confirmedReachabilityExpiracyTime = 2 * 60 * 60 * 1000;

public:
    UdpServerReachabilityAnalysis();
    ~UdpServerReachabilityAnalysis();

    void start(const asio::executor& executor, const asio_utp::udp_multiplexer& udp_socket);
    void stop();

    Reachability judgement() const;

    Signal<void()>& on_judgement_change();

private:
    class State;
    std::shared_ptr<State> _state;
};

}} // namespaces
