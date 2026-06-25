#include "MessageParser.h"

#include <limits>
#include <optional>
#include <vector>

namespace me {
namespace {

std::string trim(const std::string& s) {
    const char* ws = " \t\r\n\f\v";
    const auto begin = s.find_first_not_of(ws);
    if (begin == std::string::npos) return std::string();
    const auto end = s.find_last_not_of(ws);
    return s.substr(begin, end - begin + 1);
}

// Drop a trailing line comment introduced by "//" or "#".
std::string stripComment(const std::string& s) {
    const auto slashes = s.find("//");
    const auto hash = s.find('#');
    const auto cut = std::min(slashes, hash);
    return cut == std::string::npos ? s : s.substr(0, cut);
}

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (true) {
        const auto comma = s.find(',', start);
        if (comma == std::string::npos) {
            fields.push_back(trim(s.substr(start)));
            break;
        }
        fields.push_back(trim(s.substr(start, comma - start)));
        start = comma + 1;
    }
    return fields;
}

// Parse a non-negative integer composed solely of digits (with an optional
// leading '+'). Rejects empty strings, stray characters, and overflow.
std::optional<std::uint64_t> parseUInt(const std::string& s) {
    std::size_t i = 0;
    if (i < s.size() && s[i] == '+') ++i;
    if (i >= s.size()) return std::nullopt;

    std::uint64_t value = 0;
    const std::uint64_t kMax = std::numeric_limits<std::uint64_t>::max();
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(s[i] - '0');
        if (value > (kMax - digit) / 10) return std::nullopt; // overflow
        value = value * 10 + digit;
    }
    return value;
}

ParseResult parseAdd(const std::vector<std::string>& f, const std::string& line) {
    if (f.size() != 5)
        return ParseResult::makeError("Malformed AddOrderRequest (expected 5 fields): " + line);

    const auto id = parseUInt(f[1]);
    if (!id || *id == 0)
        return ParseResult::makeError("Invalid order id in AddOrderRequest: " + line);

    if (f[2] != "0" && f[2] != "1")
        return ParseResult::makeError("Invalid side (expected 0 or 1): " + line);

    const auto qty = parseUInt(f[3]);
    if (!qty || *qty == 0)
        return ParseResult::makeError("Invalid quantity (expected positive integer): " + line);

    const auto price = Price::parse(f[4]);
    if (!price)
        return ParseResult::makeError("Invalid price (expected positive decimal): " + line);

    AddOrderRequest req;
    req.id = *id;
    req.side = (f[2] == "0") ? Side::Buy : Side::Sell;
    req.quantity = *qty;
    req.price = *price;
    return ParseResult::makeAdd(req);
}

ParseResult parseCancel(const std::vector<std::string>& f, const std::string& line) {
    if (f.size() != 2)
        return ParseResult::makeError("Malformed CancelOrderRequest (expected 2 fields): " + line);

    const auto id = parseUInt(f[1]);
    if (!id || *id == 0)
        return ParseResult::makeError("Invalid order id in CancelOrderRequest: " + line);

    CancelOrderRequest req;
    req.id = *id;
    return ParseResult::makeCancel(req);
}

} // namespace

ParseResult MessageParser::parseLine(const std::string& rawLine) const {
    const std::string line = trim(stripComment(rawLine));
    if (line.empty()) return ParseResult::makeSkip();

    const std::vector<std::string> fields = splitCsv(line);

    const auto msgType = parseUInt(fields[0]);
    if (!msgType)
        return ParseResult::makeError("Unknown message type: " + fields[0]);

    switch (static_cast<MsgType>(*msgType)) {
        case MsgType::AddOrder:    return parseAdd(fields, line);
        case MsgType::CancelOrder: return parseCancel(fields, line);
        default:
            // 2/3/4 are engine outputs; anything else is simply unrecognized.
            return ParseResult::makeError("Unknown message type: " + fields[0]);
    }
}

} // namespace me
