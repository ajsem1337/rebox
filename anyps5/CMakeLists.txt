# rebox_anyps5: AnyPS5's RDNA -> SPIR-V shader recompiler (GPL-2.0-only), built on
# its own (without AnyPS5's AGC driver or runtime) for d3d12_xs to load as a DLL.
# See docs/anyps5.md.
#
#   cmake -S anyps5 -B build/anyps5 -DANYPS5_SOURCE=<AnyPS5 checkout> \
#         -DCMAKE_TOOLCHAIN_FILE=anyps5/mingw-w64.cmake
#
# ANYPS5_SOURCE needs 3rdparty/SPIRV-Headers, SPIRV-Tools and glslang checked out,
# with rebox's patches/anyps5/ applied. MIMALLOC_SOURCE is a mimalloc 3 checkout
# (allocator.cpp).
cmake_minimum_required(VERSION 3.22)
project(rebox_anyps5 C CXX)

set(ANYPS5_SOURCE "" CACHE PATH "AnyPS5 source tree")
if(NOT EXISTS "${ANYPS5_SOURCE}/core/shader/recompiler/CMakeLists.txt")
    message(FATAL_ERROR "ANYPS5_SOURCE (${ANYPS5_SOURCE}) is not an AnyPS5 checkout")
endif()

# add_shader_recompiler() expects to run inside AnyPS5's own top-level project.
set(PROJECT_SOURCE_DIR "${ANYPS5_SOURCE}")
set(ANYPS5_USE_SYSTEM_DEPS OFF)
set(ANYPS5_ENABLE_SPIRV_TOOLS ON)
set(ANYPS5_SPIRV_HEADERS_SOURCE_DIR "${ANYPS5_SOURCE}/3rdparty/SPIRV-Headers")
set(ANYPS5_SPIRV_HEADERS_INCLUDE_DIR "${ANYPS5_SOURCE}/3rdparty/SPIRV-Headers/include")
set(ANYPS5_GLSLANG_SOURCE_DIR "${ANYPS5_SOURCE}/3rdparty/glslang")
set(ANYPS5_GLSLANG_INCLUDE_DIRS "${ANYPS5_SOURCE}/3rdparty/glslang")

# Stand-in for AnyPS5's guest libc: the recompiler only needs a few of its
# host-side headers (HostThreadLocal.hpp and friends).
add_library(rebox_anyps5_runtime INTERFACE)
target_include_directories(rebox_anyps5_runtime INTERFACE "${ANYPS5_SOURCE}/core/libs")

# mimalloc (MIT) for the DLL's operator new and delete (allocator.cpp), built static and without
# overriding malloc: the C runtime's own allocations stay on its heap. MI_WIN_INIT=FLS: mimalloc
# starts from a constructor and learns of thread exits through a fiber-local callback. Its default
# TLS callback runs before mcfgthread's (both in .CRT$XLB), and its first thread-local access then
# goes through emutls into an mcfgthread that isn't set up yet (a fault on a bad heap handle later).
set(MIMALLOC_SOURCE "" CACHE PATH "mimalloc source tree")
if(NOT EXISTS "${MIMALLOC_SOURCE}/include/mimalloc.h")
    message(FATAL_ERROR "MIMALLOC_SOURCE (${MIMALLOC_SOURCE}) is not a mimalloc checkout")
endif()
set(MI_OVERRIDE OFF)
set(MI_BUILD_SHARED OFF)
set(MI_BUILD_OBJECT OFF)
set(MI_BUILD_TESTS OFF)
set(MI_WIN_INIT FLS CACHE STRING "" FORCE)
add_subdirectory("${MIMALLOC_SOURCE}" mimalloc EXCLUDE_FROM_ALL)

include("${ANYPS5_SOURCE}/core/shader/recompiler/CMakeLists.txt")
add_shader_recompiler(anyps5_recompiler rebox_anyps5_runtime)

add_library(rebox_anyps5 SHARED rebox_anyps5.cpp spirv_remap.cpp layout.cpp layout_export.cpp host_streams.c
            allocator.cpp)
target_link_libraries(rebox_anyps5 PRIVATE anyps5_recompiler SPIRV-Tools-opt SPIRV-Tools-static mimalloc-static pthread)
# One self-contained DLL for the Wine prefix: no libstdc++/libgcc/winpthread DLLs.
target_link_options(rebox_anyps5 PRIVATE -static -static-libgcc -static-libstdc++)
set_target_properties(rebox_anyps5 PROPERTIES PREFIX "")

add_executable(sa5_check sa5_check.c)
target_link_libraries(sa5_check PRIVATE rebox_anyps5)
target_link_options(sa5_check PRIVATE -static)
