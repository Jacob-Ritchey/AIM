#pragma once
#ifdef _WIN32
#  include <windows.h>
#  include <time.h>
#  ifndef CLOCK_MONOTONIC
#    define CLOCK_MONOTONIC 1
#  endif
static inline int aim3_clock_gettime(struct timespec *ts) {
    static LARGE_INTEGER freq = {0};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    ts->tv_sec  = (time_t)(count.QuadPart / freq.QuadPart);
    ts->tv_nsec = (long)((count.QuadPart % freq.QuadPart)
                         * 1000000000LL / freq.QuadPart);
    return 0;
}
#  define clock_gettime(clk, ts) aim3_clock_gettime(ts)
#else
#  include <time.h>
#endif
