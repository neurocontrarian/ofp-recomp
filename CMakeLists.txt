cmake_minimum_required(VERSION 3.20)
project(ofp_recomp C CXX)
# 32-bit host (build-linux.sh): guest addresses are host addresses. 64-bit
# host (build-linux64.sh): the guest's 4 GB is one block of its own at
# RECOMP_MEM_BASE, the host's libraries and the GL driver outside it.
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(OFP_64 ON)
endif()
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(PCRECOMP "${CMAKE_SOURCE_DIR}/third_party/pcrecomp" CACHE PATH "pcrecomp checkout")
# SDL2_ttf for i386 when the system has none (unpacked without root, README)
set(SYSROOT_I386 "${CMAKE_SOURCE_DIR}/third_party/sysroot-i386" CACHE PATH "extra i386 libraries")

file(GLOB GEN_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/recomp/gen/*.c")
if(NOT GEN_SOURCES)
  message(FATAL_ERROR "src/recomp/gen is empty: run run_lift.py --all first")
endif()

# SDL2 and SDL2_ttf: the system's (pkg-config), unless a sysroot unpacked
# without root (third_party/sysroot-*) is there
set(SYS64 ${CMAKE_SOURCE_DIR}/third_party/sysroot-x86_64)
if(NOT OFP_64 OR NOT EXISTS ${SYS64}/usr/include/SDL2)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  pkg_check_modules(SDL2_TTF IMPORTED_TARGET SDL2_ttf)
  if(OFP_64 AND NOT SDL2_TTF_FOUND)
    message(FATAL_ERROR "SDL2_ttf not found (libsdl2-ttf-dev)")
  endif()
endif()
set(HLE ${PCRECOMP}/runtime/win32hle)
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS "${HLE}/*.c")
list(FILTER HLE_SOURCES EXCLUDE REGEX "_selftest\\.c$")
file(GLOB OFP_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/linux/*.c")
# the Vulkan renderer (d3d8_vk.c): Vulkan's headers and the memory allocator
# (the system's, or third_party/vk-tools unpacked without root); Vulkan itself
# is loaded at run time
set(VKROOT ${CMAKE_SOURCE_DIR}/third_party/vk-tools/root/usr/include)
list(APPEND OFP_SOURCES ${CMAKE_SOURCE_DIR}/src/linux/vma.cpp)
if(EXISTS ${VKROOT}/vk_mem_alloc.h)
  set_source_files_properties(${CMAKE_SOURCE_DIR}/src/linux/d3d8_vk.c ${CMAKE_SOURCE_DIR}/src/linux/vma.cpp
                              PROPERTIES INCLUDE_DIRECTORIES ${VKROOT})
endif()
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/linux/vma.cpp PROPERTIES COMPILE_OPTIONS "-w")
add_executable(ofp ${OFP_SOURCES} ${HLE_SOURCES} ${GEN_SOURCES})
target_include_directories(ofp PRIVATE src/linux src/recomp/gen ${HLE} ${PCRECOMP}/runtime/recomp32)
if(OFP_64)
  target_compile_definitions(ofp PRIVATE RECOMP_RETADDR=0u RECOMP_MEM_BASE=0x100000000000ull RECOMP_LOCAL_REGS RECOMP_X87_RING RECOMP_X87_LOCAL _REENTRANT)
else()
  target_compile_definitions(ofp PRIVATE RECOMP_RETADDR=0u RECOMP_FLAT_MEMORY RECOMP_LOCAL_REGS)
endif()
set(GEN_OPT "-O2" CACHE STRING "optimisation for the generated C")
# faster indirect calls (plan-acceleration.md, step 2): no trace ring (crash
# reports lose it), a per-site cache of the last target
option(OFP_FAST_CALLS "no indirect-call trace ring, per-site target cache" OFF)
if(OFP_FAST_CALLS)
  target_compile_definitions(ofp PRIVATE RECOMP_NO_ICALL_TRACE RECOMP_ICALL_SITE_CACHE)
endif()
# GEN_OPT may hold several options, space separated ("-O2 -march=x86-64-v3 -ffp-contract=off")
separate_arguments(GEN_OPT_LIST UNIX_COMMAND "${GEN_OPT}")
set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;${GEN_OPT_LIST};-fno-strict-aliasing;-fwrapv")
# hot regions of lifted functions, translated by tools/region_c.py: the
# generated code's options, and never a fused multiply-add (the x87 rounds each)
file(GLOB REGION_SOURCES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/linux/ofp_region_*.c")
set_source_files_properties(${REGION_SOURCES} PROPERTIES COMPILE_OPTIONS "${GEN_OPT_LIST};-ffp-contract=off;-fno-strict-aliasing;-fwrapv")
# lifted functions replaced by hand-written ones although their own file calls
# them: made weak there (src/linux/ofp_overrides.h)
foreach(n 0051 0054)
  set_property(SOURCE ${CMAKE_SOURCE_DIR}/src/recomp/gen/recomp_${n}.c APPEND PROPERTY COMPILE_OPTIONS
               "-include;${CMAKE_SOURCE_DIR}/src/linux/ofp_overrides.h;-DOFP_GEN_FILE=1${n}")
endforeach()
target_compile_options(ofp PRIVATE -fno-strict-aliasing -fwrapv)
# hand-written native versions of slow lifted functions (src/linux/ofp_native.c)
target_link_options(ofp PRIVATE -Wl,--wrap=sub_00618794 -Wl,--wrap=sub_00618959 -Wl,--wrap=sub_005B6637 -Wl,--wrap=sub_00508542 -Wl,--wrap=DisplayOptions__virtual_22 -Wl,--wrap=sub_0064DA91)
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/linux/ofp_native.c PROPERTIES COMPILE_OPTIONS "-O2;-msse2;-mfpmath=sse;-ffp-contract=off")
# the build, named in the performance log
execute_process(COMMAND git describe --always --dirty WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
                OUTPUT_VARIABLE OFP_GIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
set_source_files_properties(${CMAKE_SOURCE_DIR}/src/linux/perf.c PROPERTIES
                            COMPILE_DEFINITIONS "OFP_GIT=\"${OFP_GIT}\";OFP_GEN_OPT=\"${GEN_OPT}\"")
if(OFP_64 AND NOT EXISTS ${SYS64}/usr/include/SDL2)
  target_link_libraries(ofp PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF GL pthread m)
elseif(OFP_64)
  # SDL2's headers and SDL2_ttf for x86-64, unpacked without root
  target_include_directories(ofp SYSTEM BEFORE PRIVATE ${SYS64}/usr/include/x86_64-linux-gnu ${SYS64}/usr/include)
  target_link_libraries(ofp PRIVATE ${SYS64}/usr/lib/x86_64-linux-gnu/libSDL2_ttf-2.0.so.0
                        /usr/lib/x86_64-linux-gnu/libSDL2-2.0.so.0 GL pthread m)
  target_link_options(ofp PRIVATE -Wl,--disable-new-dtags -Wl,-rpath,${SYS64}/usr/lib/x86_64-linux-gnu)
elseif(SDL2_TTF_FOUND)
  target_link_libraries(ofp PRIVATE PkgConfig::SDL2 PkgConfig::SDL2_TTF GL pthread m)
else()
  set(TTF_LIB ${SYSROOT_I386}/usr/lib/i386-linux-gnu)
  target_include_directories(ofp PRIVATE ${SYSROOT_I386}/usr/include)
  target_link_libraries(ofp PRIVATE PkgConfig::SDL2 ${TTF_LIB}/libSDL2_ttf.so GL pthread m)
  target_link_options(ofp PRIVATE -Wl,--disable-new-dtags -Wl,-rpath,${TTF_LIB} -Wl,-rpath-link,${TTF_LIB})
endif()
