# CUDA host API and UTRACE verification

Run these commands from the repository root with a built `src/wprof`.
The CUDA API catalog is checked in, so building wprof does not require a CUDA
SDK. Live captures require root privileges. The capture runners invoke wprof
with `sudo -n`; run them as the ordinary user with working noninteractive sudo.
Replay needs only read access to the capture.
Preset stack captures and replay use `--stacks=utrace`.

## CLI and controlled CPU fixtures

CLI checks require Python 3 and wprof, with no root, GPU or SDK:

```sh
python3 test/cuda_api_cli.py src/wprof
```

The CPU fixtures compile small shared libraries and capture actual uprobes.
They require Python 3, a C compiler and `sudo -n`, with no GPU or CUDA SDK:

```sh
python3 test/cuda_api_fixture.py src/wprof --mode plain --out /tmp/cuda-api-fixture
python3 test/cuda_api_fixture.py src/wprof --mode stacks
python3 test/cuda_api_fixture.py src/wprof --mode sealed
```

Additional modes exercise these cases:

| Mode | Check |
|------|-------|
| `fallback` | SysV ELF hash lookup |
| `versions` | Ambiguous non-default symbol versions |
| `versions-fallback` | The same version ambiguity with SysV hashes |
| `worker` | An eligible worker thread when the main thread is filtered out |
| `low-fds` | Capture under a fixed 256-descriptor soft and hard limit |
| `multi-fail-load` | Individual fallback after uprobe-multi program load fails |
| `multi-fail-attach-entry` | Individual fallback after multi entry attachment fails |
| `multi-fail-attach-return` | Individual fallback after multi return attachment fails |

Every successful capture checks target PID, available sites, independently
recorded alias names, distinct variant offsets and signed status policy. A
call at a shared file address appears under each configured alias name;
capture logs warn about this and `-v` identifies the names. The
`stacks` mode checks the original `coverage_run` caller; `sealed` checks that
status arguments are absent. `--out` preserves capture and result files;
otherwise each fixture uses a temporary directory. CPU fixtures qualify the
UTRACE behavior they exercise, not GPU execution coverage.

Worker-thread filtering with uprobe-multi was tested on Linux 6.16.1.
The same filtering on older kernels has not been verified.

The ordinary individual-probe descriptor check also needs `sudo -n`:

```sh
python3 test/utrace_fd_limits.py src/wprof
```

It captures real `getpid` observations with 256/256 and 256/1024 descriptor
limits and verifies that both remain unchanged. The forced multi-failure
modes and the caller runner's individual mode start with 256/1024 limits and
check that mapped individual fallback raises the soft limit to 1024. These
checks read the owned profiler PID's actual `/proc` limits after recording
readiness. Mapped `uspan` multi attachment uses two links per PID/library.

## Real CUDA workload

This runner requires an NVIDIA GPU, compatible CUDA SDK and compiler, Python
3, `g++`, binutils and working `sudo -n`. Supply all environment paths:

```sh
python3 test/cuda_api_real.py src/wprof \
    --out /tmp/cuda-api-real \
    --sdk /path/to/cuda-sdk \
    --nvcc /path/to/cuda/bin/nvcc \
    --gpu-lock /path/to/shared-gpu.lock
```

`--sdk`, `--nvcc` and `--gpu-lock` are required. The SDK root contains
`include` and `lib`; the compiler installation supplies its matching headers.
Use one writable lock file for all workloads sharing the GPU. The runner
holds its exclusive lock during compilation and measurement.
Its targets dynamically link `libcudart.so` for runtime API probes. For a
separately compiled target, `nvcc --cudart shared` selects a shared runtime;
the default static runtime supplies no mapped `libcudart.so` for the preset.

The native workload is built for default-stream and per-thread default-stream
semantics. Each binary runs an untraced baseline and `plain`, `stacks` and
`sealed` captures. `--repeat` defaults to 5 measurements for each case, and
`--iterations` defaults to 100 typed-workload iterations per native
measurement. Override these to match the measurement duration needed on the
test machine:

```sh
python3 test/cuda_api_real.py src/wprof \
    --out /tmp/cuda-api-real-long \
    --sdk /path/to/cuda-sdk \
    --nvcc /path/to/cuda/bin/nvcc \
    --gpu-lock /path/to/shared-gpu.lock \
    --repeat 10 --iterations 1000
```

