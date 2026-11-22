#pragma once
#include "ob/types.hpp"
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ob {

// Fast field splitting with string_view + from_chars: no allocation, no
// locale, no exceptions (unlike std::stoi / stringstream).
namespace detail {
inline std::string_view nextField(std::string_view& s) {
    const auto comma = s.find(',');
    std::string_view f = s.substr(0, comma);
    s = (comma == std::string_view::npos) ? std::string_view{} : s.substr(comma + 1);
    return f;
}

template <class Int>
inline bool toInt(std::string_view f, Int& out) {
    if (f.empty()) { out = 0; return true; }  // empty field (e.g. cancel) -> 0
    auto [p, ec] = std::from_chars(f.data(), f.data() + f.size(), out);
    return ec == std::errc{} && p == f.data() + f.size();
}

inline std::string_view trimEol(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.remove_suffix(1);
    return line;
}
}  // namespace detail

// ---------------------------------------------------------------------------
// Our own CSV format, one command per line:
//   # type,id,side,price,qty
//   A,1,S,10125,100
//   C,1,,,
//   M,2,B,10122,40
// Returns false for blank lines, comments and malformed lines.
// ---------------------------------------------------------------------------
inline bool parseCommand(std::string_view line, Command& c) {
    line = detail::trimEol(line);
    if (line.empty() || line[0] == '#') return false;

    std::string_view type = detail::nextField(line);
    std::string_view id   = detail::nextField(line);
    std::string_view side = detail::nextField(line);
    std::string_view px   = detail::nextField(line);
    std::string_view qty  = detail::nextField(line);

    if (type.size() != 1) return false;
    switch (type[0]) {
        case 'A': c.type = CmdType::Add; break;
        case 'C': c.type = CmdType::Cancel; break;
        case 'M': c.type = CmdType::Modify; break;
        default: return false;
    }
    if (id.empty() || !detail::toInt(id, c.id)) return false;
    if (side == "B") c.side = Side::Buy;
    else if (side == "S") c.side = Side::Sell;
    else if (side.empty() && c.type == CmdType::Cancel) c.side = Side::Buy;
    else return false;
    if (!detail::toInt(px, c.price) || !detail::toInt(qty, c.qty)) return false;
    if (c.type == CmdType::Add && c.qty == 0) return false;
    return true;
}

// Calls fn(line) for every line in a buffer, without copying.
template <class Fn>
inline void forEachLine(std::string_view buf, Fn&& fn) {
    while (!buf.empty()) {
        const auto nl = buf.find('\n');
        fn(buf.substr(0, nl));
        if (nl == std::string_view::npos) break;
        buf.remove_prefix(nl + 1);
    }
}

// Formats a command back into the CSV format above (used by gen).
inline std::string formatCommand(const Command& c) {
    std::string s;
    s += (c.type == CmdType::Add ? 'A' : c.type == CmdType::Cancel ? 'C' : 'M');
    s += ',';
    s += std::to_string(c.id);
    if (c.type == CmdType::Cancel) return s + ",,,";
    s += (c.side == Side::Buy ? ",B," : ",S,");
    s += std::to_string(c.price);
    s += ',';
    s += std::to_string(c.qty);
    return s;
}

// ---------------------------------------------------------------------------
// Databento MBO (market-by-order) CSV.
//
// Columns are found by name from the header line, so column order doesn't
// matter. We use: action, side, price, size, order_id.
//   action  A/C/M/R/T/F/N   (T, F, N don't change the book; R clears it)
//   side    B / A / N       (A = ask = sell)
//   price   fixed-point int, 1 unit = 1e-9 dollars (or a decimal string if
//           the file was exported with pretty prices)
//   size    for C, the quantity removed (cancels can be partial)
// ---------------------------------------------------------------------------
struct MboRecord {
    char     action;   // 'A','C','M','R','T','F','N'
    Side     side;
    OrderId  id;
    Price    price;    // already converted to ticks
    Quantity size;
};

struct MboColumns {
    int action = -1, side = -1, price = -1, size = -1, orderId = -1;
    int count = 0;
    bool ok() const { return action >= 0 && side >= 0 && price >= 0 && size >= 0 && orderId >= 0; }
};

inline MboColumns parseMboHeader(std::string_view header) {
    header = detail::trimEol(header);
    MboColumns cols;
    int i = 0;
    while (true) {
        std::string_view f = detail::nextField(header);
        if (f == "action") cols.action = i;
        else if (f == "side") cols.side = i;
        else if (f == "price") cols.price = i;
        else if (f == "size") cols.size = i;
        else if (f == "order_id") cols.orderId = i;
        ++i;
        if (header.empty()) break;
    }
    cols.count = i;
    return cols;
}

// Databento's "undefined price" sentinel (INT64_MAX).
inline constexpr std::int64_t kMboUndefPrice = INT64_MAX;

// Convert a Databento price field to ticks. tickNanos = size of one tick in
// 1e-9 dollar units (cents = 10'000'000).
inline bool mboPriceToTicks(std::string_view f, std::int64_t tickNanos, Price& out) {
    if (f.empty()) return false;
    const auto dot = f.find('.');
    if (dot == std::string_view::npos) {               // fixed-point integer
        std::int64_t raw = 0;
        if (!detail::toInt(f, raw) || raw == kMboUndefPrice) return false;
        out = raw / tickNanos;
        return true;
    }
    // Decimal string, e.g. "101.25": build nanos = whole*1e9 + frac*1e9.
    std::int64_t whole = 0;
    if (!detail::toInt(f.substr(0, dot), whole)) return false;
    std::string_view frac = f.substr(dot + 1);
    std::int64_t nanos = 0;
    int digits = 0;
    for (char ch : frac) {
        if (ch < '0' || ch > '9') return false;
        if (digits < 9) { nanos = nanos * 10 + (ch - '0'); ++digits; }
    }
    while (digits < 9) { nanos *= 10; ++digits; }
    out = (whole * 1'000'000'000 + nanos) / tickNanos;
    return true;
}

inline bool parseMboLine(std::string_view line, const MboColumns& cols,
                         std::int64_t tickNanos, MboRecord& r) {
    line = detail::trimEol(line);
    if (line.empty()) return false;
    std::string_view action, side, price, size, orderId;
    for (int i = 0; i < cols.count; ++i) {
        std::string_view f = detail::nextField(line);
        if (i == cols.action) action = f;
        else if (i == cols.side) side = f;
        else if (i == cols.price) price = f;
        else if (i == cols.size) size = f;
        else if (i == cols.orderId) orderId = f;
    }
    if (action.size() != 1) return false;
    r.action = action[0];
    r.side = (side == "B") ? Side::Buy : Side::Sell;   // 'A' (ask) or 'N'
    if (!detail::toInt(orderId, r.id) || !detail::toInt(size, r.size)) return false;
    if (r.action == 'A' || r.action == 'M') {
        if (side != "B" && side != "A") return false;
        if (!mboPriceToTicks(price, tickNanos, r.price)) return false;
    } else {
        r.price = 0;
    }
    return true;
}

// Apply one Databento record to any book (v1 or v2).
// Returns false if the record was ignored (T/F/N or unknown).
template <class Book>
inline bool applyMbo(Book& book, const MboRecord& r, std::vector<Trade>& out) {
    switch (r.action) {
        case 'A': book.add(r.id, r.side, r.price, r.size, out); return true;
        case 'C': return book.reduce(r.id, r.size);        // partial or full
        case 'M':
            if (book.contains(r.id))
                book.modify(r.id, r.price, r.size, out);
            else if (r.size > 0)  // unknown id (placed before our data starts)
                book.add(r.id, r.side, r.price, r.size, out);
            return true;
        case 'R': book.clear(); return true;
        default: return false;                             // T, F, N
    }
}
}  // namespace ob
