#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/async_result.hpp>
#include <memory>

namespace ouinet::util {

struct Semaphore {
private:
    struct State;

public:
    class Lock {
    public:
        Lock(Lock&&) = default;
        Lock(Lock const&) = delete;

        ~Lock() {
            if (std::shared_ptr<State> s = _state.lock()) {
                s->on_one_unlocked(sys::error_code());
            }
        }

    private:
        friend class State;
        friend class Semaphore;
        std::weak_ptr<State> _state;
        Lock(std::weak_ptr<State> state) : _state(std::move(state)) {}
    };

private:
    using Handler = asio::any_completion_handler<void (sys::error_code, Lock)>;

    struct State : std::enable_shared_from_this<State> {
        const size_t _max_lock_count;
        size_t _lock_count = 0;

        std::deque<Handler> _handlers;
        asio::any_io_executor _executor;

        State(size_t max, asio::any_io_executor executor) :
            _max_lock_count(max),
            _executor(std::move(executor))
        {}

        bool can_create_lock() const {
            return _lock_count < _max_lock_count;
        }

        Lock create_lock() {
            ++_lock_count;
            Lock lock(weak_from_this());
            return lock;
        }

        void on_one_unlocked(sys::error_code ec) {
            if (_handlers.empty()) {
                --_lock_count;
                return;
            }

            auto handler = std::move(_handlers.front());
            _handlers.pop_front();

            asio::post(_executor, [ec, h = std::move(handler), state = weak_from_this()] () mutable {
                if (auto s = state.lock()) {
                    h(sys::error_code(), s->create_lock());
                }
                else {
                    h(asio::error::operation_aborted, Lock(std::move(state)));
                }
            });
        }

        void abort() {
            while (!_handlers.empty()) {
                on_one_unlocked(asio::error::operation_aborted);
            }
        }
    };

    std::shared_ptr<State> _state;

public:
    Semaphore(size_t max, asio::any_io_executor executor) :
        _state(std::make_shared<State>(max, std::move(executor)))
    {}

    Semaphore(Semaphore&&) = default;

    template<class Token>
    auto await_lock(Token token) {
        return boost::asio::async_initiate<
            Token,
            void(sys::error_code, Lock lock)>(
          [this] (auto handler) {
              if (!_state) {
                  return handler(asio::error::operation_aborted, Lock(std::weak_ptr<State>()));
              }
              if (_state->can_create_lock()) {
                  return handler({}, _state->create_lock());
              }
              _state->_handlers.push_back(std::move(handler));
          },
          token);
    }

    ~Semaphore() {
        if (!_state) return; // moved
        _state->abort();
    }
};

} // namespace
