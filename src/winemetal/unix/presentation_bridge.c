// Experimental only: extend DXMT's GPU-finish wait to actual presentation.
// Built into winemetal.so; only calls originating from that module
// observe the extended completion. Metal and the measurement observer see the
// original GPU status. A production change should use a separate display fence.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

@interface DXMTPresentationCompletion : NSObject {
@public
    dispatch_group_t group;
    atomic_bool finished;
}
- (void)finish;
@end
@implementation DXMTPresentationCompletion
- (instancetype)init {
    self = [super init];
    if (self) {
        atomic_init(&finished, false);
        group = dispatch_group_create();
        dispatch_group_enter(group);
    }
    return self;
}
- (void)finish {
    if (!atomic_exchange(&finished, true)) dispatch_group_leave(group);
}
@end

static char state_key;
static IMP original_status, original_wait, original_plain, original_minimum, original_at;
static atomic_ulong tracked, waits, timeouts, deferred_statuses;
static _Thread_local int nesting;

static bool from_winemetal(void *address) {
    Dl_info info;
    if (!dladdr(address, &info) || !info.dli_fname) return false;
    const char *leaf = strrchr(info.dli_fname, '/');
    return strcmp(leaf ? leaf + 1 : info.dli_fname, "winemetal.so") == 0;
}

static void track(id buffer, id<MTLDrawable> drawable) {
    if (nesting || !drawable) return;
    DXMTPresentationCompletion *state = [DXMTPresentationCompletion new];
    objc_setAssociatedObject(buffer, &state_key, state, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    atomic_fetch_add(&tracked, 1);
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
        (void)shown;
        // A skipped drawable (presentedTime == 0) also returns capacity.
        [state finish];
    }];
}

static MTLCommandBufferStatus status(id self, SEL selector) {
    MTLCommandBufferStatus actual = ((MTLCommandBufferStatus (*)(id, SEL))original_status)(self, selector);
    if (actual != MTLCommandBufferStatusCompleted ||
        !from_winemetal(__builtin_return_address(0))) return actual;
    DXMTPresentationCompletion *state = objc_getAssociatedObject(self, &state_key);
    if (state && !atomic_load(&state->finished)) {
        atomic_fetch_add(&deferred_statuses, 1);
        return MTLCommandBufferStatusScheduled;
    }
    return actual;
}

static void wait_completed(id self, SEL selector) {
    bool bridge_call = from_winemetal(__builtin_return_address(0));
    ((void (*)(id, SEL))original_wait)(self, selector);
    if (!bridge_call) return;
    DXMTPresentationCompletion *state = objc_getAssociatedObject(self, &state_key);
    if (!state || atomic_load(&state->finished)) return;
    atomic_fetch_add(&waits, 1);
    if (dispatch_group_wait(state->group, dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC))) {
        atomic_fetch_add(&timeouts, 1);
        fprintf(stderr, "Presentation bridge: display callback timed out; releasing experimental wait\n");
        [state finish];
    }
}

static void plain(id self, SEL selector, id<MTLDrawable> drawable) {
    track(self, drawable); nesting++;
    ((void (*)(id, SEL, id))original_plain)(self, selector, drawable);
    nesting--;
}
static void minimum(id self, SEL selector, id<MTLDrawable> drawable, double duration) {
    track(self, drawable); nesting++;
    ((void (*)(id, SEL, id, double))original_minimum)(self, selector, drawable, duration);
    nesting--;
}
static void at(id self, SEL selector, id<MTLDrawable> drawable, double time) {
    track(self, drawable); nesting++;
    ((void (*)(id, SEL, id, double))original_at)(self, selector, drawable, time);
    nesting--;
}

static IMP replace(Class type, SEL selector, IMP hook) {
    Method method = class_getInstanceMethod(type, selector);
    if (!method) return NULL;
    IMP previous = method_getImplementation(method);
    class_replaceMethod(type, selector, hook, method_getTypeEncoding(method));
    return previous;
}

__attribute__((constructor)) static void initialize_bridge(void) {
    const char *enabled = getenv("DXMT_PRESENTATION_BRIDGE");
    if (enabled && strcmp(enabled, "0") == 0) return;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        if (!buffer) return;
        Class type = object_getClass(buffer);
        original_status = replace(type, @selector(status), (IMP)status);
        original_wait = replace(type, @selector(waitUntilCompleted), (IMP)wait_completed);
        original_plain = replace(type, @selector(presentDrawable:), (IMP)plain);
        original_minimum = replace(type, @selector(presentDrawable:afterMinimumDuration:), (IMP)minimum);
        original_at = replace(type, @selector(presentDrawable:atTime:), (IMP)at);
        fprintf(stderr, "Presentation bridge: enabled for winemetal.so on %s\n", class_getName(type));
    }
}

__attribute__((destructor)) static void finish_bridge(void) {
    if (atomic_load(&tracked)) fprintf(stderr,
        "Presentation bridge totals: tracked=%lu waits=%lu deferred_statuses=%lu timeouts=%lu\n",
        atomic_load(&tracked), atomic_load(&waits), atomic_load(&deferred_statuses), atomic_load(&timeouts));
}
