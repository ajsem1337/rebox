# Cross-compile for the Wine side: x86_64 Windows with mingw-w64.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
# winpthreads (AnyPS5's ThreadOwned.hpp uses pthread.h); the flake passes its path.
if(DEFINED MINGW_PTHREADS)
    include_directories(SYSTEM "${MINGW_PTHREADS}/include")
    link_directories("${MINGW_PTHREADS}/lib")
endif()
