#pragma once
#include <stdint.h>

// Returned objects follow NSObject ownership: Create returns a retained object.
void *WMTNativePresentationFenceCreate(void);
// Returns the new completion count, or UINT64_MAX after cancellation.
uint64_t WMTNativePresentationFenceWait(void *fence, uint64_t previous);
void WMTNativePresentationFenceCancel(void *fence);
void WMTNativePresentationFenceTrack(void *fence, void *drawable, void *command_buffer);
