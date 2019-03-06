#pragma once

namespace ouinet {

/*
 * // This is a utility function to help working with asio::yield_context. In
 * // particular, yield_context is missing some API to determine whether the
 * // caller expects the async call to throw, or set an error code hiding in
 * // yield.
 *
 * // Consider an implementation this function:
 *
 * int my_async_function(yield_context yield) {
 *    ...
 *
 *    // Here we don't know whether the caller called this function as:
 *    //
 *    // A1: `my_async_function(yield);`
 *    // Or
 *    // B1: `my_async_function(yield[ec]);`
 *    //
 *    // If we want to "yield" an error, we don't know whether to do it
 *    // with:
 *    //
 *    // A2: `throw system_error(my_error_code);`
 *    // Or, assuming there exists a yield_context::set_error function
 *    // B2: `yield.set_error(my_error_code); return {};`
 *    //
 *    // Note that A2 can only be used in context of A1 and B2 in context of
 *    // B1.
 *    //
 *    // Thus, we need to either explicitly distinguish between A1 and B1 by
 *    // inspecting the `yield.ec_` pointer to an error_code instance, or use
 *    // the below defined function `or_throw` which does it for us.
 *
 *    return or_throw(yield, my_error, 42);
 *
 *    // Note that `or_throw` is always meant to be used after the `return`
 *    // keyword (hence the name). Not using it with return would - again -
 *    // lead to two non deterministic code paths (depending on whether
 *    // we're in the A1 or B1 context).
 *    ...
 * }
 *
 */

template<class Ret>
inline
Ret or_throw( asio::yield_context yield
            , const sys::error_code& ec
            , Ret&& ret = {})
{
    if (!ec) return std::forward<Ret>(ret);
    if (yield.ec_) { *yield.ec_ = ec; }
    else { throw sys::system_error(ec); }
    return std::forward<Ret>(ret);
}

inline
void or_throw(asio::yield_context yield, const sys::error_code& ec)
{
    if (!ec) return;
    if (yield.ec_) { *yield.ec_ = ec; }
    else { throw sys::system_error(ec); }
}

#define return_or_throw_on_error(yield, cancel, ec, ...) \
    if (cancel || ec) {\
        assert(!cancel || ec == asio::error::operation_aborted); \
        if (cancel) ec = asio::error::operation_aborted; \
        return or_throw(yield, ec, ##__VA_ARGS__); \
    }

}
