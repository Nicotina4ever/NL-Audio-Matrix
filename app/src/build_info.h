#pragma once

// Build identity.  These values are intentionally in one header so the GUI,
// router and diagnostics report the same compile-time identity.  CMake may
// override NL_BUILD_ID with a configure-time stamp; otherwise the compiler
// timestamp distinguishes binaries that share a nominal product version.
#ifndef NL_BUILD_ID
 #define NL_BUILD_ID (__DATE__ " " __TIME__)
#endif

#ifndef NL_BUILD_VERSION
 #define NL_BUILD_VERSION "0.5.0-alpha"
#endif

#ifndef NL_BUILD_ARCH
 #if defined(_M_X64) || defined(__x86_64__)
  #define NL_BUILD_ARCH "x86_64"
 #else
  #define NL_BUILD_ARCH "unknown"
 #endif
#endif

#ifndef NL_BUILD_COMPILER
 #if defined(_MSC_VER)
  #define NL_BUILD_COMPILER "MSVC"
 #else
  #define NL_BUILD_COMPILER "unknown"
 #endif
#endif

namespace nlmatrix
{
    inline const char* buildId() noexcept      { return NL_BUILD_ID; }
    inline const char* buildVersion() noexcept { return NL_BUILD_VERSION; }
    inline const char* buildArch() noexcept    { return NL_BUILD_ARCH; }
    inline const char* buildCompiler() noexcept { return NL_BUILD_COMPILER; }
}
