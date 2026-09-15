# Display-driven pacing experiment

Windows gets a freshly made frame onto the screen quickly. We now have a native
Metal reference that does so on this Mac while preserving 60 FPS and even spacing.
The next step is to make display timing control when the game produces its next
frame through DXGI, using this measured reference.

## Verified reference

On an M5 Pro, macOS 26.6.2, in the original 120-Hz/ProMotion mode, ordinary
CADisplayLink at 60 callbacks/s wakes the render thread before input sampling.
The renderer then uses ordinary presentation on an independently acquired drawable.
It retains three drawables, acquires the next drawable at the end of the previous
iteration, and bounds outstanding presentations at two using presented callbacks.

The 45-second run produced 60 FPS; all 2519 warm display intervals were 16.67 ms.
Median present delay was 5.76 ms and input sampling to reported display was 7.92 ms.
Instruments independently matched 660/660 warm frames to distinct Direct hardware
display events with even spacing. The independent-timer control retained uneven
spacing. Explicit future target times were steady but increased delay.

The fixed physical 60-Hz boundary test remained around 30.70-ms present delay.
This is not a universal Metal latency floor or a completed DXMT/HSR fix. Workload
was a constant 2-ms CPU simulation and light GPU clear. More expensive or variable
rendering, window transitions, and the Wine game loop remain unverified.

The two-mode reproducer is in [tests/presentation/native](../tests/presentation/native).
The original measurements and Instruments recordings are retained in the local
task's `outputs/native-display-pacing/` directory; recordings are not build inputs.

## Retained DXMT changes

- The waitable swapchain returns frame capacity after actual presentation or a
  dropped/failed drawable, rather than while recording a future presentation.
- Native callback state has explicit cancellation and ownership. GPU success
  does not return presentation capacity; error and presentation callbacks return
  it at most once. A Wine-owned worker performs Win32 signaling.
- Waitable swapchains use their own frame capacity without the separate device
  GPU throttle. Nonwaitable swapchains retain the original GPU-completion throttle.
- The device default is restored to three frames. DXGI's zero-means-default and
  upper-limit validation are retained.
- The encoder uses WaitOnAddress on Windows to avoid the measured polling delay
  in this toolchain's atomic wait fallback.

The waitable correction fixes an early signal, but waiting only for completed
presentations did not by itself deliver the desired frame-start schedule. The
native display-driven wakeup is not yet wired into DXMT.

## Removed experiments

- The automatically loaded Objective-C hook that extended GPU status/wait calls
  until presentation, including its timeout fallback.
- The forced one-frame device default and optional device-wide wait for actual
  presentations, which reduced queued work but could reduce frame rate.
- Observer overrides for presentation methods, drawable count, VSync, and pending
  frames; the remaining observer only records timing.
- The older CAMetalDisplayLink demo, layer-setting runner, old implementation plan,
  and superseded timing guide. The native reference keeps only the successful
  display wakeup and independent-timer control.
- The D3D11 diagnostic's external timer cap and alternative wait modes.

The pre-cleanup state, including the previously uncommitted changes, is preserved
in commit `d79e6ff`. The original `hsr-presentation-feedback` branch is unchanged.
No game, Yaagl runtime, or original Wineprefix files are changed by this cleanup.

## Next work

Bring the native frame-start signal to the game thread through DXGI. Keep actual
presentation counts and GPU completion distinct. Verify input age, distinct FPS,
displayed intervals, and outstanding frames together before installing anything.
Do not reintroduce 120-callback skipping, layer-property sweeps, or timed-present
offsets to compensate for an unexplained schedule.

## Cleanup verification

The full Wine cross-build and builtin-DLL postprocessing passed. All five native
presentation-counter checks passed. The built WineMetal library has no old bridge
initializer or destructor and still exports the presentation-counter functions.
The passive observer and simplified D3D11 diagnostic compiled successfully.

In the isolated diagnostic Wine runtime, both eight-second smoke runs exited
normally: the waitable path recorded 321 frames/callbacks and the legacy path
487 frames/callbacks. These check loading, progress, and teardown, not a claim of
low game latency. Dropped startup/teardown presentations remain in the raw logs.

The trimmed native reference was rebuilt and rerun for 14 seconds: 660/660 warm
frames displayed, all 659 intervals at 16.67 ms, 60 FPS, 5.68-ms median present
delay, and 7.84-ms median input-to-display. No warm display-link updates were
missed. The original Instruments verification is unchanged; no new Instruments
capture was needed for removal of the unused modes.
