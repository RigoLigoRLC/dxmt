# Presentation timing findings — 15 September 2026

The tests found a real encoder wake-up delay and separated it from Metal's display
schedule. The encoder fix is implemented in the DXMT experiment branch. The native
presentation demo now exposes the actual API deadlines and offers a lower-delay
configuration for comparison. Nothing from this round has been installed into
Yaagl; its existing game prefix and DXMT bundle remain untouched.

## What the HUD measures

Apple defines Present Delay as the interval from calling `presentDrawable` until
the drawable reaches the display. It excludes the game's earlier CPU work and
DXMT's earlier encoder wait. It is not total input latency and need not decrease
when an earlier pipeline stage gets faster.
[Apple's metric definition](https://developer.apple.com/documentation/xcode/understanding-metal-performance-hud-metrics)

Our trace records the same endpoints explicitly, plus GPU start/end, CPU wake,
D3D11 Present entry/return, and the presentation callback's arrival. These are API
measurements, not camera/photodiode measurements of physical input to photons.

## 1. A source-level cause: the encoder sleeps instead of being woken

`CommandQueue::EncodingThread()` in `src/dxmt/dxmt_command_queue.cpp` waits on
`ready_for_encode`. The llvm-mingw 21.1.8 C++ runtime linked into this build
implements its `std::atomic::wait` path using polling and sleeping. Disassembly
of `__libcpp_contention_wait` shows calls to `__libcpp_thread_sleep_for`; a process
sample also shows substantial `NtDelayExecution` time. The sample's Wine stack
unwinding is incomplete, so the isolated benchmark and intervention below are
the stronger attribution evidence.

A 120-handoff benchmark gave:

| Wait implementation | Mean wake delay | 95th percentile |
| --- | ---: | ---: |
| C++ runtime polling, first run | 6.19 ms | 14.46 ms |
| Windows `WaitOnAddress`, first run | 0.021 ms | 0.032 ms |
| C++ runtime polling, final repeat | 7.15 ms | 16.66 ms |
| DXMT address-wait helper, final repeat | 0.020 ms | 0.028 ms |

The patch replaces only the encoder's wait/notification with
`WaitForAtomicChange` / `NotifyAtomicChange` in `src/util/util_atomic_wait.hpp`.
It uses acquire reloads around `WaitOnAddress` and wakes the single encoder with
`WakeByAddressSingle`; submission and shutdown use the same notification path.
Other atomic waits, including GPU retirement, are unchanged.

A full Wine/D3D11 comparison, with the callback feedback enabled and exactly the
same timed Metal presentation, isolates its effect:

| Configuration | Presented FPS | D3D11 return → Metal present | Present delay |
| --- | ---: | ---: | ---: |
| Callback feedback + original encoder wait | 32.44 | 7.82 ms | 22.11 ms |
| Callback feedback + address-wait fix | 40.00 | 0.40 ms | 23.85 ms |

This is a useful example of the HUD metric's limit: the overall one-frame cycle
improved from about 30.8 to 25.0 ms, although the present-delay component increased.
It establishes a problem in this rebuilt/tested stack, not proof that all of
HSR's original delay came from this wait.

## 2. A separate cause: Metal is deliberately scheduling the image later

The native demo runs without Wine or DXMT and logs `CAMetalDisplayLink.Update`'s
`targetTimestamp` and `targetPresentationTimestamp` separately.

- `targetTimestamp` is the deadline to submit the drawable with `present()`;
  GPU execution may continue afterward.
- `targetPresentationTimestamp` is Metal's prediction of when the image will
  appear. It is not the CPU submission deadline.
- `preferredFrameLatency` requests a rendering allowance. Its accepted values
  are 1 and 2; Apple explicitly allows greater latency in macOS windowed modes.
- For this API, the demo commits the GPU command buffer, then calls the update
  drawable's plain `present()` before the CPU deadline.

Sources: [submission deadline](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targettimestamp),
[presentation prediction](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/update/targetpresentationtimestamp),
[preferred latency](https://developer.apple.com/documentation/quartzcore/cametaldisplaylink/preferredframelatency).

On this Mac's built-in 120 Hz display, the windowed native control produced:

| Display-link updates/s | Render every | Layer sync | Actual presented FPS | Mean present delay | Deadline → predicted display |
| ---: | ---: | --- | ---: | ---: | ---: |
| 60 | 1 update | On | 60.00 | 49.76 ms | 33.33 ms |
| 120 | 2 updates | On | 60.00 | 41.47 ms | 33.33 ms |
| 60 | 1 update | Off | 53.61 | 33.65 ms | 16.67 ms |
| 120 | 2 updates | Off | 59.50 | 26.10 ms | 16.67 ms |

These runs rendered a simple clear. All four had zero measured CPU-deadline
misses in their sampled windows. With synchronization on, actual display matched
Metal's prediction to about 0.01 ms. With it off, actual display averaged about
0.94–1.26 ms later than predicted.

The last configuration requests more scheduling updates, while still rendering
only every other update. It does not turn the game into a 120 FPS renderer.
Changing the layer synchronization property directly changed the API's predicted
post-deadline allowance by one 60 Hz interval in this test. That identifies a
specific presentation-policy choice in the native demo; it does not prove a
universal Metal minimum latency or explain every fullscreen/direct presentation.

The original native control left `CAMetalLayer.displaySyncEnabled` at its default
of true. The corrected demo makes this choice explicit. DXMT already sets that
property false, so this native-demo correction is not by itself a new DXMT fix.
DXMT's current presentation also uses the timed command-buffer API, whereas the
native display-link demo uses the update drawable and its submission deadline.

## 3. Why changing the cap or only the semaphore is insufficient

`afterMinimumDuration` constrains how long the preceding drawable remains on
screen. It is not a signal that the CPU may begin rendering the next frame.
[Apple's API documentation](https://developer.apple.com/documentation/metal/mtlcommandbuffer/present(_:afterminimumduration:))

In the Wine experiment, setting that duration to zero retained roughly the same
32 FPS / 22 ms behavior before the encoder fix. Replacing it with plain present
changed behavior, as did the layer's synchronization property. This rules out the
numeric 60 FPS cap as a sufficient explanation. The API path and its schedule
matter too.

The previous waitable implementation returned capacity while recording work,
before actual presentation. The callback fix prevents that: with a latency of
one, measured wakes followed the preceding drawable's `presentedTime` instead of
preceding it. But using actual-display completion as the only clock serializes
the whole pipeline. After the encoder fix, the synchronized path still took
about 25 ms per frame. Allowing two pending presentations restored 60.00 FPS,
with 32.34 ms present delay. That is a queue-depth/throughput tradeoff, not the
low-latency scheduler we ultimately want.

Plain unsynchronized presentation with the corrected 60 FPS CPU limiter reached
about 56 FPS with an 8.15 ms mean delay, but its 95th percentile remained about
24.2 ms. It is therefore not a validated replacement for the synchronized path.

## Demo and verification changes

The branch contains standalone source and reproduction instructions under
`tests/presentation/`:

- A D3D11 waitable demo with ordinary/alertable/message-pumping waits, explicit
  per-swapchain latency, CPU cap, and SyncInterval. HUD and message-wait controls
  did not explain the encoder stall.
- A Metal observer recording separate absolute target times and minimum
  durations. The review caught and corrected their previous mislabeled column.
- A native display-link demo with scheduling-rate, layer-sync, render-divisor,
  preferred-latency, and variable real-GPU-work controls.
- A native runner that temporarily prevents display sleep, and a summarizer
  that rejects runs lacking enough displayed frames or steady-state duration.
- The producer/consumer benchmark using the actual encoder-wait helper.

The full DXMT cross-build succeeded. Its five-case callback regression suite
passed again; the final helper benchmark passed 120 acknowledged handoffs.
Read-only review found no blocker in the encoder fix. The new native demo and
observer compile with Apple Clang for x86_64; the D3D11 demo compiles with
llvm-mingw. No sanitizer coverage is claimed.

Two invalid experiment classes are excluded from conclusions: a partial Ninja
build omitted Wine's builtin-DLL postprocessing, causing fallback to another
DLL; and the display went off at 03:20:40, after which Metal reported dropped
frames. The harness now guards against both. Delayed-submission experiments
also missed CPU deadlines despite sometimes displaying on time; they are
retained only as diagnostics, not a proposed fixed render-time heuristic.

## Remaining before Yaagl installation

The improved native scheduling configuration still needs a complete variable-GPU
run. A partial run observed GPU work around 0.16–2.4 ms, but display sleep cut it
short. The final demo includes a heavier workload intended to reach about 10 ms;
that run currently has no usable on-screen data and is not claimed as validated.
Waking the display programmatically did not restore visible presentations; the
Mac needs to be awake and unlocked for the remaining run.

After that, the display-link schedule must be bridged into the Wine/D3D11 demo
and tested there: admit work from a scheduling opportunity, preserve the
application's synchronization choice, submit the corresponding drawable before
its deadline, and keep presentation feedback separate from GPU resource cleanup.
A callback counter alone does not provide that scheduling opportunity. No fixed
GPU-time guess is proposed. The encoder fix is independently supported; the
complete low-latency DXMT presentation path is not yet ready to install.
