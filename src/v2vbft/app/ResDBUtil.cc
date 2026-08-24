#include "veins/modules/application/resDB/ResDBUtil.h"

#include <cstdlib>

namespace veins {
namespace resdb_app_util {

std::vector<std::string> splitStr(const std::string& s, char delim)
{
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            parts.push_back(cur);
            cur.clear();
        }
        else {
            cur += c;
        }
    }
    parts.push_back(cur);
    return parts;
}

std::string toHex(const std::vector<uint8_t>& v)
{
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.reserve(v.size() * 2);
    for (uint8_t b : v) {
        out += digits[b >> 4];
        out += digits[b & 0xf];
    }
    return out;
}

std::string toHex(const uint8_t* p, size_t len)
{
    return toHex(std::vector<uint8_t>(p, p + len));
}

std::vector<uint8_t> fromHex(const std::string& s)
{
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < s.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoi(s.substr(i, 2), nullptr, 16)));
    return out;
}

std::string dirToStr(ResDBIntersectionApp::Direction d)
{
    switch (d) {
        case ResDBIntersectionApp::DIR_LEFT:  return "L";
        case ResDBIntersectionApp::DIR_RIGHT: return "R";
        case ResDBIntersectionApp::DIR_UNKNOWN: return "U";
        default:                              return "S";
    }
}

ResDBIntersectionApp::Direction strToDir(const std::string& s)
{
    if (s == "L") return ResDBIntersectionApp::DIR_LEFT;
    if (s == "R") return ResDBIntersectionApp::DIR_RIGHT;
    if (s == "U") return ResDBIntersectionApp::DIR_UNKNOWN;
    return ResDBIntersectionApp::DIR_STRAIGHT;
}

const char* phaseToStr(int p)
{
    switch (p) {
        case 0: return "IDLE";
        case 1: return "COLLECTING_CERTS";
        case 2: return "WAITING_FOR_CLEARANCE";
        case 3: return "PULLING_FORWARD";
        case 4: return "EXECUTING";
        case 5: return "DEPARTED";
    }
    return "UNKNOWN";
}

uint8_t laneCode(const std::string& lane)
{
    if (lane == "S") return 1;
    if (lane == "E") return 2;
    if (lane == "W") return 3;
    return 0;
}

uint8_t directionCode(ResDBIntersectionApp::Direction direction)
{
    if (direction == ResDBIntersectionApp::DIR_LEFT) return 1;
    if (direction == ResDBIntersectionApp::DIR_RIGHT) return 2;
    if (direction == ResDBIntersectionApp::DIR_UNKNOWN) return 3;
    return 0;
}

} // namespace resdb_app_util
} // namespace veins
