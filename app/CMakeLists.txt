cmake_minimum_required(VERSION 3.4.1)

project(melonDS-android-frontend)

add_compile_definitions(MELONDS_GL_HEADER=<GLES3/gl32.h>)
add_compile_definitions(EGL_EGLEXT_PROTOTYPES)

set(BUILD_QT_SDL OFF)
set(ENABLE_GDBSTUB OFF)

set(ANDROID-CPP src/main/cpp)
set(CORE-LIB ../melonDS-android-lib)
set(OBOE ${ANDROID-CPP}/oboe)
set(FAAD2 ${ANDROID-CPP}/faad2)
set(RCHEEVOS ${ANDROID-CPP}/rcheevos)

# LITEV_HOT_ORDER: place the emulator thread's hottest C++ functions next to each other
# in the .so (lld --symbol-ordering-file), so the hot set spans fewer i-cache lines and
# pages on the in-order A55. Every function gets its own section so the linker can move
# it; must be set before the core is added. hot-symbols.order is generated from
# capture.sh cycle + l1i_cache_refill profiles of Pokemon White. Layout only (A).
option(LITEV_HOT_ORDER "Order hot C++ functions together at link time" OFF)
if (LITEV_HOT_ORDER)
    add_compile_options(-ffunction-sections)
endif()

# LITEV_PGO_USE: clang profile-guided optimisation from a profile recorded by an
# instrumented liteDS-headless (-fprofile-instr-generate) playing Pokemon White and
# Shrek on the RG DS. Changes code layout and inlining, not semantics (no fast-math) (A).
# LITEV_THINLTO: ThinLTO across the core and frontend. The RelWithDebInfo build never got
# the core's release-only LTO, so cross-file calls (GPU3D, DMA, SPU, NDS IO helpers) were
# never inlined. Optimisation scope only, no semantic change (A).
option(LITEV_THINLTO "Build the core and frontend with ThinLTO" OFF)
if (LITEV_THINLTO)
    add_compile_options(-flto=thin)
    add_link_options(-flto=thin)
endif()

# LITEV_MTUNE_A55: schedule instructions for the Cortex-A55 (the RG DS's in-order core)
# instead of the NDK's generic armv8-a model. Scheduling only: no new instructions, so the
# APK still runs on any arm64 device; no semantic change (A).
option(LITEV_MTUNE_A55 "Tune instruction scheduling for Cortex-A55" OFF)
if (LITEV_MTUNE_A55)
    add_compile_options(-mtune=cortex-a55)
    add_link_options(-mtune=cortex-a55)
endif()

