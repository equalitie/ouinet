#pragma once

#include <cassert>
#include <list>
#include <memory>
#include <boost/asio/spawn.hpp>

#include "blocker.h"

namespace ouinet {

template<typename T>
class CoroutineSet
{
    public:
    CoroutineSet(boost::asio::io_service& ios);
    void run(std::unique_ptr<T> coroutine, std::function<void(T*, boost::asio::yield_context)> run);
    T* coroutine();
    std::vector<T*> coroutines();
    void wait_empty(boost::asio::yield_context yield);

    private:
    boost::asio::io_service& _ios;
    std::list<std::unique_ptr<T>> _coroutines;
    Blocker _blocker;
};

template<typename T>
CoroutineSet<T>::CoroutineSet(boost::asio::io_service& ios):
    _ios(ios),
    _blocker(ios)
{}

template<typename T>
void CoroutineSet<T>::run(std::unique_ptr<T> coroutine, std::function<void(T*, boost::asio::yield_context)> run)
{
    typename std::list<std::unique_ptr<T>>::iterator it = _coroutines.insert(_coroutines.end(), std::move(coroutine));

    boost::asio::spawn(_ios, [this, it, run, b = _blocker.make_block()](boost::asio::yield_context yield) mutable {
        run((*it).get(), yield);

        _coroutines.erase(it);
    });
}

template<typename T>
T* CoroutineSet<T>::coroutine()
{
    std::vector<T*> output = coroutines();
    assert(output.size() == 1);
    return output[0];
}

template<typename T>
std::vector<T*> CoroutineSet<T>::coroutines()
{
    std::vector<T*> output;
    for (auto& it : _coroutines) {
        output.push_back(it.get());
    }
    return output;
}

template<typename T>
void CoroutineSet<T>::wait_empty(boost::asio::yield_context yield)
{
    _blocker.wait(yield);
}

} // ouinet namespace
