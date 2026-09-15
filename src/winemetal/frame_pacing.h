#pragma once
#include <stdint.h>

// A snapshot across the Wine/native boundary. All counters are per swapchain.
struct WMTFramePacingUpdate {
  uint64_t version;
  uint64_t completed;
  uint64_t ready;
  uint64_t tick;
  double arrival;
  double deadline;
};
