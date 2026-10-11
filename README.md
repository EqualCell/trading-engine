# C++ market-making simulator

A deterministic trading-engine project with price/time-priority matching, trader ownership, cash and inventory accounting, a one-unit fixed-spread market maker, and an interactive replay dashboard. Synthetic scenarios use the actual C++ matching engine. Recorded historical sessions use a separate C++ CSV processor without simulated executions. JavaScript replays exported results rather than implementing matching.

## Run the dashboard

The three engine-generated exports are bundled in `dashboard/data/`. No frontend build, dependency installation, or backend is required.

```bash
python3 -m http.server 8000 --bind 127.0.0.1 --directory dashboard
```

Open **http://localhost:8000**. Select a scenario, step or play through events, scrub the timeline, and inspect quotes, balances, charts, the trade tape, and the event history. All panels show state **after the selected event**. Playback runs in events per second for inspection; it does not measure engine processing speed.

Use the JSON upload input for another schema-v1 export. Uploaded text is rendered as text, not HTML. Invalid data produces an error and retains the previous valid scenario. Imported and bundled scenarios can be switched without discarding the imported data.

A browser supporting ES modules, BigInt, and the File API is required. Serve over HTTP rather than opening `index.html` directly; browsers generally restrict module imports and `fetch` for `file://` URLs.

## Build and regenerate

Requirements: a C++17 compiler, Bash, and Python 3. The C++ serializer has no third-party dependencies. The frontend uses native HTML/CSS/JavaScript and SVG, with no CDN resources. `dashboard/package.json` only declares ES module syntax for optional Node tests; there are no npm dependencies.

From the repository root:

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic market_maker_demo.cpp -o market_maker_demo
./market_maker_demo
./market_maker_demo --scenario spread-capture
./market_maker_demo --scenario inventory-risk --output /tmp/inventory-risk.json
./market_maker_demo --scenario inventory-limit --output /tmp/inventory-limit.json
```

The no-argument demo retains the original six-event inventory-risk experiment. The fixed argument order is `--scenario NAME [--output FILE]`. Output directories must already exist. Invalid arguments are rejected before writing; open, write, flush, and close failures return a nonzero status.

Regenerate all three bundled exports:

```bash
./scripts/generate_dashboard_data.sh
```

The script builds the runner with warning flags, uses `${CXX:-g++}`, and works independently of the caller's working directory. It fails immediately and reports the failed line if compilation or export fails. Each scenario gets a fresh simulator and shared monotonic order-ID source, so repeated exports are byte-for-byte deterministic.

## Scenarios

| Scenario | Events | What it demonstrates |
| --- | ---: | --- |
| `spread-capture` | 3 | The maker buys at 9900 paise and sells at 10100, ending flat with +200 paise total marked P&L before fees. |
| `inventory-risk` | 6 | The round trip earns +200 at time 2000. A later buy leaves one unit exposed; a reference drop to 9500 yields cash 90300, marked value 99800, and P&L −200 at time 5000. |
| `inventory-limit` | 11 | Five external sells independently fill maker bids. Explicit refreshes follow each fill. At inventory +5, the final refresh suppresses the buy quote but permits a sell. Fill and refresh events share timestamps and remain separately ordered. |

All use maker trader 1 (100000 paise, zero inventory), external trader 2 (100000 paise, ten units), half-spread 100 paise, inventory limit 5, and one-unit orders. Trader 1 is exclusively controlled by the strategy. The external-order path is fixed to trader 2.

## Architecture

```text
Ordered scenario events
        │
        ├── MarketMaker: cancel/replace and inventory decisions
        └── External trader: explicit counterparty orders
                       │
              TradingSimulator
              ownership + accounts
                       │
                  OrderBook
              price/time matching
                       │
        trades + active-order snapshots
                       │
              checked marked valuation
                       │
               schema-v1 JSON export
                       │
         static dashboard: playback + SVG plots
