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
2. Tie the swapchain waitable object to actual presentation capacity. Its existing
   semaphore is released while recording the presentation command. This experiment
   affects the device-level fence, not that separate waitable-object path.
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
