#include "trading_simulator.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

void printHelp(bool simulator = false) {
    cout << "\nCommands:\n";
    if (simulator) {
        cout << "CREATE_ACCOUNT trader_id initial_cash_paise initial_inventory\n"
             << "ACCOUNT trader_id\n"
             << "BUY trader_id order_id price quantity\n"
             << "SELL trader_id order_id price quantity\n"
             << "CANCEL trader_id order_id\n";
    } else {
        cout << "BUY id price quantity\nSELL id price quantity\nCANCEL id\n";
    }
    cout << "BOOK\nQUOTE\nHELP\nEXIT\n"
         << "Prices and cash are in paise: Rs 100 = 10000; inventory is in units.\n";
    if (simulator) {
        cout << "Negative balances are allowed; funds are not reserved for resting orders.\n"
             << "No buying-power checks, fees, or self-trade prevention.\n"
             << "Initial cash: +/-10^18; initial inventory: +/-10^9.\n"
             << "Maximum price: 10^9; cumulative submitted quantity: 10^9.\n";
    }
    cout << '\n';
}

// Strict signed decimal parsing, including LLONG_MIN without negating it.
ll parseSigned(const string& text) {
    const bool negative = !text.empty() && text.front() == '-';
    const size_t start = negative ? 1 : 0;
    if (text.size() == start) throw invalid_argument("Use signed whole numbers");
    const ull limit = static_cast<ull>(numeric_limits<ll>::max()) + (negative ? 1ULL : 0ULL);
    ull value = 0;
    for (size_t i = start; i < text.size(); ++i) {
        const char digit = text[i];
        if (digit < '0' || digit > '9') throw invalid_argument("Use signed whole numbers");
        const ull next = static_cast<ull>(digit - '0');
        if (value > (limit - next) / 10) throw invalid_argument("number is too large");
        value = value * 10 + next;
    }
    if (negative && value == limit) return numeric_limits<ll>::min();
    return negative ? -static_cast<ll>(value) : static_cast<ll>(value);
}

