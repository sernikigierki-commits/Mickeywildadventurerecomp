cmake_minimum_required(VERSION 3.20)
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp")
if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/runtime.cmake")
  message(FATAL_ERROR
    "PSXRecomp runtime is missing: ${PSXRECOMP_ROOT}/runtime/runtime.cmake\n"
    "Place a psxrecomp checkout at ./psxrecomp. This output was generated/tested with v0.3.3-alpha (commit 2da25c18).")
endif()

project(MickeyWildAdventureRecomp C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 17)

option(MICKEY_ANDROID_GAMEPLAY_PROFILE
  "Enable short-lived Android gameplay profiling instrumentation" OFF)

if(ANDROID)
  enable_language(ASM)
  set(MICKEY_LIBUCONTEXT "${CMAKE_CURRENT_SOURCE_DIR}/android-tv/vendor/libucontext")
  if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(MICKEY_LIBUCONTEXT_ARCH "aarch64")
  elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(MICKEY_LIBUCONTEXT_ARCH "arm")
  else()
    message(FATAL_ERROR "Unsupported Android ABI: ${ANDROID_ABI}")
  endif()
  add_library(mickey_ucontext STATIC
    "${MICKEY_LIBUCONTEXT}/arch/${MICKEY_LIBUCONTEXT_ARCH}/getcontext.S"
    "${MICKEY_LIBUCONTEXT}/arch/${MICKEY_LIBUCONTEXT_ARCH}/setcontext.S"
    "${MICKEY_LIBUCONTEXT}/arch/${MICKEY_LIBUCONTEXT_ARCH}/swapcontext.S"
    "${MICKEY_LIBUCONTEXT}/arch/${MICKEY_LIBUCONTEXT_ARCH}/makecontext.c"
    "${MICKEY_LIBUCONTEXT}/arch/${MICKEY_LIBUCONTEXT_ARCH}/trampoline.c")
  target_include_directories(mickey_ucontext PUBLIC
    "${MICKEY_LIBUCONTEXT}/include"
    "${MICKEY_LIBUCONTEXT}/arch/common/include"
    "${MICKEY_LIBUCONTEXT}/arch/common"
    "${MICKEY_LIBUCONTEXT}/arch/${MICKEY_LIBUCONTEXT_ARCH}")
  target_compile_definitions(mickey_ucontext PRIVATE _DEFAULT_SOURCE)
endif()

# Production/portable build defaults.
set(PSX_RECOMP_UI OFF CACHE BOOL "" FORCE)
set(PSX_REWIND OFF CACHE BOOL "" FORCE)
set(PSX_NETPLAY OFF CACHE BOOL "" FORCE)
set(PSX_SETUP_WIZARD OFF CACHE BOOL "" FORCE)
if(ANDROID AND NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(PSX_DEBUG_TOOLS OFF CACHE BOOL "" FORCE)
endif()
if(ANDROID)
  set(PSX_STATIC_RUNTIME OFF CACHE BOOL "" FORCE)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/build/_deps/sdl3-src/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_SDL3
      "${CMAKE_CURRENT_SOURCE_DIR}/build/_deps/sdl3-src" CACHE PATH "" FORCE)
  endif()
else()
  set(PSX_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
endif()
set(PSXRECOMP_BIOS_STEMS "OpenBIOS" CACHE STRING "" FORCE)

include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

file(GLOB GAME_FULL CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/generated/*_full.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/generated/*_full_*.c")
file(GLOB GAME_DISPATCH "${CMAKE_CURRENT_SOURCE_DIR}/generated/*_dispatch.c")

psxrecomp_add_runtime_target(psx-runtime
  GAME_GENERATED_FULL_C "${GAME_FULL}"
  GAME_GENERATED_DISPATCH_C "${GAME_DISPATCH}"
  WINDOW_TITLE "Mickey's Wild Adventure Recompiled"
  DEFAULT_GAME_CONFIG_PATH "game.toml"
)

# BEGIN EMBEDDED_OPENBIOS_DIRECT
target_sources(psx-runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/embedded_openbios.cpp")
target_compile_definitions(psx-runtime PRIVATE PSX_EMBEDDED_OPENBIOS=1 MICKEY_FRONTEND_REQUIRED=1)
if(ANDROID AND MICKEY_ANDROID_GAMEPLAY_PROFILE)
  target_compile_definitions(psx-runtime PRIVATE MICKEY_ANDROID_GAMEPLAY_PROFILE=1)
endif()
# END EMBEDDED_OPENBIOS_DIRECT

# MICKEY_RA_I18N_V1 - official RetroAchievements rcheevos
if(NOT TARGET psx-runtime)
  message(FATAL_ERROR "Mickey RA: expected psxrecomp runtime target psx-runtime was not created")
endif()
set(MICKEY_RCHEEVOS_ROOT "${CMAKE_SOURCE_DIR}/psxrecomp/third_party/rcheevos")
if(NOT EXISTS "${MICKEY_RCHEEVOS_ROOT}/include/rc_client.h")
  message(FATAL_ERROR "Mickey RA: rcheevos is missing. Run BUILD_AND_RELEASE_RA_I18N_V1.sh (it clones the pinned official library first).")
endif()
file(GLOB MICKEY_RA_TOP "${MICKEY_RCHEEVOS_ROOT}/src/*.c")
file(GLOB MICKEY_RA_CORE "${MICKEY_RCHEEVOS_ROOT}/src/rcheevos/*.c")
file(GLOB MICKEY_RA_API "${MICKEY_RCHEEVOS_ROOT}/src/rapi/*.c")
file(GLOB MICKEY_RA_HASH "${MICKEY_RCHEEVOS_ROOT}/src/rhash/*.c")
list(REMOVE_ITEM MICKEY_RA_TOP "${MICKEY_RCHEEVOS_ROOT}/src/rc_libretro.c" "${MICKEY_RCHEEVOS_ROOT}/src/rc_client_external.c")
target_sources(psx-runtime PRIVATE
  "${CMAKE_SOURCE_DIR}/psxrecomp/runtime/src/mickey_ra.cpp"
  ${MICKEY_RA_TOP} ${MICKEY_RA_CORE} ${MICKEY_RA_API} ${MICKEY_RA_HASH})
target_include_directories(psx-runtime PRIVATE "${MICKEY_RCHEEVOS_ROOT}/include")
target_compile_definitions(psx-runtime PRIVATE RC_DISABLE_LUA RC_CLIENT_SUPPORTS_HASH)

if(ANDROID)
  target_sources(psx-runtime PRIVATE "${CMAKE_SOURCE_DIR}/psxrecomp/runtime/src/mickey_http.cpp")
  if(ANDROID_ABI STREQUAL "armeabi-v7a")
    target_sources(psx-runtime PRIVATE "${CMAKE_SOURCE_DIR}/psxrecomp/runtime/src/mickey_wait_accel.c")
  endif()
  target_link_libraries(psx-runtime PRIVATE mickey_ucontext GLESv3 EGL log android)
elseif(NOT WIN32)
  find_package(CURL REQUIRED)
  target_sources(psx-runtime PRIVATE "${CMAKE_SOURCE_DIR}/psxrecomp/runtime/src/mickey_http.cpp")
  target_link_libraries(psx-runtime PRIVATE CURL::libcurl)
endif()
