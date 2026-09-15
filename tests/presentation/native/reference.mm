// Isolated reproduction of the drawable-before-input loop shared by
// Moonlight metal-v6 and PsychMetal 0.4.0, extended with measured display timing.
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <atomic>
#include <mutex>
#include <memory>
#include <vector>
#include <string>
#include <cmath>
#include <chrono>
#include <thread>
#include <condition_variable>

struct Frame {
    unsigned id;
    double input=0, logicEnd=0, acquireStart=0, acquireEnd=0;
    double request=0, commit=0, scheduled=0, presentCall=0;
    double gpuStart=0, gpuEnd=0, completed=0, displayed=0, callback=0;
    unsigned drawableID=0;
    int gpuStatus=0;
    double linkTimestamp=0, linkDeadline=0, linkArrival=0;
    unsigned linkSequence=0;
};
struct DisplayTick { unsigned sequence=0; double timestamp=0, deadline=0, arrival=0; };
static std::mutex tickLock;
static std::condition_variable tickChanged;
static DisplayTick latestTick;
static std::vector<DisplayTick> ticks;
static std::mutex traceLock;
static std::vector<std::shared_ptr<Frame>> frames;
static std::atomic<bool> running{true};
static std::atomic<unsigned> errors{0};
static std::string method, base;
static bool useDisplayLink() { return method=="display60"; }
static double runSeconds=12;
static CGDirectDisplayID displayID;
static CGDisplayModeRef savedMode;
static bool changedMode=false;
static bool capturedDisplay=false;
static void restoreDisplay() {
    if(capturedDisplay) {CGDisplayRelease(displayID);capturedDisplay=false;}
    if (changedMode && savedMode) {
        CGError result=CGDisplaySetDisplayMode(displayID,savedMode,nullptr);
        fprintf(stderr,"restore_display=%d\n",result);
        changedMode=false;
    }
}
static NSDictionary *modeInfo(CGDisplayModeRef m) {
    if (!m) return @{};
    return @{ @"width":@(CGDisplayModeGetWidth(m)), @"height":@(CGDisplayModeGetHeight(m)),
        @"pixelWidth":@(CGDisplayModeGetPixelWidth(m)), @"pixelHeight":@(CGDisplayModeGetPixelHeight(m)),
        @"refreshHz":@(CGDisplayModeGetRefreshRate(m)), @"id":@(CGDisplayModeGetIODisplayModeID(m)) };
}
static NSArray *displayModes() {
    CFArrayRef list=CGDisplayCopyAllDisplayModes(displayID,(__bridge CFDictionaryRef)@{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes:@YES});
    NSMutableArray *result=[NSMutableArray array];
    for (id item in (__bridge NSArray *)list) [result addObject:modeInfo((__bridge CGDisplayModeRef)item)];
    if (list) CFRelease(list);
    return result;
}
static void writeJSON(NSString *path,id object) {
    NSData *data=[NSJSONSerialization dataWithJSONObject:object options:NSJSONWritingPrettyPrinted error:nil];
    [data writeToFile:path atomically:YES];
}

@interface Reference : NSObject <NSApplicationDelegate>
@property NSWindow *window;
@property CAMetalLayer *layer;
@property id<MTLCommandQueue> queue;
@property CADisplayLink *displayLink;
@property BOOL fullscreen;
@property BOOL force60;
@end