The target waits for the profiler's actual recording-window state through
the shared test helper before making measured calls and stays alive through
stack symbolization. The helper identifies the owned profiler PID and its
BPF map, resolves recording timestamps through BTF, and checks timeout/early
exit. It never interprets progress logs. Capture starts
with a 1024-descriptor soft limit. The report records workload durations,
resolution and attachment timing, selected attachment mode and recorded loss.
`cuda-api.attach_mode` reports the preset's attachment mechanism.

Inspect a preserved capture with `src/wprof -RI -D capture.data` for the compact
preset summary; add `-v` for generated UTRACE definitions and site diagnostics.

Each case has a warmup run excluded from the measured samples, in addition to
the workload's internal warmup. `--seed` defaults to 0 and selects a reproducible
case order.

Each `default-CASE` or `per-thread-CASE` directory contains `warmup` and
`sample-01` through `sample-05` with the default repeat count. Sample
directories contain `workload.jsonl`, `expected-manifest.json` and `result.json`;
captures also contain
`capture.data`, `capture.json`, `capture.log`, `capture.info` and `replay.log`.
`summary.json` preserves raw samples and execution order, with medians,
min/max, median absolute deviation and standard deviation. `elapsed_ratio` is
the ratio of case and baseline medians; `median_elapsed_ratio` is the median
of matched-round ratios. Attachment timings have their own medians and
dispersion.

Repeated elapsed-time ratios describe this workload and machine; short
single-pass measurements do not establish definitive profiler overhead.

Expected calls come from the workload manifest and compiler forwarding,
independently of recorded events. Each configured library alias retains its
own name and may record the same underlying call. SDK wrappers and application-side occupancy
algorithms are reported separately from directly observed exported APIs.

The full catalog is not an execution coverage list. A passing run qualifies
the typed scenarios it ran; missing exports and untested catalog entries
remain separate outcomes.

The standalone NDJSON checker requires the expected workload manifest and
replay log, with no GPU or root privileges when the inputs are readable:

```sh
python3 test/cuda_api_check.py /path/to/capture.json \
    --pid 1234 \
    --manifest /path/to/expected-manifest.json \
    --replay-log /path/to/replay.log
```

Use `--stacks` for entry caller stacks or `--status-disabled` for a sealed
capture. The manifest supplies the expected calls; the checker does not infer
the expected set from observed trace names. Generic replay drop and recursion
diagnostics cause verification to fail.

The manifest builder omits the preset's intentionally excluded getter
families. Workload calls and their failure accounting still execute.

Manifest version 1 records a caller, a `count_policy` (`minimum` in generated
manifests; `exact` is also supported), and calls with `name`, `count` and
`status` (`i32` or `none`). Declared scalar returns also have expected value
counts in `returns`. Compiler-wrapper and inline-helper resolution is recorded
as independent workload evidence. Pairing follows a stack per thread and
configured name, so independently recorded aliases do not impose a cross-name
ordering requirement.

### Reuse an API-coverage workload

The API-coverage workload is optional, supplied externally and not included in
this repository.

Add this option to the real-runner command:

```sh
--coverage-dir /path/to/cuda-bpf-only-example/api-coverage
```

The directory provides `demo.cu` and its includes. The runner copies and gates
the source in its output directory, preserving the original sources. It checks
typed calls in the target PID against direct UTRACE observations in both stream
modes and all three capture policies. The coverage workload has its own call
set; the native `--iterations` setting does not expand that source.

## Prior full-catalog measurements

Prior A100 measurements with CUDA SDK 13.2 used the following source
fingerprint, retained before the metadata-boundary cleanup:

```text
1bcbb15d30abac3f4b129e87e35d9ab785492449f96927a6ecab542ea811e160
```

These results qualify that measured source, these workloads and this
environment. The measured catalog had 1,139 names; 1,108 resolved in the installed
libraries and were included in each capture. Each case has five measured
samples after warmup, with randomized case order. The native sample runs 100
typed API iterations; the externally supplied broad demo runs one pass.

| Workload | Stream | Baseline median (ms) | Plain ratio | Stacks ratio | Sealed ratio |
|---|---|---:|---:|---:|---:|
| native | default | 72.74 | 1.278x | 1.330x | 1.318x |
| native | per-thread | 70.96 | 1.234x | 1.370x | 1.453x |
| coverage | default | 1896.31 | 1.013x | 0.968x | 0.985x |
| coverage | per-thread | 1842.30 | 1.036x | 1.065x | 1.038x |

Ratios compare each case median with the baseline median. Treat values below
1 as measurement variability. The native workload exposes API-heavy costs;
the broader workload includes substantial GPU, callback-observer and I/O time.