```

- `order_book.h`: matching, cancellation, quotes, and `activeOrder(id) const`. The lookup returns an optional order **copy**, preserving engine encapsulation. Filled and cancelled orders are absent; partial fills expose the actual remaining quantity.
- `trading_simulator.h`: order ownership and account updates at each actual execution price. Only simulator APIs may submit or cancel account-owned orders. Ownership records survive fills and cancellation.
- `market_maker.h`: fixed-spread decisions, shared ID source, and checked integer valuation. Buys precede sells, and the sell decision re-reads inventory after a potentially immediate buy fill.
- `market_maker_scenarios.h`: deterministic scenario events, snapshots, and JSON serialization. Active maker quotes are resolved from their IDs, not inferred from the whole book's best prices or the strategy's last-submitted IDs. Both external-order trades and immediate strategy fills are captured once.
- `market_maker_demo.cpp`: text demonstration, named-scenario selection, and checked JSON file output.
- `dashboard/`: exact BigInt formatting, schema validation, synchronized playback controls, and step-style SVG charts. Missing quotes end their paths, leaving inactive intervals as gaps. Only checked relative time/value offsets convert to Number for plotting; text remains exact. Equal timestamps share chart x coordinates; event indexes distinguish each state in the slider, tape, and log.

## JSON schema v1

Each document contains `schema_version` (the number `1`), `scenario`, `description`, `units`, `assumptions`, `strategy`, `initial_accounts`, `initial_portfolio_value_paise`, and ordered `events`.

**All IDs, timestamps, indexes, money, and quantities are decimal strings**, including small values. JSON numbers must not be used for these fields. The fixed strategy uses one-unit orders.

Each event contains:

```text
event_index                 unique increasing index (decimal string)
timestamp_us                nondecreasing microsecond timestamp (decimal string)
type, description           event label and readable text
reference_price_paise       externally supplied synthetic mark
maker_bid, maker_ask         null, or {order_id, price_paise, remaining_quantity}
cash_paise, inventory       maker account state after the event
portfolio_value_paise       cash + inventory × reference
total_pnl_paise             marked value − initial portfolio value
trades                      ordered [{buy_id, sell_id, price_paise, quantity}]
```

Snapshots include only operations explicitly belonging to their event; there are no hidden quote refreshes. An event index distinguishes equal-time events. P&L is **total P&L marked to an externally supplied synthetic reference**, not realized P&L or liquidation value.

The frontend checks schema fields, integer ranges/encoding, valuation identities, inventory bounds, timestamp/index ordering, and plotting precision. It is an inspection tool for small exports: file size ≤64 MB, ≤10000 events, ≤20000 fills in total, and ≤1000 fills per event. Exports with relative plotting ranges beyond JavaScript's safe integer range are rejected rather than rounded. Cash and IDs can still exceed Number's safe integer range because they are never converted to Number.

## Tests

```bash
./scripts/generate_dashboard_data.sh
g++ -std=c++17 -Wall -Wextra -Wpedantic tests.cpp -o tests
g++ -std=c++17 -Wall -Wextra -Wpedantic simulator_tests.cpp -o simulator_tests
g++ -std=c++17 -Wall -Wextra -Wpedantic market_maker_tests.cpp -o market_maker_tests
g++ -std=c++17 -Wall -Wextra -Wpedantic main.cpp -o trading_engine
./tests
./simulator_tests
./market_maker_tests
PYTHONDONTWRITEBYTECODE=1 python3 test_replay.py
PYTHONDONTWRITEBYTECODE=1 python3 test_simulator_cli.py
python3 test_dashboard_exports.py
```

Optional frontend unit checks require Node.js 18+ (no npm install):

```bash
node test_dashboard_frontend.mjs
```

The export tests check valid JSON, required fields, deterministic/still-current bundled files, exact accounting and P&L, quote disappearance, unique trade records, equal-time ordering, invalid arguments, and file errors. The frontend tests use a minimal DOM fixture to exercise the actual control/render code, schema errors, exact rupee formatting, step-path quote gaps, synchronized panels, safe imported text, and scenario switching. **They are not browser layout tests.**

Browser verification and a screenshot were unavailable in the implementation environment because the browser automation service had no accessible browser. For manual QA, load all three scenarios; step, scrub, restart, play/pause, and switch scenarios; confirm inventory-risk ends at **−₹2.00**, inventory-limit ends at **5 units with no active bid**, and invalid JSON upload shows an error. Check the browser console and a narrow viewport as well.

## Existing CLI

The existing command and replay formats remain unchanged:

```bash
./trading_engine
./trading_engine --replay examples/orders.txt
./trading_engine --replay-timed examples/timed_orders.txt --trades-csv /tmp/trades.csv
./trading_engine --simulator
./trading_engine --simulator --replay examples/accounts.txt
./trading_engine --simulator --replay-timed examples/timed_accounts.txt --trades-csv /tmp/account-trades.csv
```

## Current limitations

This is a synthetic educational experiment, not evidence of strategy profitability. It models no fees, latency, slippage beyond book matching, margin, funding costs, live market connections, buying-power checks, reservations, or self-trade prevention. Negative cash and inventory are permitted. Portfolio valuation uses a supplied reference, not an executable liquidation price.

The strategy posts at most one one-unit buy and sell. Its independent-fill inventory limits require exclusive control of the maker's account. Refresh loses FIFO priority, immediate matching is allowed (no post-only rule), and quotes replenish only on explicit refresh events.

Simulator safety bounds are initial cash ±10^18 paise, initial inventory ±10^9 units, maximum price 10^9 paise, and lifetime accepted submitted quantity 10^9 units. Cancellations do not restore the quantity budget. These are overflow bounds, not financial risk controls. Valuation additionally checks multiplication, addition, and subtraction.

Strategy refresh and engine allocation failures are not transactional. The runner terminates on such failures rather than continuing with possibly inconsistent state. Export failure may leave an incomplete output file; no success message is printed. The static dashboard has no backend and replays only exported events.

## Historical-market viewer — first real-data milestone

The dashboard also replays **recorded Binance spot BTCUSDT** top-five snapshots and exchange trades. This mode displays observed market activity only: it does not generate strategy fills, account balances, inventory, or P&L. Prices are **USDT per BTC**, amounts are **BTC**, and times are UTC.

> Recorded historical market data. Top-five displayed depth. No strategy executions simulated.

### Obtain and prepare the window

The verified source is Tardis, **Binance spot** (exchange ID `binance`, not `binance-futures`), BTCUSDT, **2024-01-01**, the first five **capture/local-time** minutes: `[00:00:00, 00:05:00)` UTC. Tardis documents first-of-month CSV samples without an API key. Both requested endpoints returned HTTP 200 and the expected instrument and CSV headers:

- [Binance spot book_snapshot_5 source](https://datasets.tardis.dev/v1/binance/book_snapshot_5/2024/01/01/BTCUSDT.csv.gz)
- [Binance spot trades source](https://datasets.tardis.dev/v1/binance/trades/2024/01/01/BTCUSDT.csv.gz)

Provider references: [CSV overview](https://docs.tardis.dev/downloadable-csv-files/overview), [CSV schemas](https://docs.tardis.dev/downloadable-csv-files/data-types), [dataset API](https://docs.tardis.dev/downloadable-csv-files/api), and [Binance spot collection details](https://docs.tardis.dev/historical-data-details/binance).

From the repository root:

```bash
python3 scripts/prepare_historical.py --date 2024-01-01
```

The standard-library Python downloader streams each HTTP response through gzip and CSV parsing, stops after encountering the first row at the end of the capture window, and closes the response. It saves **CSV window extracts, not complete daily gzip archives**. Defaults are a **10 MiB compressed limit per response**, **20-second socket timeout**, **120-second response deadline**, **32 MiB decompressed scan limit**, **200000-row scan limit**, and **16 KiB record limit**. HTTP errors, non-CSV/error content, bad headers, incomplete windows, and backward capture ordering fail clearly.

The retained files live under the ignored `.local_data/binance-BTCUSDT/2024-01-01/` directory:

```text
book_snapshot_5.csv   retained snapshot rows
trades.csv            retained trade rows
manifest.json         URLs, retrieval time, row counts, SHA-256, limits and conventions
provenance.csv        equivalent flat metadata for the dependency-free C++ reader
```

A completed cached window is reused only after manifest/header/row-count/checksum checks; no redownload is necessary. An incomplete or altered cache fails rather than being overwritten. To make a fresh independent retrieval, use a new directory, for example `--directory /tmp/btcusdt-second-retrieval`. The script also accepts another first-of-month `--date`; use its corresponding directory in the commands below.

**Redistribution:** [Tardis terms, clause 9.2](https://docs.tardis.dev/legal/terms-of-service) restrict raw data redistribution; the aggregation exception does not authorize publishing this raw five-minute depth/trade session. Raw retained CSVs, their manifest, and the derived historical tick export remain ignored/local. Do not add them to tracked dashboard assets or publicly host them without appropriate permission. Existing synthetic scenarios remain bundled and usable without downloading anything.

### Build, export, and view

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic historical_replay.cpp -o historical_replay
mkdir -p dashboard/data/historical
./historical_replay \
  --snapshots .local_data/binance-BTCUSDT/2024-01-01/book_snapshot_5.csv \
  --trades .local_data/binance-BTCUSDT/2024-01-01/trades.csv \
  --provenance .local_data/binance-BTCUSDT/2024-01-01/provenance.csv \
  --output dashboard/data/historical/btcusdt.json
python3 -m http.server 8000 --bind 127.0.0.1 --directory dashboard
```

