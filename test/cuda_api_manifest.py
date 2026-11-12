#!/usr/bin/env python3
"""Build typed CUDA expectations from workload reports and compiled evidence."""
from collections import Counter
import re

from cuda_api_check import load_manifest
from gen_cuda_api_catalog import EXCLUDED, api_family


NO_STATUS_APIS = {'cudaGetErrorString', 'cudaGetErrorName', 'cudaCreateChannelDesc'}
OCCUPANCY_ALGORITHMS = {'cudaOccupancyMaxActiveBlocksPerMultiprocessor', 'cudaOccupancyMaxPotentialBlockSize'}
OCCUPANCY_EXPORT = 'cudaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags'


def compiled_evidence(assembly, symbols):
    imports = set(re.findall(r'<([^<>]+)@plt>', assembly))
    wrappers = {}
    for block in re.split(r'\n(?=[0-9a-f]+ <)', assembly):
        label = re.match(r'[0-9a-f]+ <(.+)>:', block)
        if not label or '@plt' in label[1]:
            continue
        api = re.search(r'\b((?:cuda|cu)[A-Za-z0-9_]+)(?:<|\(|$)', label[1])
        if not api:
            continue
        name = api[1]
        forwarded = {target for target in re.findall(r'<([^<>]+)@plt>', block)
                     if api_family(target) == api_family(name) and target != name}
        if len(forwarded) == 1:
            wrappers[name] = forwarded.pop()
    inline_algorithms = {}
    for helper in OCCUPANCY_ALGORITHMS:
        # These SDK algorithms run in the application and can issue several underlying API calls.
        if helper not in imports and OCCUPANCY_EXPORT in imports:
            if helper in symbols or helper in assembly:
                inline_algorithms[helper] = OCCUPANCY_EXPORT
    return {'compiler_wrappers': wrappers, 'inline_algorithms': inline_algorithms,
            'static_imports': sorted(imports)}


def build_manifest(calls, evidence, caller='coverage_run', iterations=1):
    expected, resolution = {}, Counter()
    wrappers = evidence.get('compiler_wrappers', {})
    inline = evidence.get('inline_algorithms', {})
    for row in calls:
        requested = row.get('symbol', row.get('name'))
        if not requested:
            continue
        kind = row.get('call_kind', 'api')
        if kind not in ('api', 'direct_export', 'inline_algorithm'):
            raise ValueError('unknown typed workload call_kind ' + kind)
        name, resolved = requested, 'export'
        if kind != 'direct_export':
            if requested in inline:
                name, resolved = inline[requested], 'sdk_inline_algorithm'
            elif kind == 'inline_algorithm':
                raise ValueError('missing compiled evidence for SDK inline algorithm ' + requested)
            elif requested in wrappers:
                name, resolved = wrappers[requested], 'compiled_wrapper'
        if api_family(name) in EXCLUDED:
            continue
        return_kind = row.get('return_kind', 'none' if requested in NO_STATUS_APIS else 'i32')
        value = row.get('status')
        if return_kind == 'i32' and (type(value) is not int or not -(2**31) <= value < 2**31):
            raise ValueError('typed workload needs a signed i32 status for ' + requested)
        if return_kind not in ('i32', 'none'):
            raise ValueError('unknown typed workload return_kind ' + return_kind)
        if name not in expected:
            expected[name] = {'name': name, 'count': 0, 'status': return_kind, 'returns': Counter()}
        call = expected[name]
        if call['status'] != return_kind:
            raise ValueError('conflicting typed workload return policies for ' + name)
        call['count'] += 1
        if return_kind == 'i32':
            call['returns'][str(value)] += 1
        resolution[requested, kind, name, resolved] += 1
    manifest = {
        'version': 1, 'count_policy': 'minimum', 'caller': caller, 'iterations': iterations,
        'calls': [{**call, 'returns': dict(call['returns'])} for _, call in sorted(expected.items())],
        'resolution': [{'requested': requested, 'call_kind': kind, 'name': name, 'resolution': resolved, 'count': count}
                       for (requested, kind, name, resolved), count in sorted(resolution.items())],
        'evidence': evidence,
        'scope': 'typed scenarios in this workload within preset policy; other catalog entries are not claimed exercised',
    }
    return load_manifest(manifest)
