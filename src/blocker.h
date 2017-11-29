#pragma once

#include <memory>
#include <boost/asio/spawn.hpp>

#include "namespaces.h"

/*
 * // This file defines the class Blocker to make it easy to wait for a
 * // set of spawned coroutines.
 *
 * // Usage:
 *
 * Blocker blocker(ios);
 *
 * {
 *     spawn(ios, [b = blocker.make_block()](auto yield) {
 *             Timer timer(ios);
 *             timer.expires_from_now(5s);
 *             timer.async_wait(yield);
 *         });
 *
 *     spawn(ios, [b = blocker.make_block()](auto yield) {
 *             Timer timer(ios);
 *             timer.expires_from_now(10s);
 *             timer.async_wait(yield);
 *         });
 *
 *     blocker.wait(yield); // shall wait 10 seconds (=max(5s, 10s)).
 * }
 *
 * // OR
 *
 * {
 *     spawn(ios, [b = blocker.make_block()](auto yield) {
 *             Timer timer(ios);
 *             timer.expires_from_now(5s);
 *             timer.async_wait(yield);
 *             // Now we instruct the 'blocker' to no longer wait
 *             // for the remaining blocks to get destroyed.
 *             b.release();
 *         });
 *
 *     spawn(ios, [b = blocker.make_block()](auto yield) {
 *             Timer timer(ios);
 *             timer.expires_from_now(10s);
 *             timer.async_wait(yield);
 *         });
 *
 *     blocker.wait(yield); // shall wait 5 seconds.
 * }
 *
 */
namespace ouinet {

class Blocker
{
    private:
    using token_t = asio::handler_type<asio::yield_context, void()>::type;
    
    struct WaitState {
        asio::async_result<token_t> result;
        std::function<void()> handler;

        WaitState(token_t&);
    };

    public:
    class Block
    {
        friend class Blocker;

        private:
        Block(Blocker& b, unsigned int round);
        
        public:
        Block(const Block&) = delete;
        Block& operator=(const Block&) = delete;

        Block(Block&&);
        Block& operator=(Block&&);

        void release();

        ~Block();
        
        private:
        Blocker* _blocker;
        unsigned int _round;
    };
    
    Blocker(asio::io_service& ios);

    Blocker(const Blocker&) = delete;
    Blocker& operator=(const Blocker&) = delete;

    Block make_block();
    
    void wait(asio::yield_context yield);

    asio::io_service& get_io_service();

    ~Blocker();

    private:
    asio::io_service& _ios;
    unsigned int _round = 0;
    unsigned int _block_count = 0;
    bool _released = false;
    WaitState* _wait_state = nullptr;
};


inline
Blocker::Blocker(asio::io_service& ios)
    : _ios(ios)
{
}


inline
Blocker::Block Blocker::make_block()
{
    return Block(*this, _round);
}
    

inline
void Blocker::wait(asio::yield_context yield)
{
    // Exit right a way if no block has been created.
    if (!_block_count) return;

    sys::error_code ec;
    token_t token(yield[ec]);
    WaitState ws(token);
    _wait_state = &ws;

    _wait_state->result.get();

    // Prepare Blocker for re-use with a new set of Blocks.
    ++_round;
    _released = false;
    _block_count = 0;
    _wait_state = nullptr;
}


inline
asio::io_service& Blocker::get_io_service()
{
    return _ios;
}


inline
Blocker::~Blocker()
{
    assert(_block_count == 0);
}


inline
Blocker::Block::Block(Blocker& b, unsigned int round)
    : _blocker(&b)
    , _round(round)
{
    ++_blocker->_block_count;
}


inline
Blocker::Block::Block(Block&& other)
    : _blocker(other._blocker)
    , _round(other._round)
{
    other._blocker = nullptr;
}


inline
Blocker::Block& Blocker::Block::operator=(Block&& other)
{
    _blocker = other._blocker;
    _round = other._round;
    other._blocker = nullptr;
    return *this;
}


inline
void Blocker::Block::release()
{
    if (_round != _blocker->_round) return;

    // Idempotent
    if (_blocker->_released) return;
    _blocker->_released = true;

    _blocker->get_io_service()
        .post(std::move(_blocker->_wait_state->handler));
}


inline
Blocker::Block::~Block()
{
    if (!_blocker) return; // Was moved from

    if (_round != _blocker->_round) return;

    if (--_blocker->_block_count == 0 && !_blocker->_released) {
        assert(_blocker->_wait_state->handler);

        _blocker->get_io_service()
            .post(std::move(_blocker->_wait_state->handler));
    }
}

        
inline
Blocker::WaitState::WaitState(token_t& token)
    : result(token)
    , handler(token)
{}


} // ouinet namespace
