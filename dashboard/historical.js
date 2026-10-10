import { safeOffset } from './schema.js';

const MAX_I64 = (1n << 63n) - 1n, MAX_U64 = (1n << 64n) - 1n;
function check(ok, message) { if (!ok) throw new Error(message); }
function object(value, name) { check(value && typeof value === 'object' && !Array.isArray(value), `${name} must be an object.`); }
function text(value, name, empty = false) { check(typeof value === 'string' && (empty || value.length) && value.length <= 2000, `${name} must be text.`); }
function integer(value, name) {
  check(typeof value === 'string' && /^(0|[1-9][0-9]*)$/.test(value) && value.length <= 20, `${name} must be a decimal integer string.`);
  const number = BigInt(value); check(number <= MAX_U64, `${name} overflow.`); return number;
}
export function decimalUnits(value, places = 8, positive = true) {
  check(typeof value === 'string' && value.length <= 40 && /^(0|[1-9][0-9]*)(\.[0-9]+)?$/.test(value), 'Invalid exact market decimal.');
  const [whole, fraction = ''] = value.split('.');
  check(fraction.length <= places, `Market decimal exceeds ${places} places.`);
  const number = BigInt(whole) * 10n ** BigInt(places) + BigInt(fraction.padEnd(places, '0'));
  check(!positive || number > 0n, 'Market prices and quantities must be positive.');
  return number;
}
export function marketPrice(value) {
  let number = BigInt(value);
  const sign = number < 0n ? '−' : '';
  if (number < 0n) number = -number;
  const whole = number / 1000000000n;
  let fraction = (number % 1000000000n).toString().padStart(9, '0').replace(/0+$/, '');
  fraction = fraction.padEnd(2, '0');
  return `${sign}${whole.toLocaleString('en-US')}.${fraction} USDT`;
}
export function utc(value) {
  const micros = BigInt(value), millis = micros / 1000n;
  check(micros >= 0n && micros <= 253402300799999999n, 'Timestamp exceeds supported UTC date range (1970–9999).');
  return new Date(Number(millis)).toISOString().slice(0, 19) + '.' + (micros % 1000000n).toString().padStart(6, '0') + 'Z';
}
export function metric(book, field) {
  return book?.[field] === null || !book ? null : decimalUnits(book[field], 9, field !== 'spread').toString();
}
export function validateHistorical(data) {
  object(data, 'Historical export');
  check(data.schema === 'historical-market' && data.schema_version === 1, 'Unsupported historical schema/version.');
  object(data.instrument, 'Instrument');
  check(data.instrument.exchange === 'binance' && data.instrument.symbol === 'BTCUSDT' && data.instrument.market === 'spot' && data.instrument.base === 'BTC' && data.instrument.quote === 'USDT', 'Expected Binance spot BTCUSDT instrument metadata.');
  object(data.units, 'Units');
  check(data.units.price === 'USDT per BTC' && data.units.quantity === 'BTC' && data.units.time === 'UTC epoch microseconds', 'Historical units must be USDT per BTC, BTC, and UTC epoch microseconds.');
  text(data.description, 'Description'); text(data.ordering, 'Ordering convention');
  object(data.provenance, 'Provenance'); const p = data.provenance;
  check(p.provider === 'Tardis', 'Historical data source is missing or unsupported.');
  for (const field of ['date', 'retrieved_at_utc', 'snapshot_url', 'trade_url']) text(p[field], field);
  check(/^\d{4}-\d{2}-01$/.test(p.date), 'Expected a first-of-month source date.');
  for (const field of ['snapshot_sha256', 'trade_sha256']) check(typeof p[field] === 'string' && /^[a-f0-9]{64}$/.test(p[field]), 'Missing retained-input SHA-256.');
  const start = integer(p.window_start_us, 'Window start'), end = integer(p.window_end_us, 'Window end');
  check(end === start + 300000000n && utc(start).startsWith(p.date + 'T00:00:00'), 'Expected initial five-minute UTC capture window.');
  const expectedSnapshots = integer(p.snapshot_rows, 'Snapshot count'), expectedTrades = integer(p.trade_rows, 'Trade count');
  check(Array.isArray(data.anomalies) && data.anomalies.length <= 100, 'Anomalies must be reported as an array.');
  data.anomalies.forEach((entry) => text(entry, 'Anomaly'));
  check(Array.isArray(data.events) && data.events.length > 0 && data.events.length <= 100000, 'Historical export must contain 1–100000 events.');
  let previousIndex = -1n, previousTime = -1n, previousSnapshotRow = 0n, previousTradeRow = 0n, previousType = null;
  let snapshots = 0n, trades = 0n, priceOrigin = null;
  const price = (value) => {
    const number = decimalUnits(value); check(number <= MAX_I64 / 2n, 'Historical price overflow.');
    if (priceOrigin === null) priceOrigin = number * 10n;
    safeOffset(number * 10n, priceOrigin); return number;
  };
  data.events.forEach((event) => {
    object(event, 'Historical event');
    const index = integer(event.event_index, 'Event index'), time = integer(event.timestamp_us, 'Capture timestamp');
    integer(event.exchange_timestamp_us, 'Exchange timestamp'); utc(event.exchange_timestamp_us);
    const row = integer(event.source_row, 'Source row');
    check(index > previousIndex && time >= previousTime && time >= start && time < end, 'Unexpected historical ordering/window.');
    check(!(time === previousTime && previousType === 'trade' && event.type === 'snapshot'), 'Cross-file tie must place snapshots before trades.');
    previousIndex = index; previousTime = time; previousType = event.type;
    safeOffset(event.timestamp_us, data.events[0].timestamp_us);
    if (event.type === 'snapshot') {
      ++snapshots; check(row > previousSnapshotRow && !('trade' in event), 'Snapshot source rows must retain input order.'); previousSnapshotRow = row;
      object(event.snapshot, 'Snapshot'); const book = event.snapshot;
      for (const side of ['bids', 'asks']) {
        check(Array.isArray(book[side]) && book[side].length <= 5, 'Expected at most five displayed levels per side.');
        let previous = null;
        for (const level of book[side]) {
          object(level, 'Depth level'); const value = price(level.price);
          check(decimalUnits(level.amount) <= MAX_I64, 'BTC amount overflow.');
          check(previous === null || (side === 'bids' ? value < previous : value > previous), 'Unsorted historical depth levels.'); previous = value;
        }
        const best = side === 'bids' ? 'best_bid' : 'best_ask';
        check(book[best] === (book[side].length ? book[side][0].price : null), 'Best price must match displayed depth.');
      }
      if (book.bids.length && book.asks.length) {
        const bid = decimalUnits(book.best_bid), ask = decimalUnits(book.best_ask);
        check(bid < ask, 'Crossed/locked historical snapshot.');
        check(decimalUnits(book.spread) === ask - bid && decimalUnits(book.midpoint, 9) === (ask + bid) * 5n, 'Inconsistent spread/midpoint.');
      } else check(book.spread === null && book.midpoint === null, 'Missing book sides must leave metrics unavailable.');
    } else {
      check(event.type === 'trade' && !('snapshot' in event), 'Unknown historical event type.');
      ++trades; check(row > previousTradeRow, 'Trade source rows must retain input order.'); previousTradeRow = row;
      object(event.trade, 'Recorded trade'); text(event.trade.id, 'Recorded trade ID', true);
      check(['buy', 'sell', 'unknown'].includes(event.trade.side), 'Unknown aggressor side.');
      price(event.trade.price); check(decimalUnits(event.trade.amount) <= MAX_I64, 'Trade BTC amount overflow.');
    }
  });
  check(snapshots === expectedSnapshots && trades === expectedTrades, 'Event counts disagree with provenance.');
  return data;
}

