#include "order_book.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

struct ReplayStats {
    uint64_t accepted_orders = 0;
    uint64_t rejected_commands = 0;
    uint64_t trades = 0;
    uint64_t traded_quantity = 0;
    uint64_t successful_cancellations = 0;
};

void printHelp() {
    cout << "\nCommands:\n"
         << "BUY id price quantity\n"
         << "SELL id price quantity\n"
         << "CANCEL id\n"
         << "BOOK\n"
         << "QUOTE\n"
         << "HELP\n"
         << "EXIT\n"
         << "Prices are in paise: Rs 100 = 10000\n\n";
}

// Accept only positive integers that fit in long long.
ll parsePositive(const string& s) {
    if (s.empty()) {
        throw invalid_argument("Missing number");
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            throw invalid_argument("Use positive whole numbers");
        }
    }
    ll value = stoll(s);
    if (value <= 0) {
        throw invalid_argument("Numbers must be positive");
    }
    return value;
}

// Replay time permits zero and the full uint64_t range, unlike order fields.
uint64_t parseTimestamp(const string& text) {
    if (text.empty()) {
        throw invalid_argument("missing timestamp");
    }
    uint64_t value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') {
            throw invalid_argument("invalid timestamp");
        }
        const uint64_t next = static_cast<uint64_t>(digit - '0');
        if (value > (numeric_limits<uint64_t>::max() - next) / 10) {
            throw invalid_argument("timestamp is too large");
        }
        value = value * 10 + next;
    }
    return value;
}

// Returns true only for a valid EXIT command.
bool processLine(const string& line, OrderBook& book, ReplayStats& stats,
                 bool replay, size_t line_number) {
    stringstream ss(line);
    string command;
    ss >> command;
    if (command.empty()) {
        return false;
    }

    for (char& c : command) {
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    }

    auto reject = [&](const string& reason, bool rejected_prefix = false) {
        ++stats.rejected_commands;
        if (replay) {
            cout << "Line " << line_number << ": " << reason << '\n';
        } else {
            cout << (rejected_prefix ? "REJECTED: " : "") << reason << '\n';
        }
    };

    try {
        if (command == "BUY" || command == "SELL") {
            string idText, priceText, qtyText, extra;
            if (!(ss >> idText >> priceText >> qtyText) || (ss >> extra)) {
                reject("Usage: " + command + " id price quantity");
                return false;
            }

            ull id = static_cast<ull>(parsePositive(idText));
            ll price = parsePositive(priceText);
            ll qty = parsePositive(qtyText);
            Side side = (command == "BUY") ? Side::Buy : Side::Sell;
            vector<Trade> trades = book.addOrder({id, side, price, qty});
            ++stats.accepted_orders;

            ll filled = 0;
            cout << "ACCEPTED order " << id << '\n';
            for (const Trade& trade : trades) {
                cout << "TRADE buy=" << trade.buy_id
                     << " sell=" << trade.sell_id
                     << " price=" << trade.price
                     << " qty=" << trade.quantity << '\n';
                filled += trade.quantity;
                ++stats.trades;
                stats.traded_quantity += static_cast<uint64_t>(trade.quantity);
            }
            cout << "Filled: " << filled
                 << " | Resting: " << qty - filled << '\n';
        }
        else if (command == "CANCEL") {
            string idText, extra;
            if (!(ss >> idText) || (ss >> extra)) {
                reject("Usage: CANCEL id");
                return false;
            }
            ull id = static_cast<ull>(parsePositive(idText));
            if (book.cancelOrder(id)) {
                ++stats.successful_cancellations;
                cout << "CANCELLED order " << id << '\n';
            } else {
                reject("no active order with ID " + to_string(id), true);
            }
        }
        else if (command == "BOOK" || command == "QUOTE" ||
                 command == "HELP" || command == "EXIT") {
            string extra;
            if (ss >> extra) {
                reject(command + " takes no arguments");
                return false;
            }
            if (command == "BOOK") {
                book.printBook();
            } else if (command == "QUOTE") {
                const auto bid = book.bestBid();
                const auto ask = book.bestAsk();
                const auto spread = book.spread();
                cout << "QUOTE (prices in paise)\n"
                     << "Best bid: " << (bid ? to_string(*bid) : "N/A") << '\n'
                     << "Best ask: " << (ask ? to_string(*ask) : "N/A") << '\n'
                     << "Spread: " << (spread ? to_string(*spread) : "N/A") << '\n';
            } else if (command == "HELP") {
                printHelp();
            } else {
                return true;
            }
        }
        else {
            reject("Unknown command. Type HELP.");
        }
    }
    catch (const invalid_argument& error) {
        reject(error.what(), true);
    }
    catch (const out_of_range&) {
        reject("number is too large", true);
    }
    return false;
}

