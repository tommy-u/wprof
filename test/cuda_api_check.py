#!/usr/bin/env python3
"""Check raw CUDA UTRACE records against an independent typed-workload manifest."""
import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path
import re


def has_recorded_loss(text):
    return bool(re.search(r'^!!! (?:Drops \(|Ringbuf fetch misses:|(?:Task|Request) state drops:|'
                          r'.+: [1-9][0-9]* recursion misses!)', text, re.MULTILINE))


def load_manifest(manifest):
    if not isinstance(manifest, dict):
        manifest = json.loads(Path(manifest).read_text())
    if manifest.get('version') != 1 or not manifest.get('calls'):
        raise ValueError('manifest needs version 1 and nonempty typed calls')
    if manifest.get('count_policy', 'minimum') not in ('minimum', 'exact'):
        raise ValueError('manifest count_policy must be minimum or exact')
    for call in manifest['calls']:
        if not isinstance(call.get('name'), str) or not call['name']:
            raise ValueError('manifest call needs an export name')
        if type(call.get('count')) is not int or call['count'] <= 0:
            raise ValueError('manifest call count must be positive')
        if call.get('status') not in ('i32', 'none'):
            raise ValueError('manifest status must be i32 or none')
        returns = call.get('returns', {})
        if call['status'] == 'none' and returns:
            raise ValueError('a no-status call cannot declare scalar returns')
        if call['status'] == 'i32':
            if not returns or sum(returns.values()) != call['count']:
                raise ValueError('manifest return counts must equal the typed call count')
            for value, count in returns.items():
                if not -(2**31) <= int(value) < 2**31 or type(count) is not int or count <= 0:
                    raise ValueError('manifest returns must have signed i32 values and positive counts')
    return manifest


