// Assertions perform the checks even when the project is built in release mode.
#ifdef NDEBUG
#undef NDEBUG
#endif
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <assert.h>
#include "presentation_feedback.h"

// These objects expose only the callbacks used by the production tracker. Tests
// control GPU and display completion independently, without needing a window.
@interface TestDrawable : NSObject
@property(copy) MTLDrawablePresentedHandler presentedHandler;
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler;
- (void)displayOrDrop;
@end
@implementation TestDrawable
- (void)addPresentedHandler:(MTLDrawablePresentedHandler)handler { self.presentedHandler = handler; }
- (void)displayOrDrop { self.presentedHandler((id<MTLDrawable>)self); }
@end

@interface TestCommandBuffer : NSObject
@property MTLCommandBufferStatus status;
@property(copy) MTLCommandBufferHandler completedHandler;
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler;
- (void)complete:(MTLCommandBufferStatus)status;
@end
@implementation TestCommandBuffer
- (void)addCompletedHandler:(MTLCommandBufferHandler)handler { self.completedHandler = handler; }
- (void)complete:(MTLCommandBufferStatus)status {
  self.status = status;
  self.completedHandler((id<MTLCommandBuffer>)self);
}
@end

static dispatch_semaphore_t wait_async(void *fence, uint64_t previous, uint64_t *result) {
  dispatch_semaphore_t finished = dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
    @autoreleasepool {
      *result = WMTNativePresentationFenceWait(fence, previous);
      dispatch_semaphore_signal(finished);
    }
  });
  return finished;
}

static void expect_blocked(dispatch_semaphore_t finished) {
  assert(dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_MSEC)) != 0);
}

static void expect_finished(dispatch_semaphore_t finished) {
  assert(dispatch_semaphore_wait(finished, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) == 0);
}

static void track(void *fence, TestDrawable *drawable, TestCommandBuffer *buffer) {
  WMTNativePresentationFenceTrack(fence, (__bridge void *)drawable, (__bridge void *)buffer);
}

static void release_fence(void *fence) { (void)CFBridgingRelease(fence); }

static void test_gpu_completion_does_not_release_capacity(void) {
  void *fence = WMTNativePresentationFenceCreate();
  TestDrawable *drawable = [TestDrawable new];
  TestCommandBuffer *buffer = [TestCommandBuffer new];
  track(fence, drawable, buffer);
  __block uint64_t value = 0;
  dispatch_semaphore_t finished = wait_async(fence, 0, &value);
  expect_blocked(finished); // Recording a present does not grant another frame.
  [buffer complete:MTLCommandBufferStatusCompleted];
  expect_blocked(finished); // Neither does finishing its GPU work.
  [drawable displayOrDrop];
  expect_finished(finished);
  assert(value == 1);
  WMTNativePresentationFenceCancel(fence);
  release_fence(fence);
}

static void test_drop_and_error_return_one_credit(void) {
  void *fence = WMTNativePresentationFenceCreate();
  TestDrawable *drawable = [TestDrawable new];
  TestCommandBuffer *buffer = [TestCommandBuffer new];
  track(fence, drawable, buffer);
  [buffer complete:MTLCommandBufferStatusError];
  assert(WMTNativePresentationFenceWait(fence, 0) == 1);
  [drawable displayOrDrop]; // A later drop callback must not return a second credit.
  __block uint64_t value = 0;
  dispatch_semaphore_t finished = wait_async(fence, 1, &value);
  expect_blocked(finished);
  WMTNativePresentationFenceCancel(fence);
  expect_finished(finished);
  assert(value == UINT64_MAX);
  release_fence(fence);
}

static void test_out_of_order_callbacks_count_each_frame(void) {
  void *fence = WMTNativePresentationFenceCreate();
  TestDrawable *first = [TestDrawable new], *second = [TestDrawable new];
  TestCommandBuffer *a = [TestCommandBuffer new], *b = [TestCommandBuffer new];
  track(fence, first, a);
  track(fence, second, b);
  [second displayOrDrop];
  assert(WMTNativePresentationFenceWait(fence, 0) == 1);
  [first displayOrDrop];
  assert(WMTNativePresentationFenceWait(fence, 1) == 2);
  WMTNativePresentationFenceCancel(fence);
  release_fence(fence);
}

static void test_cancel_and_late_callback(void) {
  void *fence = WMTNativePresentationFenceCreate();
  TestDrawable *drawable = [TestDrawable new];
  TestCommandBuffer *buffer = [TestCommandBuffer new];
  track(fence, drawable, buffer);
  __block uint64_t value = 0;
  dispatch_semaphore_t finished = wait_async(fence, 0, &value);
  WMTNativePresentationFenceCancel(fence);
  expect_finished(finished);
  assert(value == UINT64_MAX);
  release_fence(fence);
  [drawable displayOrDrop]; // The callback owns native state after swapchain teardown.
  [buffer complete:MTLCommandBufferStatusError];
}

static void test_swapchains_have_independent_capacity(void) {
  void *a = WMTNativePresentationFenceCreate(), *b = WMTNativePresentationFenceCreate();
  TestDrawable *drawable = [TestDrawable new];
  TestCommandBuffer *buffer = [TestCommandBuffer new];
  track(a, drawable, buffer);
  __block uint64_t value = 0;
  dispatch_semaphore_t finished = wait_async(b, 0, &value);
  [drawable displayOrDrop];
  assert(WMTNativePresentationFenceWait(a, 0) == 1);
  expect_blocked(finished);
  WMTNativePresentationFenceCancel(b);
  expect_finished(finished);
  assert(value == UINT64_MAX);
  WMTNativePresentationFenceCancel(a);
  release_fence(a);
  release_fence(b);
}