Open **http://localhost:8000**, then choose **Recorded · Binance BTCUSDT (local)**. Alternatively upload `dashboard/data/historical/btcusdt.json` through the existing file input. If the optional local entry has not been generated, the dashboard reports a loading error and retains the previous valid scenario. Synthetic INR scenarios still work unchanged.

### Processing and playback conventions

`historical_market.h` and `historical_replay.cpp` are separate from synthetic matching. **No historical record is submitted to `OrderBook::addOrder()`.** CSV headers, row widths, instrument, decimal syntax/precision, timestamps, paired missing levels, side sorting, and locked/crossed depth are validated. Invalid input fails with file/line information, before opening the output. Output/input aliasing and open/write/flush/close failures are rejected.

Prices and BTC amounts are parsed as checked fixed-point integers with **eight decimal places**, never `double`. Original decimal strings are preserved in exported levels and trades. Spread is exact; midpoint retains a possible **ninth decimal digit**. Price bounds prevent midpoint addition overflow. Leading zeroes other than a single zero before the decimal point, signs, exponents, nonpositive values, trailing junk, and excess precision are unsupported rather than rounded.

Replay uses the original **`local_timestamp` (capture time)**, retaining row order within each input file. A stable two-way merge places **snapshots before trades for cross-file capture-time ties**. This is a deterministic viewing convention, not a claim of recoverable exchange-wide ordering. Source exchange timestamps are retained separately, including regressions and timestamps just outside the capture window. The exporter reports these observations instead of sorting or excluding them.