option(LITEV_PGO_USE "Build the core with the recorded PGO profile" OFF)
if (LITEV_PGO_USE)
    # IR-level profile plus a context-sensitive (CSPGO) pass recorded on the ThinLTO build;
    # the CS part is applied at link time, so the link needs -fprofile-use as well.
    set(LITEV_PGO_PROFILE ${CMAKE_CURRENT_SOURCE_DIR}/${ANDROID-CPP}/pgo/core-cs.profdata)
    add_compile_options(-fprofile-use=${LITEV_PGO_PROFILE}
        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
    # The profile comes from liteDS-headless, so functions that differ in this build (main
    # of the bundled faad_cli, Android-only paths) mismatch; lld warns, and the NDK links
    # with --fatal-warnings. A mismatched function just gets no profile data.
    add_link_options(-fprofile-use=${LITEV_PGO_PROFILE} -Wl,--no-lto-pgo-warn-mismatch)
endif()

# LITEV_LAN_STATS: measurement-only LAN multiplayer wait statistics (core src/net/LAN.cpp).
option(LITEV_LAN_STATS "Log LAN multiplayer wait statistics" OFF)
if (LITEV_LAN_STATS)
    add_compile_definitions(LITEV_LAN_STATS)
endif()
# + kernel receive timestamps per stage: needs ENet patched (profiler tools/profiler/enet-rx-timestamps.patch)
option(LITEV_LAN_KTIME "LAN stats: per-stage times from kernel receive timestamps (patched ENet)" OFF)
if (LITEV_LAN_KTIME)
    add_compile_definitions(LITEV_LAN_KTIME)
endif()
# Hosted Netplay (core src/net/HostedMP): the frontend and net-utils need the define too
option(LITEV_HOSTED_NETPLAY "Hosted Netplay: one device runs every console, the others replay their own" OFF)
if (LITEV_HOSTED_NETPLAY)
    add_compile_definitions(LITEV_HOSTED_NETPLAY=1)
endif()

add_subdirectory(${CORE-LIB} ./melonDS-android-lib)
add_subdirectory(${OBOE} ./oboe)
add_subdirectory(${FAAD2} ./faad2)
add_subdirectory(${RCHEEVOS} ./rcheevos)
add_subdirectory(${ANDROID-CPP}/enet ./enet)
target_include_directories(enet INTERFACE ${ANDROID-CPP}/enet/include)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/${CORE-LIB}/cmake")
add_subdirectory(${CORE-LIB}/src/net ./net-utils)

add_library(
        melonDS-android-frontend

        SHARED

        ${ANDROID-CPP}/AndroidMelonEventMessenger.cpp
        ${ANDROID-CPP}/CartLoader.cpp
        ${ANDROID-CPP}/DSiSupport.cpp
        ${ANDROID-CPP}/EmulatorArgsBuilder.cpp
        ${ANDROID-CPP}/EmulatorMessageQueueJNI.cpp
        ${ANDROID-CPP}/JniEnvHandler.cpp
        ${ANDROID-CPP}/MelonDS.cpp
        ${ANDROID-CPP}/MelonDSAndroidCameraHandler.cpp
        ${ANDROID-CPP}/MelonDSAndroidConfiguration.cpp
        ${ANDROID-CPP}/MelonDSAndroidInterface.cpp
        ${ANDROID-CPP}/MelonDSAndroidJNI.cpp
        ${ANDROID-CPP}/MelonDSAudio.cpp
        ${ANDROID-CPP}/MelonDSNandJNI.cpp
        ${ANDROID-CPP}/MelonInstance.cpp
        ${ANDROID-CPP}/MicInputOboeCallback.cpp
        ${ANDROID-CPP}/NativeGlContext.cpp
        ${ANDROID-CPP}/OboeCallback.cpp
        ${ANDROID-CPP}/OpenGLContext.cpp
        ${ANDROID-CPP}/PlatformAndroid.cpp
        ${ANDROID-CPP}/PlatformAndroid_AAC.cpp
        ${ANDROID-CPP}/RetroAchievementsMapper.cpp
        ${ANDROID-CPP}/RewindManager.cpp
        ${ANDROID-CPP}/RomIconBuilder.cpp
        ${ANDROID-CPP}/SDCardArgsBuilder.cpp
        ${ANDROID-CPP}/SaveManager.cpp
        ${ANDROID-CPP}/UriFileHandler.cpp
        ${ANDROID-CPP}/performancehint/JniPerformanceHintManager.cpp
        ${ANDROID-CPP}/performancehint/NdkPerformanceHintManager.cpp
        ${ANDROID-CPP}/performancehint/PerformanceHintManagerFactory.cpp
        ${ANDROID-CPP}/performancehint/ThreadSafePerformanceHintSession.cpp
        ${ANDROID-CPP}/renderer/FrameQueue.cpp
        ${ANDROID-CPP}/renderer/ScreenshotRenderer.cpp
        ${ANDROID-CPP}/renderer/RecordVideo.cpp
        ${ANDROID-CPP}/retroachievements/RetroAchievementsManager.cpp
)

target_include_directories(melonDS-android-frontend PUBLIC
        ${ANDROID-CPP}/
        ${CORE-LIB}/src/frontend
        ${OBOE}/include
        ${FAAD2}/include
        ${RCHEEVOS}/include
)
target_link_libraries(melonDS-android-frontend android core net-utils oboe faad enet rcheevos EGL GLESv3 mediandk z)

# LITEV_LINKOPT (app half). The core is a STATIC archive and the final shared
# object libmelonDS-android-frontend.so is linked here, so the link flag belongs
# on this target. The core applies -fno-plt to its own TUs; mirror it onto this
# target's TUs and add -Wl,-Bsymbolic-functions so calls between functions inside
# the .so bind locally instead of going through the PLT/GOT. Host linkage only.
if (LITEV_LINKOPT)
    target_compile_options(melonDS-android-frontend PRIVATE -fno-plt)
    target_link_options(melonDS-android-frontend PRIVATE -Wl,-Bsymbolic-functions)
endif()

if (LITEV_HOT_ORDER)
    target_link_options(melonDS-android-frontend PRIVATE
        -Wl,--symbol-ordering-file=${CMAKE_CURRENT_SOURCE_DIR}/${ANDROID-CPP}/hot-symbols.order
        -Wl,--no-warn-symbol-ordering)
endif()
