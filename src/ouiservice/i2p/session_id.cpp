#include "session_id.h"
#include "util/random.h"
#include <cstdint>
#include <sstream>

namespace ouinet {

/* static */
SessionId SessionId::random() {
  std::stringstream s;
  s << util::random::number<uint64_t>();
  s << util::random::number<uint64_t>();
  return SessionId{s.str()};
}

} // namespace ouinet