Tardis `book_snapshot_5` is a provider-reconstructed top-five view from captured L2 updates; its documentation notes that crossed levels are removed upstream. This pipeline preserves those supplied levels. A snapshot replaces the observed top-five levels. Trailing missing price/amount pairs remain missing and are reported; unpaired or non-trailing holes fail. Before the first snapshot, book metrics and depth are unavailable. A recorded trade appears once and **never decrements the observed snapshot**. The result is aggregated top-five displayed depth, not a complete order-level book, queue reconstruction, or liquidation model.

The discriminated export is `schema: "historical-market"`, `schema_version: 1`, with instrument, units, provenance, ordering convention, anomaly messages, and ordered events. Each event has string-encoded `event_index`, `timestamp_us` (capture), `exchange_timestamp_us`, and `source_row` (one-based data-row index in its retained CSV, excluding the header). A `snapshot` event contains observed bid/ask levels and nullable best bid/ask/spread/midpoint; a `trade` event contains only that recorded trade's ID, aggressor side, price, and amount. No cumulative trade history is copied into events.

The provenance sidecar includes the retained-input SHA-256 values calculated by Python. C++ verifies its schema, source URLs, instrument, date/window, and input row counts; the preparation cache check and audit test independently recompute checksums. C++ does not implement a cryptographic hash library.

The frontend validates historical schema, exact derived metrics, per-stream row order, cross-stream ties, and precision. `dashboard/historical.js` builds snapshot/trade prefix indexes once; stepping and scrubbing never look ahead. It displays only the **latest 100 trades** and **latest 50 event details** through the selection. Chart-only sampling retains the first/last event in up to **240 buckets**, plus every missing-state boundary. All original events remain available for stepping. Dense pathological missing-state transitions may add chart points; above 1200 points, only the current-point circles are drawn. Sampling does not alter depth, metrics, trades, or replay order. The UI accepts historical exports up to 64 MB and 100000 events, with checked relative plotting ranges.

### Verified actual window

Retrieved at **2026-10-10T11:51:59.829228+00:00**. No date fallback was needed.

| Stream | Retained rows | Retained CSV bytes | First capture UTC | Last capture UTC |
| --- | ---: | ---: | --- | --- |
| `book_snapshot_5` | 2429 | 518854 | 00:00:01.370515 | 00:04:59.905464 |
| `trades` | 6357 | 520117 | 00:00:00.002133 | 00:04:59.997827 |

The session contains **8786 events** and the historical JSON is **2931382 bytes**. Only **131082 compressed bytes per HTTP response** were read before early termination, rather than retrieving either complete daily archive.

First/last original exchange timestamps (epoch microseconds): snapshots `1704067201370515` → `1704067499902000`; trades `1704067199989000` → `1704067499994000`. The first trade's exchange time is **2023-12-31T23:59:59.989000Z**, captured just after midnight. This is why the selection is explicitly described as a **capture-time window**.

Reported observations: **one snapshot exchange-timestamp regression** and **14 trades with exchange timestamps just before the capture window**. They remain in the export with original timestamps. No retained-window row was sorted, deduplicated, or excluded. No missing top-five levels or invalid prices/amounts were found in this retained window. Each stream's first boundary row was examined but excluded as outside the half-open capture window.

The audit compares **every exported snapshot level and every recorded trade** directly to its retained source CSV row, checks all timestamps and exact spread/midpoint values, recomputes retained checksums, and confirms byte-identical repeated exports from the same inputs.

### Historical tests

The tracked files in `test_fixtures/historical/` are explicitly **synthetic parser test fixtures**, not provider data. They cover initial missing book state, exact eight-place values and a nine-place midpoint, snapshot replacement, unchanged depth on trades, per-stream order and cross-file ties, quoted IDs, trailing missing levels, bad headers/rows/instruments/precision, capture regressions, input/output errors, and bounded downloader/error-response behavior.

```bash
# Offline fixture tests; no real dataset download required:
PYTHONDONTWRITEBYTECODE=1 python3 test_historical.py

# Audit the downloaded window against every source row:
PYTHONDONTWRITEBYTECODE=1 python3 test_historical.py \
  --actual .local_data/binance-BTCUSDT/2024-01-01

# Optional Node.js frontend unit checks with the actual historical export:
node test_dashboard_frontend.mjs dashboard/data/historical/btcusdt.json
```

The frontend tests exercise the actual uploaded historical session's metric/depth/tape rendering, exact UTC/USDT/BTC formatting, unavailable initial book state, trade/snapshot separation, bounded table/log sizes, playback, invalid imports, and switching back to synthetic INR mode. They use a DOM fixture and **do not constitute browser layout or console verification**. Browser automation had no accessible browser during this milestone, so historical browser verification remains manual. Open the local view, step to the first snapshot, play/scrub/restart, compare the depth and tape with source CSV rows, switch back to an INR scenario, and check the browser console.

