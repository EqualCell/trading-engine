#pragma once

#include "json_utils.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>
#include <utility>

namespace historical {
using Integer = long long;
using Timestamp = unsigned long long;
constexpr Integer SCALE = 100000000;
struct Level { std::string price, amount; Integer price_units, amount_units; };
struct Book {
    std::vector<Level> bids, asks;
    std::optional<std::string> best_bid, best_ask, spread, midpoint;
};
struct Trade { std::string id, side, price, amount; };
struct Event {
    Timestamp exchange_time, local_time;
    std::size_t source_row;
    std::optional<Book> snapshot;
    std::optional<Trade> trade;
};
struct Provenance {
    std::vector<std::string> fields;
    Timestamp start, end;
    std::size_t snapshot_rows, trade_rows;
};
struct Session {
    Provenance provenance;
    std::vector<Event> snapshots, trades;
    std::vector<std::string> anomalies;
};

inline void require(bool condition, const std::string& message) {
    if (!condition) throw std::invalid_argument(message);
}
inline Timestamp unsignedInteger(const std::string& text) {
    require(!text.empty(), "Missing unsigned integer");
    Timestamp value = 0;
    for (char c : text) {
        require(c >= '0' && c <= '9', "Invalid unsigned integer: " + text);
        const auto digit = static_cast<Timestamp>(c - '0');
        require(value <= (std::numeric_limits<Timestamp>::max() - digit) / 10, "Timestamp/count overflow");
        value = value * 10 + digit;
    }
    return value;
}
// Fixed 10^-8 units, preserving the original decimal string for export. No double.
inline Integer decimal(const std::string& text, bool price = false) {
    require(!text.empty(), "Missing decimal");
    const auto dot = text.find('.');
    const std::size_t whole = dot == std::string::npos ? text.size() : dot;
    require(whole > 0 && (whole == 1 || text[0] != '0'), "Noncanonical decimal: " + text);
    const std::size_t fraction = dot == std::string::npos ? 0 : text.size() - dot - 1;
    require(dot == std::string::npos || (fraction > 0 && fraction <= 8), "Decimal precision exceeds 8 places or empty fraction: " + text);
    Integer value = 0;
    const Integer maximum = price ? std::numeric_limits<Integer>::max() / 2 : std::numeric_limits<Integer>::max();
    auto digit = [&](char c) {
        require(c >= '0' && c <= '9', "Invalid decimal: " + text);
        const Integer next = c - '0';
        require(value <= (maximum - next) / 10, "Decimal overflow: " + text);
        value = value * 10 + next;
    };
    for (std::size_t i = 0; i < whole; ++i) digit(text[i]);
    for (std::size_t i = 0; i < fraction; ++i) digit(text[whole + 1 + i]);
    for (std::size_t i = fraction; i < 8; ++i) digit('0');
    require(value > 0, "Prices and amounts must be positive: " + text);
    return value;
}
inline std::string formatted(Integer value, bool half = false) {
    // half=true expresses value/2 at 10^-8 scale, retaining a possible ninth digit.
    const bool fractional_half = half && value % 2 != 0;
    if (half) value /= 2;
    std::string result = std::to_string(value / SCALE);
    std::string fraction = std::to_string(value % SCALE);
    fraction.insert(0, 8 - fraction.size(), '0');
    if (fractional_half) fraction += '5';
    while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
    if (!fraction.empty()) result += '.' + fraction;
    return result;
}

// Strict single-record RFC-style quoting. Tardis rows are line records; embedded
// newlines are unsupported and fail rather than silently splitting a record.
inline std::vector<std::string> csv(const std::string& line) {
    std::vector<std::string> fields;
    std::size_t i = 0;
    while (true) {
        std::string value;
        if (i < line.size() && line[i] == '"') {
            ++i; bool closed = false;
            while (i < line.size()) {
                char c = line[i++];
                if (c == '"') {
                    if (i < line.size() && line[i] == '"') { value += '"'; ++i; }
                    else { closed = true; break; }
                } else value += c;
            }
            require(closed && (i == line.size() || line[i] == ','), "Malformed quoted CSV field");
        } else {
            while (i < line.size() && line[i] != ',') {
                require(line[i] != '"', "Unexpected quote in CSV field");
                value += line[i++];
            }
        }
        fields.push_back(value);
        if (i == line.size()) break;
        require(line[i++] == ',', "Malformed CSV separator");
        if (i == line.size()) { fields.emplace_back(); break; }
    }
    return fields;
}
class CsvReader {
public:
    CsvReader(const std::string& path, const std::vector<std::string>& header) : path(path) {
        require(std::filesystem::is_regular_file(path), "CSV input must be a regular file: " + path);
        require(std::filesystem::file_size(path) <= 64 * 1024 * 1024, "CSV input exceeds 64 MiB: " + path);
        file.open(path, std::ios::binary);
        require(static_cast<bool>(file), "Cannot open input: " + path);
        const auto found = next();
        require(found && *found == header, "Unsupported CSV header: " + path);
    }
    std::optional<std::vector<std::string>> next() {
        std::string line;
        if (!std::getline(file, line)) {
            if (!file.eof() || file.bad()) throw std::runtime_error("Error reading input: " + path);
            return std::nullopt;
        }
        ++line_number;
        require(line_number <= 100002, "Input exceeds 100000 data rows: " + path);
        require(line.size() <= 16384, "CSV record exceeds 16 KiB: " + path);
        // Prepared files use LF. CRLF input is accepted, but embedded CR rejected.
        if (!line.empty() && line.back() == '\r') line.pop_back();
        require(line.find('\r') == std::string::npos, "Embedded CR in CSV record: " + path);
        try { return csv(line); }
        catch (const std::exception& error) {
            throw std::invalid_argument(path + ": line " + std::to_string(line_number) + ": " + error.what());
        }
    }
    std::size_t lineNumber() const { return line_number; }
private:
    std::string path;
    std::ifstream file;
    std::size_t line_number = 0;
};
inline std::vector<std::string> snapshotHeader() {
    std::vector<std::string> header = {"exchange", "symbol", "timestamp", "local_timestamp"};
    for (int i = 0; i < 5; ++i) {
        for (const auto& side : {"asks", "bids"}) {
            header.push_back(std::string(side) + '[' + std::to_string(i) + "].price");
            header.push_back(std::string(side) + '[' + std::to_string(i) + "].amount");
        }
    }
    return header;
}
inline Provenance readProvenance(const std::string& path) {
    CsvReader input(path, {"provider", "exchange", "symbol", "market", "date", "window_start_us", "window_end_us",
                         "retrieved_at_utc", "snapshot_url", "trade_url", "snapshot_sha256", "trade_sha256", "snapshot_rows", "trade_rows"});
    auto row = input.next();
    require(row && row->size() == 14 && !input.next(), "Expected one provenance record with 14 fields");
    const auto& f = *row;
    require(f[0] == "Tardis" && f[1] == "binance" && f[2] == "BTCUSDT" && f[3] == "spot", "Only Tardis Binance spot BTCUSDT is supported");
    require(f[4].size() == 10 && f[4][4] == '-' && f[4][7] == '-' && f[4].substr(8) == "01", "Expected first-of-month date YYYY-MM-01");
    const auto year = unsignedInteger(f[4].substr(0, 4)), month = unsignedInteger(f[4].substr(5, 2));
    require(year >= 2019 && year <= 2100 && month >= 1 && month <= 12, "Unsupported date");
    auto leap = [](Timestamp y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); };
    Timestamp days = 0;
    for (Timestamp y = 1970; y < year; ++y) days += leap(y) ? 366 : 365;
    constexpr std::array<int, 12> month_days = {31,28,31,30,31,30,31,31,30,31,30,31};
    for (Timestamp m = 1; m < month; ++m) days += month_days[m - 1] + (m == 2 && leap(year) ? 1 : 0);
    const Timestamp start = unsignedInteger(f[5]), end = unsignedInteger(f[6]);
    require(start == days * 86400ULL * 1000000ULL && end == start + 300000000ULL, "Provenance window must be first five capture-time minutes UTC");
    require(!f[7].empty(), "Missing retrieval timestamp");
    std::string date_path = f[4];
    for (char& c : date_path) if (c == '-') c = '/';
    for (int i = 0; i < 2; ++i) {
        const std::string kind = i == 0 ? "book_snapshot_5" : "trades";
        require(f[8 + i] == "https://datasets.tardis.dev/v1/binance/" + kind + '/' + date_path + "/BTCUSDT.csv.gz", "Unexpected provenance source URL");
        require(f[10 + i].size() == 64, "Expected SHA-256 checksum");
        for (char c : f[10 + i]) require((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'), "Invalid SHA-256 checksum");
    }
    const auto snapshots = unsignedInteger(f[12]), trades = unsignedInteger(f[13]);
    require(snapshots <= 100000 && trades <= 100000, "Provenance row count exceeds limit");
    return {*row, start, end, static_cast<std::size_t>(snapshots), static_cast<std::size_t>(trades)};
}
inline std::vector<Level> levels(const std::vector<std::string>& row, std::size_t offset, bool bids) {
    std::vector<Level> result;
    bool missing = false;
    for (std::size_t i = 0; i < 5; ++i) {
        const auto& price = row[offset + i * 4];
        const auto& amount = row[offset + i * 4 + 1];
        if (price.empty() || amount.empty()) {
            require(price.empty() && amount.empty(), "Unpaired missing price/amount level");
            missing = true;
        } else {
            require(!missing, "Non-trailing missing depth level");
            Level level{price, amount, decimal(price, true), decimal(amount)};
            if (!result.empty()) require(bids ? level.price_units < result.back().price_units : level.price_units > result.back().price_units,
                                         "Depth levels are unsorted or duplicated");
            result.push_back(level);
        }
    }
    return result;
}
inline Session readSession(const std::string& snapshots_path, const std::string& trades_path, const std::string& provenance_path) {
    Session session{readProvenance(provenance_path), {}, {}, {}};
    for (bool snapshots : {true, false}) {
        const std::string& path = snapshots ? snapshots_path : trades_path;
        CsvReader input(path, snapshots ? snapshotHeader() : std::vector<std::string>{"exchange", "symbol", "timestamp", "local_timestamp", "id", "side", "price", "amount"});
        std::optional<Timestamp> previous_local, previous_exchange;
        std::size_t regressions = 0, incomplete = 0, exchange_outside_window = 0;
        while (auto row = input.next()) {
            try {
                require(row->size() == (snapshots ? 24 : 8), "Unexpected CSV field count");
                require((*row)[0] == "binance" && (*row)[1] == "BTCUSDT", "Unsupported instrument");
                Event event{unsignedInteger((*row)[2]), unsignedInteger((*row)[3]), input.lineNumber() - 1, {}, {}};
                require(event.local_time >= session.provenance.start && event.local_time < session.provenance.end, "Capture timestamp outside retained window");
                require(!previous_local || event.local_time >= *previous_local, "Capture timestamps moved backward; refusing to sort or discard rows");
                if (previous_exchange && event.exchange_time < *previous_exchange) ++regressions;
                previous_local = event.local_time; previous_exchange = event.exchange_time;
                if (event.exchange_time < session.provenance.start || event.exchange_time >= session.provenance.end) ++exchange_outside_window;
                if (snapshots) {
                    Book book{levels(*row, 6, true), levels(*row, 4, false), {}, {}, {}, {}};
                    if (book.bids.size() != 5 || book.asks.size() != 5) ++incomplete;
                    if (!book.bids.empty()) book.best_bid = book.bids[0].price;
                    if (!book.asks.empty()) book.best_ask = book.asks[0].price;
                    if (book.best_bid && book.best_ask) {
                        const auto bid = book.bids[0].price_units, ask = book.asks[0].price_units;
                        require(bid < ask, "Crossed or locked snapshot depth");
                        book.spread = formatted(ask - bid);
                        book.midpoint = formatted(ask + bid, true); // each price <= LLONG_MAX/2
                    }
                    event.snapshot = std::move(book); session.snapshots.push_back(std::move(event));
                } else {
                    require((*row)[5] == "buy" || (*row)[5] == "sell" || (*row)[5] == "unknown", "Unknown recorded trade side");
                    decimal((*row)[6], true); decimal((*row)[7]);
                    event.trade = Trade{(*row)[4], (*row)[5], (*row)[6], (*row)[7]};
                    session.trades.push_back(std::move(event));
                }
            } catch (const std::exception& error) {
                throw std::invalid_argument(path + ": line " + std::to_string(input.lineNumber()) + ": " + error.what());
            }
        }
        if (regressions) session.anomalies.push_back((snapshots ? "snapshots" : "trades") + std::string(": ") + std::to_string(regressions) + " exchange-timestamp regressions; retained original capture ordering");
        if (exchange_outside_window) session.anomalies.push_back((snapshots ? "snapshots" : "trades") + std::string(": ") + std::to_string(exchange_outside_window) + " exchange timestamps outside the capture-time window; retained with original timestamps");
        if (incomplete) session.anomalies.push_back("snapshots: " + std::to_string(incomplete) + " rows with trailing missing levels; retained with nullable metrics");
    }
    require(session.snapshots.size() == session.provenance.snapshot_rows && session.trades.size() == session.provenance.trade_rows,
            "Input row counts disagree with provenance");
    require(!session.snapshots.empty() || !session.trades.empty(), "No historical events");
    return session;
}
inline void nullable(std::ostream& out, const std::optional<std::string>& value) {
    out << (value ? jsonString(*value) : "null");
}
inline void writeLevels(std::ostream& out, const std::vector<Level>& levels) {
    out << '[';
    for (std::size_t i = 0; i < levels.size(); ++i) {
        if (i) out << ',';
        out << "{\"price\":" << jsonString(levels[i].price) << ",\"amount\":" << jsonString(levels[i].amount) << '}';
    }
    out << ']';
}
inline void writeSession(std::ostream& out, const Session& session) {
    const auto& p = session.provenance.fields;
    out << "{\n\"schema\":\"historical-market\",\"schema_version\":1,\n"
        << "\"instrument\":{\"exchange\":\"binance\",\"symbol\":\"BTCUSDT\",\"market\":\"spot\",\"base\":\"BTC\",\"quote\":\"USDT\"},\n"
        << "\"units\":{\"price\":\"USDT per BTC\",\"quantity\":\"BTC\",\"time\":\"UTC epoch microseconds\"},\n"
        << "\"provenance\":{\"provider\":\"Tardis\",\"date\":" << jsonString(p[4])
        << ",\"window_start_us\":" << jsonString(p[5]) << ",\"window_end_us\":" << jsonString(p[6])
        << ",\"retrieved_at_utc\":" << jsonString(p[7]) << ",\"snapshot_url\":" << jsonString(p[8])
        << ",\"trade_url\":" << jsonString(p[9]) << ",\"snapshot_sha256\":" << jsonString(p[10])
        << ",\"trade_sha256\":" << jsonString(p[11]) << ",\"snapshot_rows\":" << jsonString(p[12])
        << ",\"trade_rows\":" << jsonString(p[13]) << "},\n"
        << "\"ordering\":\"Capture/local_timestamp ascending; row order within each stream; snapshots before trades for cross-file ties. This does not recover exchange-wide ordering.\",\n"
        << "\"description\":\"Recorded historical market data. Top-five displayed depth. No strategy executions simulated.\",\n"
        << "\"anomalies\":[";
    for (std::size_t i = 0; i < session.anomalies.size(); ++i) { if (i) out << ','; out << jsonString(session.anomalies[i]); }
    out << "],\n\"events\":[\n";
    std::size_t bid = 0, trade = 0, index = 0;
    while (bid < session.snapshots.size() || trade < session.trades.size()) {
        // A stable two-way merge: never sort away a per-stream regression.
        const bool snapshot = trade == session.trades.size() ||
            (bid < session.snapshots.size() && session.snapshots[bid].local_time <= session.trades[trade].local_time);
        const auto& event = snapshot ? session.snapshots[bid++] : session.trades[trade++];
        if (index) out << ",\n";
        out << "{\"event_index\":\"" << index++ << "\",\"timestamp_us\":\"" << event.local_time
            << "\",\"exchange_timestamp_us\":\"" << event.exchange_time << "\",\"source_row\":\"" << event.source_row
            << "\",\"type\":" << jsonString(snapshot ? "snapshot" : "trade");
        if (snapshot) {
            const auto& book = *event.snapshot;
            out << ",\"snapshot\":{\"bids\":"; writeLevels(out, book.bids);
            out << ",\"asks\":"; writeLevels(out, book.asks);
            out << ",\"best_bid\":"; nullable(out, book.best_bid);
            out << ",\"best_ask\":"; nullable(out, book.best_ask);
            out << ",\"spread\":"; nullable(out, book.spread);
            out << ",\"midpoint\":"; nullable(out, book.midpoint);
            out << '}';
        } else {
            const auto& fill = *event.trade;
            out << ",\"trade\":{\"id\":" << jsonString(fill.id) << ",\"side\":" << jsonString(fill.side)
                << ",\"price\":" << jsonString(fill.price) << ",\"amount\":" << jsonString(fill.amount) << '}';
        }
        out << '}';
    }
    out << "\n]}\n";
}
} // namespace historical