// One pass at load time. Snapshot references are shared, not copied per trade.
// Prefix indexes make random-access stepping and bounded tape rendering cheap.
export function indexHistorical(data) {
  const books = new Int32Array(data.events.length), tradePositions = [];
  const gapPositions = [];
  let previousAvailability = null;
  const missing = new Int32Array(data.events.length + 1);
  let book = -1;
  data.events.forEach((event, i) => {
    if (event.type === 'snapshot') book = i;
    else tradePositions.push(i);
    books[i] = book;
    const state = book < 0 ? null : data.events[book].snapshot;
    missing[i + 1] = missing[i] + (!state || state.midpoint === null ? 1 : 0);
    const availability = ['best_bid', 'best_ask', 'midpoint', 'spread'].map((field) => !!state && state[field] !== null).join(',');
    if (previousAvailability !== null && availability !== previousAvailability) gapPositions.push(i - 1, i);
    previousAvailability = availability;
  });
  return { books, tradePositions, missing, gapPositions };
}
export function bookAt(data, index, position) {
  const source = index.books[position];
  return source < 0 ? null : data.events[source].snapshot;
}
export function prefixCount(positions, selected) {
  let low = 0, high = positions.length;
  while (low < high) { const mid = Math.floor((low + high) / 2); if (positions[mid] <= selected) low = mid + 1; else high = mid; }
  return low;
}
// Visual-only bucket sampling: first/last per bucket, plus every missing-state
// boundary (both sides). Never bridge a gap. All events remain in playback.
export function historicalPlot(data, index, selected) {
  const bucket = Math.max(1, Math.ceil((selected + 1) / 240)), positions = new Set([0, selected]);
  for (let first = 0; first <= selected; first += bucket) {
    positions.add(first); positions.add(Math.min(selected, first + bucket - 1));
  }
  for (let i = 0, count = prefixCount(index.gapPositions, selected); i < count; ++i) positions.add(index.gapPositions[i]);
  return [...positions].sort((a, b) => a - b).map((position) => ({
    ...data.events[position], book: bookAt(data, index, position)
  }));
}