## Historical market-making backtest

`historical_backtest` is a deterministic **simulated** Binance spot BTCUSDT
experiment. It reuses the historical CSV parser, provenance, exact price helpers,
and shared capture-time merge; historical observations never enter the synthetic
matching engine. The synthetic account and fixed-spread strategy remain unchanged.
The new baseline buys at the latest observed best bid and sells at the latest
observed best ask, with at most one active order per side.

### Build and configure

Requires GCC or Clang supporting checked signed 128-bit integers; no additional
library or backend is needed.

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic historical_backtest.cpp -o historical_backtest
g++ -std=c++17 -Wall -Wextra -Wpedantic historical_backtest_tests.cpp -o historical_backtest_tests
./historical_backtest --help
```

Required options are `--snapshots FILE --trades FILE --provenance FILE --output
FILE`, in any order. Duplicate, unknown, incomplete, or invalid options fail.
Optional parameters:

| Option | Default | Meaning |
| --- | --- | --- |
| `--order-size` | `0.001` | BTC per new order |
| `--initial-cash` | `1000` | USDT |
| `--initial-inventory` | `0.01` | BTC |
| `--maximum-inventory` | `0.02` | BTC, including fully filled pending buys |
| `--refresh-us` | `1000000` | Positive integer capture-time interval, microseconds |
| `--maker-fee-bps` | `0` | Nonnegative hypothetical maker fee in basis points |

Zero fees are a baseline assumption, **not a claim about Binance fees**. The
second demonstration uses an explicitly hypothetical **1 bp**. No parameters
were selected to manufacture fills or profit.

### Execution, reservations, and exact arithmetic

**Exact-price, trade-driven queue approximation; zero modeled order latency;
no market impact.** Each event first applies the snapshot or eligible recorded
trade to existing orders, then settles simulated fills/fees, then refreshes when
due, then records the resulting state. Capture ties use the existing convention:
snapshots before trades across streams, original row order within each stream.

Quoting starts only with a valid two-sided snapshot. Refreshes are evaluated on
arriving events; no scheduler, interpolation, or sleeping is used. New orders
reserve the configured quantity and initialize queue ahead to the displayed
quantity at that price. Unchanged quotes retain their queue and remaining
quantity, including partial fills. Changed targets cancel and replace, losing
priority. Completed orders replenish only when a refresh is due. Submission and
cancellation happen instantaneously locally after observation, without true
exchange arrival times.

Only opposite-aggressor recorded trades at the **exact same price**, with capture
time strictly after placement and exchange time strictly after the **maximum
exchange timestamp observed through placement**, can consume queue ahead. The
watermark is stored on each order. Equal-time, delayed earlier, wrong-side,
unknown-side, and different-price records are ineligible. Eligible volume first
consumes queue, then fills the remaining order, capped at its remaining quantity.
A source trade is processed once; leftover volume cannot fill a newly replenished
order. A 0.05 BTC queue and 0.01 BTC own order consume a 0.03 BTC trade without a
fill, then a 0.025 BTC trade fills 0.005 BTC. Snapshots never reduce queue ahead,
and unknown cancellations never improve priority.

After **every snapshot**, a quote outside its corresponding visible top-five
side or crossing the opposite best is cancelled with an inspectable reason.
This is an explicit conservative coverage policy, not evidence that an exchange
would have cancelled it. Recorded depth and trades remain unchanged.

There is no borrowing or short selling. Sell inventory and buy cash including
fees are reserved. Pending buys cannot exceed the inventory limit. Cash debits
and credits use only simulated filled quantity; each fill charges its fee once.
Buy reservations use the per-quantity-atom ceiling fee times remaining atoms,
covering even maximally fragmented fills. Fees above 100% are supported when
arithmetic permits: any sell fee liability exceeding proceeds is also reserved
in cash; insufficient funding suppresses that quote.

Numerical conventions:

- Prices, BTC quantities, and basis-point fees: at most **8 decimal places**;
  quantity/fee units fit a nonnegative signed 64-bit integer. Historical price
  bounds remain unchanged. Order size must be positive; balances may be zero.
- Cash settlement: **16 decimal places in USDT**. A price-times-quantity product
  is exact at this scale. Cash inputs with greater precision fail.
- Fee: `price × fill quantity × fee_bps / 10000`, **ceiling** to `10^-16 USDT`
  separately for each fill. Fee multiplication uses checked 128-bit intermediates.
- Inventory valuation: exact latest midpoint times inventory, **floor** to
  `10^-16 USDT`. Midpoints retain the historical possible ninth decimal place.
- Every wide addition and multiplication checks overflow. The symmetric supported
  bound is `[-(2^127-1), 2^127-1]`; unsupported intermediate products fail even if
  a later division might make them smaller. No binary floating-point accounting,
  silent truncation of inputs, leverage, or negative spot inventory is allowed.

Equity is cash plus inventory marked at the **latest available** midpoint. Before
the first valid midpoint, equity and P&L are unavailable. Initial equity includes
the initial BTC holding valued at that first midpoint. A later incomplete book
retains the most recent available mark. Final P&L includes fees and is **marked,
not realized**. Inventory and outstanding orders are reported without forced
liquidation.

### Tests and verified local runs

```bash
./historical_backtest_tests
# Offline CLI/export/ledger fixtures; creates an ignored synthetic frontend export:
PYTHONDONTWRITEBYTECODE=1 python3 test_historical_backtest.py
# Offline dashboard regression and backtest fill inspector:
node test_dashboard_frontend.mjs '' .local_data/backtest-fixture/result.json

