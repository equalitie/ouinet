#pragma once

#include <iomanip>

namespace ouinet { namespace bittorrent {

class BytePrinter {
public:
    using const_iterator = const char*;

    BytePrinter(const std::string& s)
        : _begin(s.data())
        , _end(s.data() + s.size())
    {}

    template<size_t Size>
    BytePrinter(const std::array<uint8_t, Size>& s)
        : _begin((const char*) s.data())
        , _end((const char*) s.data() + s.size())
    {}

    const char* begin() const { return _begin; }
    const char* end()   const { return _end; }

private:
    const char* _begin;
    const char* _end;
};

inline
std::ostream& operator<<(std::ostream& os, const BytePrinter& bp)
{
    for (char ch : bp) {
        if (ch == '\\' || ch == '"') {
            os << "\\" << ch;
        }
        else if (' ' <= ch && ch <= '~') {
            os << ch;
        }
        else {
            os << "\\x" << std::setw(2)
                        << std::setfill('0')
                        << std::hex
                        << int((uint8_t) ch)
                        << std::dec;
        }
    }
    return os;
}

}} // namespaces
