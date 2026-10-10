#!/usr/bin/env python3
"""Dependency-free checks against exports produced by the actual C++ runner."""
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
DEMO = ROOT / 'market_maker_demo'
NAMES = ('spread-capture', 'inventory-risk', 'inventory-limit')


def run(*args):
    return subprocess.run([str(DEMO), *map(str, args)], capture_output=True, text=True)


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def validate(data):
    check(data['schema_version'] == 1, 'schema version')
    check(data['scenario'] in NAMES and data['description'], 'scenario metadata')
    check(data['units'] == {'money': 'paise', 'quantity': 'units', 'time': 'microseconds'}, 'units')
    check(data['assumptions'] and data['strategy'] and len(data['initial_accounts']) == 2, 'configuration')
    events = data['events']
    check(events, 'empty snapshots')
    previous_index = previous_time = -1
    maker_ids = set()
    trade_pairs = []
    for event in events:
        for field in ('event_index', 'timestamp_us', 'reference_price_paise', 'cash_paise',
                      'inventory', 'portfolio_value_paise', 'total_pnl_paise'):
            check(isinstance(event[field], str), f'{field} not a precision-preserving string')
            int(event[field])
        index, time = int(event['event_index']), int(event['timestamp_us'])
        check(index > previous_index and time >= previous_time, 'event order')
        previous_index, previous_time = index, time
        check(event['type'] and event['description'], 'event labels')
        cash, inventory, reference = map(int, (event['cash_paise'], event['inventory'], event['reference_price_paise']))
        check(int(event['portfolio_value_paise']) == cash + inventory * reference, 'valuation')
        check(int(event['total_pnl_paise']) == cash + inventory * reference - int(data['initial_portfolio_value_paise']), 'P&L')
        check(abs(inventory) <= int(data['strategy']['inventory_limit']), 'inventory bound')
        for side in ('maker_bid', 'maker_ask'):
            quote = event[side]
            if quote is not None:
                check(set(quote) == {'order_id', 'price_paise', 'remaining_quantity'}, 'quote fields')
                check(all(isinstance(value, str) for value in quote.values()), 'quote integer encoding')
                check(int(quote['remaining_quantity']) == 1 and int(quote['price_paise']) > 0, 'active quote')
                maker_ids.add(quote['order_id'])
        for trade in event['trades']:
            check(set(trade) == {'buy_id', 'sell_id', 'price_paise', 'quantity'}, 'trade schema')
            check(all(isinstance(value, str) for value in trade.values()), 'trade integer encoding')
            check(all(int(value) > 0 for value in trade.values()), 'trade fields')
            trade_pairs.append((trade['buy_id'], trade['sell_id']))
    check(len(trade_pairs) == len(set(trade_pairs)), 'duplicate one-unit trades')
    # Independent accounting reconciliation proves tape and snapshots agree.
    cash, inventory = 100000, 0
    filled_ids = set()
    for event in events:
        for trade in event['trades']:
            quantity, price = int(trade['quantity']), int(trade['price_paise'])
            if trade['buy_id'] in maker_ids:
                cash -= price * quantity; inventory += quantity; filled_ids.add(trade['buy_id'])
            elif trade['sell_id'] in maker_ids:
                cash += price * quantity; inventory -= quantity; filled_ids.add(trade['sell_id'])
            else:
                raise AssertionError('trade not attributable to maker quote')
        check((cash, inventory) == (int(event['cash_paise']), int(event['inventory'])), 'trade/account reconciliation')
        for side in ('maker_bid', 'maker_ask'):
            check(event[side] is None or event[side]['order_id'] not in filled_ids, 'filled quote still active')
    return events


def main():
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        exported = {}
        for name in NAMES:
            first, second = folder / f'{name}.json', folder / f'{name}-again.json'
            for path in (first, second):
                result = run('--scenario', name, '--output', path)
                check(result.returncode == 0 and not result.stderr and 'Exported' in result.stdout, 'export failure')
            check(first.read_bytes() == second.read_bytes(), 'export not deterministic')
            check(first.read_bytes() == (ROOT / 'dashboard/data' / f'{name}.json').read_bytes(), 'bundled export stale')
            data = json.loads(first.read_text())
            exported[name] = validate(data)
        spread = exported['spread-capture'][-1]
        check((spread['cash_paise'], spread['inventory'], spread['portfolio_value_paise'], spread['total_pnl_paise']) ==
              ('100200', '0', '100200', '200'), 'spread capture')
        check(sum(len(e['trades']) for e in exported['spread-capture']) == 2, 'round trip trade count')
        risk = exported['inventory-risk']
        check(len(risk) == 6 and risk[2]['timestamp_us'] == '2000' and risk[2]['total_pnl_paise'] == '200', 'risk round trip')
        check((risk[-1]['cash_paise'], risk[-1]['inventory'], risk[-1]['portfolio_value_paise'], risk[-1]['total_pnl_paise']) ==
              ('90300', '1', '99800', '-200'), 'final inventory risk')
        check(risk[1]['maker_bid'] is None and risk[2]['maker_ask'] is None, 'filled quotes present')
        check(risk[3]['maker_ask']['order_id'] != risk[-1]['maker_ask']['order_id'], 'cancelled quote reused')
        check(risk[-1]['maker_bid']['price_paise'] == '9400' and risk[-1]['maker_ask']['price_paise'] == '9600', 'repricing')
        limit = exported['inventory-limit']
        check(len(limit) == 11 and limit[-1]['inventory'] == '5', 'limit scenario size/state')
        check(limit[-1]['maker_bid'] is None and limit[-1]['maker_ask'] is not None, 'quote at inventory limit')
        check(sum(len(e['trades']) for e in limit) == 5, 'limit fills')
        check(any(a['timestamp_us'] == b['timestamp_us'] and a['event_index'] != b['event_index']
                  for a, b in zip(limit, limit[1:])), 'equal-time events missing')
        # Argument validation precedes opening/truncating the output.
        sentinel = folder / 'untouched.json'; sentinel.write_text('keep me')
        for args in [('--scenario', 'bad', '--output', sentinel), ('--scenario',),
                     ('--scenario', ''), ('--output', sentinel),
                     ('--scenario', 'inventory-risk', '--output', ''),
                     ('--scenario', 'inventory-risk', '--unknown', sentinel),
                     ('--scenario', 'inventory-risk', '--output', sentinel, 'extra')]:
            result = run(*args)
            check(result.returncode != 0 and 'Exported' not in result.stdout, 'invalid args succeeded')
        check(sentinel.read_text() == 'keep me', 'invalid invocation truncated output')
        for path in (folder, folder / 'missing' / 'output.json'):
            result = run('--scenario', 'inventory-risk', '--output', path)
            check(result.returncode != 0 and 'Exported' not in result.stdout and result.stderr, 'open failure')
        if Path('/dev/full').exists():
            result = run('--scenario', 'inventory-risk', '--output', '/dev/full')
            check(result.returncode != 0 and 'Exported' not in result.stdout, 'write/flush failure')
        original = run()
        check(original.returncode == 0 and 'Time (us): 5000' in original.stdout and
              'total_pnl_paise=-200' in original.stdout, 'no-argument demo changed')
    print('PASS: dashboard schema, deterministic exports, accounting, quote lifecycle, ordering, and output errors')


if __name__ == '__main__':
    main()
