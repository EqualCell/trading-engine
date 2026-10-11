#pragma once
#include "experiment.h"

namespace execution {
enum class ScriptKind { Submit, Cancel };
struct ScriptEvent {
    Time time; ScriptKind kind; std::string key;
    Side side = Side::Sell; ll price = 0, quantity = 0;
};
struct Scenario {
    std::string name;
    Task task;
    Account initial;
    std::vector<ScriptEvent> script;
    Instrument instrument = {};
};
inline std::vector<Scenario> comparisonScenarios() {
    const Task task{Side::Buy, 6, 10, 100, 120};
    const Account initial{1000, 0};
    return {
        {"rising", task, initial, {
            {0, ScriptKind::Submit, "early", Side::Sell, 90, 6},
            {35, ScriptKind::Cancel, "early"},
            {35, ScriptKind::Submit, "middle", Side::Sell, 100, 4},
            {65, ScriptKind::Cancel, "middle"},
            {65, ScriptKind::Submit, "last", Side::Sell, 110, 2}}},
        {"falling", task, initial, {
            {0, ScriptKind::Submit, "early", Side::Sell, 110, 6},
            {35, ScriptKind::Cancel, "early"},
            {35, ScriptKind::Submit, "middle", Side::Sell, 100, 4},
            {65, ScriptKind::Cancel, "middle"},
            {65, ScriptKind::Submit, "last", Side::Sell, 90, 2}}},
        {"insufficient", task, initial, {
            {0, ScriptKind::Submit, "early", Side::Sell, 90, 2},
            {35, ScriptKind::Submit, "middle", Side::Sell, 100, 1},
            {101, ScriptKind::Submit, "late", Side::Sell, 119, 1}}}
    };
}
struct ComparisonResult { Summary summary; std::string trace; };
inline ComparisonResult compareRun(const Scenario& scenario, PolicyConfig config, Time request_delay = 2, Time response_delay = 5) {
    Experiment run(scenario.task, scenario.initial, request_delay, response_delay, config, scenario.instrument);
    // Scenario keys map to this fresh experiment's external IDs, never guessed
    // from the number of strategy orders or rewritten after observing a fill.
    std::map<std::string, ull> external;
    for (const auto& event : scenario.script) {
        if (event.kind == ScriptKind::Submit) {
            require(!external.count(event.key), "Duplicate external script key");
            external.emplace(event.key, run.externalOrder(event.time, event.side, event.price, event.quantity));
        } else {
            require(external.count(event.key), "Cancellation references an undeclared external key");
            run.externalCancel(event.time, external.at(event.key));
        }
    }
    const Summary summary = run.finish(); return {summary, run.eventTrace()};
}
inline std::string comparisonText(ll slices = 3, bool traces = false, const std::string& selected = "all", Time request_delay = 2, Time response_delay = 5) {
    require(selected == "all" || selected == "rising" || selected == "falling" || selected == "insufficient", "Unknown comparison scenario");
    require(slices > 0, "Slice count must be positive");
    std::ostringstream out;
    out << "WHOLE buy experiment; whole quantities; prices/notional/cash=paise; time=microseconds; zero fees\n"
        << "Same fixed script and fresh independent book/account per policy. request_delay_us=" << request_delay << " response_delay_us=" << response_delay << " scheduled_slices=" << slices << '\n'
        << "scenario policy target orders submitted_qty local_reject exchange_reject ontime late completed uncompleted notional avg_floor exact_avg cash inventory reserved_cash reserved_inventory cancelled unresolved\n";
    for (const auto& scenario : comparisonScenarios()) {
        if (selected != "all" && selected != scenario.name) continue;
        for (const auto kind : {PolicyKind::Immediate, PolicyKind::Scheduled}) {
            const auto result = compareRun(scenario, {kind, kind == PolicyKind::Immediate ? 1 : slices}, request_delay, response_delay);
            const auto& s = result.summary;
            out << scenario.name << ' ' << (kind == PolicyKind::Immediate ? "immediate" : "scheduled") << ' ' << s.target << ' ' << s.submitted_orders << ' ' << s.submitted_quantity
                << ' ' << s.local_rejections << ' ' << s.exchange_rejections << ' ' << s.by_deadline << ' ' << s.late << ' ' << s.completed << ' ' << s.uncompleted << ' ' << s.notional << ' ';
            if (s.completed) out << s.notional / s.completed << ' ' << s.notional << '/' << s.completed;
            else out << "N/A N/A";
            out << ' ' << s.cash << ' ' << s.inventory << ' ' << s.reserved_cash << ' ' << s.reserved_inventory << ' ' << s.successful_cancellations << ' ' << s.unresolved_orders << '\n';
            if (traces) out << "TRACE " << scenario.name << ' ' << (kind == PolicyKind::Immediate ? "immediate" : "scheduled") << '\n' << result.trace;
        }
    }
    out << "avg_floor rounds down in paise; exact_avg is fill notional/completed quantity. Compare completion beside cost; unfilled quantity has no execution price.\n";
    return out.str();
}
}
