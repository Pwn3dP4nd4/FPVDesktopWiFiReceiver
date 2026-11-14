// Windows shim for the two POSIX calls kepyur's native/radio/radio.cpp uses to raise thread priority:
//   gettid() + setpriority(PRIO_PROCESS, tid, -10)
// Only on the include path for WIN32 builds, so upstream kepyur compiles unmodified.
#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif

static inline int kp_compat_gettid(void) { return (int)GetCurrentThreadId(); }
#define gettid kp_compat_gettid

// nice value -> Windows thread priority. Only the calling thread is supported (that is all kepyur does).
static inline int kp_compat_setpriority(int which, int who, int prio) {
  (void)which;
  if (who != 0 && (DWORD)who != GetCurrentThreadId()) return -1;
  int p = THREAD_PRIORITY_NORMAL;
  if (prio <= -15) p = THREAD_PRIORITY_TIME_CRITICAL;
  else if (prio <= -5) p = THREAD_PRIORITY_HIGHEST;
  else if (prio < 0) p = THREAD_PRIORITY_ABOVE_NORMAL;
  else if (prio >= 10) p = THREAD_PRIORITY_LOWEST;
  else if (prio > 0) p = THREAD_PRIORITY_BELOW_NORMAL;
  return SetThreadPriority(GetCurrentThread(), p) ? 0 : -1;
}
#define setpriority kp_compat_setpriority
#endif
