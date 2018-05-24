#pragma once

namespace ouinet {

template<class F>
class Defer {
public:
    Defer(F on_destruct)
        : _on_destruct(std::move(on_destruct))
    {}

    ~Defer() {
        _on_destruct();
    }

private:
    F _on_destruct;
};

template<class F> Defer<F> defer(F f) {
    return Defer<F>(std::move(f));
}

} // namespace