def check_rows(rows, pid, manifest, status=True, stacks=False, recorded_loss=None):
    manifest = load_manifest(manifest)
    header = rows[0] if rows else {}
    stack_count = header.get('stack_cnt', 0)
    stack_table = {row['id']: row for row in rows[1:1 + stack_count]}
    expected = {}
    for call in manifest['calls']:
        name = call['name']
        if name not in expected:
            expected[name] = {'name': name, 'count': 0, 'status': call['status'], 'returns': Counter()}
        target = expected[name]
        if target['status'] != call['status']:
            raise ValueError('conflicting manifest return policies for ' + name)
        target['count'] += call['count']
        target['returns'].update({int(value): count for value, count in call.get('returns', {}).items()})

    events = [row for row in rows if row.get('t') in ('utrace_entry', 'utrace_exit')]
    errors, missing, pairs = [], [], []
    counts, returns, caller_counts = Counter(), Counter(), Counter()
    threads = defaultdict(list)
    caller = manifest.get('caller')
    if stacks and not caller:
        errors.append('manifest needs the original caller for stack checks')
    if recorded_loss is None:
        errors.append('recorded loss was not checked')
    elif recorded_loss:
        errors.append('recorded loss makes pairing unreliable')
    for row in events:
        name = row['name']
        if row['task']['pid'] != pid:
            errors.append('wrong target PID for ' + name)
            continue
        counts[name, row['t']] += 1
        threads[row['task']['tid']].append(row)
        args = row.get('args', {})
        if row['t'] == 'utrace_entry':
            if args:
                errors.append('return arguments captured on entry ' + name)
            if stacks and name in expected:
                entry_stack = stack_table.get(row.get('stack_id'))
                if entry_stack is None:
                    errors.append('missing/unresolved original entry stack for ' + name)
                elif caller and any(re.search(r'\b' + re.escape(caller) + r'(?:\+0x[0-9a-f]+)?$', frame)
                                    for frame in entry_stack.get('frames', [])):
                    caller_counts[name] += 1
            continue
        if not status:
            if args:
                errors.append('status present despite disabled policy for ' + name)
        elif name in expected:
            if expected[name]['status'] == 'none':
                if args:
                    errors.append('status present on no-status return ' + name)
            else:
                value = args.get('status')
                if type(value) is not int or not -(2**31) <= value < 2**31:
                    errors.append('missing signed scalar return status for ' + name)
                else:
                    returns[name, value] += 1
                    if value not in expected[name]['returns']:
                        errors.append('unexpected status ' + str(value) + ' for ' + name)
                if set(args) != {'status'}:
                    errors.append('unexpected return arguments for ' + name)

    exact = manifest.get('count_policy', 'minimum') == 'exact'
    for name, call in sorted(expected.items()):
        observed = counts[name, 'utrace_entry']
        if observed < call['count']:
            missing.append({'name': name, 'expected': call['count'], 'observed': observed})
            errors.append('missing typed call ' + name)
        elif exact and observed != call['count']:
            errors.append('unexpected typed call count for ' + name)
        if stacks and caller and caller_counts[name] < call['count']:
            errors.append('missing original ' + caller + ' caller for ' + name)
        if status:
            for value, count in call['returns'].items():
                observed = returns[name, value]
                if observed < count or (exact and observed != count):
                    errors.append('wrong status count for ' + name + ' status ' + str(value))

    for tid, thread in sorted(threads.items()):
        active, previous = defaultdict(list), None
        for row in thread:
            name = row['name']
            if previous is not None and row['ts'] < previous:
                errors.append('per-thread timestamp order for TID ' + str(tid))
            previous = row['ts']
            if row['t'] == 'utrace_entry':
                active[name].append(row)
            else:
                if not active[name]:
                    errors.append('unmatched return ' + name + ' for TID ' + str(tid))
                    continue
                entry = active[name].pop()
                pairs.append({'tid': tid, 'name': name, 'entry_ts': entry['ts'], 'exit_ts': row['ts'],
                              'status': row.get('args', {}).get('status')})
        if any(active.values()):
            errors.append('missing observed return; raw entry retained for TID ' + str(tid))
    for name in sorted({name for name, _ in counts}):
        if counts[name, 'utrace_entry'] != counts[name, 'utrace_exit']:
            errors.append('unbalanced site ' + name)
    return {
        'pass': bool(events) and not errors, 'errors': list(dict.fromkeys(errors)), 'missing_calls': missing,
        'records': len(events), 'recorded_loss': recorded_loss,
        'counts': [{'name': name, 'kind': kind, 'count': count} for (name, kind), count in sorted(counts.items())],
        'expected': [{**call, 'returns': dict(call['returns'])} for _, call in sorted(expected.items())],
        'pairs': pairs, 'status_enabled': status, 'original_caller': caller,
        'caller_entry_counts': dict(caller_counts),
        'entry_stack_count': sum(bool(row.get('stack_id')) for row in events if row['t'] == 'utrace_entry'),
        'unchecked_names': sorted({row['name'] for row in events} - expected.keys()),
        'scope': manifest.get('scope', 'typed workload calls; additional names are checked only for pairing/order'),
    }


def check(path, pid, manifest, status=True, stacks=False, recorded_loss=None):
    rows = [json.loads(line) for line in Path(path).read_text().splitlines() if line.strip()]
    return check_rows(rows, pid, manifest, status, stacks, recorded_loss)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ndjson', type=Path)
    parser.add_argument('--pid', type=int, required=True)
    parser.add_argument('--manifest', type=Path, required=True)
    parser.add_argument('--replay-log', type=Path, required=True,
                        help='Log of the replay that emitted NDJSON, including recorded loss warnings')
    parser.add_argument('--status-disabled', action='store_true')
    parser.add_argument('--stacks', action='store_true')
    args = parser.parse_args()
    try:
        loss = has_recorded_loss(args.replay_log.read_text())
        result = check(args.ndjson, args.pid, args.manifest, not args.status_disabled, args.stacks, loss)
    except (ValueError, OSError, KeyError, TypeError) as error:
        result = {'pass': False, 'errors': [str(error)]}
    print(json.dumps(result, indent=2))
    return 0 if result['pass'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
