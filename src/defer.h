#pragma once

namespace ouinet {

class Defer {
public:
    Defer(std::function<void()> on_destruct)
        : _on_destruct(std::move(on_destruct))
    {}

    ~Defer() {
        _on_destruct();
    }

private:
    std::function<void()> _on_destruct;
};

} // namespace
