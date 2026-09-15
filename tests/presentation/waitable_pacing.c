#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LARGE_INTEGER frequency;
static double seconds(void) {
  LARGE_INTEGER t; QueryPerformanceCounter(&t);
  return (double)t.QuadPart / frequency.QuadPart;
}
static double unix_seconds(void) {
  FILETIME ft; ULARGE_INTEGER t;
  GetSystemTimePreciseAsFileTime(&ft);
  t.LowPart=ft.dwLowDateTime; t.HighPart=ft.dwHighDateTime;
  return (double)(t.QuadPart - 116444736000000000ULL) / 10000000.0;
}
static void check(HRESULT hr, const char *what) {
  if (FAILED(hr)) { fprintf(stderr,"%s failed: 0x%08lx\n",what,(unsigned long)hr); exit(2); }
}
static ID3DBlob *compile_shader(const char *source, const char *entry, const char *profile) {
  ID3DBlob *code=NULL, *errors=NULL;
  HRESULT hr=D3DCompile(source,strlen(source),NULL,NULL,NULL,entry,profile,
      D3DCOMPILE_OPTIMIZATION_LEVEL3|D3DCOMPILE_ENABLE_STRICTNESS,0,&code,&errors);
  if (errors) {
    fprintf(stderr,"%.*s",(int)ID3D10Blob_GetBufferSize(errors),(char*)ID3D10Blob_GetBufferPointer(errors));
    ID3D10Blob_Release(errors);
  }
  check(hr,"Compile workload shader");
  return code;
}
static const char *workload_shader=
  "cbuffer Work : register(b0) { uint iterations; float3 padding; };"
  "float4 vs(uint id : SV_VertexID) : SV_POSITION {"
  " return float4(id==1 ? 3.0 : -1.0, id==2 ? 3.0 : -1.0, 0, 1); }"
  "float4 ps(float4 position : SV_POSITION) : SV_TARGET {"
  " float2 v=frac(position.xy*float2(0.137,0.173));"
  " [loop] for(uint i=0;i<iterations;i++)"
  "   v=frac(v*float2(1.371,1.732)+v.yx+float2(0.17,0.23));"
  " return float4(0.05+v.x*0.06,0.12+v.y*0.02,0.2,1); }";

