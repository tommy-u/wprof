#!/usr/bin/env python3
"""Generate the checked-in public host API catalog from CUDA SDK declarations."""
import argparse
from collections import defaultdict
from pathlib import Path
import re
import subprocess

BASELINE = [('libcuda.so', 'cuLaunchKernel'), ('libcuda.so', 'cuLaunchKernel_ptsz'),
            ('libcuda.so', 'cuCtxSynchronize_v2'), ('libcuda.so', 'cuCtxSynchronize'),
            ('libcuda.so', 'cuDeviceGetAttribute'), ('libcudart.so', 'cudaLaunchKernel'),
            ('libcudart.so', 'cudaLaunchKernel_ptsz'), ('libcudart.so', 'cudaDeviceSynchronize'),
            ('libcudart.so', 'cudaSetDevice')]

# Match the spammy getter filters in src/inj_cupti.c. cuCtxGetDevice also
# excludes the driver getter called internally by cudaGetDevice.
EXCLUDED = {'cuPointerGetAttribute', 'cuPointerGetAttributes', 'cuDevicePrimaryCtxGetState',
            'cuCtxGetCurrent', 'cuKernelGetAttribute', 'cudaGetLastError', 'cudaPeekAtLastError',
            'cudaGetDevice', 'cudaStreamIsCapturing', 'cuCtxGetDevice'}


def api_family(name):
    return re.sub(r'(_v\d+|_ptsz|_ptds)+$', '', name)


def family(name):
    name = api_family(name)
    if re.search(r'(Launch|Graph|Occupancy|TensorMap)', name):
        return 'launch, graph and execution'
    if re.search(r'(Memcpy|Memset|Mem|Array|Pointer|Ipc)', name):
        return 'memory and IPC'
    if re.search(r'(Stream|Event|Synchronize|ExternalSemaphore)', name):
        return 'streams, events and synchronization'
    if re.search(r'(Ctx|Device|Init|Driver|Runtime)', name):
        return 'device and context'
    if re.search(r'(Module|Func|Kernel|Library|Link|Tex|Surf)', name):
        return 'modules and resources'
    return 'queries and other public host APIs'


def generate(sdk, compiler_include, output):
    paths = [sdk / 'cuda.h', sdk / 'cuda_runtime_api.h']
    # NVIDIA distributes some host declarations with the compiler rather than the runtime package.
    for name in ('cudaGL.h', 'cudaEGL.h', 'cudaVDPAU.h', 'cuda_gl_interop.h',
                 'cuda_egl_interop.h', 'cuda_vdpau_interop.h', 'cudaProfiler.h', 'cuda_profiler_api.h'):
        path = sdk / name
        if not path.exists():
            path = compiler_include / name
        if path.exists():
            paths.append(path)
    texts = [path.read_text() for path in paths]
    texts.append(Path(__file__).with_name('cuda_api_contracts.h').read_text())
    source = '#include <cuda.h>\n#include <cuda_runtime_api.h>\n'
    for flags in ([], ['-DCUDA_API_PER_THREAD_DEFAULT_STREAM', '-D__CUDA_API_PER_THREAD_DEFAULT_STREAM'],
                  ['-D__CUDA_API_VERSION_INTERNAL']):
        command = ['cc', '-E', '-P', '-xc', '-', '-I' + str(sdk), '-I' + str(compiler_include), *flags]
        texts.append(subprocess.run(command, input=source, text=True, capture_output=True, check=True).stdout)
    sites = {}
    for text in texts:
        for kind, name in re.findall(r'\b(CUresult|cudaError_t)\s+(?:(?:CUDAAPI|CUDARTAPI)\s+)?((?:cu|cuda)\w+)\s*\(', text):
            library = 'libcuda.so' if kind == 'CUresult' else 'libcudart.so'
            sites[library, name] = 'CUDA_API_STATUS_S32'
        for name in re.findall(r'const\s+char\s*\*\s*(?:CUDARTAPI\s+)?(cuda\w+)\s*\(', text):
            sites['libcudart.so', name] = 'CUDA_API_STATUS_NONE'
        for name in re.findall(r'(?:struct\s+)?cudaChannelFormatDesc\s+(?:CUDARTAPI\s+)?(cuda\w+)\s*\(', text):
            sites['libcudart.so', name] = 'CUDA_API_STATUS_NONE'
    # Interoperability headers remap their declarations to ABI and stream-specific exports.
    for text in texts:
        for base, stream, target, direct in re.findall(
                r'^\s*#\s*define\s+((?:cu|cuda)\w+)\s+(?:__(?:CUDA|CUDART)_API_(PTDS|PTSZ)\((\w+)\)|((?:cu|cuda)\w+))',
                text, re.MULTILINE):
            library = 'libcudart.so' if base.startswith('cuda') else 'libcuda.so'
            status = sites.get((library, base))
            if status is None:
                continue
            exported = target or direct
            sites[library, exported] = status
            if stream:
                sites[library, exported + '_' + stream.lower()] = status
    profiler = sdk / 'cudaProfilerTypedefs.h'
    if profiler.exists():
        for name in re.findall(r'CUresult\s*\(CUDAAPI\s*\*PFN_(cuProfiler\w+)_v4000\)', profiler.read_text()):
            sites['libcuda.so', name] = 'CUDA_API_STATUS_S32'
    sites = {key: status for key, status in sites.items() if api_family(key[1]) not in EXCLUDED}
    version = re.search(r'#define\s+CUDA_VERSION\s+(\d+)', texts[0]).group(1)
    lines = ['/* SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause) */',
             '/* Generated from CUDA SDK ' + version + ' declarations and retained profiler contracts.',
             ' * Generator: test/gen_cuda_api_catalog.py (test/cuda_api_contracts.h adds legacy controls).',
             ' * Names and enum-return policies are checked in; building wprof needs no CUDA SDK.',
             ' * Original preset names come first to keep alias representatives stable.', ' */']
    for key in BASELINE:
        # cuCtxSynchronize_v2 is an exported legacy counterpart with the same CUresult return.
        status = sites.pop(key, 'CUDA_API_STATUS_S32')
        lines.append('\t{"%s", "%s", %s},' % (*key, status))
    groups = defaultdict(list)
    for (library, name), status in sites.items():
        groups[library, family(name)].append((name, status))
    for (library, group), entries in sorted(groups.items()):
        lines += ['', '/* ' + library + ': ' + group + ' */']
        lines += ['\t{"%s", "%s", %s},' % (library, name, status) for name, status in sorted(entries)]
    output.write_text('\n'.join(lines) + '\n')
    print('Catalog:', len(sites) + len(BASELINE), 'sites from SDK', version)


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk', type=Path, required=True)
    p.add_argument('--compiler-include', type=Path, required=True)
    p.add_argument('--output', type=Path, default=Path('src/cuda_api_sites.h'))
    a = p.parse_args()
    generate(a.sdk, a.compiler_include, a.output)
