#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include "presentation_feedback.h"

@interface WMTPresentationFeedback : NSObject {
  NSCondition *condition;
  uint64_t completed;
  bool cancelled;
}
- (uint64_t)waitAfter:(uint64_t)previous;
- (void)complete;
- (void)cancel;
@end

@implementation WMTPresentationFeedback
- (instancetype)init {
  if ((self = [super init]))
    condition = [NSCondition new];
  return self;
}
- (uint64_t)waitAfter:(uint64_t)previous {
  [condition lock];
  while (!cancelled && completed <= previous)
    [condition wait];
  uint64_t result = cancelled ? UINT64_MAX : completed;
  [condition unlock];
  return result;
}
- (void)complete {
  [condition lock];
  if (!cancelled) {
    ++completed;
    [condition broadcast];
  }
  [condition unlock];
}
- (void)cancel {
  [condition lock];
  cancelled = true;
  [condition broadcast];
  [condition unlock];
}
@end

@interface WMTPresentationTicket : NSObject {
  WMTPresentationFeedback *feedback;
  atomic_bool finished;
}
- (instancetype)initWithFeedback:(WMTPresentationFeedback *)value;
- (void)finish;
@end

@implementation WMTPresentationTicket
- (instancetype)initWithFeedback:(WMTPresentationFeedback *)value {
  if ((self = [super init])) {
    feedback = value;
    atomic_init(&finished, false);
  }
  return self;
}
- (void)finish {
  if (!atomic_exchange_explicit(&finished, true, memory_order_relaxed))
    [feedback complete];
}
@end

void *WMTNativePresentationFenceCreate(void) {
  return (__bridge_retained void *)[WMTPresentationFeedback new];
}

uint64_t WMTNativePresentationFenceWait(void *fence, uint64_t previous) {
  return [(__bridge WMTPresentationFeedback *)fence waitAfter:previous];
}

void WMTNativePresentationFenceCancel(void *fence) {
  [(__bridge WMTPresentationFeedback *)fence cancel];
}

void WMTNativePresentationFenceTrack(void *fence, void *drawable_handle, void *command_buffer) {
  if (!fence)
    return;
  WMTPresentationTicket *ticket = [[WMTPresentationTicket alloc]
      initWithFeedback:(__bridge WMTPresentationFeedback *)fence];
  id<MTLDrawable> drawable = (__bridge id<MTLDrawable>)drawable_handle;
  id<MTLCommandBuffer> buffer = (__bridge id<MTLCommandBuffer>)command_buffer;
  if (!drawable || !buffer) {
    [ticket finish];
    return;
  }
  [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
    (void)shown; // Dropped drawables also return capacity (presentedTime == 0).
    [ticket finish];
  }];
  [buffer addCompletedHandler:^(id<MTLCommandBuffer> completed_buffer) {
    // Successful GPU completion is deliberately not presentation completion.
    // Errors may prevent a drawable callback; return this credit exactly once.
    if (completed_buffer.status == MTLCommandBufferStatusError)
      [ticket finish];
  }];
}