// Simulator IDs support the complete positive ull range.
ull parseId(const string& text) {
    if (text.empty()) throw invalid_argument("Missing ID");
    ull value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') throw invalid_argument("Use positive whole IDs");
        const ull next = static_cast<ull>(digit - '0');
        if (value > (numeric_limits<ull>::max() - next) / 10)
            throw invalid_argument("ID is too large");
        value = value * 10 + next;
    }
    if (value == 0) throw invalid_argument("IDs must be positive");
    return value;
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
    ll value;
    try { value = stoll(s); }
    catch (const out_of_range&) { throw invalid_argument("number is too large"); }
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
                 bool replay, size_t line_number, uint64_t timestamp = 0,
                 ostream* trades_csv = nullptr, TradingSimulator* simulator = nullptr) {
    const OrderBook& inspection = simulator ? simulator->orderBook() : book;
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
        if (simulator && command == "CREATE_ACCOUNT") {
            string trader, cash, inventory, extra;
            if (!(ss >> trader >> cash >> inventory) || (ss >> extra)) {
                reject("Usage: CREATE_ACCOUNT trader_id initial_cash_paise initial_inventory");
                return false;
            }
            const ull id = parseId(trader);
            simulator->createAccount(id, parseSigned(cash), parseSigned(inventory));
            cout << "CREATED_ACCOUNT trader=" << id << '\n';
        }
        else if (simulator && command == "ACCOUNT") {
            string trader, extra;
            if (!(ss >> trader) || (ss >> extra)) {
                reject("Usage: ACCOUNT trader_id");
                return false;
            }
            const ull id = parseId(trader);
            const Account& account = simulator->getAccount(id);
            cout << "ACCOUNT trader=" << id << " cash_paise=" << account.cash_paise
                 << " inventory=" << account.inventory << '\n';
        }
        else if (command == "BUY" || command == "SELL") {
            string traderText, idText, priceText, qtyText, extra;
            if ((simulator && !(ss >> traderText)) ||
                !(ss >> idText >> priceText >> qtyText) || (ss >> extra)) {
                reject("Usage: " + command + (simulator ? " trader_id order_id" : " id") + " price quantity");
                return false;
            }

            ull id = simulator ? parseId(idText) : static_cast<ull>(parsePositive(idText));
            ll price = parsePositive(priceText);
            ll qty = parsePositive(qtyText);
            Side side = (command == "BUY") ? Side::Buy : Side::Sell;
            vector<Trade> trades = simulator
                ? simulator->submitOrder(parseId(traderText), {id, side, price, qty})
                : book.addOrder({id, side, price, qty});
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
                if (trades_csv) {
                    *trades_csv << timestamp << ',' << trade.buy_id << ','
                                << trade.sell_id << ',' << trade.price << ','
                                << trade.quantity << '\n';
                    if (!*trades_csv) {
                        throw ios_base::failure("Error writing trades CSV");
                    }
                }
            }
            cout << "Filled: " << filled
                 << " | Resting: " << qty - filled << '\n';
        }
        else if (command == "CANCEL") {
            string traderText, idText, extra;
            if ((simulator && !(ss >> traderText)) || !(ss >> idText) || (ss >> extra)) {
                reject(simulator ? "Usage: CANCEL trader_id order_id" : "Usage: CANCEL id");
                return false;
            }
            ull id = simulator ? parseId(idText) : static_cast<ull>(parsePositive(idText));
            if (simulator ? simulator->cancelOrder(parseId(traderText), id) : book.cancelOrder(id)) {
                ++stats.successful_cancellations;
                cout << "CANCELLED order " << id << '\n';
            } else {
                reject("no active order with ID " + to_string(id) +
                       (simulator ? " owned by this trader" : ""), true);
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
                inspection.printBook();
            } else if (command == "QUOTE") {
                const auto bid = inspection.bestBid();
                const auto ask = inspection.bestAsk();
                const auto spread = inspection.spread();
                cout << "QUOTE (prices in paise)\n"
                     << "Best bid: " << (bid ? to_string(*bid) : "N/A") << '\n'
                     << "Best ask: " << (ask ? to_string(*ask) : "N/A") << '\n'
                     << "Spread: " << (spread ? to_string(*spread) : "N/A") << '\n';
            } else if (command == "HELP") {
                printHelp(simulator != nullptr);
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

int runCli(int argc, char* argv[]) {
    const bool simulator_mode = argc > 1 && string(argv[1]) == "--simulator";
    const int option = simulator_mode ? 2 : 1;
    const int remaining = argc - option;
    const bool replay = remaining == 2 && string(argv[option]) == "--replay" &&
                        argv[option + 1][0] != '\0';
    const bool timed = (remaining == 2 || remaining == 4) &&
        string(argv[option]) == "--replay-timed" && argv[option + 1][0] != '\0';
    const bool csv_requested = timed && remaining == 4 &&
        string(argv[option + 2]) == "--trades-csv" && argv[option + 3][0] != '\0';
    if (remaining != 0 && !replay && !(timed && (remaining == 2 || csv_requested))) {
        cerr << "Usage: " << argv[0]
             << " [--simulator] [--replay filename | --replay-timed filename"
             << " [--trades-csv filename]]\n"
             << "Use --simulator for trader accounts and ownership-aware orders.\n"
             << "Prices/cash are in paise; inventory/quantity are in units.\n"
             << "Simulator permits negative balances and does not reserve funds for resting orders.\n";
        return 1;
    }
    const char* input_path = (replay || timed) ? argv[option + 1] : nullptr;
    const char* csv_path = csv_requested ? argv[option + 3] : nullptr;
    TradingSimulator simulation;
    TradingSimulator* simulator = simulator_mode ? &simulation : nullptr;

    OrderBook book;
    ReplayStats stats;
    string line;
    if (replay || timed) {
        ifstream input(input_path);
        if (!input) {
            cerr << "Cannot open replay file: " << input_path << '\n';
            return 1;
        }
        ofstream csv;
        if (csv_requested) {
            // An existing output may be replaced, unless it is the input itself.
            error_code error;
            const bool output_exists = filesystem::exists(csv_path, error);
            if (error) {
                cerr << "Cannot inspect trades CSV path: " << error.message() << '\n';
                return 1;
            }
            if (output_exists) {
                const bool same_file = filesystem::equivalent(input_path, csv_path, error);
                if (error) {
                    cerr << "Cannot compare replay and trades CSV paths: "
                         << error.message() << '\n';
                    return 1;
                }
                if (same_file) {
                    cerr << "Trades CSV output refers to the replay input file\n";
                    return 1;
                }
            }
            csv.open(csv_path);
            if (!csv) {
                cerr << "Cannot open trades CSV file: " << csv_path << '\n';
                return 1;
            }
            csv << "timestamp_us,buy_id,sell_id,price_paise,quantity\n";
            if (!csv) {
                cerr << "Error writing trades CSV: " << csv_path << '\n';
                return 1;
            }
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
                bool exit_requested;
                try {
                    exit_requested = processLine(command + arguments, book, stats,
                                                 true, line_number, timestamp,
                                                 csv_requested ? &csv : nullptr, simulator);
                } catch (const ios_base::failure&) {
                    cerr << "Error writing trades CSV: " << csv_path << '\n';
                    return 1;
                }
                if (exit_requested) {
                    break;
                }
            } else if (processLine(line, book, stats, true, line_number, 0, nullptr, simulator)) {
                break;
            }
        }
        if (input.bad() || (!input.eof() && input.fail())) {
            cerr << "Error reading replay file: " << input_path << '\n';
            return 1;
        }
        if (csv_requested) {
            csv.flush();
            if (!csv) {
                cerr << "Error flushing trades CSV: " << csv_path << '\n';
                return 1;
            }
            csv.close();
            if (!csv) {
                cerr << "Error closing trades CSV: " << csv_path << '\n';
                return 1;
            }
        }
        printSummary(stats, timed, simulation_time);
        return stats.rejected_commands == 0 ? 0 : 1;
    }

    printHelp(simulator_mode);
    while (true) {
        cout << "> " << flush;
        if (!getline(cin, line)) {
            break;
        }
        if (processLine(line, book, stats, false, 0, 0, nullptr, simulator)) {
            break;
        }
    }
    return 0;
}

int main(int argc, char* argv[]) {
    try {
        return runCli(argc, argv);
    } catch (const exception& error) {
        // Allocation failures may follow partial matching. Terminate without a
        // normal completion summary instead of reusing possibly inconsistent state.
        cerr << "Fatal error: " << error.what() << '\n';
        return 1;
    }
}
