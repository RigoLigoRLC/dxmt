# HSR presentation-feedback experiment

This branch collects the presentation-latency experiment deployed with Yaagl and
DXMT v0.80. It starts from tag `v0.80`, matching the installed binaries.

## Changes

- `src/dxmt/dxmt_command_queue.hpp`: change the default device maximum frame latency
  from 3 to 1. This corresponds to the creation-time latency request used in the
  reference experiment. Applications can still change the value through DXGI.
- `src/winemetal/unix/presentation_bridge.c`: associate each presented Metal command
  buffer with a completion group and complete it from `addPresentedHandler`.
  For status/wait calls originating in `winemetal.so`, GPU completion also waits
  for presentation. Other callers continue to observe Metal's actual GPU status.
  Skipped presentations also complete the group. A 500 ms fallback releases a
  missing callback wait and prints a diagnostic.
- `src/winemetal/unix/meson.build`: compile the bridge with ARC in its own static
  target and link the entire target into WineMetal so its constructor is retained.
  Existing WineMetal sources retain their manual memory-management compilation.

The bridge activates automatically when WineMetal loads. Set
`DXMT_PRESENTATION_BRIDGE=0` to disable the hook; the changed queue default remains 1.
The Metal drawable pool and preferred frame-rate cap are unchanged.

## Relationship to the installed experiment

The installed quick patch changed the constructor instruction at file offset
`0x4244a` in v0.80's builtin `d3d11.dll` from
`c7 86 40 2d 00 00 03 00 00 00` to
`c7 86 40 2d 00 00 01 00 00 00`, and updated the PE checksum. The store initializes
`CommandQueue::max_latency_` at member offset `0x2d40`, also used by the device
maximum-frame-latency getter and setter.

Its `winemetal.so` has an added Mach-O load command for a standalone
`dxmt_presentation_bridge.dylib`, built from the same Objective-C bridge.
This branch expresses those changes as source and links the bridge directly into
WineMetal, avoiding a machine-specific dylib path in a rebuilt installation.
This source integration has not replaced the installed binaries.

No custom HSR launcher, registry edits, prefix copies, or game-file changes are
part of this branch. Yaagl continues to perform its usual game startup steps.

## Observations

The controlled D3D11 reference used the same device latency setting of 1 and the
same 60 FPS cap in both cases. After excluding the first three seconds:

| Case | Displayed FPS | Present request to display | GPU completion to display |
| --- | ---: | ---: | ---: |
| Original DXMT | 58.29 | 49.21 ms | 48.74 ms |
| Presentation callback bridge | 58.81 | 32.92 ms | 32.51 ms |

The bridge recorded 867 presentation waits and no callback timeouts in that run.
`nextDrawable()` waiting fell from about 16.68 ms to 0.04 ms, with three drawables
retained. A strict wait for the current frame lowered delay to about 23.55 ms but
reduced displayed throughput to about 40.46 FPS, so it was rejected.

After installation into Yaagl's DXMT bundle, the user reported about 32 ms and
then supplied an HSR HUD snapshot showing 60 FPS, 26.98 ms present delay, 8.08 ms
GPU time, and Composition: Composited. These HSR readings are user observations,
not a controlled same-scene comparison. They do not establish physical input to
photon latency. A separate Windows capture showed about 3.1 ms between GPU
completion and display; it does not identify the swapchain's buffer count.

## What remains

1. Separate presentation progress from GPU completion. The current hook delays
   `CommandQueue::WaitForFinishThread`, including resource retirement and the CPU
   coherence signal. A proper implementation needs an independent presentation
   fence so GPU-completed resources can be reused promptly.
2. Validate the separate waitable-swapchain fix below with real Wine applications.
   The installed quick binary patch still contains only the device-level bridge;
   rebuilding this branch is required to get the swapchain fix.
3. Reduce the remaining queue without losing the 60 FPS cadence. In this version,
   `PresentBoundary()` waits after submission; a device limit of 1 permits two
   submissions before the first wait. Waiting for the current frame lost FPS in
   the reference. Investigate the presentation schedule and measured render-time
   variation; do not assume a fixed render budget or a Metal latency floor.
4. Check variable workloads, multiple swapchains, occlusion, fullscreen transitions,
   callback loss, and shutdown. The native hook was exercised on x86_64 Wine on this
   Mac; other GPUs and architectures have not been verified.

The HUD's GPU and present-delay aggregates do not by themselves partition the
remaining delay into rendering, queued images, and display scheduling.

## Validation and rebuilding

The standalone bridge was compiled and exercised during the original experiment.
For this source collection, its native ARC object and force-loaded shared-library
link were checked with Apple Clang. A complete DXMT cross-build has not been run;
it still needs the Wine/Windows build inputs and native LLVM 15 described in the
repository README.

The branch contains no compiled binaries. Build DXMT through its normal Meson
configuration once those dependencies are available. For a native compile check:

```sh
clang -arch x86_64 -ObjC -fobjc-arc -fblocks -O2 -c \
  src/winemetal/unix/presentation_bridge.c -o /tmp/presentation_bridge.o
```

## Waitable swapchain follow-up

The source branch now also fixes the separate waitable-object path. `Present1`
no longer releases `present_semaphore_` while recording `ctx.present()`, and
`SyncFrameState` no longer reports completion when the encoder lambda is destroyed.
`PresentData` retains a native presentation counter and registers a ticket with
the actual Metal drawable before scheduling presentation. Successful GPU completion
leaves the ticket pending; a presented/dropped callback or command-buffer error
returns one credit. Duplicate error/drop callbacks cannot return two credits.

Each waitable swapchain owns a Wine-created worker that waits for counter changes,
releases semaphore credits and advances its internal admission fence. Cancellation
wakes and joins this worker before its semaphore is closed. Late Metal callbacks
own only retained native state. Swapchains without the waitable flag allocate no
counter or worker. Both ordinary and MetalFX presentation paths carry the counter.

This new path does not intercept GPU completion or delay GPU cleanup. The older
experimental device-level bridge remains independently enabled on this branch;
use `DXMT_PRESENTATION_BRIDGE=0` to isolate the waitable fix when rebuilding/testing.
Existing WineMetal unix-call indices are preserved, with four calls appended to
both the 64-bit and WoW64 tables. Rebuild and deploy matching `d3d11.dll`,
`winemetal.dll` and `winemetal.so`; mixing the new DLL with an old Unix library is
not supported. No updated binaries have been installed into Yaagl for this fix.

The callback regression suite uses controlled drawable/command-buffer objects to
separate recording, GPU completion and display completion. It covers normal GPU
completion remaining blocked, display/drop release, error/drop duplicate handling,
out-of-order callbacks, cancellation with late callbacks, and independent counters.
A negative-control build that returns a credit during registration fails the first
blocked-wait assertion. This checks the failure mechanism, not an end-to-end run
of the original DXMT binary. Run with Meson's `enable_tests` option, or directly:

```sh
clang -arch x86_64 -ObjC -fobjc-arc -fblocks -Wall -Wextra -Werror \
  -Isrc/winemetal/unix tests/winemetal/presentation_feedback.c \
  src/winemetal/unix/presentation_feedback.c \
  -framework Foundation -framework Metal -o /tmp/dxmt-presentation-feedback-test
/tmp/dxmt-presentation-feedback-test
```

The native callback tests and compilation checks do not establish an HSR latency
improvement. Real Wine presentation, transitions and full cross-build validation
remain to be performed. The local AddressSanitizer runtime hung during its own
startup before `main`; that attempt provides no sanitizer coverage.
