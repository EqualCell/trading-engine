#pragma once
#include "units.h"
#include <limits>
#include <queue>
#include <sstream>
#include <string>
#include <set>
#include <utility>

namespace execution {
// One instrument per experiment: price/quantity/cash atoms, microsecond time.
using Time = ll;
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
// Task price/target and Account inventory are instrument atoms; cash uses summed scale.
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
    Instrument instrument;
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
    explicit Exchange(std::function<void(Report)> deliver, Instrument specification = {}) : instrument(std::move(specification)), emit(std::move(deliver)) { instrument.validate(); }
    std::vector<Execution> truth;
    void submit(Order order, bool strategy, Time time) {
        require(time >= 0, "Negative exchange time");
        if (requests.count(order.id)) {
            emit({ReportType::Rejected, order.id, 0, order.side, order.price, 0, time, 0, strategy, "duplicate_order_id"}); return;
        }
        // Remember rejected requests too, so a subsequent cancellation has a reason.
        requests.emplace(order.id, order); strategy_orders.emplace(order.id, strategy);
        try {
            require(order.id > 0, "Order ID must be positive");
            require(order.side == Side::Buy || order.side == Side::Sell, "Invalid order side");
            instrument.validateOrder(order.price, order.remaining_qty);
        } catch (const std::overflow_error& error) {
            send(ReportType::Rejected, order, time, 0, error.what()); return;
        } catch (const std::invalid_argument& error) {
            send(ReportType::Rejected, order, time, 0, error.what()); return;
        }
        // Validation rejections occur before mutation. Any engine failure escapes;
        // it is never reclassified as a safe numerical rejection.
        const auto trades = book.addOrder(order);
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
enum class Outcome { Pending, Working, Filled, Cancelled, Rejected, Incomplete };
inline const char* outcomeText(Outcome outcome) {
    switch (outcome) {
        case Outcome::Pending: return "pending";
        case Outcome::Working: return "working";
        case Outcome::Filled: return "filled";
        case Outcome::Cancelled: return "cancelled";
        case Outcome::Rejected: return "rejected";
        case Outcome::Incomplete: return "incomplete";
    }
    throw std::logic_error("Unknown outcome");
}
struct OrderState {
    ull id; Side side; ll price, submitted, executed = 0, remaining;
    bool submission_sent = false, acceptance_known = false, cancel_requested = false, cancel_pending = false;
    Outcome outcome = Outcome::Pending;
    ll cancelled = 0;
    ll reserved_cash = 0, reserved_inventory = 0;
    bool rejection_known = false;
    std::string rejection_reason = "";
};
struct Submission { bool accepted; std::string reason; };
class OrderManager {
    Task task;
    Instrument instrument;
    std::map<ull, OrderState> tracked;
    std::unordered_map<ull, Report> executions;
    ll reserved_cash_total = 0, reserved_inventory_total = 0;
    void validateExposure() const {
        require(executed >= 0 && potentiallyExecutable() <= task.target - executed, "Task exposure invariant failed");
        require(reserved_cash_total >= 0 && reserved_cash_total <= account.cash && reserved_inventory_total >= 0 && reserved_inventory_total <= account.inventory, "Reservation invariant failed");
    }
public:
    Account account;
    ll notional = 0, executed = 0, submitted_quantity = 0;
    ll submitted_orders = 0, local_rejections = 0, exchange_rejections = 0, successful_cancellations = 0;
    OrderManager(const Task& task, Account initial, Instrument specification = {}) : task(task), instrument(std::move(specification)), account(initial) {
        instrument.validate();
        require(initial.cash >= 0 && initial.inventory >= 0 && task.target > 0 && (task.side == Side::Buy || task.side == Side::Sell), "Invalid manager task/account");
    }
    const std::map<ull, OrderState>& orders() const { return tracked; }
    const OrderState& order(ull id) const { return tracked.at(id); }
    ll reservedCash() const { return reserved_cash_total; }
    ll reservedInventory() const { return reserved_inventory_total; }
    ll availableCash() const { return account.cash - reserved_cash_total; }
    ll availableInventory() const { return account.inventory - reserved_inventory_total; }
    ll potentiallyExecutable() const {
        ll total = 0;
        for (const auto& pair : tracked) if (pair.second.submission_sent) total = add(total, pair.second.remaining);
        return total;
    }
    bool outstanding(ull id) const { const auto& o = order(id); return o.submission_sent && (o.remaining > 0 || o.cancel_pending); }
    bool outstanding() const { for (const auto& pair : tracked) if (outstanding(pair.first)) return true; return false; }
    ll unresolvedOrders() const { ll count = 0; for (const auto& pair : tracked) if (outstanding(pair.first)) ++count; return count; }
    Submission submit(ull id, Side side, ll price, ll quantity, const std::string& prevalidation_error = "") {
        require(id > 0 && !tracked.count(id), "Strategy order ID already used");
        require(quantity > 0 && price > 0 && side == task.side, "Invalid local order request");
        // Compute everything before committing a reservation or sending a request.
        ll cash_reserve = 0;
        const ll inventory_reserve = side == Side::Sell ? quantity : 0;
        std::string reason = prevalidation_error;
        if (reason.empty()) {
            try { instrument.validateOrder(price, quantity); if (side == Side::Buy) cash_reserve = product(price, quantity); }
            catch (const std::invalid_argument& error) { reason = error.what(); }
            catch (const std::overflow_error& error) { reason = error.what(); }
        }
        if (reason.empty() && price != task.limit_price) reason = "task_limit_mismatch";
        if (reason.empty() && quantity > task.target - executed - potentiallyExecutable()) reason = "task_target_overcommitment";
        if (reason.empty() && cash_reserve > availableCash()) reason = "insufficient_available_cash";
        if (reason.empty() && inventory_reserve > availableInventory()) reason = "insufficient_available_inventory";
        if (reason.empty()) {
            try {
                if (side == Side::Buy) add(account.inventory, add(potentiallyExecutable(), quantity));
                else add(account.cash, product(instrument.maximumPrice(), add(potentiallyExecutable(), quantity)));
            } catch (const std::overflow_error& error) { reason = error.what(); }
        }
        OrderState state{id, side, price, quantity, 0, quantity};
        if (!reason.empty()) {
            state.outcome = Outcome::Rejected; state.remaining = 0;
            state.rejection_known = true; state.rejection_reason = reason;
            const ll next_rejections = add(local_rejections, 1);
            tracked.emplace(id, std::move(state)); local_rejections = next_rejections;
            return {false, reason};
        }
        const ll next_cash_reserve = add(reserved_cash_total, cash_reserve);
        const ll next_inventory_reserve = add(reserved_inventory_total, inventory_reserve);
        const ll next_submitted = add(submitted_quantity, quantity), next_orders = add(submitted_orders, 1);
        state.submission_sent = true; state.reserved_cash = cash_reserve; state.reserved_inventory = inventory_reserve;
        tracked.emplace(id, std::move(state));
        reserved_cash_total = next_cash_reserve; reserved_inventory_total = next_inventory_reserve;
        submitted_quantity = next_submitted; submitted_orders = next_orders; validateExposure();
        return {true, ""};
    }
    void requestCancel(ull id) {
        auto& state = tracked.at(id);
        require(state.submission_sent && state.remaining > 0 && !state.cancel_requested, "Invalid cancellation request");
        state.cancel_requested = state.cancel_pending = true; validateExposure();
    }
    bool receive(const Report& report) {
        require(report.strategy && tracked.count(report.order_id) && report.exchange_time >= 0 && report.delivery_time >= report.exchange_time, "Unexpected order report identity/time");
        auto& state = tracked.at(report.order_id);
        require(report.side == state.side && state.submission_sent, "Unexpected report side/submission");
        if (report.type == ReportType::Fill) {
            require(report.execution_id > 0 && report.quantity > 0 && report.price > 0, "Invalid fill");
            instrument.validateOrder(report.price, report.quantity);
            const auto seen = executions.find(report.execution_id);
            if (seen != executions.end()) {
                const auto& prior = seen->second;
                require(prior.order_id == report.order_id && prior.side == report.side && prior.quantity == report.quantity && prior.price == report.price && prior.exchange_time == report.exchange_time, "Conflicting duplicate execution report"); return false;
            }
            require(report.quantity <= state.remaining && state.outcome != Outcome::Cancelled && state.outcome != Outcome::Rejected, "Fill after terminal outcome or overfill");
            require(state.side == Side::Buy ? report.price <= state.price : report.price >= state.price, "Fill violates limit price");
            const ll amount = product(report.price, report.quantity);
            const Account next{add(account.cash, state.side == Side::Buy ? -amount : amount), add(account.inventory, state.side == Side::Buy ? report.quantity : -report.quantity)};
            const ll cash_release = state.side == Side::Buy ? product(state.price, report.quantity) : 0;
            const ll inventory_release = state.side == Side::Sell ? report.quantity : 0;
            const ll next_notional = add(notional, amount), next_executed = add(executed, report.quantity);
            require(next.cash >= 0 && next.inventory >= 0 && cash_release <= state.reserved_cash && inventory_release <= state.reserved_inventory && next_executed <= task.target, "Fill/account/reservation invariant failed");
            executions.emplace(report.execution_id, report);
            account = next; notional = next_notional; executed = next_executed;
            state.executed = add(state.executed, report.quantity); state.remaining -= report.quantity;
            state.reserved_cash -= cash_release; state.reserved_inventory -= inventory_release;
            reserved_cash_total -= cash_release; reserved_inventory_total -= inventory_release;
            state.outcome = state.remaining ? Outcome::Working : Outcome::Filled;
        } else if (report.type == ReportType::Accepted) {
            require(report.quantity == state.submitted && report.price == state.price && state.outcome != Outcome::Rejected, "Invalid acceptance");
            state.acceptance_known = true;
            if (state.outcome == Outcome::Pending) state.outcome = Outcome::Working;
        } else if (report.type == ReportType::Rejected) {
            if (state.rejection_known) { require(state.rejection_reason == report.reason, "Conflicting duplicate rejection"); return false; }
            require(state.executed == 0 && !state.acceptance_known && state.outcome == Outcome::Pending, "Rejection after acceptance or fill");
            const ll next_rejections = add(exchange_rejections, 1);
            reserved_cash_total -= state.reserved_cash; reserved_inventory_total -= state.reserved_inventory;
            state.reserved_cash = state.reserved_inventory = 0; state.remaining = 0;
            state.outcome = Outcome::Rejected; state.rejection_known = true; state.rejection_reason = report.reason; exchange_rejections = next_rejections;
            // Keep a pending cancellation until its own response settles.
        } else if (report.type == ReportType::Cancelled) {
            if (state.outcome == Outcome::Cancelled) { require(report.quantity == state.cancelled, "Conflicting duplicate cancellation"); return false; }
            require(state.cancel_pending && report.quantity > 0 && report.quantity == state.remaining, "Invalid cancellation quantity/state");
            const ll next_cancellations = add(successful_cancellations, 1);
            reserved_cash_total -= state.reserved_cash; reserved_inventory_total -= state.reserved_inventory;
            state.reserved_cash = state.reserved_inventory = 0; state.cancelled = report.quantity; state.remaining = 0;
            state.cancel_pending = false; state.outcome = Outcome::Cancelled; successful_cancellations = next_cancellations;
        } else {
            require(report.type == ReportType::CancelRejected && state.cancel_pending, "Unexpected cancellation rejection");
            state.cancel_pending = false; // No reservation release on failed cancel.
        }
        require(state.executed >= 0 && state.executed <= state.submitted && state.remaining >= 0 && state.remaining <= state.submitted - state.executed, "Order quantity invariant failed");
        validateExposure(); return true;
    }
};
enum class PolicyKind { Immediate, Scheduled };
struct PolicyConfig { PolicyKind kind = PolicyKind::Immediate; ll slices = 1; };
struct Slice { Time time; ll quantity; };
inline std::vector<Slice> slicePlan(const Task& task, ll count, const Instrument& instrument = {}) {
    instrument.validate();
    require(task.target > 0 && task.target % instrument.quantity_step_atoms == 0, "Task quantity is not step aligned");
    const ll lots = task.target / instrument.quantity_step_atoms;
    require(task.target > 0 && count > 0 && count <= lots && task.start >= 0 && task.deadline > task.start, "Invalid slice quantity/count/task");
    const Time duration = task.deadline - task.start;
    require(duration >= count, "Time window too short for distinct slice times");
    const ll base_quantity = lots / count, extra = lots % count;
    const Time step = duration / count, remainder = duration % count;
    // Increment quotient/remainder rather than multiplying i by duration.
    // carry + remainder is computed without overflow even at LLONG_MAX.
    Time offset = 0, carry = 0;
    std::vector<Slice> result;
    for (ll i = 0; i < count; ++i) {
        result.push_back({add(task.start, offset), product(base_quantity + (i < extra ? 1 : 0), instrument.quantity_step_atoms)});
        if (i + 1 < count) {
            offset = add(offset, step);
            if (remainder && carry >= count - remainder) { carry -= count - remainder; offset = add(offset, 1); }
            else carry += remainder;
        }
    }
    return result;
}
enum class PolicyEvent { Start, SliceTimer, Deadline, OrderReport };
enum class Action { None, Submit, Cancel };
struct PolicyView { bool deadline_reached; ll due_quantity; };
// The policy sees only delivered timer/report kinds and local values.
inline Action boundedPolicy(PolicyEvent event, const PolicyView& view) {
    if ((event == PolicyEvent::Start || event == PolicyEvent::SliceTimer) && !view.deadline_reached && view.due_quantity > 0) return Action::Submit;
    if (event == PolicyEvent::Deadline) return Action::Cancel;
    return Action::None;
}
struct Summary {
    ll target, by_deadline, late, completed, uncompleted, cash, inventory, notional;
    Outcome outcome;
    ll submitted_orders, submitted_quantity, local_rejections, exchange_rejections;
    ll reserved_cash, reserved_inventory, successful_cancellations, unresolved_orders;
};
inline void writeSummary(std::ostream& out, const Summary& summary, const Task& task, const Instrument& instrument = {}) {
    instrument.validate(); const bool legacy = instrument.legacyUnits();
    const std::string quantity_suffix = legacy ? "" : "_atoms";
    out << "SUMMARY side=" << sideText(task.side) << " target" << quantity_suffix << '=' << summary.target << " deadline_us=" << task.deadline
        << " submitted_orders=" << summary.submitted_orders << " submitted_quantity" << quantity_suffix << '=' << summary.submitted_quantity
        << " local_rejections=" << summary.local_rejections << " exchange_rejections=" << summary.exchange_rejections
        << " by_deadline" << quantity_suffix << '=' << summary.by_deadline << " late" << quantity_suffix << '=' << summary.late
        << " completed" << quantity_suffix << '=' << summary.completed << " uncompleted" << quantity_suffix << '=' << summary.uncompleted
        << " state=" << outcomeText(summary.outcome) << (legacy ? " cash_paise=" : " cash_atoms=") << summary.cash
        << " inventory" << quantity_suffix << '=' << summary.inventory << (legacy ? " executed_notional_paise=" : " executed_notional_cash_atoms=") << summary.notional
        << (legacy ? " reserved_cash_paise=" : " reserved_cash_atoms=") << summary.reserved_cash << " reserved_inventory" << quantity_suffix << '=' << summary.reserved_inventory
        << " successful_cancellations=" << summary.successful_cancellations << " unresolved_orders=" << summary.unresolved_orders;
    out << (legacy ? " average_price_paise=" : " average_price_atoms=");
    if (!summary.completed) out << "unavailable\n";
    else out << summary.notional / summary.completed << " (floor display; exact ratio " << summary.notional << '/' << summary.completed << ")\n";
    if (!legacy) {
        out << "VALUES cash=" << instrument.cashText(summary.cash) << ' ' << instrument.quote << " inventory=" << instrument.quantityText(summary.inventory) << ' ' << instrument.base
            << " notional=" << instrument.cashText(summary.notional) << ' ' << instrument.quote << " reserved_cash=" << instrument.cashText(summary.reserved_cash)
            << ' ' << instrument.quote << " average_price=";
        if (!summary.completed) out << "unavailable";
        else out << instrument.priceText(summary.notional / summary.completed) << ' ' << instrument.quote << '/' << instrument.base << " (floor to price atom; exact ratio above is in price atoms)";
        out << '\n';
    }
}
class Experiment {
    Task task;
    Instrument instrument;
    Time request_delay, response_delay;
    PolicyConfig config;
    std::vector<Slice> slices;
    Ids ids;
    ull first_strategy_id;
    Scheduler scheduler;
    OrderManager manager;
    Exchange exchange;
    std::set<ull> external_ids;
    std::ostringstream trace;
    bool initialized = false, deadline_delivered = false, failed = false, first_submission = true;
    ull trace_sequence = 0;
    std::string local_error;
    void log(ull sequence, std::optional<Time> occurrence, Time delivery, const std::string& type, ull id, ull execution_id, ll price, ll quantity, const std::string& detail) {
        if (trace_sequence == std::numeric_limits<ull>::max()) throw std::overflow_error("Trace sequence exhausted");
        trace << "trace_seq=" << ++trace_sequence << " event_seq=" << sequence << " exchange_us=";
        if (occurrence) trace << *occurrence; else trace << "NA";
        trace << " delivery_us=" << delivery << " type=" << type << " order=" << id << " execution=" << execution_id
              << (instrument.legacyUnits() ? " price_paise=" : " price_atoms=") << price << (instrument.legacyUnits() ? " quantity=" : " quantity_atoms=") << quantity << " " << detail << '\n';
    }
    std::string balances() const {
        const bool legacy = instrument.legacyUnits();
        return std::string(legacy ? " cash_paise=" : " cash_atoms=") + std::to_string(manager.account.cash)
            + (legacy ? " inventory=" : " inventory_atoms=") + std::to_string(manager.account.inventory)
            + (legacy ? " reserved_cash_paise=" : " reserved_cash_atoms=") + std::to_string(manager.reservedCash())
            + (legacy ? " reserved_inventory=" : " reserved_inventory_atoms=") + std::to_string(manager.reservedInventory())
            + (legacy ? " task_executed=" : " task_executed_atoms=") + std::to_string(manager.executed)
            + (legacy ? " potentially_executable=" : " potentially_executable_atoms=") + std::to_string(manager.potentiallyExecutable());
    }
    void emit(Report report) {
        report.delivery_time = delayed(report.exchange_time, response_delay);
        scheduler.schedule(report.delivery_time, [this, report](Time now, ull sequence) {
            bool applied = true;
            if (report.strategy) applied = manager.receive(report);
            std::ostringstream detail;
            detail << (report.strategy ? "strategy" : "external") << " reason=" << (report.reason.empty() ? "none" : report.reason);
            if (report.strategy) {
                const auto& state = manager.order(report.order_id);
                detail << " state=" << outcomeText(state.outcome) << (instrument.legacyUnits() ? " executed=" : " executed_atoms=") << state.executed << (instrument.legacyUnits() ? " remaining=" : " remaining_atoms=") << state.remaining
                       << " cancel_pending=" << state.cancel_pending << balances() << " applied=" << applied;
            }
            log(sequence, report.exchange_time, now, reportText(report.type), report.order_id, report.execution_id, report.price, report.quantity, detail.str());
            policy(PolicyEvent::OrderReport, now, sequence, 0);
        });
    }
    void policy(PolicyEvent event, Time now, ull sequence, ll due_quantity) {
        const Action action = boundedPolicy(event, {deadline_delivered || now >= task.deadline, due_quantity});
        if (action == Action::Submit) {
            const ull id = first_submission ? first_strategy_id : ids.allocate(); first_submission = false;
            const Submission result = manager.submit(id, task.side, task.limit_price, due_quantity, local_error);
            if (!result.accepted) { log(sequence, std::nullopt, now, "local_rejection", id, 0, task.limit_price, due_quantity, result.reason + balances()); return; }
            const Order order{id, task.side, task.limit_price, due_quantity};
            log(sequence, std::nullopt, now, "submit_request", id, 0, order.price, order.remaining_qty, "reservation applied; account unchanged" + balances());
            scheduler.schedule(delayed(now, request_delay), [this, order](Time time, ull seq) {
                log(seq, time, time, "submit_arrival", order.id, 0, order.price, order.remaining_qty, "strategy"); exchange.submit(order, true, time);
            });
        } else if (action == Action::Cancel) {
            for (const auto& pair : manager.orders()) {
                const ull id = pair.first; const auto& state = pair.second;
                if (!state.submission_sent || state.remaining == 0 || state.cancel_requested) continue;
                manager.requestCancel(id);
                log(sequence, std::nullopt, now, "cancel_request", id, 0, state.price, state.remaining, "cancel_pending=1; reservations retained" + balances());
                scheduler.schedule(delayed(now, request_delay), [this, id](Time time, ull seq) {
                    log(seq, time, time, "cancel_arrival", id, 0, task.limit_price, 0, "strategy"); exchange.cancel(id, true, time);
                });
            }
        }
    }
    void initialize() {
        if (initialized) return;
        initialized = true;
        // Queue common script events and common timers identically for both policies.
        // Policy-only extra slice timers follow them; same-time children remain FIFO.
        scheduler.schedule(task.start, [this](Time time, ull seq) {
            log(seq, std::nullopt, time, "task_start", first_strategy_id, 0, task.limit_price, task.target, "policy delivered timer"); policy(PolicyEvent::Start, time, seq, slices[0].quantity);
        });
        scheduler.schedule(task.deadline, [this](Time time, ull seq) {
            deadline_delivered = true;
            log(seq, std::nullopt, time, "deadline", 0, 0, task.limit_price, manager.potentiallyExecutable(), "policy delivered timer"); policy(PolicyEvent::Deadline, time, seq, 0);
        });
        for (std::size_t i = 1; i < slices.size(); ++i) scheduler.schedule(slices[i].time, [this, quantity = slices[i].quantity](Time time, ull seq) {
            log(seq, std::nullopt, time, "slice_timer", 0, 0, task.limit_price, quantity, "policy delivered timer"); policy(PolicyEvent::SliceTimer, time, seq, quantity);
        });
    }
public:
    Experiment(Task task, Account initial, Time request_delay = 0, Time response_delay = 0, PolicyConfig config = {}, Instrument specification = {}) :
        task(task), instrument(std::move(specification)), request_delay(request_delay), response_delay(response_delay), config(config), first_strategy_id(ids.allocate()),
        manager(task, initial, instrument), exchange([this](Report report) { emit(std::move(report)); }, instrument) {
        require((task.side == Side::Buy || task.side == Side::Sell) && task.target > 0 && task.limit_price > 0 && task.start >= 0 && task.deadline > task.start, "Invalid execution task");
        require(initial.cash >= 0 && initial.inventory >= 0 && request_delay >= 0 && response_delay >= 0, "Invalid account or delay");
        require(config.kind == PolicyKind::Immediate || config.kind == PolicyKind::Scheduled, "Unknown policy");
        require(config.slices > 0 && (config.kind != PolicyKind::Immediate || config.slices == 1), "Invalid policy slice count");
        delayed(delayed(task.deadline, request_delay), response_delay);
        slices = slicePlan(task, config.kind == PolicyKind::Immediate ? 1 : config.slices, instrument);
        try {
            instrument.validateOrder(task.limit_price, task.target);
            if (task.side == Side::Buy) { require(product(task.limit_price, task.target) <= initial.cash, "Insufficient cash at limit price"); add(initial.inventory, task.target); }
            else { require(task.target <= initial.inventory, "Insufficient inventory"); add(initial.cash, product(instrument.maximumPrice(), task.target)); }
        } catch (const std::invalid_argument& error) { local_error = error.what(); }
        catch (const std::overflow_error& error) { local_error = error.what(); }
    }
    Experiment(const Experiment&) = delete;
    Experiment& operator=(const Experiment&) = delete;
    ull externalOrder(Time arrival, Side side, ll price, ll quantity) {
        require(!initialized, "Script must be registered before starting"); delayed(arrival, response_delay);
        const ull id = ids.allocate(); external_ids.insert(id);
        scheduler.schedule(arrival, [this, order = Order{id, side, price, quantity}](Time time, ull seq) {
            log(seq, time, time, "external_submit", order.id, 0, order.price, order.remaining_qty, "script arrival"); exchange.submit(order, false, time);
        }); return id;
    }
    void externalCancel(Time arrival, ull id) {
        require(!initialized && external_ids.count(id), "External cancellation must use a registered external ID"); delayed(arrival, response_delay);
        scheduler.schedule(arrival, [this, id](Time time, ull seq) { log(seq, time, time, "external_cancel", id, 0, 0, 0, "script arrival"); exchange.cancel(id, false, time); });
    }
    bool step() {
        require(!failed, "Failed experiment cannot resume");
        try { initialize(); return scheduler.step(); } catch (...) { failed = true; throw; }
    }
    const OrderManager& local() const { return manager; }
    const std::vector<Execution>& exchangeExecutions() const { return exchange.truth; }
    std::string eventTrace() const { return trace.str(); }
    Summary finish() {
        require(!failed, "Failed experiment cannot resume");
        try { initialize(); scheduler.drain(); } catch (...) { failed = true; throw; }
        if (!deadline_delivered || manager.outstanding() || manager.reservedCash() || manager.reservedInventory()) { failed = true; throw std::runtime_error("Execution experiment failed to settle"); }
        ll before = 0, after = 0;
        for (const auto& fill : exchange.truth) { if (fill.time <= task.deadline) before = add(before, fill.quantity); else after = add(after, fill.quantity); }
        if (add(before, after) != manager.executed) { failed = true; throw std::runtime_error("Exchange/local completion mismatch"); }
        bool all_rejected = true;
        for (const auto& pair : manager.orders()) if (pair.second.outcome != Outcome::Rejected) all_rejected = false;
        const Outcome outcome = manager.executed == task.target ? Outcome::Filled : all_rejected ? Outcome::Rejected : manager.successful_cancellations ? Outcome::Cancelled : Outcome::Incomplete;
        return {task.target, before, after, manager.executed, task.target - manager.executed, manager.account.cash, manager.account.inventory, manager.notional, outcome,
                manager.submitted_orders, manager.submitted_quantity, manager.local_rejections, manager.exchange_rejections,
                manager.reservedCash(), manager.reservedInventory(), manager.successful_cancellations, manager.unresolvedOrders()};
    }
    std::string run() {
        const Summary summary = finish();
        std::ostringstream result;
        if (instrument.legacyUnits()) result << "instrument=WHOLE units=whole price/cash=paise time=microseconds zero_fees=1";
        else result << "instrument=" << instrument.symbol << " base=" << instrument.base << " quote=" << instrument.quote
                    << " price_decimals=" << instrument.price_decimals << " quantity_decimals=" << instrument.quantity_decimals << " cash_decimals=" << instrument.cashDecimals()
                    << " price_tick_atoms=" << instrument.price_tick_atoms << " quantity_step_atoms=" << instrument.quantity_step_atoms
                    << " time=microseconds zero_fees=1";
        result << " request_delay_us=" << request_delay << " response_delay_us=" << response_delay
               << " policy=" << (config.kind == PolicyKind::Immediate ? "immediate" : "scheduled") << " slices=" << slices.size() << '\n' << eventTrace();
        writeSummary(result, summary, task, instrument); return result.str();
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
