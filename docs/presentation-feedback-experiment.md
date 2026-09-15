# Display-driven DXGI pacing experiment

The tested Mac path can display fresh frames promptly at about 60 FPS. The old
path allowed the demo to make several frames before display progress stopped it.
The new path waits before the next frame's input and simulation, using the display
clock and the count of presentations still outstanding. This supports the missing
feedback explanation; it rejects an unavoidable three-frame Metal delay for the
tested fullscreen ProMotion condition. It does not establish a fix for HSR.

## What changed

Enable the experiment with `dxgi.displayLinkPacing = True` and select the desired
cap with the existing `d3d11.preferredMaxFrameRate` option. It defaults off.

1. `WMTPresentationFeedback::displayTick:` in
   `src/winemetal/unix/presentation_feedback.c` receives ordinary window
   CADisplayLink updates. At a 60-FPS cap it requests exactly 60 callbacks/s.
2. `grantFrame` permits the next frame only after the current frame was submitted
   and actual presentation leaves capacity. Extra callbacks cannot accumulate
   permissions while the application is idle. GPU completion does not free a slot.
3. The swapchain's Wine-owned worker transfers the native `ready` counter through
   `WMTPresentationFence_waitUpdate` to the DXGI waitable semaphore or an internal
   frame-start fence. Native callbacks never call Wine's Win32 signaling functions.
4. `MTLD3D11SwapChain::Present1` in `src/d3d11/d3d11_swapchain.cpp` waits at its end
   for nonwaitable games. Waitable games wait before input using their usual DXGI
   object. The diagnostic records input immediately after that wait, then simulates,
   renders and presents. The old device GPU-completion throttle is bypassed only
   for this opt-in path (or the already corrected waitable path).
5. Frames use ordinary drawable presentation, with VSync honoring nonzero
   `SyncInterval`. `WMTPresentationTicket::finish` counts each actual presentation,
   drop or GPU failure once. The next frame is then eligible at a display update.

The maximum remains the application's swapchain or device setting; the device
default is still three. There is no 120-callback skipping, time offset, reduced
drawable count, pixel-format experiment, or GPU-status hook. Uncapped `Present(0)`
pauses the display clock and waits only for presentation capacity.

## Verified D3D11 results through Wine

M5 Pro, macOS 26.6.2, original ProMotion mode, borderless fullscreen, 60-FPS cap,
`Present(1)`, fixed 2-ms CPU simulation. Each run lasted 14 seconds; the first three
and last one seconds are excluded. Input below means the diagnostic's software
input-sampling boundary, not a physical mouse-to-photon measurement. Present delay
starts at the Metal presentation request, later than the DXGI Present call.

| Case | Displayed FPS | Median present delay | Median input-to-display | Older frames at input |
| --- | ---: | ---: | ---: | ---: |
| Old path, nonwaitable, device maximum 3 | 60.00 | 41.33 ms | 107.07 ms | median 6, maximum 6 |
| Display-driven, same device maximum 3 | 59.98 | 7.56 ms | 9.93 ms | 0 throughout |
| Display-driven, waitable maximum 1 | 59.98 | 6.88 ms | 9.17 ms | 0 throughout |
| Display-driven, waitable maximum 2 | 59.83 | 13.55 ms | 16.00 ms | median 0, maximum 1 |
| Display-driven, nonwaitable maximum 3, variable GPU work | 60.05 | 13.59 ms | 15.96 ms | median 0, maximum 1 |

All 600 warm frames were displayed in the main old/new comparison, waitable-1 and
variable-work runs; waitable-2 displayed all 599 warm frames. Six older frames in
the old path means outstanding presentation requests, not six physical buffers.
The main new run's display update reached the input boundary in median 0.158 ms.

Variable GPU work was tested only after the full feedback loop was functioning.
Measured GPU execution ranged from 1.14 to 13.56 ms (median 4.49, p95 11.85 ms),
with p95 input-to-display 24.69 ms and all 600 warm frames shown. This tests changing
render cost within the frame budget, not sustained GPU overload or HSR's engine.

