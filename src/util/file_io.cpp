#include <util/file_io.h>
#if defined(_WIN32) || defined(__MINGW32__)
#include <util/file_io/stream_file.cpp>
#else
#include <util/file_io/posix.cpp>
#endif
