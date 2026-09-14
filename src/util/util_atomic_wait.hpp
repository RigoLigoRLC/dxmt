#pragma once

#include <atomic>
#include <cstdint>
#ifdef _WIN32
#include <windows.h>
#endif

namespace dxmt {

// llvm-mingw's libc++ may implement atomic::wait by polling with Sleep.
// Use an address wake on Windows so newly recorded commands reach the encoder
// promptly even when the presentation queue is shallow and the encoder is idle.
inline void WaitForAtomicChange(std::atomic_uint64_t &sequence, uint64_t previous) {
#ifdef _WIN32
  static_assert(sizeof(sequence) == sizeof(previous));
  while (sequence.load(std::memory_order_acquire) == previous)
    WaitOnAddress(&sequence, &previous, sizeof(previous), INFINITE);
#else
  sequence.wait(previous, std::memory_order_acquire);
#endif
}

inline void NotifyAtomicChange(std::atomic_uint64_t &sequence) {
#ifdef _WIN32
  WakeByAddressSingle(&sequence);
#else
  sequence.notify_one();
#endif
}

} // namespace dxmt
