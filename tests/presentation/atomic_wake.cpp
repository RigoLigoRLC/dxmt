#include <windows.h>
#include "util_atomic_wait.hpp"
#include <atomic>
#include <algorithm>
#include <cstdio>
#include <vector>

static std::atomic<unsigned long long> sequence{0};
static HANDLE ack;
static LARGE_INTEGER frequency;
static LONGLONG signaled_at;
static bool native;
static double samples[120];
static DWORD WINAPI worker(void*) {
  for (unsigned long long frame=0; frame<120; ++frame) {
    if (native) {
      dxmt::WaitForAtomicChange(sequence,frame);
    } else sequence.wait(frame,std::memory_order_acquire);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    samples[frame]=1000.0*(now.QuadPart-signaled_at)/frequency.QuadPart;
    SetEvent(ack);
  }
  return 0;
}
int main(int argc,char**argv) {
  native=argc>1 && argv[1][0]=='1';
  QueryPerformanceFrequency(&frequency);
  ack=CreateEventW(nullptr,FALSE,FALSE,nullptr);
  HANDLE thread=CreateThread(nullptr,0,worker,nullptr,0,nullptr);
  for (unsigned long long frame=0; frame<120; ++frame) {
    Sleep(10+(frame*7)%21);
    LARGE_INTEGER now; QueryPerformanceCounter(&now); signaled_at=now.QuadPart;
    sequence.store(frame+1,std::memory_order_release);
    if(native) dxmt::NotifyAtomicChange(sequence); else sequence.notify_one();
    if(WaitForSingleObject(ack,2000)!=WAIT_OBJECT_0) return 2;
  }
  WaitForSingleObject(thread,2000);CloseHandle(thread);CloseHandle(ack);
  std::vector<double> sorted(samples,samples+120);std::sort(sorted.begin(),sorted.end());
  double total=0;for(double s:samples)total+=s;
  printf("%s wake ms mean=%.4f median=%.4f p95=%.4f max=%.4f\n",native?"WaitOnAddress":"std::atomic::wait",total/120,sorted[60],sorted[114],sorted[119]);
}