void printSummary(const ReplayStats& stats, bool timed = false,
                  optional<uint64_t> final_time = nullopt) {
    cout << "Replay summary:\n"
         << "Accepted orders: " << stats.accepted_orders << '\n'
         << "Rejected commands: " << stats.rejected_commands << '\n'
         << "Trades: " << stats.trades << '\n'
         << "Traded quantity: " << stats.traded_quantity << '\n'
         << "Successful cancellations: " << stats.successful_cancellations << '\n';
    if (timed) {
        cout << "Final timestamp (us): "
             << (final_time ? to_string(*final_time) : "N/A") << '\n';
    }
}

int main(int argc, char* argv[]) {
    const bool replay = argc == 3 && string(argv[1]) == "--replay" && argv[2][0] != '\0';
    const bool timed = argc == 3 && string(argv[1]) == "--replay-timed" && argv[2][0] != '\0';
    if (argc != 1 && !replay && !timed) {
        cerr << "Usage: " << argv[0] << " [--replay filename | --replay-timed filename]\n";
        return 1;
    }

    OrderBook book;
    ReplayStats stats;
    string line;
    if (replay || timed) {
        ifstream input(argv[2]);
        if (!input) {
            cerr << "Cannot open replay file: " << argv[2] << '\n';
            return 1;
        }
        size_t line_number = 0;
        optional<uint64_t> simulation_time;
        while (getline(input, line)) {
            ++line_number;
            if (timed) {
                if (line.find_first_not_of(" \t\r\n\v\f") == string::npos) {
                    continue;
                }
                stringstream fields(line);
                string timestamp_text, command;
                fields >> timestamp_text;
                auto reject_time = [&](const string& reason) {
                    ++stats.rejected_commands;
                    cout << "Line " << line_number << ": " << reason << '\n';
                };
                uint64_t timestamp;
                try {
                    timestamp = parseTimestamp(timestamp_text);
                } catch (const invalid_argument& error) {
                    reject_time(error.what());
                    continue;
                }
                if (!(fields >> command)) {
                    reject_time("missing command");
                    continue;
                }
                if (simulation_time && timestamp < *simulation_time) {
                    reject_time("timestamp goes backward");
                    continue;
                }
                string arguments;
                getline(fields, arguments);
                simulation_time = timestamp;
                cout << "Time (us): " << timestamp << '\n';
                if (processLine(command + arguments, book, stats, true, line_number)) {
                    break;
                }
            } else if (processLine(line, book, stats, true, line_number)) {
                break;
            }
        }
        if (input.bad() || (!input.eof() && input.fail())) {
            cerr << "Error reading replay file: " << argv[2] << '\n';
            return 1;
        }
        printSummary(stats, timed, simulation_time);
        return stats.rejected_commands == 0 ? 0 : 1;
    }

    printHelp();
    while (true) {
        cout << "> " << flush;
        if (!getline(cin, line)) {
            break;
        }
        if (processLine(line, book, stats, false, 0)) {
            break;
        }
    }
    return 0;
}
