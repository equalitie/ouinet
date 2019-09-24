#include <chrono>
#include <boost/chrono.hpp>
#include <time.h>

namespace ouinet { namespace util { namespace time {

namespace __detail {
    template<class From, class To>
    struct Convert;

    template<>
    struct Convert<std::chrono::system_clock::time_point, time_t>{
        static
        time_t convert(std::chrono::system_clock::time_point t) {
            return std::chrono::system_clock::to_time_t(t);
        }
    };

    template<>
    struct Convert<std::chrono::steady_clock::time_point, time_t>{
        static
        time_t convert(std::chrono::steady_clock::time_point t) {
            return std::chrono::system_clock::to_time_t
                ( std::chrono::system_clock::now()
                + (t - std::chrono::steady_clock::now()));
        }
    };

    template<>
    struct Convert< std::chrono::steady_clock::time_point
                  , std::chrono::system_clock::time_point>{
        static
        std::chrono::system_clock::time_point
        convert(std::chrono::steady_clock::time_point t) {
            time_t tt = Convert< std::chrono::steady_clock::time_point
                               , time_t
                               >::convert(t);

            return std::chrono::system_clock::from_time_t(tt);
        }
    };

    template<>
    struct Convert< std::chrono::system_clock::time_point
                  , std::chrono::steady_clock::time_point>{
        static
        std::chrono::steady_clock::time_point
        convert(std::chrono::system_clock::time_point t) {
            return std::chrono::steady_clock::now()
                 + (t - std::chrono::system_clock::now());
        }
    };

    template<>
    struct Convert< std::chrono::system_clock::time_point
                  , boost::chrono::system_clock::time_point>{
        static
        boost::chrono::system_clock::time_point
        convert(std::chrono::system_clock::time_point t) {
            return boost::chrono::system_clock::from_time_t(
                    std::chrono::system_clock::to_time_t(t));
        }
    };

    template<>
    struct Convert< std::chrono::steady_clock::time_point
                  , boost::chrono::system_clock::time_point>{
        static
        boost::chrono::system_clock::time_point
        convert(std::chrono::steady_clock::time_point t) {
            return Convert< std::chrono::system_clock::time_point
                          , boost::chrono::system_clock::time_point
                          >::convert( Convert< std::chrono::steady_clock::time_point
                                             , std::chrono::system_clock::time_point
                                             >::convert(t));
        }
    };
}

template<class To, class From>
To convert(const From& from) {
    return __detail::Convert<From, To>::convert(from);
}

}}} // namespaces
