#pragma once

namespace ouinet {

namespace detail {
    template<class T> struct Print {
        friend std::ostream& operator<<(std::ostream& os, const Print<T>& p) {
            return os << p.val;
        }
        T val;
    };
    
    template<> struct Print<sys::error_code> {
        friend std::ostream& operator<<(std::ostream& os, const Print<sys::error_code>& p) {
            return os << p.ec.message();
        }
        sys::error_code ec;
    };
} // detail namespace

template<typename T, typename E> T unwrap(std::expected<T, E> exp, std::source_location loc = std::source_location::current()) {
    if (!exp.has_value()) {
        BOOST_FAIL(loc.file_name() << ":" << loc.line() << " error:" << detail::Print<E>{exp.error()});
    } else {
        BOOST_CHECK(true);
    }
    return std::move(*exp);
}

template<typename E> void unwrap(std::expected<void, E> exp, std::source_location loc = std::source_location::current()) {
    if (!exp.has_value()) {
        BOOST_FAIL(loc.file_name() << ":" << loc.line() << " error:" << detail::Print<E>{exp.error()});
    } else {
        BOOST_CHECK(true);
    }
}

template<typename E> void unwrap(std::optional<E> opt, std::source_location loc = std::source_location::current()) {
    if (!opt) {
        BOOST_FAIL(loc.file_name() << ":" << loc.line() << " error: optional is `none`");
    } else {
        BOOST_CHECK(true);
    }
    return std::move(*opt);
}

template<typename E> E& unwrap(E* ptr, std::source_location loc = std::source_location::current()) {
    if (!ptr) {
        BOOST_FAIL(loc.file_name() << ":" << loc.line() << " error: pointer is `nullptr`");
    } else {
        BOOST_CHECK(true);
    }
    return *ptr;
}

void unwrap(sys::error_code ec, std::source_location loc = std::source_location::current()) {
    if (ec) {
        BOOST_FAIL(loc.file_name() << ":" << loc.line() << " error:" << ec.message());
    } else {
        BOOST_CHECK(true);
    }
}

} // ouinet namespace
