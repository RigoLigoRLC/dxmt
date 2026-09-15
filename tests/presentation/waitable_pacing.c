#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <stdio.h>
#include <stdlib.h>

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
  QueryPerformanceFrequency(&frequency);
  HINSTANCE instance=GetModuleHandleA(NULL);
  WNDCLASSA wc={0}; wc.lpfnWndProc=window_proc; wc.hInstance=instance;
  wc.lpszClassName="DXMTWaitableDiagnostic"; wc.hCursor=LoadCursorA(NULL,IDC_ARROW);
  RegisterClassA(&wc);
  HWND hwnd=CreateWindowA(wc.lpszClassName,"DXMT waitable-swapchain diagnostic (Escape closes)",
      WS_OVERLAPPEDWINDOW,80,80,960,600,NULL,NULL,instance,NULL);
  if (!hwnd) return 2;
  ShowWindow(hwnd,SW_SHOW);
  DXGI_SWAP_CHAIN_DESC desc={0};
  desc.BufferDesc.Width=960; desc.BufferDesc.Height=600;
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
  FILE *csv=fopen(output,"w"); if (!csv) return 3;
  setvbuf(csv,NULL,_IOLBF,0);
  fprintf(csv,"frame,elapsed_s,wait_ms,wake_unix_s,present_ms,present_begin_unix_s,present_end_unix_s,input_unix_s,logic_end_unix_s,input_sequence\n");
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
    ID3D11DeviceContext_ClearRenderTargetView(context,target,color);
    before=seconds();
    double present_begin=unix_seconds();
    check(IDXGISwapChain2_Present(swap,sync_interval,0),"Present");
    double present_end=unix_seconds();
    fprintf(csv,"%u,%.9f,%.6f,%.9f,%.6f,%.9f,%.9f,%.9f,%.9f,%lu\n",frame,woke-start,
        (woke-wait_begin)*1000,wall,(seconds()-before)*1000,present_begin,present_end,input_wall,logic_end,input_sequence);
    ++frame;
  }
  fprintf(stdout,"waitable=%d frames=%u elapsed=%.3f\n",waitable,frame,seconds()-start);
  fclose(csv);
  if (ready) CloseHandle(ready);
  ID3D11RenderTargetView_Release(target); ID3D11Texture2D_Release(buffer);
  IDXGISwapChain2_Release(swap);
  ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
  DestroyWindow(hwnd);
  return 0;
}
