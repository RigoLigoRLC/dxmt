# Waitable swapchain presentation feedback

Goal: release DXGI frame-latency credits when Metal presents or drops the
corresponding drawable, not while DXMT records its internal presentation command.
The user has explicitly requested fixing this path without first proving HSR uses it.

Use a reference-counted native completion counter, retained by drawable callbacks.
A worker belonging to each waitable swapchain waits on the counter, releases its
Win32 semaphore and advances its CPU admission fence. Metal callbacks never call
Win32 APIs. Cancellation wakes the worker during destruction; late callbacks retain
only native state. GPU success does not release a presentation credit; GPU failure
does, exactly once even if a drawable callback also arrives. Nonwaitable swapchains
do not allocate this state or start a worker.

This is independent of the experimental device-level bridge already on this branch.
Moving the signal merely to GPU completion would preserve the wrong boundary.
Using the existing native event-listener run loop would couple callback lifetime to
device teardown; a cancellable worker gives the swapchain explicit ownership.

Steps:
1. Add callback-order, cancellation, duplicate-completion and independent-swapchain
   tests using controlled Metal callback sources.
2. Implement the native counter and append four WineMetal unix calls, keeping
   existing call indices unchanged and supporting native and WoW64 layouts.
3. Carry retained presentation state through PresentData and register callbacks
   before Metal presentation. Move semaphore/fence signals to the swapchain worker.
4. Compile/run native regressions, check affected translation units and unix-call
   layouts, inspect the diff and commit on hsr-presentation-feedback.

No game launches or prefix edits are part of this change. A source commit alone
does not update Yaagl's installed binaries; a complete Wine cross-build is required.
