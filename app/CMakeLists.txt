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
option(LITEV_PGO_USE "Build the core with the recorded PGO profile" OFF)
if (LITEV_PGO_USE)
    add_compile_options(
        -fprofile-instr-use=${CMAKE_CURRENT_SOURCE_DIR}/${ANDROID-CPP}/pgo/core.profdata
        -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date -Wno-profile-instr-missing)
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
        ${ANDROID-CPP}/retroachievements/RetroAchievementsManager.cpp
)

target_include_directories(melonDS-android-frontend PUBLIC
        ${ANDROID-CPP}/
        ${CORE-LIB}/src/frontend
        ${OBOE}/include
        ${FAAD2}/include
        ${RCHEEVOS}/include
)
target_link_libraries(melonDS-android-frontend android core net-utils oboe faad enet rcheevos EGL GLESv3)

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
