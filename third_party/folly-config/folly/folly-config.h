// cpp-pi vendored folly 的静态配置 —— 目标平台: macOS (Apple Silicon, clang)
// 对应 folly/CMake/folly-config.h.cmake, 裁剪到 json/dynamic 所需的最小面.

#pragma once

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

#if !defined(FOLLY_MOBILE)
#if defined(__ANDROID__) || \
    (defined(__APPLE__) &&  \
     (TARGET_IPHONE_SIMULATOR || TARGET_OS_SIMULATOR || TARGET_OS_IPHONE))
#define FOLLY_MOBILE 1
#else
#define FOLLY_MOBILE 0
#endif
#endif // FOLLY_MOBILE

#define FOLLY_HAVE_PTHREAD 1
#define FOLLY_HAVE_PTHREAD_ATFORK 1

/* #undef FOLLY_HAVE_LIBGFLAGS */

#define FOLLY_HAVE_LIBGLOG 1

/* #undef FOLLY_USE_JEMALLOC */

#if __has_include(<features.h>)
#include <features.h>
#endif

/* #undef FOLLY_HAVE_ACCEPT4 */
#define FOLLY_HAVE_GETRANDOM 0
/* #undef FOLLY_HAVE_PREADV */
/* #undef FOLLY_HAVE_PWRITEV */
#define FOLLY_HAVE_CLOCK_GETTIME 1
/* #undef FOLLY_HAVE_PIPE2 */

/* #undef FOLLY_HAVE_IFUNC */
#define FOLLY_HAVE_UNALIGNED_ACCESS 1
#define FOLLY_HAVE_VLA 1
#define FOLLY_HAVE_WEAK_SYMBOLS 1
/* #undef FOLLY_HAVE_LINUX_VDSO */
/* #undef FOLLY_HAVE_MALLOC_USABLE_SIZE */
#define FOLLY_HAVE_INT128_T 1
#define FOLLY_HAVE_WCHAR_SUPPORT 1
/* #undef FOLLY_HAVE_EXTRANDOM_SFMT19937 */
/* #undef HAVE_VSNPRINTF_ERRORS */

/* #undef FOLLY_HAVE_LIBUNWIND */
#define FOLLY_HAVE_DWARF 0
#define FOLLY_HAVE_ELF 0
/* #undef FOLLY_HAVE_SWAPCONTEXT */
#define FOLLY_HAVE_BACKTRACE 1
/* #undef FOLLY_USE_SYMBOLIZER */
#define FOLLY_DEMANGLE_MAX_SYMBOL_SIZE 1024

/* #undef FOLLY_HAVE_SHADOW_LOCAL_WARNINGS */

#define FOLLY_HAVE_LIBLZ4 0
#define FOLLY_HAVE_LIBLZMA 0
#define FOLLY_HAVE_LIBSNAPPY 0
#define FOLLY_HAVE_LIBZ 0
#define FOLLY_HAVE_LIBZSTD 0
#define FOLLY_HAVE_LIBBZ2 0

#define FOLLY_LIBRARY_SANITIZE_ADDRESS 0

/* #undef FOLLY_SUPPORT_SHARED_LIBRARY */

#define FOLLY_HAVE_LIBRT 0

#define FOLLY_HAVE_VSOCK 0
