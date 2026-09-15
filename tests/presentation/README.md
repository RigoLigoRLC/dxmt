# Presentation timing diagnostics

The active comparison is whether the display wakes frame production early enough
to use fresh input, while keeping 60 distinct frames per second and even display
spacing. These tools do not launch HSR or install anything into Yaagl.

## Native reference

```sh
bash tests/presentation/native/build.sh
python3 tests/presentation/native/run.py display60 --seconds 45
python3 tests/presentation/native/run.py producer60
```

`display60` uses ordinary CADisplayLink at exactly 60 callbacks/s to wake the
render thread before input sampling, with ordinary drawable presentation and a
two-presentation limit. `producer60` is the independent-timer control. Both retain
three drawables, end-of-frame acquisition, VSync, and constant 2-ms CPU work.
The GPU performs the same light clear in every frame. No variable GPU work is
included. The app temporarily captures the fullscreen display and exits on its own.

Use the original 120-Hz/ProMotion display mode for the verified low-delay condition.
`--fixed-60hz` is the boundary test and restores the original mode afterward.
Build products, CSVs, callback logs, and summaries go under `native/out/`, which
is ignored by Git. `--output-dir` selects a different result directory.

Instruments' Metal System Trace can launch the built app with these arguments:

```
reference display60 /absolute/path/to/results/run 14 1 0
```

Set `REFERENCE_PSYCH_WINDOW=1` and `MTL_HUD_ENABLED=1` when launching directly.
`native/export_trace.py /absolute/path/to/run.trace` exports the relevant tables.
`native/verify_trace.py /absolute/path/to/run` matches CSV frames to hardware
surface/swap events. Keep the CSV and exported tables at the same filename prefix.

## D3D11 game-loop diagnostic

Use Homebrew `mingw-w64`, as the release package jobs in `.github/workflows/ci.yml` do:

```sh
x86_64-w64-mingw32-gcc -O2 tests/presentation/waitable_pacing.c \
  -ld3d11 -ldxgi -ldxguid -o /tmp/waitable_pacing.exe
clang -arch x86_64 -dynamiclib -O2 -fobjc-arc -fblocks \
  tests/presentation/present_trace_wall.m -framework Metal -framework QuartzCore \
  -o /tmp/present_trace_wall.dylib
```

```
waitable_pacing.exe output.csv durationSeconds waitable maxLatency syncInterval logicMilliseconds
```

The loop pumps window messages while waiting, records the input/state-update
boundary, performs constant CPU work (default 2 ms), and presents. The old CPU
timer-cap and alternative wait-mode controls are removed.

For the optional observer, set `DYLD_INSERT_LIBRARIES` to its absolute dylib path
and `PACING_TRACE_BASE` to an output filename prefix. It records original present
arguments, GPU times, actual presentation, and drawable waits. It adds no queue
waits and does not alter presentation methods, VSync, or drawable count. It observes
command-buffer presentation methods; use Instruments for direct drawable calls.
The next-drawable CSV now contains only the actual drawable wait, with no separate
experimental presentation-wait column.

Use the isolated diagnostic Wine runtime/prefix. Run the full Ninja build before
testing DLLs: Wine's builtin postprocessing is a separate target. Force builtin
loading with `WINEDLLOVERRIDES=d3d11,dxgi,winemetal=b` to avoid a prefix fallback.
The old presentation bridge and its disable flag no longer exist on this branch.

## Encoder wake-up regression

```sh
x86_64-w64-mingw32-g++ -std=c++20 -O2 -static \
  tests/presentation/atomic_wake.cpp -o /tmp/atomic_wake.exe
```

Run the executable with the isolated Wine runtime, without arguments. It checks
120 delayed producer/consumer handoffs using only standard C++ synchronization
and clocks. A two-second handoff timeout detects a stuck worker. The latency
distribution is a measurement, not a strict CI threshold.

Compile the same source with another toolchain to compare its atomic wait.
The release package depends on the GCC/MinGW-w64 jobs, not the LLVM-MinGW jobs.
In three same-source comparisons in the same Wine prefix, GCC 16.2.0 standard
atomic wait had median wakeup 0.051–0.057 ms; LLVM-MinGW 20251216 / libc++ 21.1.8
had 5.80–7.48 ms. GCC's binary calls a blocking condition-variable wait and its
notification wakes that wait. The earlier explicit Win32 address-wait helper and
its synchronization import library are no longer needed with the release
toolchain. This does not repair LLVM-MinGW's library fallback.

Native presentation-counter tests run with
`meson test -C BUILD_DIR presentation-feedback --print-errorlogs`.
