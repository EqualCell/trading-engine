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
