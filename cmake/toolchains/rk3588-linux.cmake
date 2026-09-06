set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

if(NOT DEFINED ENV{RK3588_SYSROOT} OR "$ENV{RK3588_SYSROOT}" STREQUAL "")
    message(FATAL_ERROR "Set RK3588_SYSROOT to the absolute path of the board sysroot")
endif()
set(CMAKE_SYSROOT "$ENV{RK3588_SYSROOT}")
if(NOT IS_ABSOLUTE "${CMAKE_SYSROOT}" OR
   NOT EXISTS "${CMAKE_SYSROOT}/usr/include/stdio.h" OR
   NOT EXISTS "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu")
    message(FATAL_ERROR "RK3588_SYSROOT must be an absolute path containing board development headers and ARM64 libraries")
endif()

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Run the host pkg-config, but only search the target metadata.
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${CMAKE_SYSROOT}")
set(ENV{PKG_CONFIG_LIBDIR}
    "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${CMAKE_SYSROOT}/usr/lib/pkgconfig:${CMAKE_SYSROOT}/usr/share/pkgconfig")
