# Presentation timing diagnostics

These programs measure CPU admission, encoding, GPU work, and Metal presentation
separately. They do not launch HSR or install anything into Yaagl. Use a disposable
Wine runtime/prefix when comparing DXMT binaries. Keep the display awake and the
diagnostic window visible: dropped callbacks while the display sleeps are not a
latency measurement.

## Native Metal schedule

From the repository root on macOS:

```sh
clang -arch x86_64 -O2 -fobjc-arc -fblocks -Wall -Wextra \
  tests/presentation/displaylink_deadlines.m \
  -framework Cocoa -framework Metal -framework QuartzCore \
  -o /tmp/displaylink_deadlines
python3 tests/presentation/run_native.py /tmp/displaylink_deadlines /tmp/metal-timing
```

The runner temporarily holds the display awake. Both controls request 60 updates
per second and render every update, comparing layer synchronization on and off.
There is no callback-skipping or delayed-submission mode. Variable GPU work is
optional but disabled; its tests are deferred until the complete feedback loop
is implemented.

Direct arguments are:

```
displaylink_deadlines output.csv preferredLatency framesPerSecond layerSync variableGPU
```

`preferredLatency` is 1 or 2. Timing starts after the first displayed frame,
with a 45-second startup watchdog. The summary rejects runs without enough
on-screen data. These controls measure the native API; they do not implement
the D3D11 feedback loop.

The CSV records the API-supplied CPU submission deadline, predicted presentation,
actual `presentedTime`, the point before input polling, and actual GPU start/end.
A zero `presentedTime` is a dropped drawable, not a zero-latency presentation.
Physical input-to-photon latency is not measured.

## D3D11 waitable swapchain

With llvm-mingw's bin directory on PATH:

```sh
x86_64-w64-mingw32-clang -O2 tests/presentation/waitable_pacing.c \
  -ld3d11 -ldxgi -ldxguid -lwinmm -o /tmp/waitable_pacing.exe
clang -arch x86_64 -dynamiclib -O2 -fobjc-arc -fblocks \
  tests/presentation/present_trace_wall.m -framework Metal -framework QuartzCore \
  -o /tmp/present_trace_wall.dylib
```

Executable arguments:

```
waitable_pacing.exe output.csv durationSeconds waitable maxLatency waitMode cpuCap syncInterval
```

Wait mode 0 uses an ordinary wait; 1 is alertable; 2 pumps window messages while
waiting. The CPU cap advances absolute frame targets and skips missed slots; it
does not estimate GPU cost. It brackets a timer-resolution request with
`timeBeginPeriod/timeEndPeriod`.

For the native observer, set `DYLD_INSERT_LIBRARIES` to its absolute dylib path
and `PACING_TRACE_BASE` to an output filename prefix. It creates per-process CSVs.
`PACING_PRESENT_MODE`: 0 unchanged, 1 plain-present override, 2 present-at-now,
3 minimum duration zero. `PACING_DISPLAY_SYNC`: -1 unchanged, 0 off, 1 on.
`PACING_DRAWABLE_COUNT=0` and `PACING_MAX_PENDING=0` leave those controls unchanged.
API overrides are diagnostic interventions and may change synchronization/rate
semantics. The requested absolute target and applied minimum duration have
separate CSV fields. Use unchanged mode for final source-fix validation.

Set `DXMT_PRESENTATION_BRIDGE=0` to isolate the new waitable path from the older
device bridge. Supply `DXMT_CONFIG=d3d11.preferredMaxFrameRate=60` for the timed
presentation comparison. A separate CPU-cap experiment uses DXMT cap 0 and
`Present(0)`, so it is not equivalent to the VSync-enabled test.

When copying compiled DLLs into a Wine runtime, run the **full Ninja build**, not
only the `d3d11.dll` target: Wine's `--builtin` postprocessing is a separate target.
Reject DLLs whose first 128 bytes lack `Wine builtin DLL`; force builtin loading
with `WINEDLLOVERRIDES=d3d11,dxgi,winemetal=b` to avoid silently testing a prefix
fallback. This is a format check, not a file hash.

## Encoder wake-up regression

```sh
x86_64-w64-mingw32-clang++ -std=c++20 -O2 -static \
  tests/presentation/atomic_wake.cpp -Isrc/util -lsynchronization \
  -o /tmp/atomic_wake.exe
```

Run with argument 0 for this toolchain's `std::atomic::wait`, then 1 for DXMT's
`WaitForAtomicChange` helper. Each run performs 120 delayed producer/consumer
handoffs, checks bounded acknowledgement waits, and reports wake-up latency.
The address-wait loop rechecks with acquire ordering, including races between
notification and entering the wait. Timing is an experiment, not a universal
hard timing assertion for shared CI hosts.
