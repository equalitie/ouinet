#include <util/file_io.h>
#ifdef _WIN32
#include <util/file_io/stream_file.cpp>
#else
#include <util/file_io/posix.cpp>
#endif