# First audit the existing provider CSVs (build historical_replay as above):
PYTHONDONTWRITEBYTECODE=1 python3 test_historical.py \
  --actual .local_data/binance-BTCUSDT/2024-01-01
# Run both default and hypothetical 1-bp experiments, independent Decimal audit,
# byte-identical repeated exports, and an explicitly unfunded zero-fill test:
PYTHONDONTWRITEBYTECODE=1 python3 test_historical_backtest.py \
  --actual .local_data/binance-BTCUSDT/2024-01-01
node test_dashboard_frontend.mjs dashboard/data/historical/btcusdt.json \
  .local_data/backtest-fixture/result.json dashboard/data/backtest
```

Tiny tracked `test_fixtures/backtest/` CSVs are **synthetic**, not provider data.
C++ tests cover the queue example, partials, volume caps, duplicate records,
ineligible aggressors/prices/timestamps, watermark regressions, snapshot
non-consumption, coverage/crossing cancellation, replacement priority, unchanged
partial quotes, scheduled replenishment, event-first handling of leftover volume,
spot reserves/fees including extreme fees, nonzero initial holdings, rounding,
precision, overflow, and pre-placement/post-cancellation non-fills. Prefix runs
produce the same event states and fills as the corresponding prefix of a full
run, excluding summary data. Python independently audits fee/cash/inventory/
valuation and source checksums, tests errors and output preservation, and checks
deterministic repeated exports. Frontend tests validate ledgers and confirm
unavailable values, queue explanations, zero-fill results, backward stepping,
no future fills, and switching among all three modes.

The existing engine, simulator, market-maker, CLI replay, simulator CLI, synthetic
export, historical parser/audit, and frontend regression suites passed. Browser
automation returned no available browsers: **visual layout and browser-console
checks remain manual**, and DOM tests do not substitute for them.

Verified on the retained **2024-01-01 00:00–00:05 UTC capture window**, with 2429
snapshots and 6357 recorded trades, using the default parameters above:

| Result | Zero-fee baseline | Hypothetical 1 bp |
| --- | ---: | ---: |
| Initial marked equity · USDT | 1422.83585 | 1422.83585 |
| Orders placed | 118 (80 buys / 38 sells) | 118 (80 buys / 38 sells) |
| Cancellations | 104 | 104 |
| Simulated fills | 14 (2 buys / 12 sells) | 14 (2 buys / 12 sells) |
| Simulated volume · BTC | 0.01369 | 0.01369 |
| Final inventory · BTC | 0.00031 | 0.00031 |
| Fees · USDT | 0 | 0.0579234276 |
| Final cash · USDT | 1409.819596 | 1409.7616725724 |
| Final equity · USDT | 1422.96273575 | 1422.9048123224 |
| Final marked P&L · USDT | 0.12688575 | 0.0689623224 |

Both runs: buy volume 0.002 BTC, sell volume 0.01169 BTC, 13 completed orders,
283 scheduled refreshes, 310 unchanged-quote retentions, and 138 sell risk skips.
Cancellations: 57 outside visible depth, 36 crossing the opposite best, and 11
scheduled target changes. Queue diagnostics count **trade/order attempts**, not
unique trades: 2333 eligible, 558 different-price, 4497 wrong-aggressor, and 10
exchange-watermark-ineligible attempts; 918 recorded trades had no active orders.
Eligible trades consumed 63.02909 BTC of estimated queue across quote lifetimes.
The final remaining buy is 0.001 BTC at 42397.22 USDT/BTC with 9.75209 BTC queue
estimated ahead. Final reserved cash is 42.39722 USDT at zero fees and
42.401459722 USDT at 1 bp; there is no active sell. The unfunded diagnostic run
has zero orders/fills, zero equity/P&L, and valid playback.

This five-minute exercise is an **integration experiment, not strategy
validation**. Top-five snapshots cannot reveal individual-order priority;
placements assume displayed volume ahead, cancellations do not improve it, and
coverage cancellations discard quotes conservatively. Exact-price rules can
underestimate fills and miss trade-through executions. True latency, market
impact, exchange fee tiers, adverse-selection calibration, and liquidation costs
are absent. Estimated strategy fills are never actual historical executions.

### Generate and inspect playback

The Python integration command generates local exports automatically. Equivalent
explicit commands (after preparing the existing local CSVs):

```bash
mkdir -p dashboard/data/backtest
./historical_backtest \
  --snapshots .local_data/binance-BTCUSDT/2024-01-01/book_snapshot_5.csv \
  --trades .local_data/binance-BTCUSDT/2024-01-01/trades.csv \
  --provenance .local_data/binance-BTCUSDT/2024-01-01/provenance.csv \
  --output dashboard/data/backtest/btcusdt-zero.json
