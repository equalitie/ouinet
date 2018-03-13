#pragma once

#include <android/log.h>

namespace ouinet {

class RedirectToAndroidLog {
private:
    struct AndroidLogBuf : public std::streambuf {
        // TODO: We're currently appending to the buffer char-by-char.
        // There is likely a more efficient way.
        int overflow(int c) {
    	    if (c != traits_type::eof()) {
                if (c == '\n') {
                    __android_log_print(ANDROID_LOG_INFO,
                                       "Ouinet",
                                       "%s",
                                       buffer.c_str());
                    buffer.clear();
                }
                else {
                    buffer.push_back(c);
                }
    	    }
    	    return c;
        }
    
        std::string buffer;
    };

public:
    RedirectToAndroidLog(std::ostream& os)
        : _os(os)
        , _old_rdbuf(os.rdbuf(&_android_log_buf))
    { }

    ~RedirectToAndroidLog()
    {
        _os.rdbuf(_old_rdbuf);
    }

private:
    std::ostream& _os;
    AndroidLogBuf _android_log_buf;
    std::streambuf* _old_rdbuf;
};

} // ouinet namespace