struct UpdateResult {
  bool success;
  struct WMTFramePacingUpdate state;
};

static dispatch_semaphore_t update_async(void *fence, uint64_t previous, struct UpdateResult *result) {
  dispatch_semaphore_t finished = dispatch_semaphore_create(0);
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
    @autoreleasepool {
      result->success = WMTNativePresentationFenceWaitUpdate(fence, previous, &result->state);
      dispatch_semaphore_signal(finished);
    }
  });
  return finished;
}

static void test_frame_start_needs_capacity_and_display_tick(void) {
  void *fence = WMTNativePresentationFenceCreate();
  TestDrawable *drawable = [TestDrawable new];
  TestCommandBuffer *buffer = [TestCommandBuffer new];
  WMTNativePresentationFenceSetFrameLimit(fence, 1);
  WMTNativePresentationFenceSubmit(fence);
  track(fence, drawable, buffer);
  struct UpdateResult result = {0};
  dispatch_semaphore_t finished = update_async(fence, 0, &result);
  WMTNativePresentationFenceDisplayTick(fence, 1.0, 1.016);
  expect_blocked(finished); // The display tick cannot overrun the full queue.
  [buffer complete:MTLCommandBufferStatusCompleted];
  expect_blocked(finished); // A successful GPU completion does not empty it.
  [drawable displayOrDrop];
  expect_finished(finished);
  assert(result.success && result.state.completed == 1 && result.state.ready == 0);
  WMTNativePresentationFenceDisplayTick(fence, 1.016, 1.032);
  assert(WMTNativePresentationFenceWaitUpdate(fence, result.state.version, &result.state));
  assert(result.state.ready == 1 && result.state.tick == 2);
  finished = update_async(fence, result.state.version, &result);
  WMTNativePresentationFenceDisplayTick(fence, 1.032, 1.048);
  expect_blocked(finished); // Don't accumulate credits while the app is idle.
  WMTNativePresentationFenceCancel(fence);
  expect_finished(finished);
  assert(!result.success);
  release_fence(fence);
}

static void test_frame_limit_allows_overlap_without_extra_credits(void) {
  void *fence = WMTNativePresentationFenceCreate();
  WMTNativePresentationFenceSetFrameLimit(fence, 2);
  TestDrawable *a = [TestDrawable new], *b = [TestDrawable new];
  TestCommandBuffer *ca = [TestCommandBuffer new], *cb = [TestCommandBuffer new];
  WMTNativePresentationFenceSubmit(fence);
  track(fence, a, ca);
  WMTNativePresentationFenceDisplayTick(fence, 1, 2);
  struct WMTFramePacingUpdate state = {0};
  assert(WMTNativePresentationFenceWaitUpdate(fence, 0, &state));
  assert(state.ready == 1 && state.completed == 0);
  WMTNativePresentationFenceSubmit(fence);
  track(fence, b, cb);
  struct UpdateResult result = {0};
  dispatch_semaphore_t finished = update_async(fence, state.version, &result);
  WMTNativePresentationFenceDisplayTick(fence, 2, 3);
  expect_blocked(finished); // Both presentation slots are now occupied.
  [a displayOrDrop];
  expect_finished(finished);
  assert(result.state.completed == 1 && result.state.ready == 1);
  WMTNativePresentationFenceDisplayTick(fence, 3, 4);
  assert(WMTNativePresentationFenceWaitUpdate(fence, result.state.version, &state));
  assert(state.ready == 2);
  WMTNativePresentationFenceCancel(fence);
  release_fence(fence);
  [b displayOrDrop];
}

static void test_uncapped_waits_for_capacity_without_display_ticks(void) {
  void *fence = WMTNativePresentationFenceCreate();
  assert(WMTNativePresentationFenceConfigureDisplayLink(fence, NULL, 0, 2));
  TestDrawable *a = [TestDrawable new], *b = [TestDrawable new];
  TestCommandBuffer *ca = [TestCommandBuffer new], *cb = [TestCommandBuffer new];
  WMTNativePresentationFenceSubmit(fence);
  track(fence, a, ca);
  struct WMTFramePacingUpdate state = {0};
  assert(WMTNativePresentationFenceWaitUpdate(fence, 0, &state));
  assert(state.ready == 1 && state.completed == 0);
  WMTNativePresentationFenceSubmit(fence);
  track(fence, b, cb);
  struct UpdateResult result = {0};
  dispatch_semaphore_t finished = update_async(fence, state.version, &result);
  expect_blocked(finished); // Full even though no display clock is requested.
  [ca complete:MTLCommandBufferStatusCompleted];
  expect_blocked(finished);
  [a displayOrDrop];
  expect_finished(finished); // Capacity returns without waiting for a tick.
  assert(result.state.ready == 2 && result.state.completed == 1);
  finished = update_async(fence, result.state.version, &result);
  WMTNativePresentationFenceDisplayTick(fence, 3, 4);
  expect_blocked(finished); // Paused display callbacks cannot add a credit.
  WMTNativePresentationFenceCancel(fence);
  expect_finished(finished);
  assert(!result.success);
  release_fence(fence);
  [b displayOrDrop];
}

int main(void) {
  @autoreleasepool {
    test_gpu_completion_does_not_release_capacity();
    test_drop_and_error_return_one_credit();
    test_out_of_order_callbacks_count_each_frame();
    test_cancel_and_late_callback();
    test_swapchains_have_independent_capacity();
    test_frame_start_needs_capacity_and_display_tick();
    test_frame_limit_allows_overlap_without_extra_credits();
    test_uncapped_waits_for_capacity_without_display_ticks();
    puts("PASS: 8 presentation feedback tests");
  }
}
