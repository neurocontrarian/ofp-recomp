# The native Linux host: a 32-bit program, so guest addresses are host addresses.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(CMAKE_C_FLAGS_INIT "-m32")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-m32 -no-pie")
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)
set(ENV{PKG_CONFIG_PATH} "/usr/lib/i386-linux-gnu/pkgconfig:/usr/lib32/pkgconfig:/usr/lib/pkgconfig")
