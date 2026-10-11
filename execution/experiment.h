#pragma once
#include "../order_book.h"
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <set>
#include <utility>

namespace execution {
// Fictional WHOLE instrument: whole quantities, paise prices/cash, microsecond time.
using Time = ll;
constexpr ll MAX_PRICE = 1000000000, MAX_QUANTITY = 1000000000;
inline void require(bool ok, const std::string& message) { if (!ok) throw std::invalid_argument(message); }
inline ll add(ll a, ll b) {
    if ((b > 0 && a > std::numeric_limits<ll>::max() - b) ||
        (b < 0 && a < std::numeric_limits<ll>::min() - b)) throw std::overflow_error("Integer addition overflow");
    return a + b;
}
inline ll product(ll a, ll b) {
    require(a >= 0 && b >= 0, "Expected nonnegative product");
    if (b && a > std::numeric_limits<ll>::max() / b) throw std::overflow_error("Integer product overflow");
    return a * b;
}
inline Time delayed(Time time, Time delay) { require(time >= 0 && delay >= 0, "Negative simulated time/delay"); return add(time, delay); }
inline const char* sideText(Side side) { return side == Side::Buy ? "buy" : "sell"; }

class Scheduler {
    struct Event {
        Time time; ull sequence; std::function<void(Time, ull)> action;
    };
    struct Later {
        bool operator()(const Event& a, const Event& b) const {
            return a.time > b.time || (a.time == b.time && a.sequence > b.sequence);
        }
    };
    std::priority_queue<Event, std::vector<Event>, Later> events;
    Time current = 0;
    ull next_sequence = 1;
    bool sequence_exhausted = false;
public:
    explicit Scheduler(ull first_sequence = 1) : next_sequence(first_sequence) { require(first_sequence > 0, "Sequence must be positive"); }
    Time now() const { return current; }
    ull schedule(Time time, std::function<void(Time, ull)> action) {
        require(time >= current && time >= 0, "Cannot schedule into the past");
        if (sequence_exhausted) throw std::overflow_error("Scheduler sequence exhausted");
        const ull sequence = next_sequence;
        if (next_sequence == std::numeric_limits<ull>::max()) sequence_exhausted = true;
        else ++next_sequence;
        events.push({time, sequence, std::move(action)}); return sequence;
    }
    bool step() {
        if (events.empty()) return false;
        Event event = events.top(); events.pop(); current = event.time;
        event.action(event.time, event.sequence); return true;
    }
    void drain() { while (step()) {} }
};
class Ids {
    ull next_id = 1;
    bool exhausted = false;
public:
    ull allocate() {
        if (exhausted) throw std::overflow_error("Order IDs exhausted");
        const ull id = next_id;
        if (next_id == std::numeric_limits<ull>::max()) exhausted = true; else ++next_id;
        return id;
    }
};
struct Task { Side side; ll target; Time start, deadline; ll limit_price; };
struct Account { ll cash, inventory; };
enum class ReportType { Accepted, Rejected, Fill, Cancelled, CancelRejected };
inline const char* reportText(ReportType type) {
    switch (type) {
        case ReportType::Accepted: return "accepted";
        case ReportType::Rejected: return "rejected";
        case ReportType::Fill: return "fill";
        case ReportType::Cancelled: return "cancelled";
        case ReportType::CancelRejected: return "cancel_rejected";
    }
    throw std::logic_error("Unknown report type");
}
struct Report {
    ReportType type;
    ull order_id, execution_id = 0;
    Side side;
    ll price = 0, quantity = 0;
    Time exchange_time = 0, delivery_time = 0;
    bool strategy = false;
    std::string reason;
};
// Exchange truth: not used to update the locally observed account.
struct Execution { ull id, order_id; Side side; ll price, quantity; Time time; };
class Exchange {
    OrderBook book;
    std::unordered_map<ull, bool> strategy_orders;
    std::unordered_map<ull, Order> requests;
    ull next_execution = 1;
    bool execution_exhausted = false;
    std::function<void(Report)> emit;
    void send(ReportType type, const Order& order, Time time, ll quantity, const std::string& reason = "", ull execution_id = 0, ll price = 0) {
        emit({type, order.id, execution_id, order.side, price ? price : order.price, quantity, time, 0, strategy_orders.at(order.id), reason});
    }
public:
    explicit Exchange(std::function<void(Report)> deliver) : emit(std::move(deliver)) {}
    std::vector<Execution> truth;
    void submit(Order order, bool strategy, Time time) {
        require(time >= 0, "Negative exchange time");
        if (requests.count(order.id)) {
            emit({ReportType::Rejected, order.id, 0, order.side, order.price, 0, time, 0, strategy, "duplicate_order_id"}); return;
        }
        // Remember rejected requests too, so a subsequent cancellation has a reason.
        requests.emplace(order.id, order); strategy_orders.emplace(order.id, strategy);
        std::vector<Trade> trades;
        try {
            require(order.id > 0 && order.price > 0 && order.price <= MAX_PRICE && order.remaining_qty > 0 && order.remaining_qty <= MAX_QUANTITY, "Instrument price/quantity/ID bounds");
            trades = book.addOrder(order);
        } catch (const std::invalid_argument& error) {
            send(ReportType::Rejected, order, time, 0, error.what()); return;
        }
        // Acceptance precedes individual fills. Its quantity is submitted quantity,
        // never an execution. Other exceptions escape: engine is not transactional.
        send(ReportType::Accepted, order, time, order.remaining_qty);
        for (const auto& trade : trades) {
            if (execution_exhausted) throw std::overflow_error("Execution IDs exhausted");
            const ull execution_id = next_execution;
            if (next_execution == std::numeric_limits<ull>::max()) execution_exhausted = true; else ++next_execution;
            for (const ull id : {trade.buy_id, trade.sell_id}) {
                const auto& request = requests.at(id);
                if (strategy_orders.at(id)) truth.push_back({execution_id, id, request.side, trade.price, trade.quantity, time});
                send(ReportType::Fill, request, time, trade.quantity, "", execution_id, trade.price);
            }
        }
    }
    void cancel(ull id, bool strategy, Time time) {
        require(time >= 0, "Negative exchange time");
        const auto request = requests.find(id);
        if (request == requests.end()) { emit({ReportType::CancelRejected, id, 0, Side::Buy, 0, 0, time, 0, strategy, "unknown_order"}); return; }
        const auto active = book.activeOrder(id);
        if (!active) { send(ReportType::CancelRejected, request->second, time, 0, "not_resting: filled, rejected, or already_cancelled"); return; }
        if (!book.cancelOrder(id)) throw std::logic_error("Active order cancellation failed");
        send(ReportType::Cancelled, request->second, time, active->remaining_qty);
    }
};
enum class Outcome { Pending, Working, Filled, Cancelled, Rejected };
inline const char* outcomeText(Outcome outcome) {
    switch (outcome) {
        case Outcome::Pending: return "pending";
        case Outcome::Working: return "working";
        case Outcome::Filled: return "filled";
        case Outcome::Cancelled: return "cancelled";
        case Outcome::Rejected: return "rejected";
    }
    throw std::logic_error("Unknown outcome");
}
struct OrderState {
    ull id; Side side; ll price, submitted, executed = 0, remaining;
    bool submission_sent = false, acceptance_known = false, cancel_requested = false, cancel_pending = false;
    Outcome outcome = Outcome::Pending;
    ll cancelled = 0;
};
class OrderManager {
    std::unordered_map<ull, Report> executions;
public:
    OrderState order;
    Account account;
    ll notional = 0;
    OrderManager(ull id, const Task& task, Account initial) :
        order{id, task.side, task.limit_price, task.target, 0, task.target}, account(initial) {}
    bool outstanding() const { return order.submission_sent && order.remaining > 0 && order.outcome != Outcome::Rejected; }
    void rejectLocal() { order.outcome = Outcome::Rejected; order.remaining = 0; }
    void submitted() { require(!order.submission_sent, "Duplicate strategy submission"); order.submission_sent = true; }
    void requestCancel() { require(outstanding() && !order.cancel_requested, "Invalid cancellation request"); order.cancel_requested = order.cancel_pending = true; }
    // Return false for an identical fill duplicate. Delivery timestamp can differ.
    bool receive(const Report& report) {
        require(report.strategy && report.order_id == order.id && report.side == order.side && report.exchange_time >= 0 && report.delivery_time >= report.exchange_time, "Unexpected order report identity/time");
        if (report.type == ReportType::Fill) {
            require(report.execution_id > 0 && report.quantity > 0 && report.price > 0 && report.price <= MAX_PRICE, "Invalid fill");
            const auto seen = executions.find(report.execution_id);
            if (seen != executions.end()) {
                const auto& prior = seen->second;
                require(prior.order_id == report.order_id && prior.side == report.side && prior.quantity == report.quantity && prior.price == report.price && prior.exchange_time == report.exchange_time, "Conflicting duplicate execution report"); return false;
            }
            require(order.submission_sent && report.quantity <= order.remaining && order.outcome != Outcome::Cancelled && order.outcome != Outcome::Rejected, "Fill after terminal outcome or overfill");
            require(order.side == Side::Buy ? report.price <= order.price : report.price >= order.price, "Fill violates limit price");
            const ll amount = product(report.price, report.quantity);
            const Account next{add(account.cash, order.side == Side::Buy ? -amount : amount), add(account.inventory, order.side == Side::Buy ? report.quantity : -report.quantity)};
            require(next.cash >= 0 && next.inventory >= 0, "Spot account invariant failed");
            const ll next_notional = add(notional, amount);
            executions.emplace(report.execution_id, report);
            account = next; notional = next_notional;
            order.executed = add(order.executed, report.quantity); order.remaining -= report.quantity;
            order.outcome = order.remaining ? Outcome::Working : Outcome::Filled;
        } else if (report.type == ReportType::Accepted) {
            require(order.submission_sent && report.quantity == order.submitted && report.price == order.price && order.outcome != Outcome::Rejected, "Invalid acceptance");
            order.acceptance_known = true;
            if (order.outcome == Outcome::Pending) order.outcome = Outcome::Working;
        } else if (report.type == ReportType::Rejected) {
            require(order.executed == 0 && !order.acceptance_known, "Rejection after acceptance or fill");
            order.outcome = Outcome::Rejected; order.remaining = 0;
        } else if (report.type == ReportType::Cancelled) {
            require(order.cancel_pending && report.quantity > 0 && report.quantity == order.remaining, "Invalid cancellation quantity/state");
            order.cancelled = report.quantity; order.remaining = 0; order.cancel_pending = false; order.outcome = Outcome::Cancelled;
        } else {
            require(report.type == ReportType::CancelRejected && order.cancel_pending, "Unexpected cancellation rejection");
            order.cancel_pending = false; // A terminal full fill stays terminal.
        }
        require(order.executed >= 0 && order.executed <= order.submitted && order.remaining >= 0 && order.remaining <= order.submitted - order.executed, "Order quantity invariant failed");
        return true;
    }
};
enum class PolicyEvent { Start, Deadline, OrderReport };
enum class Action { None, Submit, Cancel };
// Value-only local view; no scheduler, book, exchange truth, or script access.
struct PolicyView { bool submitted, outstanding, cancel_requested; };
inline Action immediatePolicy(PolicyEvent event, const PolicyView& view) {
    if (event == PolicyEvent::Start && !view.submitted) return Action::Submit;
    if (event == PolicyEvent::Deadline && view.outstanding && !view.cancel_requested) return Action::Cancel;
    return Action::None;
}
struct Summary {
    ll target, by_deadline, late, completed, uncompleted, cash, inventory, notional;
    Outcome outcome;
};
class Experiment {
    Task task;
    Time request_delay, response_delay;
    Ids ids;
    Scheduler scheduler;
    OrderManager manager;
    Exchange exchange;
    std::ostringstream trace;
    bool initialized = false, deadline_delivered = false, failed = false;
    ull trace_sequence = 0;
    std::string local_error;
    // Trace seq is scheduler insertion sequence, not a second report chronology.
    void log(ull sequence, std::optional<Time> occurrence, Time delivery, const std::string& type, ull id, ull execution_id, ll price, ll quantity, const std::string& detail) {
        if (trace_sequence == std::numeric_limits<ull>::max()) throw std::overflow_error("Trace sequence exhausted");
        trace << "trace_seq=" << ++trace_sequence << " event_seq=" << sequence << " exchange_us=";
        if (occurrence) trace << *occurrence; else trace << "NA";
        trace << " delivery_us=" << delivery
              << " type=" << type << " order=" << id << " execution=" << execution_id
              << " price_paise=" << price << " quantity=" << quantity << " " << detail << '\n';
    }
    void emit(Report report) {
        report.delivery_time = delayed(report.exchange_time, response_delay);
        scheduler.schedule(report.delivery_time, [this, report](Time now, ull sequence) {
            bool applied = true;
            if (report.strategy) applied = manager.receive(report);
            std::ostringstream detail;
            detail << (report.strategy ? "strategy" : "external") << " reason=" << (report.reason.empty() ? "none" : report.reason);
            if (report.strategy) detail << " state=" << outcomeText(manager.order.outcome) << " executed=" << manager.order.executed << " remaining=" << manager.order.remaining
                                       << " cancel_pending=" << manager.order.cancel_pending << " cash_paise=" << manager.account.cash << " inventory=" << manager.account.inventory << " applied=" << applied;
            log(sequence, report.exchange_time, now, reportText(report.type), report.order_id, report.execution_id, report.price, report.quantity, detail.str());
            policy(PolicyEvent::OrderReport, now, sequence);
        });
    }
    void policy(PolicyEvent event, Time now, ull sequence) {
        const Action action = immediatePolicy(event, {manager.order.submission_sent, manager.outstanding(), manager.order.cancel_requested});
        if (action == Action::Submit) {
            if (!local_error.empty()) { manager.rejectLocal(); log(sequence, std::nullopt, now, "local_rejection", manager.order.id, 0, task.limit_price, task.target, local_error); return; }
            const Order order{manager.order.id, task.side, task.limit_price, task.target};
            manager.submitted();
            log(sequence, std::nullopt, now, "submit_request", order.id, 0, order.price, order.remaining_qty, "local account unchanged; exchange arrival pending");
            scheduler.schedule(delayed(now, request_delay), [this, order](Time time, ull seq) {
                log(seq, time, time, "submit_arrival", order.id, 0, order.price, order.remaining_qty, "strategy"); exchange.submit(order, true, time);
            });
        } else if (action == Action::Cancel) {
            manager.requestCancel();
            log(sequence, std::nullopt, now, "cancel_request", manager.order.id, 0, task.limit_price, manager.order.remaining, "cancel_pending=1; order not cancelled");
            scheduler.schedule(delayed(now, request_delay), [this](Time time, ull seq) {
                log(seq, time, time, "cancel_arrival", manager.order.id, 0, task.limit_price, 0, "strategy"); exchange.cancel(manager.order.id, true, time);
            });
        }
    }
    void initialize() {
        if (initialized) return;
        initialized = true;
        // Script is queued before timers. Equal-time order follows insertion order,
        // including same-time requests/reports created by a timer or exchange event.
        scheduler.schedule(task.start, [this](Time time, ull seq) {
            log(seq, std::nullopt, time, "task_start", manager.order.id, 0, task.limit_price, task.target, "policy delivered timer"); policy(PolicyEvent::Start, time, seq);
        });
        scheduler.schedule(task.deadline, [this](Time time, ull seq) {
            deadline_delivered = true;
            log(seq, std::nullopt, time, "deadline", manager.order.id, 0, task.limit_price, manager.order.remaining, "policy delivered timer"); policy(PolicyEvent::Deadline, time, seq);
        });
    }
public:
    Experiment(Task task, Account initial, Time request_delay = 0, Time response_delay = 0) :
        task(task), request_delay(request_delay), response_delay(response_delay),
        manager(ids.allocate(), task, initial), exchange([this](Report report) { emit(std::move(report)); }) {
        require((task.side == Side::Buy || task.side == Side::Sell) && task.target > 0 && task.limit_price > 0 && task.start >= 0 && task.deadline > task.start, "Invalid execution task");
        require(initial.cash >= 0 && initial.inventory >= 0 && request_delay >= 0 && response_delay >= 0, "Invalid account or delay");
        // All task-generated request/report times must be representable before mutation.
        delayed(delayed(task.deadline, request_delay), response_delay);
        try {
            require(task.limit_price <= MAX_PRICE && task.target <= MAX_QUANTITY, "Task exceeds fictional instrument bounds");
            if (task.side == Side::Buy) { require(product(task.limit_price, task.target) <= initial.cash, "Insufficient cash at limit price"); add(initial.inventory, task.target); }
            else { require(task.target <= initial.inventory, "Insufficient inventory"); add(initial.cash, product(MAX_PRICE, task.target)); }
        } catch (const std::exception& error) { local_error = error.what(); }
    }
    Experiment(const Experiment&) = delete;
    Experiment& operator=(const Experiment&) = delete;
    ull externalOrder(Time arrival, Side side, ll price, ll quantity) {
        require(!initialized, "Script must be registered before starting");
        delayed(arrival, response_delay); // Reject invalid/overflowing report times before engine mutation.
        const ull id = ids.allocate();
        scheduler.schedule(arrival, [this, order = Order{id, side, price, quantity}](Time time, ull seq) {
            log(seq, time, time, "external_submit", order.id, 0, order.price, order.remaining_qty, "script arrival"); exchange.submit(order, false, time);
        }); return id;
    }
    void externalCancel(Time arrival, ull id) {
        require(!initialized && id != manager.order.id, "Invalid external cancellation script"); delayed(arrival, response_delay);
        scheduler.schedule(arrival, [this, id](Time time, ull seq) { log(seq, time, time, "external_cancel", id, 0, 0, 0, "script arrival"); exchange.cancel(id, false, time); });
    }
    bool step() {
        require(!failed, "Failed experiment cannot resume");
        try { initialize(); return scheduler.step(); }
        catch (...) { failed = true; throw; }
    }
    const OrderManager& local() const { return manager; }
    const std::vector<Execution>& exchangeExecutions() const { return exchange.truth; }
    std::string eventTrace() const { return trace.str(); }
    Summary finish() {
        require(!failed, "Failed experiment cannot resume");
        try { initialize(); scheduler.drain(); } catch (...) { failed = true; throw; }
        if (!deadline_delivered || manager.order.cancel_pending || manager.outstanding() ||
            (manager.order.outcome != Outcome::Filled && manager.order.outcome != Outcome::Cancelled && manager.order.outcome != Outcome::Rejected)) { failed = true; throw std::runtime_error("Execution experiment failed to settle"); }
        ll before = 0, after = 0;
        for (const auto& fill : exchange.truth) {
            if (fill.time <= task.deadline) before = add(before, fill.quantity); else after = add(after, fill.quantity);
        }
        if (add(before, after) != manager.order.executed) { failed = true; throw std::runtime_error("Exchange/local completion mismatch"); }
        return {task.target, before, after, manager.order.executed, task.target - manager.order.executed, manager.account.cash, manager.account.inventory, manager.notional, manager.order.outcome};
    }
    std::string run() {
        const Summary summary = finish();
        std::ostringstream result; result << "instrument=WHOLE units=whole price/cash=paise time=microseconds zero_fees=1 request_delay_us=" << request_delay << " response_delay_us=" << response_delay << '\n' << eventTrace();
        result << "SUMMARY side=" << sideText(task.side) << " target=" << summary.target << " deadline_us=" << task.deadline
               << " by_deadline=" << summary.by_deadline << " late=" << summary.late << " completed=" << summary.completed << " uncompleted=" << summary.uncompleted
               << " state=" << outcomeText(summary.outcome) << " cash_paise=" << summary.cash << " inventory=" << summary.inventory << " executed_notional_paise=" << summary.notional;
        if (!summary.completed) result << " average_price_paise=unavailable\n";
        else result << " average_price_paise=" << summary.notional / summary.completed << " (floor display; exact ratio " << summary.notional << '/' << summary.completed << ")\n";
        return result.str();
    }
};
inline std::string immediateDemo() {
    Experiment experiment({Side::Buy, 5, 10, 100, 105}, {1000, 0}, 5, 7);
    experiment.externalOrder(0, Side::Sell, 103, 2);
    experiment.externalOrder(0, Side::Sell, 100, 2);
    experiment.externalOrder(0, Side::Sell, 101, 2);
    return experiment.run();
}
inline std::string raceDemo() {
    Experiment experiment({Side::Buy, 3, 10, 30, 100}, {1000, 0}, 10, 25);
    experiment.externalOrder(35, Side::Sell, 99, 3);
    return experiment.run();
}
}
