#pragma once

// Roundtrip timing instrumentation for VBox GPU Bridge.
// Provides microsecond timestamps and [RT seq=N] tagged logging.
// Define VBOXGPU_TIMING=1 to enable, 0 to disable (zero overhead).
//
// On the Guest ICD side (static CRT, fprintf(stderr) doesn't work),
// define VBOXGPU_TIMING_WIN32_LOG before including this header to use
// a Win32 file-based log (S:\bld\vboxgpu\icd_timing.log).

#ifndef VBOXGPU_TIMING
#define VBOXGPU_TIMING 1
#endif

#include <cstdint>
#include <cstdio>
#include <chrono>

#if VBOXGPU_TIMING

// Microseconds since process start (monotonic)
inline uint64_t rtNowUs() {
    static auto t0 = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(now - t0).count();
}

// Milliseconds (float) since process start
inline double rtNowMs() {
    return rtNowUs() / 1000.0;
}

#ifdef VBOXGPU_TIMING_WIN32_LOG

// Win32 file-based timing log for Guest ICD (static CRT can't use fprintf/stderr)
#include <windows.h>
inline void rtLogWrite(const char* msg) {
    HANDLE h = CreateFileA("S:\\bld\\vboxgpu\\icd_timing.log",
        FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(h, msg, (DWORD)strlen(msg), &written, NULL);
        WriteFile(h, "\r\n", 2, &written, NULL);
        CloseHandle(h);
    }
}

#define RT_LOG(seq, tag, fmt, ...) do { \
    char _rtBuf[512]; \
    snprintf(_rtBuf, sizeof(_rtBuf), "[RT seq=%u %s @%.2fms] " fmt, \
             (unsigned)(seq), (tag), rtNowMs(), ##__VA_ARGS__); \
    rtLogWrite(_rtBuf); \
} while(0)

#else

#define RT_LOG(seq, tag, fmt, ...) \
    fprintf(stderr, "[RT seq=%u %s @%.2fms] " fmt "\n", \
            (unsigned)(seq), (tag), rtNowMs(), ##__VA_ARGS__)

#endif // VBOXGPU_TIMING_WIN32_LOG

#else

#define RT_LOG(seq, tag, fmt, ...) ((void)0)
inline uint64_t rtNowUs() { return 0; }
inline double rtNowMs() { return 0; }

#endif // VBOXGPU_TIMING