./historical_backtest \
  --snapshots .local_data/binance-BTCUSDT/2024-01-01/book_snapshot_5.csv \
  --trades .local_data/binance-BTCUSDT/2024-01-01/trades.csv \
  --provenance .local_data/binance-BTCUSDT/2024-01-01/provenance.csv \
  --maker-fee-bps 1 --output dashboard/data/backtest/btcusdt-1bp.json
python3 -m http.server 8000 --bind 127.0.0.1 --directory dashboard
```

Open **http://localhost:8000** and select **Historical backtest · zero fees
(local)** or **Historical backtest · hypothetical 1 bp (local)**. Upload is also
supported. Step, play, scrub, and restart using the shared controls. The observed
price lines have separate legends from orange simulated buys and purple simulated
sells. The recorded trade tape remains separate from simulated fills. Expand a
simulated fill to inspect source trade/event, placement and exchange watermark,
queue before/after, quantity cap, fees, and account changes. Cash, inventory,
reserves, equity, fees, P&L, orders, decisions, and inspectors use only the selected
prefix. Parameters and execution assumptions appear above the portfolio and in
export metadata. Final summary statistics are not exposed as current values.

The distinct schema is `historical-backtest` version 1. `market` embeds the
unchanged historical schema and checksum references, storing each market trade
once. `states` aligns one-to-one with market events; `fills` and `lifecycle` store
simulated fills and placement/cancellation/completion records once, referenced by
string event indexes/order IDs. Each fill includes its source trade ID, capture
and exchange timestamps, pre-fill order, queue calculation, price, fee, and signed
cash/inventory changes. `parameters`, `conventions`, and `assumptions` document
the run; `summary` includes counters, initial equity, volume, and final marked
state. All monetary values, quantities, large IDs, and timestamps are strings;
null valuations mean unavailable. The backtest supports at most 100000 events.
Charts sample ordinary history but preserve quote transitions and missing-book
boundaries; all events remain step-accessible. The latest 100 simulated fill
inspectors are rendered. No cumulative histories are repeated inside states.

Provider files, derived exports under `dashboard/data/backtest/`, and executables
remain ignored/local. The executable carries the existing checksum references;
the preparation cache and independent audit recompute them. No cryptographic
hash implementation was duplicated in C++. Invalid configuration, precision,
overflow, or input is rejected before opening output, and input aliases and
output I/O failures return nonzero status. No commit, push, or deployment is part
of this change.

## Milestone A: controlled execution experiment

This standalone, single-threaded C++17 experiment uses the existing `OrderBook`
as a **controlled exchange for one fictional instrument**, named `WHOLE`.
Prices and cash are integer **paise**, quantities are **whole units**, and
simulated time is integer **microseconds**. It is not a BTC experiment and does
not consume historical data. Fees are explicitly zero.

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic execution_demo.cpp -o execution_demo
g++ -std=c++17 -Wall -Wextra -Wpedantic execution_tests.cpp -o execution_tests
./execution_tests
./execution_demo             # both deterministic demonstrations
./execution_demo immediate   # purchase only
./execution_demo race        # cancellation race only
```

`execution/experiment.h` contains the compact vertical slice:

- A validated `Task`: side, positive target quantity and limit, start, and a
  strictly later deadline. `immediatePolicy` receives a delivered event kind and
  a value-only local order view. It submits one bounded-limit order at start,
  requests cancellation of an outstanding order at deadline, and never resubmits.
  It has no book, scheduler, script, or exchange-truth access. There are no market
  observations in this milestone because this policy needs only delivered timers
  and order reports.
- `Scheduler`: a priority queue ordered by `(delivery timestamp, insertion
  sequence)`. Newly created same-time events go behind already queued peers.
  Time never sleeps; past scheduling, negative time/delays, timestamp addition
  overflow, and sequence exhaustion fail. External scripts are queued in caller
  registration order before start/deadline timers. Strategy requests and responses
  generated by those events join the same queue.
- `Exchange`: the only component that mutates its private `OrderBook`. External
  and strategy requests share an ID allocator; execution IDs identify actual
  returned engine trades. Each submission reports acceptance **before individual
  fills** at that occurrence time. Acceptance quantity is submitted quantity,
  never accounted as execution. Both sides of returned trades receive reports,
  so later external orders correctly fill resting strategy orders. Validation
  failures produce rejections; cancellations report the removed remainder or an
  explicit failure reason. Unexpected failures stop the experiment permanently;
  engine mutation is not assumed transactional.
