#pragma once

#include "veins/modules/application/resDB/protocol/Primitives.h"

#include <cstdint>
#include <string>
#include <vector>

namespace veins {
namespace resdb_app_util {

constexpr int kArrivalAnnounceType   = 1;
constexpr int kArrivalEchoType       = 4;
constexpr int kArrivalCertType       = 5;
constexpr int kResdbConsensusMsgType = 8;

std::vector<std::string> splitStr(const std::string& s, char delim);
std::string toHex(const std::vector<uint8_t>& v);
std::string toHex(const uint8_t* p, size_t len);
std::vector<uint8_t> fromHex(const std::string& s);
std::string dirToStr(Direction d);
Direction strToDir(const std::string& s);
const char* phaseToStr(int p);
uint8_t laneCode(const std::string& lane);
uint8_t directionCode(Direction direction);

} // namespace resdb_app_util
} // namespace veins