An independent Instruments Metal System Trace matched 596 of 600 warm frames to
596 distinct Direct hardware display events: median request-to-hardware-display
7.43 ms. Four frames had ambiguous trace mappings and remain unverified. The
hardware events also confirm the two display-spacing peaks; this is not merely
the callback timestamp histogram. The trace had address-mapping/signpost warnings.

## What the Windows comparison changed

The user's Windows HSR capture has 1,798 frames on a 160-Hz VRR-capable display.
Present calls average 16.667 ms apart, but actual display intervals cluster around
12.494 ms (33.37%) and 18.754 ms (66.46%). Present-to-display averages 4.444 ms.
The previous frame was already displayed before the next Present in all 1,797
transitions. The peaks fit two/three 6.25-ms refresh intervals; display capability
alone does not prove VRR was active during this capture.

Therefore a single exact 16.67-ms display-spacing peak is not an acceptance
requirement. Verify fresh input, short queues and sustained production together.
The Mac's 12.5/20.83-ms peaks have different proportions and need not share the
Windows mechanism. Windows HSR and this light Mac demo are different workloads;
their latency numbers are not a controlled platform comparison.

## Verification and limits

The full x64 GCC/MinGW release build and builtin-DLL processing passed. Eight
native tests cover successful GPU completion versus presentation, errors/drops,
out-of-order callbacks, cancellation, independent swapchains, display timing plus
capacity, overlap without extra permissions, and uncapped capacity without ticks.
They use controlled callbacks and need no visible window. The final uncapped-mode
handling passed these offscreen tests; its uncapped Wine path has not been run.
The capped path used for the measurements is unchanged by that last adjustment.

Earlier unsynchronized fullscreen numbers (3.66-ms delay at 63 FPS) are invalid:
the first integration left Metal VSync disabled while switching to plain present.
The accepted measurements honor VSync. The first GPU workload was accidentally
culled; its short GPU durations were not workload validation, and its dropped
frame/266-ms interval remain recorded. The corrected shader test disables culling.
Locked/asleep runs reporting zero presentation timestamps are also invalid.

Windowed operation, occlusion/minimization and display migration remain integration
limits. Earlier windowed tests did not reproduce fullscreen latency. Fixed physical
60 Hz remains slower in native controls. Multiple concurrent Present producers on
one swapchain and dynamic maximum-latency changes are not validated. The experiment
is opt-in while those behaviors and HSR remain unverified.

The last dark fullscreen workload completed normally after 14 seconds. The user
reported a black screen and subsequently confirmed the desktop returned. Further
verification in this session stays offscreen. Yaagl and its prefix are unchanged.

## Reproduction and provenance

See [tests/presentation/README.md](../tests/presentation/README.md) for compilation,
CLI arguments, the passive timing observer and the native reference. Use matching
`d3d11.dll`, `dxgi.dll`, `winemetal.dll` and native `winemetal.so` from one build.
The Wine/native call tables gained three entries; mixing old/new WineMetal is invalid.

Local evidence is retained in the task's `outputs/dxmt-display-pacing/` and
`outputs/frame-spacing/` directories. Raw Instruments recording remains in its
`work/display-pacing-verification/` directory; timing CSVs and hardware matches are
copied to outputs. No game files are build inputs or modified by these tests.

The native reference previously demonstrated 60 FPS and about 5.7-ms present delay
in a display-captured ProMotion window. Removing capture while keeping ordinary
borderless fullscreen increased its delay; moving drawable acquisition alone did
not explain that difference. No drawable-prefetch machinery was added to DXMT.

Earlier cleanup removed the forced one-frame default, GPU-status/wait hook,
diagnostic timing/layer overrides and strict device-wide presentation wait.
Commit `d79e6ff` preserves that history. The release CI uses Homebrew GCC/MinGW;
its standard atomic wait measured 0.051–0.057 ms, versus the previous LLVM-MinGW
libc++ polling path's 5.80–7.48 ms. Standard C++ wait/notify remains; no custom
Win32 address-wake wrapper or MSVC workflow was introduced. The local native
link uses Xcode 26.2's libc++ and libunwind; CI uses Xcode 16.1.
