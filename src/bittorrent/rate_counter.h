#pragma once

namespace ouinet {

class RateCounter {
private:
    using Clock = std::chrono::steady_clock;

public:
    RateCounter()
        : _period(std::chrono::milliseconds(100))
        , _start(Clock::now())
        , _rate_since(_start)
    {}

    void update(float s)
    {
        _amount += s;
        normalize();
    }

    // Per second (*not* per `_period`)
    float rate() const
    {
        auto now = Clock::now();

        if (now - _start < _period) {
            return _amount;
        }

        return _amount / seconds(now - _rate_since);
    }

    void normalize()
    {
        auto now = Clock::now();
        if (now - _start < _period) return;
        _amount *= seconds(_period) / seconds(now - _rate_since);
        _rate_since = now - _period;
    }

    static float seconds(Clock::duration d) {
        using namespace std::chrono;
        return duration_cast<milliseconds>(d).count() / 1000.f;
    }

private:
    Clock::duration _period;
    Clock::time_point _start;
    Clock::time_point _rate_since;
    float _amount = 0;
};

} // namespace