@implementation Reference
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    NSScreen *screen=NSScreen.screens.firstObject;
    displayID=[screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    savedMode=CGDisplayCopyDisplayMode(displayID);
    NSDictionary *original=modeInfo(savedMode);
    if (self.force60 && fabs(CGDisplayModeGetRefreshRate(savedMode)-60.0)>0.01) {
        CFArrayRef modes=CGDisplayCopyAllDisplayModes(displayID,(__bridge CFDictionaryRef)@{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes:@YES});
        CGDisplayModeRef chosen=nullptr;
        for (id item in (__bridge NSArray *)modes) {
            CGDisplayModeRef m=(__bridge CGDisplayModeRef)item;
            if (fabs(CGDisplayModeGetRefreshRate(m)-60.0)<0.01 &&
                CGDisplayModeGetWidth(m)==CGDisplayModeGetWidth(savedMode) &&
                CGDisplayModeGetHeight(m)==CGDisplayModeGetHeight(savedMode) &&
                CGDisplayModeGetPixelWidth(m)==CGDisplayModeGetPixelWidth(savedMode) &&
                CGDisplayModeGetPixelHeight(m)==CGDisplayModeGetPixelHeight(savedMode)) {chosen=m; break;}
        }
        if (!chosen || CGDisplaySetDisplayMode(displayID,chosen,nullptr)!=kCGErrorSuccess) {
            fprintf(stderr,"Cannot select a matching 60-Hz display mode; stopping.\n");
            if(modes) CFRelease(modes);
            [NSApp terminate:nil]; return;
        }
        changedMode=true;
        if(modes) CFRelease(modes);
    }
    CGDisplayModeRef active=CGDisplayCopyDisplayMode(displayID);
    id<MTLDevice> device=MTLCreateSystemDefaultDevice();
    self.queue=[device newCommandQueue];
    if (!device || !self.queue) {fprintf(stderr,"No Metal device\n"); [NSApp terminate:nil];return;}
    NSRect rect=self.fullscreen?screen.frame:NSMakeRect(80,80,960,600);
    // Reproduce PsychMetal's documented fullscreen window setup when requested.
    if(getenv("REFERENCE_PSYCH_WINDOW")) capturedDisplay=(CGDisplayCapture(displayID)==kCGErrorSuccess);
    NSUInteger style=self.fullscreen?NSWindowStyleMaskBorderless:NSWindowStyleMaskTitled;
    self.window=[[NSWindow alloc] initWithContentRect:rect styleMask:style backing:NSBackingStoreBuffered defer:NO screen:screen];
    self.window.title=@"Native presentation reference";
    self.window.releasedWhenClosed=NO;
    if(getenv("REFERENCE_PSYCH_WINDOW")) {
        self.window.animationBehavior=NSWindowAnimationBehaviorNone;
        self.window.level=(NSInteger)CGShieldingWindowLevel();
        self.window.collectionBehavior=NSWindowCollectionBehaviorStationary|NSWindowCollectionBehaviorFullScreenNone|NSWindowCollectionBehaviorIgnoresCycle;
    }
    if(self.fullscreen) [NSApp setPresentationOptions:NSApplicationPresentationAutoHideDock|NSApplicationPresentationAutoHideMenuBar];
    self.layer=[CAMetalLayer layer];
    self.layer.device=device;
    self.layer.pixelFormat=MTLPixelFormatBGRA8Unorm;
    self.layer.framebufferOnly=YES;
    self.layer.opaque=YES;
    self.layer.displaySyncEnabled=YES;
    self.layer.maximumDrawableCount=3;
    self.layer.contentsScale=screen.backingScaleFactor;
    self.layer.drawableSize=CGSizeMake(rect.size.width*screen.backingScaleFactor,rect.size.height*screen.backingScaleFactor);
    self.window.contentView.wantsLayer=YES;
    self.window.contentView.layer=self.layer;
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    if(useDisplayLink()) {
        self.displayLink=[self.window displayLinkWithTarget:self selector:@selector(displayTick:)];
        self.displayLink.preferredFrameRateRange=CAFrameRateRangeMake(60,60,60);
        [self.displayLink addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    }
    NSDictionary *meta=@{ @"method":@(method.c_str()),
        @"originalMode":original, @"activeMode":modeInfo(active), @"displayID":@(displayID),
        @"windowID":@(self.window.windowNumber), @"fullscreen":@(self.fullscreen),
        @"device":device.name, @"os":NSProcessInfo.processInfo.operatingSystemVersionString,
        @"drawableWidth":@(self.layer.drawableSize.width), @"drawableHeight":@(self.layer.drawableSize.height),
        @"drawables":@(self.layer.maximumDrawableCount), @"vsync":@(self.layer.displaySyncEnabled),
        @"timingSource":useDisplayLink()?@"CADisplayLink":@"none",
        @"requestedCallbackHz":@(useDisplayLink()?60:0), @"outstandingPresentationLimit":@(useDisplayLink()?2:0),
        @"logicMilliseconds":@2, @"seconds":@(runSeconds), @"psychWindow":@(getenv("REFERENCE_PSYCH_WINDOW")!=nullptr), @"displayCaptured":@(capturedDisplay) };
    writeJSON(@((base+"-meta.json").c_str()),meta);
    fprintf(stderr,"READY window=%ld method=%s active_hz=%.4f drawable=%.0fx%.0f\n",
        (long)self.window.windowNumber,method.c_str(),CGDisplayModeGetRefreshRate(active),self.layer.drawableSize.width,self.layer.drawableSize.height);
    CFRelease(active);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC),dispatch_get_main_queue(),^{
        CFArrayRef info=CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow,(CGWindowID)self.window.windowNumber);
        NSDictionary *state=@{ @"windowFrame":NSStringFromRect(self.window.frame),
            @"viewBounds":NSStringFromRect(self.window.contentView.bounds), @"layerFrame":NSStringFromRect(self.layer.frame),
            @"visible":@(self.window.visible), @"active":@(NSApp.active),
            @"bundle":NSBundle.mainBundle.bundleIdentifier ?: @"none", @"windowInfo":(__bridge NSArray *)info ?: @[] };
        writeJSON(@((base+"-window.json").c_str()),state);
        if(info) CFRelease(info);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(runSeconds+15)*NSEC_PER_SEC),dispatch_get_main_queue(),^{
        fprintf(stderr,"Watchdog fired\n"); running=false; [NSApp terminate:nil];
    });
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE,0),^{[self renderLoop];});
}
- (void)displayTick:(CADisplayLink *)link {
    const double arrival=CACurrentMediaTime();
    {std::lock_guard<std::mutex> lock(tickLock);
        latestTick={latestTick.sequence+1,link.timestamp,link.targetTimestamp,arrival};
        ticks.push_back(latestTick);
    }
    tickChanged.notify_one();
}
- (void)renderLoop {
    @autoreleasepool {
        dispatch_semaphore_t occupancy=dispatch_semaphore_create(2);
        const double start=CACurrentMediaTime();
        unsigned sequence=0;
        unsigned consumedTick=0;
        const auto producerStart=std::chrono::steady_clock::now();
        id<CAMetalDrawable> held=nil;
        double heldAcquireStart=0,heldAcquireEnd=0;
        while(running && CACurrentMediaTime()-start<runSeconds) { @autoreleasepool {
            DisplayTick tick;
            if(useDisplayLink()) {
                // Bound outstanding presentations before sampling fresh input.
                // The display link requests exactly 60 callbacks, one per new frame.
                if(dispatch_semaphore_wait(occupancy,dispatch_time(DISPATCH_TIME_NOW,2*NSEC_PER_SEC))!=0) {
                    fprintf(stderr,"Outstanding presentation limit timed out\n");errors++;break;
                }
                std::unique_lock<std::mutex> lock(tickLock);
                if(!tickChanged.wait_for(lock,std::chrono::seconds(2),[&]{return latestTick.sequence>consumedTick || !running;})) {
                    fprintf(stderr,"Display timing wait timed out\n");errors++;break;
                }
                if(!running) break;
                tick=latestTick; consumedTick=tick.sequence;
            }
            // Controlled analogue of Moonlight's incoming 60-FPS video.
            // This is an independent source clock, not a proposed VSync pacer.
            // Each source tick produces one distinct rendered frame; no repeats.
            if(method=="producer60") {
                std::this_thread::sleep_until(producerStart+std::chrono::nanoseconds((uint64_t)sequence*1000000000ULL/60));
            }
            auto frame=std::make_shared<Frame>(); frame->id=sequence++;
            if(useDisplayLink()) {
                frame->linkSequence=tick.sequence;
                frame->linkTimestamp=tick.timestamp; frame->linkDeadline=tick.deadline; frame->linkArrival=tick.arrival;
            }
            __attribute__((objc_precise_lifetime)) id<CAMetalDrawable> drawable=nil;
            if(held) {
                drawable=held;held=nil;
                frame->acquireStart=heldAcquireStart; frame->acquireEnd=heldAcquireEnd;
            }
            else {
                frame->acquireStart=CACurrentMediaTime();
                drawable=[self.layer nextDrawable];
                frame->acquireEnd=CACurrentMediaTime();
            }
            // Sample current input only after this arm's chosen wait point.
            frame->input=CACurrentMediaTime();
            const bool button=CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,kCGMouseButtonLeft);
            // Fixed CPU simulation cost; GPU work is identical in every frame/arm.
            while(CACurrentMediaTime()-frame->input<0.002) { }
            frame->logicEnd=CACurrentMediaTime();
            if(!drawable) {fprintf(stderr,"No drawable\n");errors++;break;}
            frame->drawableID=(unsigned)drawable.drawableID;
            id<MTLCommandBuffer> command=[self.queue commandBuffer];
            MTLRenderPassDescriptor *pass=[MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture=drawable.texture;
            pass.colorAttachments[0].loadAction=MTLLoadActionClear;
            pass.colorAttachments[0].storeAction=MTLStoreActionStore;
            double shade=0.08+0.02*sin(frame->id*0.03);
            pass.colorAttachments[0].clearColor=MTLClearColorMake(shade,button?0.22:0.13,0.20,1);
            id<MTLRenderCommandEncoder> encoder=[command renderCommandEncoderWithDescriptor:pass];
            [encoder endEncoding];
            {std::lock_guard<std::mutex> lock(traceLock);frames.push_back(frame);}
            [drawable addPresentedHandler:^(id<MTLDrawable> d) {
                const double callback=CACurrentMediaTime(),displayed=d.presentedTime;
                {std::lock_guard<std::mutex> lock(traceLock);frame->displayed=displayed;frame->callback=callback;}
                if(useDisplayLink()) dispatch_semaphore_signal(occupancy);
            }];
            [command addCompletedHandler:^(id<MTLCommandBuffer> cb) {
                const double completed=CACurrentMediaTime();
                std::lock_guard<std::mutex> lock(traceLock);
                frame->gpuStart=cb.GPUStartTime; frame->gpuEnd=cb.GPUEndTime;
                frame->gpuStatus=(int)cb.status; frame->completed=completed;
            }];
            // Same scheduled-handler placement as Moonlight. The timestamp at
            // the actual drawable call is separate from registering the request.
            [command addScheduledHandler:^(id<MTLCommandBuffer> cb) {
                double scheduled=CACurrentMediaTime();
                double actualCall=CACurrentMediaTime();
                [drawable present];
                std::lock_guard<std::mutex> lock(traceLock);
                frame->scheduled=scheduled;frame->presentCall=actualCall;
            }];
            frame->request=CACurrentMediaTime();
            frame->commit=CACurrentMediaTime();
            [command commit];
            // Match the references' end-of-frame acquisition, including the
            // lifetime of the current frame's drawable/temporary objects.
            heldAcquireStart=CACurrentMediaTime();
            held=[self.layer nextDrawable];
            heldAcquireEnd=CACurrentMediaTime();
        }}
        held=nil;
        // GPU drain does not pretend to be a presentation drain. Keep the
        // window alive another second so final display callbacks can arrive.
        id<MTLCommandBuffer> drain=[self.queue commandBuffer];
        [drain commit]; [drain waitUntilCompleted];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_SEC),dispatch_get_main_queue(),^{[self finish];});
    }
}
- (void)finish {
    [self.displayLink invalidate];
    FILE *out=fopen((base+".csv").c_str(),"w");
    if(out) {
        fprintf(out,"frame,drawable_id,input_s,logic_end_s,acquire_start_s,acquire_end_s,request_s,commit_s,scheduled_s,present_call_s,gpu_start_s,gpu_end_s,completed_s,displayed_s,callback_s,gpu_status,link_sequence,link_timestamp_s,link_deadline_s,link_arrival_s\n");
        std::lock_guard<std::mutex> lock(traceLock);
        for(auto &f:frames) fprintf(out,"%u,%u,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%.9f,%d,%u,%.9f,%.9f,%.9f\n",
            f->id,f->drawableID,f->input,f->logicEnd,f->acquireStart,f->acquireEnd,f->request,f->commit,
            f->scheduled,f->presentCall,f->gpuStart,f->gpuEnd,f->completed,f->displayed,f->callback,f->gpuStatus,
            f->linkSequence,f->linkTimestamp,f->linkDeadline,f->linkArrival);
        fclose(out);
        fprintf(stderr,"DONE frames=%lu errors=%u\n",frames.size(),errors.load());
    }
    FILE *tickOut=fopen((base+"-ticks.csv").c_str(),"w");
    if(tickOut) {
        fprintf(tickOut,"sequence,timestamp_s,deadline_s,arrival_s\n");
        std::lock_guard<std::mutex> lock(tickLock);
        for(auto &t:ticks) fprintf(tickOut,"%u,%.9f,%.9f,%.9f\n",t.sequence,t.timestamp,t.deadline,t.arrival);
        fclose(tickOut);
    }
    [NSApp terminate:nil];
}
- (void)applicationWillTerminate:(NSNotification *)notification {running=false;tickChanged.notify_all();[self.displayLink invalidate];restoreDisplay();}
@end

int main(int argc,const char **argv) { @autoreleasepool {
    [NSApplication sharedApplication];
    displayID=CGMainDisplayID();
    if(argc==2 && std::string(argv[1])=="--info") {
        CGDisplayModeRef m=CGDisplayCopyDisplayMode(displayID);
        NSData *data=[NSJSONSerialization dataWithJSONObject:@{@"current":modeInfo(m),@"modes":displayModes()} options:NSJSONWritingPrettyPrinted error:nil];
        fwrite(data.bytes,1,data.length,stdout); if(m) CFRelease(m); return 0;
    }
    if(argc<3) {fprintf(stderr,"usage: reference producer60|display60 BASE [seconds=12] [fullscreen=1] [force60=0]\n");return 2;}
    method=argv[1];base=argv[2];
    if(method!="producer60" && !useDisplayLink()) return 2;
    if(argc>3) runSeconds=atof(argv[3]);
    atexit(restoreDisplay);
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    Reference *delegate=[Reference new];
    delegate.fullscreen=argc>4?atoi(argv[4]):YES;
    delegate.force60=argc>5?atoi(argv[5]):NO;
    NSApp.delegate=delegate;
    [NSApp run];
    if(savedMode) CFRelease(savedMode);
    return errors?1:0;
}}
