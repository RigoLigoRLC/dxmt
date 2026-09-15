#import <Cocoa/Cocoa.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>
#import <Metal/Metal.h>
#include <stdatomic.h>
#include "presentation_feedback.h"

@interface WMTPresentationFeedback : NSObject {
  NSCondition *condition;
  uint64_t completed;
  bool cancelled;
  CADisplayLink *display_link;
  double frame_rate;
  bool display_configured, display_paced;
  uint32_t frame_limit;
  uint64_t submitted, ready_submission;
  struct WMTFramePacingUpdate state;
  FILE *trace;
}
- (uint64_t)waitAfter:(uint64_t)previous;
- (void)complete;
- (void)cancel;
- (BOOL)configureLayer:(CAMetalLayer *)layer fps:(double)fps limit:(uint32_t)limit;
- (void)setFrameLimit:(uint32_t)limit;
- (void)submit;
- (void)grantFrame;
- (void)tickAt:(double)timestamp deadline:(double)deadline;
- (BOOL)waitUpdate:(uint64_t)previous result:(struct WMTFramePacingUpdate *)result;
@end

@implementation WMTPresentationFeedback
- (instancetype)init {
  if ((self = [super init])) {
    condition = [NSCondition new];
    frame_limit = 1;
    display_paced = true;
    const char *base = getenv("DXMT_PACING_TRACE");
    if (base) {
      char path[4096];
      snprintf(path, sizeof(path), "%s-%d-%p.csv", base, getpid(), (__bridge void *)self);
      trace = fopen(path, "w");
      if (trace) {
        setvbuf(trace, NULL, _IOLBF, 0);
        fprintf(trace, "event,version,completed,submitted,ready,tick,timestamp_s,deadline_s,arrival_s\n");
      }
    }
  }
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
    state.completed = completed;
    if (!display_paced) [self grantFrame];
    ++state.version;
    [condition broadcast];
  }
  [condition unlock];
}
- (void)cancel {
  [condition lock];
  cancelled = true;
  [condition broadcast];
  [condition unlock];
  // Break the display-link/target retain cycle on the AppKit thread.
  void (^stop)(void) = ^{
    [display_link invalidate];
    display_link = nil;
  };
  if ([NSThread isMainThread]) stop();
  else dispatch_sync(dispatch_get_main_queue(), stop);
}
- (void)dealloc {
  if (trace) fclose(trace);
}
- (void)setFrameLimit:(uint32_t)limit {
  [condition lock];
  frame_limit = MAX(1, limit);
  if (!display_paced) [self grantFrame];
  [condition unlock];
}
- (BOOL)configureLayer:(CAMetalLayer *)layer fps:(double)fps limit:(uint32_t)limit {
  if (!isfinite(fps) || fps < 0 || fps > 1000) return NO;
  [self setFrameLimit:limit];
  [condition lock];
  BOOL unchanged = display_configured && frame_rate == fps;
  [condition unlock];
  if (unchanged) return YES;
  __block BOOL success = NO;
  void (^configure)(void) = ^{
    // Present(0) without a frame-rate cap needs only queue capacity, not a
    // display clock. Keep the existing link paused for a later VSync request.
    if (fps == 0) {
      display_link.paused = YES;
      success = YES;
      return;
    }
    if (@available(macOS 14.0, *)) {
      if (!display_link) {
        NSView *view = [layer.delegate isKindOfClass:NSView.class] ? (NSView *)layer.delegate : nil;
        if (!view.window) return;
        display_link = [view.window displayLinkWithTarget:self selector:@selector(displayTick:)];
        [display_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
      }
      display_link.preferredFrameRateRange = CAFrameRateRangeMake(fps, fps, fps);
      display_link.paused = NO;
      success = display_link != nil;
    }
  };
  if ([NSThread isMainThread]) configure();
  else dispatch_sync(dispatch_get_main_queue(), configure);
  if (success) {
    [condition lock];
    display_configured = true;
    display_paced = fps > 0;
    frame_rate = fps;
    if (!display_paced) [self grantFrame];
    [condition unlock];
  }
  return success;
}
// Called with condition held. A pending permission is never multiplied by
// extra display callbacks while the application is idle.
- (void)grantFrame {
  if (!cancelled && submitted > ready_submission && submitted - completed < frame_limit) {
    ready_submission = submitted;
    ++state.ready;
    ++state.version;
    [condition broadcast];
  }
}
- (void)displayTick:(CADisplayLink *)link {
  [self tickAt:link.timestamp deadline:link.targetTimestamp];
}
- (void)tickAt:(double)timestamp deadline:(double)deadline {
  [condition lock];
  if (!cancelled) {
    ++state.tick;
    state.arrival = CACurrentMediaTime();
    state.deadline = deadline;
    // One fresh frame-start permission, only after the previous frame was
    // submitted and actual presentation leaves room for another frame.
    // Neither GPU completion nor a display tick on its own returns capacity.
    if (display_paced) [self grantFrame];
    if (trace) fprintf(trace, "tick,%llu,%llu,%llu,%llu,%llu,%.9f,%.9f,%.9f\n",
        state.version, completed, submitted, state.ready, state.tick, timestamp, deadline, state.arrival);
  }
  [condition unlock];
}
- (void)submit {
  [condition lock];
  ++submitted;
  if (!display_paced) [self grantFrame];
  [condition unlock];
}
- (BOOL)waitUpdate:(uint64_t)previous result:(struct WMTFramePacingUpdate *)result {
  [condition lock];
  while (!cancelled && state.version <= previous) [condition wait];
  *result = state;
  BOOL success = !cancelled;
  [condition unlock];
  return success;
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

bool WMTNativePresentationFenceConfigureDisplayLink(void *fence, void *layer, double fps, uint32_t max_latency) {
  return [(__bridge WMTPresentationFeedback *)fence configureLayer:(__bridge CAMetalLayer *)layer fps:fps limit:max_latency];
}
void WMTNativePresentationFenceSubmit(void *fence) {
  [(__bridge WMTPresentationFeedback *)fence submit];
}
bool WMTNativePresentationFenceWaitUpdate(void *fence, uint64_t previous, struct WMTFramePacingUpdate *update) {
  return [(__bridge WMTPresentationFeedback *)fence waitUpdate:previous result:update];
}
void WMTNativePresentationFenceSetFrameLimit(void *fence, uint32_t max_latency) {
  [(__bridge WMTPresentationFeedback *)fence setFrameLimit:max_latency];
}
void WMTNativePresentationFenceDisplayTick(void *fence, double timestamp, double deadline) {
  [(__bridge WMTPresentationFeedback *)fence tickAt:timestamp deadline:deadline];
}

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
