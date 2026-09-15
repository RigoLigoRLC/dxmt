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
    }];
}
static id next_drawable(CAMetalLayer *self, SEL selector) {
    double before_next = CACurrentMediaTime();
    id result = ((id(*)(id,SEL))original_next)(self,selector);
    double after_next = CACurrentMediaTime();
    if (next_trace) fprintf(next_trace,"%.9f,%.6f,%lu,%d,%d\n",before_next,(after_next-before_next)*1000,(unsigned long)self.maximumDrawableCount,self.displaySyncEnabled,self.presentsWithTransaction);
    return result;
}
static void plain(id self, SEL sel, id<MTLDrawable> drawable) {
    observe(self,drawable,"plain",0,0); nesting++;
    ((void(*)(id,SEL,id))original_plain)(self,sel,drawable); nesting--;
}
static void minimum(id self, SEL sel, id<MTLDrawable> drawable, double duration) {
    observe(self,drawable,"minimum",duration,0);
    nesting++;
    ((void(*)(id,SEL,id,double))original_minimum)(self,sel,drawable,duration);
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
        snprintf(path,sizeof(path),"%s-%d-next.csv",base,getpid());
        next_trace = fopen(path,"w");
        if (next_trace) {
            setvbuf(next_trace,NULL,_IOLBF,0);
            fprintf(next_trace,"request_s,next_drawable_wait_ms,drawable_count,display_sync_enabled,presents_with_transaction\n");
        }
        m = class_getInstanceMethod([CAMetalLayer class],@selector(nextDrawable));
        if (m) original_next = method_setImplementation(m,(IMP)next_drawable);
    }
}
