#pragma once

namespace ouinet {

template<class Ret>
inline
Ret or_throw( asio::yield_context yield
            , const sys::error_code& ec
            , Ret&& ret = {})
{
    if (!ec) return std::forward<Ret>(ret);
    if (yield.ec_) { *yield.ec_ = ec; }
    else { throw sys::system_error(ec); }
    return {}; // Dead code, but avoids warnings.
}

inline
void or_throw(asio::yield_context yield, const sys::error_code& ec)
{
    if (!ec) return;
    if (yield.ec_) { *yield.ec_ = ec; }
    else { throw sys::system_error(ec); }
}

}
