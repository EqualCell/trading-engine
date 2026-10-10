// Dependency-free unit checks. The DOM fixture tests app logic, not browser layout.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { validateScenario, money, safeOffset } from './dashboard/schema.js';

const root = new URL('./', import.meta.url);
const load = async (name) => JSON.parse(await readFile(new URL(`dashboard/data/${name}.json`, root), 'utf8'));
for (const name of ['spread-capture', 'inventory-risk', 'inventory-limit']) validateScenario(await load(name));
assert.equal(money('-200'), '−₹2.00');
assert.equal(money('200', true), '+₹2.00');
assert.equal(money('900719925474099301'), '₹9,00,71,99,25,47,40,993.01');
assert.equal(safeOffset('18446744073709551615', '18446744073709551614'), 1);
assert.throws(() => safeOffset('9007199254740992', '0'), /precision/);
const valid = await load('inventory-risk');
for (const mutate of [
  (d) => { d.schema_version = 2; },
  (d) => { d.events[1].timestamp_us = '-1'; },
  (d) => { d.events[1].event_index = d.events[0].event_index; },
  (d) => { d.events[0].cash_paise = 100000; },
  (d) => { d.events[0].cash_paise = '1.5'; },
  (d) => { d.events[0].total_pnl_paise = '1'; },
  (d) => { delete d.events[0].maker_bid; },
  (d) => { d.events[0].maker_bid.order_id = '18446744073709551616'; },
  (d) => { d.events[1].timestamp_us = '9007199254740992'; },
  (d) => { d.initial_accounts[1].trader_id = '1'; },
  (d) => { d.events = []; },
]) {
  const bad = structuredClone(valid); mutate(bad);
  assert.throws(() => validateScenario(bad));
}

