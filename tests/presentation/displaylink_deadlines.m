#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdatomic.h>
static atomic_bool duration_started;
@interface DisplayLinkReference : NSObject <NSApplicationDelegate, CAMetalDisplayLinkDelegate>
@property NSWindow *window;
@property CAMetalLayer *layer;
@property CAMetalDisplayLink *link;
@property id<MTLDevice> device;
@property id<MTLCommandQueue> queue;
@property FILE *trace;
@property unsigned long frame;
@property float latency;
@property BOOL syncEnabled;
@property BOOL variableGPU;
@property id<MTLComputePipelineState> workload;
@property id<MTLBuffer> scratch;
@property float schedulingRate;
@end

@implementation DisplayLinkReference
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    self.device = MTLCreateSystemDefaultDevice();
    self.queue = [self.device newCommandQueue];
    if (self.variableGPU) {
        NSError *error=nil;
        NSString *source=@"#include <metal_stdlib>\nusing namespace metal; kernel void workload(device float *output [[buffer(0)]], constant uint &loops [[buffer(1)]], uint tid [[thread_position_in_grid]]) { float x=float(tid)*0.00001f; for(uint i=0;i<loops;++i) x=sin(x+float(i)*0.0001f)*1.001f; output[tid]=x; }";
        id<MTLLibrary> library=[self.device newLibraryWithSource:source options:nil error:&error];
        self.workload=[self.device newComputePipelineStateWithFunction:[library newFunctionWithName:@"workload"] error:&error];
        self.scratch=[self.device newBufferWithLength:65536*sizeof(float) options:MTLResourceStorageModePrivate];
        if(!self.workload || !self.scratch) { fprintf(stderr,"Workload setup failed: %s\n",error.description.UTF8String);exit(2); }
    }
    NSScreen *screen = NSScreen.screens.firstObject;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 960, 600)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
        backing:NSBackingStoreBuffered defer:NO screen:screen];
    self.window.title = [NSString stringWithFormat:@"Metal timing: %.0f frames/s, latency %.0f, sync %d", self.schedulingRate,self.latency,self.syncEnabled];
    self.layer = [CAMetalLayer layer];
    self.layer.device = self.device;
    self.layer.displaySyncEnabled=self.syncEnabled;
    self.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    self.layer.framebufferOnly = YES;
    self.layer.opaque = YES;
    self.layer.contentsScale = screen.backingScaleFactor;
    self.layer.drawableSize = CGSizeMake(960*screen.backingScaleFactor, 600*screen.backingScaleFactor);
    self.window.contentView.wantsLayer = YES;
    self.window.contentView.layer = self.layer;
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    self.link = [[CAMetalDisplayLink alloc] initWithMetalLayer:self.layer];
    self.link.preferredFrameLatency = self.latency;
    self.link.preferredFrameRateRange = CAFrameRateRangeMake(self.schedulingRate,self.schedulingRate,self.schedulingRate);
    self.link.delegate = self;
    [self.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    fprintf(stderr, "Native Metal control: device=%s screen=%s maxRefresh=%ld drawables=%lu\n",
        self.device.name.UTF8String, screen.localizedName.UTF8String,
        (long)screen.maximumFramesPerSecond, (unsigned long)self.layer.maximumDrawableCount);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 45*NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        [self.link invalidate];
        [NSApp terminate:nil];
    });
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender { return YES; }
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    double began = CACurrentMediaTime();
    double input_sample=CACurrentMediaTime();
    id<CAMetalDrawable> drawable = update.drawable;
    id<MTLCommandBuffer> command = [self.queue commandBuffer];
    const unsigned costs[]={64,512,8192};
    unsigned loops=self.variableGPU?costs[(self.frame/60)%3]:0;
    if (loops) {
        id<MTLComputeCommandEncoder> compute=[command computeCommandEncoder];
        [compute setComputePipelineState:self.workload];
        [compute setBuffer:self.scratch offset:0 atIndex:0];
        [compute setBytes:&loops length:sizeof(loops) atIndex:1];
        [compute dispatchThreads:MTLSizeMake(65536,1,1) threadsPerThreadgroup:MTLSizeMake(256,1,1)];
        [compute endEncoding];
    }
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = drawable.texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    BOOL pressed = NSEvent.pressedMouseButtons != 0;
    pass.colorAttachments[0].clearColor = pressed ? MTLClearColorMake(.85,.85,.85,1)
                                                : MTLClearColorMake(.06,.10,.16,1);
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder endEncoding];
    unsigned long frame = self.frame++;
    double target = update.targetTimestamp;
    double predicted = update.targetPresentationTimestamp;
    FILE *trace = self.trace;
    double requested = CACurrentMediaTime();
    [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
        double callback = CACurrentMediaTime();
        double displayed = shown.presentedTime;
        if (displayed>0 && !atomic_exchange(&duration_started,true)) {
            // Start the measurement duration after a frame actually displays.
            // Cold shader/HUD/window startup must not consume the whole run.
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,12*NSEC_PER_SEC),dispatch_get_main_queue(),^{
                [self.link invalidate]; [NSApp terminate:nil];
            });
        }
        double done = command.GPUEndTime;
        fprintf(trace, "%lu,displaylink,%.9f,%.9f,%.6f,0,%.9f,%.9f,%.6f,%.9f,%.9f,%.9f,%.6f,%.9f,%u\n",
            frame, requested, displayed, displayed>0 ? (displayed-requested)*1000 : -1,
            command.GPUStartTime, done, displayed>0 && done>0 ? (displayed-done)*1000 : -1,
            began, target, predicted, displayed>0 ? (callback-displayed)*1000 : -1,input_sample,loops);
    }];
    // CAMetalDisplayLink requires plain drawable.present after command submission.
    [command commit];
    [drawable present];
}
@end

int main(int argc, const char **argv) {
    if (argc < 2) return 2;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        DisplayLinkReference *delegate = [DisplayLinkReference new];
        delegate.variableGPU=argc>5?atoi(argv[5]):NO;
        delegate.syncEnabled=argc>4?atoi(argv[4]):YES;
        delegate.schedulingRate=argc>3?atof(argv[3]):60;
        delegate.latency=argc>2?atof(argv[2]):1;
        delegate.trace = fopen(argv[1], "w");
        if (!delegate.trace) return 3;
        setvbuf(delegate.trace, NULL, _IOLBF, 0);
        fprintf(delegate.trace, "frame,method,request_s,presented_s,present_delay_ms,minimum_duration_ms,gpu_start_s,gpu_end_s,gpu_done_to_display_ms,frame_start_s,commit_deadline_s,predicted_display_s,callback_delay_ms,input_sample_s,workload_loops\n");
        NSApp.delegate = delegate;
        [NSApp run];
    }
    return 0;
}
