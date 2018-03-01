#pragma once

#include <boost/intrusive/list.hpp>

namespace ouinet {

class Shutter {
private:
    using Hook
        = boost::intrusive::list_base_hook
            <boost::intrusive::link_mode
                <boost::intrusive::auto_unlink>>;

public:
    using Cancel = std::function<void()>;

    Shutter()                          = default;
    Shutter(const Shutter&)            = delete;
    Shutter& operator=(const Shutter&) = delete;

    class Handle : public Hook {
        friend class Shutter;
        Cancel cancel;
    };

    void close_everything()
    {
        auto hs = std::move(_handles);
        for (auto& h : hs) {
            // Some functions, such as the tcp::socket::close()
            // may throw if the socket is already closed. We just
            // ignore such exceptions because the purpose of
            // this function (to leave such socket in a closed
            // state) will be fulfilled in either case.
            try { h.cancel(); } catch (...) {}
        }
    }

    Handle add(Cancel cancel)
    {
        Handle handle;
        handle.cancel = std::move(cancel);
        _handles.push_back(handle);
        return handle;
    }

private:
    boost::intrusive::list
        < Handle
        , boost::intrusive::constant_time_size<false>
        > _handles;
};

} // ouinet namespace
