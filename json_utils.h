#pragma once

#include <iomanip>
#include <sstream>
#include <string>

// JSON escaping for arbitrary text; no external serialization dependency.
inline std::string jsonString(const std::string& text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<unsigned>(c) << std::dec;
            } else out << c;
        }
    }
    out << '"';
    return out.str();
}
