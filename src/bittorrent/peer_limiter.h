#pragma once

#include "../util/scheduler.h"
#include <iostream>

namespace ouinet {
namespace bittorrent {

class PeerLimiter {
private:
    using Udp = asio::ip::udp;

    template<class K>
    using List = boost::intrusive::list
        <K, boost::intrusive::constant_time_size<false>>;

    using Hook = boost::intrusive::list_base_hook
        <boost::intrusive::link_mode<boost::intrusive::auto_unlink>>;

public:
    class Slot : public Hook {
    public:
        Slot() : pl(nullptr) {}

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;

        Slot(Slot&& o)
        {
            assert(!o.pl || o.s.use_count() >= 2);
            swap_nodes(o);
            pl = o.pl;
            o.pl = nullptr;
            ep = o.ep;
            o.ep = Udp::endpoint();
            s = std::move(o.s);
            assert(!pl || s.use_count() >= 2);
        }

        Slot& operator=(Slot&& o)
        {
            assert(!o.pl || o.s.use_count() >= 2);
            swap_nodes(o);
            std::swap(pl, o.pl);
            std::swap(ep, o.ep);
            std::swap(s, o.s);
            assert(!pl || s.use_count() >= 2);
            return *this;
        }

        ~Slot() {
            if (!pl) return;
            assert(s.use_count() >= 2);
            if (s.use_count() == 2) {
                pl->_slots.erase(ep);
            }
        }

    private:
        friend class PeerLimiter;

        Slot( PeerLimiter* pl
            , Udp::endpoint ep
            , std::shared_ptr<Scheduler::Slot> s)
            : pl(pl), ep(ep), s(std::move(s)) {
            assert(this->s.use_count() >= 2);
            }

        PeerLimiter* pl;
        Udp::endpoint ep;
        std::shared_ptr<Scheduler::Slot> s;
    };

public:
    PeerLimiter(asio::io_service& ios, size_t max_active_peers)
        : _scheduler(ios, max_active_peers)
    {}

    boost::optional<Slot> get_slot(Udp::endpoint ep)
    {
        auto i = _slots.find(ep);
        if (i == _slots.end()) {
            if (_slots.size() >= _scheduler.max_running_jobs())
                return boost::none;
            auto slot = std::make_shared<Scheduler::Slot>(_scheduler.get_slot());
            _slots.insert({ep, slot});
            return make_slot(ep, std::move(slot));
        }
        return make_slot(ep, i->second);
    }

    Slot wait_for_slot( Udp::endpoint ep
                      , Cancel& cancel
                      , asio::yield_context yield)
    {
        assert(!cancel);

        auto i = _slots.find(ep);
        if (i != _slots.end()) {
            return make_slot(ep, i->second);
        }

        sys::error_code ec;
        auto slot = _scheduler.wait_for_slot(cancel, yield[ec]);

        return_or_throw_on_error(yield, cancel, ec, Slot());

        i = _slots.find(ep);
        if (i != _slots.end()) {
            return make_slot(ep, i->second);
        }

        auto s = std::make_shared<Scheduler::Slot>(std::move(slot));
        assert(s.use_count() == 1);
        bool inserted = _slots.insert({ep, s}).second;
        assert(inserted);
        assert(s.use_count() == 2);
        auto ss = make_slot(ep, std::move(s));
        return ss;
    }

    size_t size() const { return _slots.size(); }

    ~PeerLimiter() {
        for (auto& s : _slot_list) {
            s.pl = nullptr;
        }
    }

private:
    Slot make_slot(Udp::endpoint ep, std::shared_ptr<Scheduler::Slot> s)
    {
        assert(s.use_count() >= 2);
        Slot ss(this, ep, std::move(s));
        _slot_list.push_back(ss);
        return ss;
    }

private:
    Scheduler _scheduler;
    std::map<Udp::endpoint, std::shared_ptr<Scheduler::Slot>> _slots;
    List<Slot> _slot_list;
};

}} // namespaces