Median attachment intervals ranged from 21.09 to 32.36 ms across cases.
Individual attachment samples ranged from 15.89 to 96.75 ms. These intervals
include UTRACE attachment; they exclude preceding
preparation and later teardown. All measured captures reported no recorded
loss. Raw samples, min/max, MAD and standard deviation are retained in the
runner's `summary.json` output.

## Regenerate the catalog

Catalog generation requires CUDA headers and a C preprocessor, with no GPU or
root privileges:

```sh
python3 test/gen_cuda_api_catalog.py \
    --sdk /path/to/cuda-sdk/include \
    --compiler-include /path/to/cuda/targets/x86_64-linux/include \
    --output /tmp/cuda_api_sites.h
```

The generator reads SDK declarations and retained legacy contracts in
`cuda_api_contracts.h`, preserving the original preset ordering. Inspect the generated diff before replacing
`src/cuda_api_sites.h`. The current checked-in catalog has 1,127 names from
CUDA SDK 13.2; installed libraries may expose a subset.

### Bounded PyTorch polling observation

A single A100/PyTorch CUDA 13.2 comparison used a 400 ms loop calling
`event.query()`, `stream.query()` and `current_device()`. Baseline completed
106,722 loops; a full-preset capture completed 20,053 loops (0.188x relative
throughput). The capture lasted 500 ms with four 4 MB ring buffers and no
stacks, PMUs, scheduler, wakeup or task-lifetime tracing; default IRQ tracing
remained enabled. It recorded 220,583 entries and 220,583 exits, with zero
ring-buffer or task-state drops.

This is one illustrative sample, not a statistical performance claim. It
shows that polling-heavy API traffic can substantially slow an explicitly
requested full API capture even without losing records. This observation
preceded the getter exclusions.

### Getter exclusion comparison

A fresh A100/CUDA 13.2 comparison used identical targets and CUPTI preload
for baseline, the prior preset (1,139 names; 1,108 resolved), the reduced
preset (1,127 names; 1,096 resolved), and CUPTI. Each case ran five samples
after warmup in balanced randomized rounds, with the same capture options
as the polling observation above.

Polling medians were 3.85 us/loop baseline, 23.57 prior, 11.09 reduced and
8.61 CUPTI. Matched prior/reduced rounds saved a median 49.5% of per-loop
time (range 44.4-53.7%). Records per loop fell from approximately 22 to 8,
or 63.64% fewer records. The reduced preset retained runtime and driver
event/stream queries: four entry/exit pairs per loop.

Normalized record counts fell approximately 2.5% in the native workloads
and 3.9% in the broad workload. Elapsed and startup differences outside
polling were inconclusive across these five samples. All 80 measured
samples completed; all 60 captures reported zero ring-buffer/task-state
drops and balanced UTRACE entry/exit totals. These measurements qualify
this environment and these workloads.

## CUDA driver caller observations and optional filter

The controlled caller test needs root, but no GPU or CUDA SDK:

```sh
python3 test/cuda_api_caller_filter.py ./src/wprof --out /tmp/wprof-caller-filter-check
```

It checks multi and individual attachment, plus static-runtime behavior,
against fixed call/status sequences. Capture policy metadata selects filtered
or unfiltered expectations. When the optional filter is available and reports
`skipped`, generated driver probes omit shared-runtime callers; `retained` or
absent policy metadata expects those calls. Entry stacks check the selected
behavior and manual probes always observe runtime callers. The same caller
check runs in both attachment paths. Static runtime calls retain their driver
observations because no shared runtime mapping applies.

An earlier A100/CUDA 13.2 comparison used the caller filter without a
failure hook, with five balanced samples per case after warmup. Captures
waited for actual recording through the shared helper, using the owned
PID/BPF timestamps.

| Workload | Baseline | Getter cut | Caller filter | CUPTI |
|---|---:|---:|---:|---:|
| polling, us/loop | 3.83 | 10.71 | 9.89 | 8.87 |
| native default, ms | 71.23 | 94.27 | 90.07 | 88.64 |
| native per-thread, ms | 78.26 | 91.81 | 88.41 | 97.15 |
| broad default, ms | 1846.91 | 2005.08 | 1820.60 | 1862.62 |

Paired polling savings had median 7.8% (range 1.9% to 12.4%). Records fell
from eight to four per loop, approximately 31% in native and 51% in broad.
Other timing gains were variable. All 80 measured samples and 60 captures
completed without recorded drops. Ten native captures matched prior
name/type/status counts after excluding the temporary teardown gate.

Prior fingerprints and numbers identify their original measured builds.
The timing endpoint and executable identity are recorded
separately; no speed threshold is a test requirement.