// Test real control/render code with a minimal in-memory DOM fixture.
class Element {
  constructor(tag = 'div') {
    this.tagName = tag; this.children = []; this.attributes = {}; this.listeners = {};
    this.value = ''; this._text = ''; this.disabled = false; this.hidden = false;
    this.clientWidth = 600; this.options = []; this.className = ''; this.files = [];
    this.classList = { contains: (name) => this.className.split(' ').includes(name) };
  }
  set textContent(text) { this._text = String(text); this.children = []; }
  get textContent() { return this._text + this.children.map((child) => child.textContent).join(''); }
  setAttribute(name, value) { this.attributes[name] = String(value); }
  append(...nodes) { this.children.push(...nodes); }
  replaceChildren(...nodes) { this._text = ''; this.children = nodes; }
  add(option) { this.options.push(option); }
  addEventListener(name, handler) { this.listeners[name] = handler; }
  async fire(name) { return this.listeners[name]?.({ target: this }); }
}
const html = await readFile(new URL('dashboard/index.html', root), 'utf8');
const elements = new Map([...html.matchAll(/id="([^"]+)"/g)].map((match) => [match[1], new Element()]));
const el = (id) => { assert(elements.has(id), `Unknown HTML ID ${id}`); return elements.get(id); };
el('speed').value = '1'; el('scenario').value = 'inventory-risk';
el('scenario').options = ['inventory-risk', 'spread-capture', 'inventory-limit'].map((value) => ({ value }));
el('inventory-chart').className = 'chart small'; el('pnl-chart').className = 'chart small';
globalThis.document = { getElementById: el, createElement: (tag) => new Element(tag), createElementNS: (_, tag) => new Element(tag) };
globalThis.window = { addEventListener: () => {} };
globalThis.Option = class extends Element { constructor(text, value) { super('option'); this.textContent = text; this.value = value; } };
globalThis.fetch = async (path) => ({ ok: true, text: async () => readFile(new URL(`dashboard/${path}`, root), 'utf8') });
let timerId = 0;
const timers = new Map();
globalThis.setInterval = (fn) => { timers.set(++timerId, fn); return timerId; };
globalThis.clearInterval = (id) => { timers.delete(id); };
const settle = async () => {
  // Wait for the asynchronous local file read, with a bounded test timeout.
  for (let i = 0; i < 100 && el('status').textContent.startsWith('Loading'); ++i)
    await new Promise((resolve) => setTimeout(resolve, 5));
  assert(!el('status').textContent.startsWith('Loading'), 'Scenario load timed out');
};
await import('./dashboard/app.js');
await settle();
assert.equal(el('event-count').textContent, '1 / 6');
assert.equal(el('reference').textContent, '₹100.00');
assert.equal(el('trade-count').textContent, '0 FILLS');
assert.equal(el('event-log').children.length, 1);
await el('next').fire('click');
assert.equal(el('cash').textContent, '₹901.00');
assert.equal(el('inventory').textContent, '1 units');
assert.equal(el('bid').textContent, 'No active bid');
assert.equal(el('trades').children.length, 1);
await el('previous').fire('click');
assert.equal(el('trade-count').textContent, '0 FILLS');
el('timeline').value = '5'; await el('timeline').fire('input');
assert.equal(el('pnl').textContent, '−₹2.00');
assert.equal(el('cash').textContent, '₹903.00');
assert.equal(el('value').textContent, '₹998.00');
assert.equal(el('trades').children.length, 3);
assert.equal(el('event-log').children.length, 6);
const pricePaths = el('price-chart').children[0].children.filter((child) => child.tagName === 'path');
assert.equal((pricePaths[1].attributes.d.match(/M/g) || []).length, 3, 'Inactive bid intervals must split paths');
await el('restart').fire('click');
assert.equal(el('event-count').textContent, '1 / 6');
assert.equal(el('trades').children.length, 0);
await el('play').fire('click');
assert.equal(el('play').textContent, 'Ⅱ Pause');
[...timers.values()][0]();
assert.equal(el('event-count').textContent, '2 / 6');
await el('play').fire('click');
assert.equal(timers.size, 0);
for (const [name, count] of [['spread-capture', 3], ['inventory-limit', 11], ['inventory-risk', 6]]) {
  el('scenario').value = name; await el('scenario').fire('change'); await settle();
  assert.equal(el('event-count').textContent, `1 / ${count}`);
  el('timeline').value = String(count - 1); await el('timeline').fire('input');
  if (name === 'spread-capture') assert.equal(el('pnl').textContent, '+₹2.00');
  if (name === 'inventory-limit') {
    assert.equal(el('inventory').textContent, '5 units');
    assert.equal(el('bid').textContent, 'No active bid');
    assert.match(el('ask').textContent, /ASK ₹101.00/);
  }
}
el('upload').files = [{ size: 2, text: async () => '{}' }];
await el('upload').fire('change');
assert.equal(el('error').hidden, false);
assert.match(el('error').textContent, /Invalid export.*schema version/);
assert.equal(el('pnl').textContent, '−₹2.00', 'Invalid import must preserve previous data');
const imported = structuredClone(valid); imported.description = '<img src=x onerror=alert(1)>';
el('upload').files = [{ size: 2000, text: async () => JSON.stringify(imported) }];
await el('upload').fire('change');
assert.equal(el('description').textContent, imported.description);
assert.equal(el('description').children.length, 0, 'Imported text must not be parsed as HTML');
assert.equal(el('scenario').value, 'imported');
el('scenario').value = 'spread-capture'; await el('scenario').fire('change'); await settle();
el('scenario').value = 'imported'; await el('scenario').fire('change');
assert.equal(el('description').textContent, imported.description, 'Switching back must restore uploaded data');
console.log('PASS: frontend schema, exact formatting, controls, synchronized panels, quote gaps, and invalid imports (DOM fixture; not browser QA)');

// Optional actual historical-session checks. The ignored dataset is never needed
// for normal synthetic-mode unit tests; pass its export path after downloading.
if (process.argv[2]) {
  const { validateHistorical, indexHistorical, bookAt, prefixCount, historicalPlot, metric, marketPrice, utc } = await import('./dashboard/historical.js');
  const data = JSON.parse(await readFile(process.argv[2], 'utf8'));
  validateHistorical(data);
  const index = indexHistorical(data), firstSnapshot = data.events.findIndex((event) => event.type === 'snapshot');
  assert(firstSnapshot > 0, 'Actual window must have trades before the first snapshot');
  assert.equal(bookAt(data, index, 0), null, 'No future book at first trade');
  for (let i = 1; i < data.events.length; ++i) if (data.events[i].type === 'trade')
    assert.equal(bookAt(data, index, i), bookAt(data, index, i - 1), 'Trade must not mutate observed depth');
  assert.equal(utc('1704067200002133'), '2024-01-01T00:00:00.002133Z');
  assert.equal(marketPrice('100000000005'), '100.000000005 USDT');
  assert.equal(prefixCount(index.tradePositions, 0), 1);
  const plot = historicalPlot(data, index, data.events.length - 1);
  assert(plot.length < 600, 'Actual chart should be sampled, without dropping playback events');
  assert(plot[0].book === null, 'Initial unavailable depth must remain a chart gap');
  for (const mutate of [
    (d) => { d.schema_version = 2; },
    (d) => { d.instrument.market = 'futures'; },
    (d) => { d.units.price = 'USD'; },
    (d) => { d.events[0].trade.amount = '0.000000001'; },
    (d) => { d.events[0].trade.price = 42283.59; },
    (d) => { d.events[0].timestamp_us = '1704067199999999'; },
    (d) => { d.events[firstSnapshot].snapshot.midpoint = '1'; },
  ]) { const bad = structuredClone(data); mutate(bad); assert.throws(() => validateHistorical(bad)); }
  el('upload').files = [{ size: 1, text: async () => JSON.stringify(data) }];
  await el('upload').fire('change');
  assert.equal(el('synthetic-metrics').hidden, true);
  assert.equal(el('historical-metrics').hidden, false);
  assert.equal(el('historical-notes').hidden, false);
  assert.match(el('historical-notes').textContent, /14 exchange timestamps/);
  assert.equal(el('historical-bid').textContent, 'Unavailable');
  assert.equal(el('historical-depth').children.length, 0);
  assert.equal(el('historical-trades').children.length, 1);
  assert.equal(el('historical-time').textContent, utc(data.events[0].timestamp_us));
  el('timeline').value = String(firstSnapshot); await el('timeline').fire('input');
  assert.equal(el('historical-depth').children.length, 5);
  const source = data.events[firstSnapshot].snapshot;
  assert.equal(el('historical-bid').textContent, marketPrice(metric(source, 'best_bid')));
  assert.equal(el('historical-depth').children[0].children[1].textContent, source.bids[0].amount);
  const tapeBefore = prefixCount(index.tradePositions, firstSnapshot);
  assert.equal(el('historical-trades').children.length, Math.min(100, tapeBefore));
  el('timeline').value = String(data.events.length - 1); await el('timeline').fire('input');
  assert.equal(el('historical-trades').children.length, 100, 'Tape DOM must remain bounded');
  assert.equal(el('event-log').children.length, 50, 'Event log DOM must remain bounded');
  assert.equal(el('historical-depth').children.length, 5);
  assert.match(el('trade-count').textContent, /^6357 RECORDED/);
  assert(!el('historical-spread').textContent.includes('₹'), 'Wrong historical units');
  assert.equal(el('synthetic-charts').hidden, true);
  assert.equal(el('historical-charts').hidden, false);
  await el('restart').fire('click');
  assert.equal(el('historical-depth').children.length, 0);
  await el('play').fire('click'); [...timers.values()][0](); await el('play').fire('click');
  assert.equal(el('event-count').textContent, `2 / ${data.events.length}`);
  el('scenario').value = 'inventory-risk'; await el('scenario').fire('change'); await settle();
  assert.equal(el('historical-metrics').hidden, true);
  assert.equal(el('synthetic-metrics').hidden, false);
  assert.equal(el('reference').textContent, '₹100.00');
  el('scenario').value = 'imported'; await el('scenario').fire('change');
  assert.equal(el('historical-metrics').hidden, false);
  el('upload').files = [{ size: 1, text: async () => '{"schema":"historical-market","schema_version":99}' }];
  await el('upload').fire('change'); assert.match(el('error').textContent, /Unsupported historical/);
  console.log('PASS: actual historical schema, no look-ahead, snapshot persistence, UTC/USDT/BTC formatting, playback, bounded tape/log, and synthetic switching (DOM fixture)');
}
