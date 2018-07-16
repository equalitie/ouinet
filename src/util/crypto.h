#pragma once

#include <array>
#include <string>
#include <vector>

namespace ouinet {
namespace util {

std::string random(unsigned int size);

std::array<uint8_t, 20> sha1(const std::string& data);
std::array<uint8_t, 20> sha1(const std::vector<unsigned char>& data);

} // util namespace
} // ouinet namespace
