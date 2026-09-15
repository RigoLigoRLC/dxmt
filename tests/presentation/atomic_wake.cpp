#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

// Compare toolchains with the same delayed producer/consumer handoff.
// No rendering or display timing is involved in this test.
int main() {
  using Clock = std::chrono::steady_clock;
  constexpr unsigned sample_count = 120;
  std::atomic_uint64_t sequence{0};
  std::array<double, sample_count> samples{};
  Clock::time_point signaled_at;
  std::mutex ack_mutex;
  std::condition_variable ack_changed;
  unsigned acknowledged = 0;

  std::thread worker([&] {
    for (unsigned frame = 0; frame < sample_count; ++frame) {
      sequence.wait(frame, std::memory_order_acquire);
      const auto woke_at = Clock::now();
      samples[frame] = std::chrono::duration<double, std::milli>(woke_at - signaled_at).count();
      {
        std::lock_guard lock(ack_mutex);
        acknowledged = frame + 1;
      }
      ack_changed.notify_one();
    }
  });

  for (unsigned frame = 0; frame < sample_count; ++frame) {
    // Leave enough idle time to reach the library's blocking wait path.
    std::this_thread::sleep_for(std::chrono::milliseconds(10 + (frame * 7) % 21));
    signaled_at = Clock::now();
    sequence.store(frame + 1, std::memory_order_release);
    sequence.notify_one();
    std::unique_lock lock(ack_mutex);
    if (!ack_changed.wait_for(lock, std::chrono::seconds(2), [&] { return acknowledged == frame + 1; })) {
      std::fputs("atomic wake handoff timed out\n", stderr);
      // atomic::wait has no timed variant; don't hang joining a stuck worker.
      std::_Exit(2);
    }
  }
  worker.join();

  double total = 0;
  for (double sample : samples)
    total += sample;
  std::sort(samples.begin(), samples.end());
  std::printf("std::atomic::wait wake ms mean=%.4f median=%.4f p95=%.4f max=%.4f samples=%u\n",
              total / sample_count, samples[60], samples[114], samples[119], sample_count);
}