static LRESULT CALLBACK window_proc(HWND w, UINT m, WPARAM a, LPARAM b) {
  if (m == WM_CLOSE || (m == WM_KEYDOWN && a == VK_ESCAPE)) {
    PostQuitMessage(0); return 0;
  }
  return DefWindowProcA(w,m,a,b);
}
static int pump_messages(void) {
  MSG msg;
  while (PeekMessageA(&msg,NULL,0,0,PM_REMOVE)) {
    if (msg.message==WM_QUIT) return 0;
    TranslateMessage(&msg); DispatchMessageA(&msg);
  }
  return 1;
}
static DWORD wait_for_frame(HANDLE ready) {
  const double deadline=seconds()+2;
  for (;;) {
    DWORD timeout=(DWORD)((deadline-seconds())*1000);
    if (seconds()>=deadline) return WAIT_TIMEOUT;
    DWORD result = MsgWaitForMultipleObjectsEx(1,&ready,timeout,QS_ALLINPUT,MWMO_INPUTAVAILABLE|MWMO_ALERTABLE);
    if (result==WAIT_IO_COMPLETION) continue;
    if (result==WAIT_OBJECT_0+1) {
      if (!pump_messages()) return WAIT_FAILED;
      continue;
    }
    return result;
  }
}
int main(int argc, char **argv) {
  const char *output=argc>1?argv[1]:"waitable.csv";
  double duration=argc>2?atof(argv[2]):12;
  int waitable=argc>3?atoi(argv[3]):1;
  int latency=argc>4?atoi(argv[4]):1;
  int sync_interval=argc>5?atoi(argv[5]):1;
  double logic_ms=argc>6?atof(argv[6]):2;
  int fullscreen=argc>7?atoi(argv[7]):0;
  int gpu_work=argc>8?atoi(argv[8]):0;
  QueryPerformanceFrequency(&frequency);
  HINSTANCE instance=GetModuleHandleA(NULL);
  WNDCLASSA wc={0}; wc.lpfnWndProc=window_proc; wc.hInstance=instance;
  wc.lpszClassName="DXMTWaitableDiagnostic"; wc.hCursor=LoadCursorA(NULL,IDC_ARROW);
  RegisterClassA(&wc);
  const int width=fullscreen?GetSystemMetrics(SM_CXSCREEN):960;
  const int height=fullscreen?GetSystemMetrics(SM_CYSCREEN):600;
  HWND hwnd=CreateWindowA(wc.lpszClassName,"DXMT waitable-swapchain diagnostic (Escape closes)",
      fullscreen?WS_POPUP:WS_OVERLAPPEDWINDOW,fullscreen?0:80,fullscreen?0:80,width,height,NULL,NULL,instance,NULL);
  if (!hwnd) return 2;
  ShowWindow(hwnd,SW_SHOW);
  SetForegroundWindow(hwnd);
  DXGI_SWAP_CHAIN_DESC desc={0};
  desc.BufferDesc.Width=width; desc.BufferDesc.Height=height;
  desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
  desc.SampleDesc.Count=1; desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount=2; desc.OutputWindow=hwnd; desc.Windowed=TRUE;
  desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.Flags=waitable?DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT:0;
  ID3D11Device *device=NULL; ID3D11DeviceContext *context=NULL;
  IDXGISwapChain *base=NULL; IDXGISwapChain2 *swap=NULL;
  check(D3D11CreateDeviceAndSwapChain(NULL,D3D_DRIVER_TYPE_HARDWARE,NULL,0,NULL,0,
      D3D11_SDK_VERSION,&desc,&base,&device,NULL,&context),"CreateDeviceAndSwapChain");
  check(IDXGISwapChain_QueryInterface(base,&IID_IDXGISwapChain2,(void**)&swap),"Swapchain2");
  IDXGISwapChain_Release(base);
  IDXGIDevice1 *dxgi_device=NULL;
  check(ID3D11Device_QueryInterface(device,&IID_IDXGIDevice1,(void**)&dxgi_device),"DXGI device");
  check(IDXGIDevice1_SetMaximumFrameLatency(dxgi_device,waitable?1:latency),"Device latency");
  IDXGIDevice1_Release(dxgi_device);
  if (waitable) check(IDXGISwapChain2_SetMaximumFrameLatency(swap,latency),"Swapchain latency");
  HANDLE ready=IDXGISwapChain2_GetFrameLatencyWaitableObject(swap);
  if (waitable && !ready) { fprintf(stderr,"No waitable object\n"); return 2; }
  ID3D11Texture2D *buffer=NULL; ID3D11RenderTargetView *target=NULL;
  check(IDXGISwapChain2_GetBuffer(swap,0,&IID_ID3D11Texture2D,(void**)&buffer),"GetBuffer");
  check(ID3D11Device_CreateRenderTargetView(device,(ID3D11Resource*)buffer,NULL,&target),"Create target");
  ID3D11VertexShader *vertex_shader=NULL;
  ID3D11PixelShader *pixel_shader=NULL;
  ID3D11Buffer *work_buffer=NULL;
  ID3D11RasterizerState *work_rasterizer=NULL;
  if (gpu_work) {
    ID3DBlob *vs=compile_shader(workload_shader,"vs","vs_5_0");
    ID3DBlob *ps=compile_shader(workload_shader,"ps","ps_5_0");
    check(ID3D11Device_CreateVertexShader(device,ID3D10Blob_GetBufferPointer(vs),ID3D10Blob_GetBufferSize(vs),NULL,&vertex_shader),"Vertex shader");
    check(ID3D11Device_CreatePixelShader(device,ID3D10Blob_GetBufferPointer(ps),ID3D10Blob_GetBufferSize(ps),NULL,&pixel_shader),"Pixel shader");
    ID3D10Blob_Release(vs); ID3D10Blob_Release(ps);
    D3D11_BUFFER_DESC work_desc={0};
    work_desc.ByteWidth=16; work_desc.Usage=D3D11_USAGE_DEFAULT;
    work_desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
    check(ID3D11Device_CreateBuffer(device,&work_desc,NULL,&work_buffer),"GPU workload buffer");
    D3D11_RASTERIZER_DESC raster_desc={0};
    raster_desc.FillMode=D3D11_FILL_SOLID; raster_desc.CullMode=D3D11_CULL_NONE;
    raster_desc.DepthClipEnable=TRUE;
    check(ID3D11Device_CreateRasterizerState(device,&raster_desc,&work_rasterizer),"Workload rasterizer");
    ID3D11DeviceContext_RSSetState(context,work_rasterizer);
    ID3D11DeviceContext_VSSetShader(context,vertex_shader,NULL,0);
    ID3D11DeviceContext_PSSetShader(context,pixel_shader,NULL,0);
    ID3D11DeviceContext_PSSetConstantBuffers(context,0,1,&work_buffer);
    ID3D11DeviceContext_OMSetRenderTargets(context,1,&target,NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(context,D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport={0,0,(float)width,(float)height,0,1};
    ID3D11DeviceContext_RSSetViewports(context,1,&viewport);
  }
  FILE *csv=fopen(output,"w"); if (!csv) return 3;
  setvbuf(csv,NULL,_IOLBF,0);
  fprintf(csv,"frame,elapsed_s,wait_ms,wake_unix_s,present_ms,present_begin_unix_s,present_end_unix_s,input_unix_s,logic_end_unix_s,input_sequence,gpu_iterations\n");
  unsigned frame=0; int running=1; const double start=seconds();
  while (running && seconds()-start<duration) {
    double before=seconds(), wait_begin=before;
    if (ready && wait_for_frame(ready)!=WAIT_OBJECT_0) {
      fprintf(stderr,"Waitable object timeout at frame %u\n",frame); return 4;
    }
    double woke=seconds(), wall=unix_seconds();
    if (!pump_messages()) break;
    // Model the engine boundary explicitly: input is sampled only after its
    // pacing wait. Optional CPU work is constant across every frame and case.
    double input_wall=unix_seconds();
    unsigned long input_sequence=GetTickCount();
    double logic_deadline=seconds()+logic_ms/1000.0;
    while (seconds()<logic_deadline) YieldProcessor();
    double logic_end=unix_seconds();
    float color[4]={0.05f+(input_sequence%120)/200.0f,0.12f,0.2f,1};
    unsigned gpu_iterations=0;
    if (gpu_work) {
      // Change actual GPU work per frame only after display-driven admission.
      // No timer offsets, altered display rate, or synthetic queue waits.
      const unsigned pattern[]={1,3,6,2,4,8,2,5};
      gpu_iterations=256*gpu_work*pattern[frame%8];
      unsigned params[4]={gpu_iterations,0,0,0};
      ID3D11DeviceContext_UpdateSubresource(context,(ID3D11Resource*)work_buffer,0,NULL,params,0,0);
      ID3D11DeviceContext_Draw(context,3,0);
    } else {
      ID3D11DeviceContext_ClearRenderTargetView(context,target,color);
    }
    before=seconds();
    double present_begin=unix_seconds();
    check(IDXGISwapChain2_Present(swap,sync_interval,0),"Present");
    double present_end=unix_seconds();
    fprintf(csv,"%u,%.9f,%.6f,%.9f,%.6f,%.9f,%.9f,%.9f,%.9f,%lu,%u\n",frame,woke-start,
        (woke-wait_begin)*1000,wall,(seconds()-before)*1000,present_begin,present_end,input_wall,logic_end,input_sequence,gpu_iterations);
    ++frame;
  }
  fprintf(stdout,"waitable=%d frames=%u elapsed=%.3f\n",waitable,frame,seconds()-start);
  fclose(csv);
  if (ready) CloseHandle(ready);
  if (work_rasterizer) ID3D11RasterizerState_Release(work_rasterizer);
  if (work_buffer) ID3D11Buffer_Release(work_buffer);
  if (pixel_shader) ID3D11PixelShader_Release(pixel_shader);
  if (vertex_shader) ID3D11VertexShader_Release(vertex_shader);
  ID3D11RenderTargetView_Release(target); ID3D11Texture2D_Release(buffer);
  IDXGISwapChain2_Release(swap);
  ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
  DestroyWindow(hwnd);
  return 0;
}
