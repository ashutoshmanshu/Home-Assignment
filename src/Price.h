#ifndef MATCHING_ENGINE_PRICE_H
#define MATCHING_ENGINE_PRICE_H

#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace me {

// Fixed-point price.
//
// Prices in the spec are "decimal numbers", but comparing them with binary
// floating point (double) is unsafe: values like 0.1 are not representable, so
// two prices that should be equal can compare unequal and break price priority.
// We therefore store the price as a 64-bit integer count of "ticks", where one
// unit of currency == SCALE ticks. All comparison and ordering is exact integer
// arithmetic, which is also faster than floating point.
//
// FRAC_DIGITS controls the supported precision (4 decimal places here, ample for
// equity-style prices). Inputs with more fractional digits are rejected at parse
// time rather than silently rounded, so we never lose information unknowingly.
class Price {
public:
    static constexpr int FRAC_DIGITS = 4;
    static constexpr std::int64_t SCALE = 10000; // 10^FRAC_DIGITS

    Price() = default;

    static Price fromTicks(std::int64_t ticks) { return Price(ticks); }
    std::int64_t ticks() const { return ticks_; }

    // Parse a positive decimal price. Returns nullopt on any malformed input
    // (garbage characters, negative/zero value, or excess precision).
    static std::optional<Price> parse(const std::string& s) {
        std::size_t i = 0;
        if (i < s.size() && s[i] == '+') ++i;

        bool anyDigit = false;
        bool overflow = false;
        std::int64_t intPart = 0;
        for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
            anyDigit = true;
            if (intPart > std::numeric_limits<std::int64_t>::max() / 10) overflow = true;
            intPart = intPart * 10 + (s[i] - '0');
        }

        std::int64_t fracTicks = 0;
        int fracDigits = 0;
        if (i < s.size() && s[i] == '.') {
            ++i;
            for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
                anyDigit = true;
                if (fracDigits >= FRAC_DIGITS) return std::nullopt; // too much precision
                fracTicks = fracTicks * 10 + (s[i] - '0');
                ++fracDigits;
            }
        }

        if (i != s.size() || !anyDigit || overflow) return std::nullopt;

        for (; fracDigits < FRAC_DIGITS; ++fracDigits) fracTicks *= 10;
        if (intPart > (std::numeric_limits<std::int64_t>::max() - fracTicks) / SCALE)
            return std::nullopt;

        std::int64_t ticks = intPart * SCALE + fracTicks;
        if (ticks <= 0) return std::nullopt; // price must be strictly positive
        return Price(ticks);
    }

    // Render without trailing fractional zeros so "1025.0000" prints as "1025".
    std::string toString() const {
        std::int64_t intPart = ticks_ / SCALE;
        std::int64_t frac = ticks_ % SCALE;
        std::string out = std::to_string(intPart);
        if (frac != 0) {
            char buf[FRAC_DIGITS];
            for (int d = FRAC_DIGITS - 1; d >= 0; --d) {
                buf[d] = static_cast<char>('0' + frac % 10);
                frac /= 10;
            }
            int len = FRAC_DIGITS;
            while (len > 0 && buf[len - 1] == '0') --len;
            out.push_back('.');
            out.append(buf, buf + len);
        }
        return out;
    }

    bool operator==(Price o) const { return ticks_ == o.ticks_; }
    bool operator!=(Price o) const { return ticks_ != o.ticks_; }
    bool operator<(Price o) const { return ticks_ < o.ticks_; }
    bool operator>(Price o) const { return ticks_ > o.ticks_; }
    bool operator<=(Price o) const { return ticks_ <= o.ticks_; }
    bool operator>=(Price o) const { return ticks_ >= o.ticks_; }

private:
    explicit Price(std::int64_t ticks) : ticks_(ticks) {}
    std::int64_t ticks_ = 0;
};

} // namespace me

#endif // MATCHING_ENGINE_PRICE_H
