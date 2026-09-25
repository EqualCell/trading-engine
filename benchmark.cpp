#include "order_book.h"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

enum class OperationType { Add, Cancel };

struct BenchmarkOperation {
    OperationType type;
    Order order;
    ull cancel_id;
};

struct BenchmarkResult {
    const char* name;
    std::size_t operations;
    std::chrono::nanoseconds elapsed;
    std::uint64_t trades;
    std::uint64_t successful_cancellations;
};

BenchmarkOperation add(ull id, Side side, ll price, ll quantity) {
    return {OperationType::Add, {id, side, price, quantity}, 0};
}

BenchmarkOperation cancel(ull id) {
    return {OperationType::Cancel, {}, id};
}

std::size_t parseCount(int argc, char* argv[]) {
    if (argc > 2) {
        throw std::invalid_argument("usage: ./benchmark [positive operation count]");
    }
    if (argc == 1) {
        return 200000;
    }

    const std::string text = argv[1];
    std::size_t count = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), count);
    if (text.empty() || result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || count == 0) {
        throw std::invalid_argument("operation count must be a positive whole number");
    }
    // Scenario B creates fewer than 3 * count distinct IDs. Scenario C
    // creates at most count IDs. This also keeps reserve arithmetic safe.
    if (count > std::numeric_limits<ull>::max() / 4 ||
        count > std::numeric_limits<std::size_t>::max() / 4) {
        throw std::invalid_argument("operation count is too large");
    }
    return count;
}

std::vector<BenchmarkOperation> makeRestingOperations(std::size_t count) {
    std::vector<BenchmarkOperation> operations;
    operations.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const bool buy = i % 2 == 0;
        const ll level = static_cast<ll>((i / 2) % 32);
        operations.push_back(add(static_cast<ull>(i) + 1,
                                 buy ? Side::Buy : Side::Sell,
                                 buy ? 10000 - level : 10100 + level,
                                 1 + static_cast<ll>(i % 7)));
    }
    return operations;
}

struct MatchingWorkload {
    std::vector<Order> initial_orders;
    std::vector<BenchmarkOperation> measured_operations;
};

MatchingWorkload makeMatchingWorkload(std::size_t count) {
    MatchingWorkload workload;
    // Each resting order has two units; each aggressive buy takes three.
    // The extra resting order for odd counts leaves a single unit behind.
    const std::size_t resting_count = count + count / 2 + count % 2;
    workload.initial_orders.reserve(resting_count);
    workload.measured_operations.reserve(count);
    for (std::size_t i = 0; i < resting_count; ++i) {
        workload.initial_orders.push_back({static_cast<ull>(i) + 1,
                                           Side::Sell,
                                           10100 + static_cast<ll>(i % 32), 2});
    }
    for (std::size_t i = 0; i < count; ++i) {
        workload.measured_operations.push_back(
            add(static_cast<ull>(resting_count) + static_cast<ull>(i) + 1,
                Side::Buy, 10131, 3));
    }
    return workload;
}

std::vector<BenchmarkOperation> makeMixedOperations(std::size_t count) {
    std::vector<BenchmarkOperation> operations;
    operations.reserve(count);
    ull next_id = 1;
    while (operations.size() < count) {
        // Each full cycle ends with an empty book. A truncated final cycle
        // contains only a prefix, so its cancellations still target live IDs.
        const ll level = static_cast<ll>((operations.size() / 9) % 16);
        const ll bid = 10000 + level;
        const ll ask = 10100 + level;
        const ull first_bid = next_id++;
        operations.push_back(add(first_bid, Side::Buy, bid, 3));
        if (operations.size() == count) break;
        const ull second_bid = next_id++;
        operations.push_back(add(second_bid, Side::Buy, bid, 2));
        if (operations.size() == count) break;
        operations.push_back(add(next_id++, Side::Sell, ask, 2));
        if (operations.size() == count) break;
        operations.push_back(add(next_id++, Side::Sell, ask, 1));
        if (operations.size() == count) break;
        operations.push_back(add(next_id++, Side::Sell, bid, 1));
        if (operations.size() == count) break;
        const ull residual_buy = next_id++;
        operations.push_back(add(residual_buy, Side::Buy, ask, 4));
        if (operations.size() == count) break;
        operations.push_back(cancel(first_bid));
        if (operations.size() == count) break;
        operations.push_back(cancel(residual_buy));
        if (operations.size() == count) break;
        operations.push_back(add(next_id++, Side::Sell, bid, 2));
    }
    return operations;
}

BenchmarkResult measure(const char* name, OrderBook& book,
                        const std::vector<BenchmarkOperation>& operations) {
    std::uint64_t trades = 0;
    std::uint64_t cancellations = 0;
    const auto start = std::chrono::steady_clock::now();
    for (const BenchmarkOperation& operation : operations) {
        if (operation.type == OperationType::Add) {
            trades += book.addOrder(operation.order).size();
        } else {
            if (!book.cancelOrder(operation.cancel_id)) {
                throw std::runtime_error("expected cancellation failed");
            }
            ++cancellations;
        }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start);
    return {name, operations.size(), elapsed, trades, cancellations};
}

void printResult(const BenchmarkResult& result) {
    const long double nanoseconds = static_cast<long double>(result.elapsed.count());
    const long double safe_nanoseconds = nanoseconds > 0 ? nanoseconds : 1;
    const long double count = static_cast<long double>(result.operations);
    const long double operations_per_second = count * 1000000000.0L / safe_nanoseconds;
    std::cout << result.name << '\n'
              << "  Measured operations: " << result.operations << '\n'
              << std::fixed << std::setprecision(3)
              << "  Total elapsed: " << nanoseconds / 1000000.0L << " ms\n"
              << "  Throughput: " << operations_per_second << " operations/second\n"
              << "  Throughput: " << operations_per_second / 1000000.0L
              << " million operations/second\n"
              << "  Average: " << nanoseconds / count << " ns/operation\n"
              << "  Trades generated: " << result.trades << '\n'
              << "  Successful cancellations: " << result.successful_cancellations << '\n';
}

void warmUp() {
    OrderBook book;
    const auto operations = makeMixedOperations(9000);
    for (const BenchmarkOperation& operation : operations) {
        if (operation.type == OperationType::Add) {
            book.addOrder(operation.order);
        } else if (!book.cancelOrder(operation.cancel_id)) {
            throw std::runtime_error("warm-up cancellation failed");
        }
    }
}

int main(int argc, char* argv[]) {
    try {
        const std::size_t count = parseCount(argc, argv);
        warmUp();

        {
            const auto operations = makeRestingOperations(count);
            OrderBook book;
            const auto result = measure("Resting-order insertion", book, operations);
            if (result.trades != 0 || result.successful_cancellations != 0) {
                throw std::runtime_error("resting workload unexpectedly matched");
            }
            printResult(result);
        }
        {
            const auto workload = makeMatchingWorkload(count);
            OrderBook book;
            for (const Order& order : workload.initial_orders) {
                if (!book.addOrder(order).empty()) {
                    throw std::runtime_error("initial matching liquidity crossed");
                }
            }
            const auto result = measure("Aggressive matching", book,
                                        workload.measured_operations);
            if (result.trades != 2 * static_cast<std::uint64_t>(count) ||
                result.successful_cancellations != 0) {
                throw std::runtime_error("aggressive workload produced unexpected trades");
            }
            printResult(result);
        }
        {
            const auto operations = makeMixedOperations(count);
            OrderBook book;
            printResult(measure("Mixed workload", book, operations));
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Benchmark error: " << error.what() << '\n';
        return 1;
    }
}
