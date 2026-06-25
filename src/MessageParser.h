#ifndef MATCHING_ENGINE_MESSAGE_PARSER_H
#define MATCHING_ENGINE_MESSAGE_PARSER_H

#include "Message.h"

#include <string>

namespace me {

// Outcome of parsing a single input line.
//
// The parser never throws and never partially applies a request: a line is
// either a fully valid Add/Cancel, a blank/comment line to skip, or an Error
// carrying a human-readable reason for stderr. This is what lets the driver
// guarantee "no input crashes the application".
struct ParseResult {
    enum class Kind { Add, Cancel, Skip, Error };

    Kind kind = Kind::Error;
    AddOrderRequest add;
    CancelOrderRequest cancel;
    std::string error;

    static ParseResult makeAdd(const AddOrderRequest& r) { ParseResult p; p.kind = Kind::Add; p.add = r; return p; }
    static ParseResult makeCancel(const CancelOrderRequest& r) { ParseResult p; p.kind = Kind::Cancel; p.cancel = r; return p; }
    static ParseResult makeSkip() { ParseResult p; p.kind = Kind::Skip; return p; }
    static ParseResult makeError(std::string msg) { ParseResult p; p.kind = Kind::Error; p.error = std::move(msg); return p; }
};

// Stateless line parser. Tolerates surrounding whitespace, blank lines, and
// trailing `//` or `#` comments (handy for human-authored datasets); the
// canonical protocol input simply contains none of those.
class MessageParser {
public:
    ParseResult parseLine(const std::string& rawLine) const;
};

} // namespace me

#endif // MATCHING_ENGINE_MESSAGE_PARSER_H
