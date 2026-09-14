#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdatomic.h>
#include <dispatch/dispatch.h>
#include <time.h>

static FILE *trace;
static atomic_ulong sequence;
static _Thread_local int nesting;
static IMP original_plain, original_minimum, original_at;
static IMP original_next;
static int drawable_count, limit_pending, force_sync=-1, present_mode;
static dispatch_semaphore_t presentation_slots;
static FILE *next_trace;
static void observe(id<MTLCommandBuffer> command, id<MTLDrawable> drawable, const char *kind, double duration, double target) {
    if (!trace || nesting) return;
    const double requested = CACurrentMediaTime();
    struct timespec request_wall_clock; clock_gettime(CLOCK_REALTIME,&request_wall_clock);
    const double requested_wall=request_wall_clock.tv_sec+request_wall_clock.tv_nsec/1e9;
    const unsigned long index = atomic_fetch_add(&sequence, 1);
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
        const double presented = shown.presentedTime;
        struct timespec wall;
        clock_gettime(CLOCK_REALTIME, &wall);
        const double wall_now = wall.tv_sec + wall.tv_nsec / 1e9;
        const double presented_wall = presented > 0 ? wall_now - (CACurrentMediaTime()-presented) : 0;
        fprintf(trace, "%lu,%s,%.9f,%.9f,%.6f,%.6f,%.9f,%.9f,%.6f,%.9f,%.9f,%.6f,%.9f\n", index, kind, requested, presented,
                presented > 0 ? (presented-requested)*1000 : -1.0, duration*1000,command.GPUStartTime,command.GPUEndTime,presented > 0 && command.GPUEndTime > 0 ? (presented-command.GPUEndTime)*1000 : -1.0,presented_wall,requested_wall,presented>0?(CACurrentMediaTime()-presented)*1000:-1.0,target);
        if (limit_pending) dispatch_semaphore_signal(presentation_slots);
    }];
}
static id next_drawable(CAMetalLayer *self, SEL selector) {
    if (drawable_count && self.maximumDrawableCount != (NSUInteger)drawable_count)
        self.maximumDrawableCount = drawable_count;
    if (force_sync >= 0 && self.displaySyncEnabled != (BOOL)force_sync)
        self.displaySyncEnabled=(BOOL)force_sync;
    double enter = CACurrentMediaTime();
    if (limit_pending) dispatch_semaphore_wait(presentation_slots,DISPATCH_TIME_FOREVER);
    double before_next = CACurrentMediaTime();
    id result = ((id(*)(id,SEL))original_next)(self,selector);
    double after_next = CACurrentMediaTime();
    if (next_trace) fprintf(next_trace,"%.9f,%.6f,%.6f,%lu,%d,%d\n",enter,(before_next-enter)*1000,(after_next-before_next)*1000,(unsigned long)self.maximumDrawableCount,self.displaySyncEnabled,self.presentsWithTransaction);
    if (!result && limit_pending) dispatch_semaphore_signal(presentation_slots);
    return result;
}
static void plain(id self, SEL sel, id<MTLDrawable> drawable) {
    observe(self,drawable,"plain",0,0); nesting++;
    ((void(*)(id,SEL,id))original_plain)(self,sel,drawable); nesting--;
}
static void minimum(id self, SEL sel, id<MTLDrawable> drawable, double duration) {
    double requested_duration=present_mode?0:duration;
    double target=present_mode==2?CACurrentMediaTime():0;
    observe(self,drawable,present_mode==1?"plain-override":present_mode==2?"at-now-override":"minimum",requested_duration,target);
    nesting++;
    if (present_mode==1)
        ((void(*)(id,SEL,id))original_plain)(self,@selector(presentDrawable:),drawable);
    else if (present_mode==2)
        ((void(*)(id,SEL,id,double))original_at)(self,@selector(presentDrawable:atTime:),drawable,target);
    else
        ((void(*)(id,SEL,id,double))original_minimum)(self,sel,drawable,requested_duration);
    nesting--;
}
static void at(id self, SEL sel, id<MTLDrawable> drawable, double when) {
    observe(self,drawable,"at",0,when); nesting++;
    ((void(*)(id,SEL,id,double))original_at)(self,sel,drawable,when); nesting--;
}
__attribute__((constructor)) static void initialize(void) {
    const char *base = getenv("PACING_TRACE_BASE");
    if (!base) return;
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        id<MTLCommandQueue> queue = [device newCommandQueue];
        id<MTLCommandBuffer> buffer = [queue commandBuffer];
        if (!buffer) return;
        char path[4096]; snprintf(path,sizeof(path),"%s-%d.csv",base,getpid());
        trace = fopen(path,"w");
        if (!trace) return;
        setvbuf(trace,NULL,_IOLBF,0);
        fprintf(trace,"frame,method,request_s,presented_s,present_delay_ms,minimum_duration_ms,gpu_start_s,gpu_end_s,gpu_done_to_display_ms,presented_unix_s,request_unix_s,callback_delay_ms,target_presentation_s\n");
        Class type = object_getClass(buffer);
        fprintf(stderr,"Pacing trace: command buffer class %s, file %s\n",class_getName(type),path);
        Method m = class_getInstanceMethod(type,@selector(presentDrawable:));
        if (m) original_plain = method_setImplementation(m,(IMP)plain);
        m = class_getInstanceMethod(type,@selector(presentDrawable:afterMinimumDuration:));
        if (m) original_minimum = method_setImplementation(m,(IMP)minimum);
        m = class_getInstanceMethod(type,@selector(presentDrawable:atTime:));
        if (m) original_at = method_setImplementation(m,(IMP)at);
        force_sync=getenv("PACING_DISPLAY_SYNC")?atoi(getenv("PACING_DISPLAY_SYNC")):-1;
        present_mode=getenv("PACING_PRESENT_MODE")?atoi(getenv("PACING_PRESENT_MODE")):0;
        drawable_count = getenv("PACING_DRAWABLE_COUNT") ? atoi(getenv("PACING_DRAWABLE_COUNT")) : 0;
        limit_pending = getenv("PACING_MAX_PENDING") ? atoi(getenv("PACING_MAX_PENDING")) : 0;
        if (limit_pending) presentation_slots = dispatch_semaphore_create(limit_pending);
        snprintf(path,sizeof(path),"%s-%d-next.csv",base,getpid());
        next_trace = fopen(path,"w");
        if (next_trace) {
            setvbuf(next_trace,NULL,_IOLBF,0);
            fprintf(next_trace,"request_s,presentation_wait_ms,next_drawable_wait_ms,drawable_count,display_sync_enabled,presents_with_transaction\n");
        }
        m = class_getInstanceMethod([CAMetalLayer class],@selector(nextDrawable));
        if (m) original_next = method_setImplementation(m,(IMP)next_drawable);
    }
}
