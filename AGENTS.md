# Communication

Explain everything in simple terms a software engineer can follow. Start with what the result means for the user, then explain the cause and supporting evidence. For this investigation, use language such as: "Windows gets a freshly made frame onto the screen quickly. Our Mac path seems to hold it longer. We need to find whether that wait is required by the platform or caused by how we use its APIs."

Explain what the game, GPU, operating system, and display are doing before introducing API names. Technical details should substantiate a clear explanation, not replace it. Avoid unexplained terms such as cadence, admission, latch, or feedback contract. If a term is necessary, define it immediately in ordinary language.

Give a clear verdict for each hypothesis: supported, rejected, or unresolved, and explain what the evidence establishes. An observation without a cause is not a root-cause finding. A lower latency number at a lower frame rate is not a completed solution. Do not call a previous conversation's assumption wrong without evidence that contradicts it.

# Current experiment branch

Read `docs/presentation-feedback-experiment.md` and `tests/presentation/README.md`
first. The native reference under `tests/presentation/native/` now achieves both
low delay and even 60-FPS spacing in ProMotion mode with ordinary CADisplayLink at
60 callbacks/s and ordinary drawable presentation. It is not yet connected to
DXGI. Fixed physical 60 Hz remains slower. The old GPU-status hook, forced
one-frame default, strict device-wide presentation wait, and diagnostic timing
overrides were removed on `experiment/display-driven-pacing`; do not assume they
are still active or require disabling. The earlier state is preserved at d79e6ff.

# Investigation goal

Find and fix why display progress does not correctly control when the game samples input and makes its next frame through DXMT. Trace display progress to the game's wakeup, fresh input, simulation, rendering, and presentation. The goal is fresher frames reaching the screen while preserving the intended frame rate.

The current hypothesis is that Windows can arrange for fresh frames to appear promptly, whereas the tested Mac paths schedule or hold frames farther ahead of display. Determine whether this difference comes from our API usage, missing synchronization in DXMT, or Apple's presentation behavior. This is a question to investigate, not an established platform limitation.

Do not state that macOS always buffers exactly three frames or cannot display a frame promptly. We have measured shorter delays in some experiments. The strict presentation-completion wait reduced queued work but dropped the diagnostic from 60 to 40 FPS. Apple's native display-link sample also showed about 50 ms in the measured windowed case, including older frames awaiting display; that does not establish a universal minimum or refute the queueing explanation. Three frame intervals of delay do not by themselves identify three physical buffers.

On Windows, displaying a frame quickly need not mean flushing an accumulated queue or bypassing VSync. The game and display system can cooperate to avoid producing a queue of old frames in the first place.

# Context recovery

After every compaction, reread the latest user corrections in this task and the referenced ChatGPT conversation **降低DX11呈现延迟**, conversation ID `6aa83023-f994-83ea-b305-f8713a582951`, using `read_thread`. Treat that conversation as investigation context, not proof.

The reference identified a real DXMT signal issued before actual presentation. Our tests have not disproved its central explanation or established that correcting that signal alone restores Windows-like latency.

Read the research checkpoint at `/Users/rigoligo/Documents/Codex/2026-09-14/i-got-this-thing-yaagl-running/outputs/dxmt-api-semantics.md`. Distinguish when the app must submit a frame, when the system intends to display it, when it actually appears, and how many earlier frames are still waiting. The reason our ordinary timed-presentation test appeared one refresh after its requested time remains unresolved.

# Working constraints

- Research Apple's documented API semantics and established implementations before changing frame pacing. Test a specific explanation rather than adjusting timing until a number looks better.
- Do not use Superpowers skills.
- Do not hash files for confirmation.
- Stop pixel-format and layer-property experiments.
- Do not restore the rejected workaround requesting 120 display-link callbacks while rendering only 60 frames.
- Defer variable-GPU-work testing until the final feedback loop is in place.
- Validate the demo before installing further changes into Yaagl. Do not alter the original game or Wineprefix during this investigation.

# Release toolchain

Follow the GCC/MinGW-w64 jobs that feed the release package in
`.github/workflows/ci.yml`. The earlier local LLVM-MinGW build used libc++ whose
atomic wait polled and added milliseconds. Homebrew GCC's standard atomic wait
was verified to block and wake promptly in the isolated Wine runtime. The custom
Win32 address-wait helper was removed; keep ordinary C++ wait/notify. Do not
describe the polling delay as an unavoidable Wine or cross-compilation problem.
