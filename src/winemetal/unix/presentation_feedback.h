#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../frame_pacing.h"

bool WMTNativePresentationFenceConfigureDisplayLink(void *fence, void *layer, double fps, uint32_t max_latency);
void WMTNativePresentationFenceSubmit(void *fence);
bool WMTNativePresentationFenceWaitUpdate(void *fence, uint64_t previous, struct WMTFramePacingUpdate *update);
void WMTNativePresentationFenceSetFrameLimit(void *fence, uint32_t max_latency);
void WMTNativePresentationFenceDisplayTick(void *fence, double timestamp, double deadline);


// Returned objects follow NSObject ownership: Create returns a retained object.
void *WMTNativePresentationFenceCreate(void);
// Returns the new completion count, or UINT64_MAX after cancellation.
uint64_t WMTNativePresentationFenceWait(void *fence, uint64_t previous);
void WMTNativePresentationFenceCancel(void *fence);
void WMTNativePresentationFenceTrack(void *fence, void *drawable, void *command_buffer);