- `OrderManager`: tracks request details, submission, acceptance knowledge,
  executed/remaining quantities, cancellation requested/pending, cancelled
  quantity, and terminal outcome. Cancellation pending coexists with fills; a
  request itself never cancels an order. A cancellation rejection after a complete
  fill cannot reopen it. The local account changes only when a fill report is
  delivered. Identical execution duplicates are ignored; conflicting duplicates,
  overfills, limit violations, negative balances, and terminal-state violations
  fail clearly.
- `Experiment`: connects policy actions to delayed exchange requests and delayed
  reports, logs delivery, drains scheduled events, and requires a settled terminal
  outcome and matching exchange/local completion. Request and response delays are
  independently configurable nonnegative microseconds through its constructor.

Task buying power is checked against **limit price × target quantity**; sells
require inventory. All accounting multiplication/addition is checked. Adapter
prices and individual quantities are bounded at `1,000,000,000` for this fictional
instrument without changing the engine's public API. Buys must leave representable
inventory. Sells reserve numerical headroom for proceeds at the adapter's maximum
possible price, so a task can be rejected conservatively for representability.
Negative accounts are unsupported. Funding/precision/bound failures are local
order rejections before sending a strategy request; malformed task/time/account
configuration throws before a run. Scripted counterparties are controlled
liquidity, not separately funded portfolios.

The deadline requests cancellation using the locally delivered state, even when
the original acknowledgment has not arrived. The same fixed request delay for
submission and cancellation preserves submission-before-cancellation arrival.
Cancellation may lose a race. Completion **by deadline** uses exchange execution
time `<= deadline`, regardless of report delivery time; later occurrences are
reported separately. No end-of-loop liquidation or implicit cancellation occurs.
An unresolved order, pending cancellation, or exchange/local mismatch fails
instead of producing a successful summary.

Trace rows contain a monotonic `trace_seq`, scheduler insertion `event_seq`,
exchange occurrence time (`NA` for local timers/requests), delivery time, event
type, order/execution ID, price, quantity, and applicable local state/account.
Scheduler sequence values need not increase in delivery order across timestamps;
equal-time delivery does follow their order. Exchange executions are available
separately from locally observed balances. Average execution price is printed as
an exact `notional/completed` ratio and an explicitly **floor-rounded display in
paise**; zero fills display unavailable, with no division by zero.

Hand-checkable demonstrations:

| Result | A: bounded-limit purchase | B: cancellation race |
| --- | ---: | ---: |
| Target | 5 buys at limit 105 paise | 3 buys at limit 100 paise |
| Request / response delay | 5 / 7 µs | 10 / 25 µs |
| Deadline | 100 µs | 30 µs |
| Engine executions | 2 at 100, 2 at 101, 1 at 103 | 3 at resting bid 100 |
| Exchange execution time | 15 µs | 35 µs |
| Local fill delivery | 22 µs | 60 µs |
| Completed by deadline / late | 5 / 0 | 0 / 3 |
| Final completed / uncompleted | 5 / 0 | 3 / 0 |
| Final cash / inventory | 495 paise / 5 | 700 paise / 3 |
| Execution notional | 505 paise | 300 paise |
| Average execution price | 101 paise | 100 paise |

Both start with 1000 paise and zero inventory. A seeds several genuine resting
sell orders; best-price priority and a partial resting fill determine the result.
B submits at 10 µs, arrives at 20 µs, requests cancellation at 30 µs before its
acknowledgment is delivered at 45 µs, and fills against an external sell at 35 µs.
Cancellation reaches the exchange at 40 µs and fails because the order is fully
filled. Its rejection is delivered at 65 µs; the local filled state remains final.

The eight focused execution test groups cover best prices/partials, both
initiating sides, report-only accounting, resting remainder cancellation, full
and partial cancellation races, after-cancellation non-fills, deadline before
submission arrival/acknowledgment, pre-deadline execution reported late,
post-deadline and equal-deadline classification, buy/sell validation and overflow,
external cancellation/rejection, duplicate/conflicting fills, equal-time and
zero-delay scheduling, invalid configuration/time arithmetic, and byte-identical
repeated traces. Existing engine, simulator, CLI, historical, backtest, and
frontend tests remain the regression checks documented above.

This is one task, one strategy order/account, a FIFO/price-priority controlled
engine, and fixed deterministic delays. It models neither real-venue fidelity,
network jitter, exchange-specific protocols, fees, general portfolio reservations,
nor historical precision. No threads, services, frontend schema, TWAP, live
adapter, historical integration, or additional policy is introduced. Executables
are ignored. Previous historical/backtest/dashboard edits and unrelated artifacts
are preserved; this milestone does not commit or publish them.
