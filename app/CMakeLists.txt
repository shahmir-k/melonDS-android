project(melonDS-android-frontend)

cmake_minimum_required(VERSION 3.4.1)

# liteDS optimization feature flags — all OFF by default so the build is
# identical to upstream melonDS-android-lib out of the box.
option(LITEV_NEON_RENDERER    "Enable NEON SIMD 2D renderer"           OFF)
option(LITEV_SCANLINE_BATCH   "Enable scanline batching"              OFF)
option(LITEV_THREADED_RENDER  "Enable threaded renderer"              OFF)
option(LITEV_ARM7_HLE_AUDIO   "Enable ARM7 audio/SPI HLE"             OFF)
option(LITEV_ARM7_HLE_WIFI    "Enable ARM7 WiFi firmware HLE (Ph. 3)" OFF)
option(LITEV_ARM7_THREAD      "Enable experimental ARM7 CPU thread"   OFF)
option(LITEV_AGGRESSIVE_SKIP  "Enable aggressive frameskip mode"      OFF)
option(LITEV_SPU_FAST_INTERP  "Enable linear SPU interpolation"       OFF)
option(LITEV_WIDE_SCHEDULER   "Use wider NDS scheduler slices"        OFF)
option(LITEV_PBO_UPLOAD       "Use PBOs for software frame upload"    OFF)
option(LITEV_SINGLE_INSTANCE_CURRENT "Use non-TLS NDS::Current for single-instance Android builds" OFF)
option(LITEV_HEADLESS         "Build headless benchmark mode"         OFF)
option(LITEV_PGO              "PGO mode: OFF | instrument | use"      OFF)
option(LITEV_PROFILE          "Enable quickmelonDS internal frame profiler" OFF)
option(LITEV_ARM7_PROFILE     "Enable ARM7 cycle profiler"            OFF)

# LITEV_ARM7_HLE_WIFI depends on LITEV_ARM7_HLE_AUDIO (provides ARM7HLECore).
if(LITEV_ARM7_HLE_WIFI AND NOT LITEV_ARM7_HLE_AUDIO)
    message(FATAL_ERROR
        "LITEV_ARM7_HLE_WIFI requires LITEV_ARM7_HLE_AUDIO=ON "
        "(ARM7HLECore dispatch table)")
endif()

# Propagate flags into the core library build.
# The core library's CMakeLists.txt checks these via if(LITEV_*).
if(LITEV_NEON_RENDERER)
    add_compile_definitions(LITEV_NEON_RENDERER)
endif()
if(LITEV_SCANLINE_BATCH)
    add_compile_definitions(LITEV_SCANLINE_BATCH)
endif()
if(LITEV_THREADED_RENDER)
    add_compile_definitions(LITEV_THREADED_RENDER)
endif()
if(LITEV_ARM7_HLE_AUDIO)
    add_compile_definitions(LITEV_ARM7_HLE_AUDIO)
endif()
if(LITEV_ARM7_HLE_WIFI)
    add_compile_definitions(LITEV_ARM7_HLE_WIFI)
endif()
if(LITEV_ARM7_THREAD)
    add_compile_definitions(LITEV_ARM7_THREAD)
endif()
if(LITEV_AGGRESSIVE_SKIP)
    add_compile_definitions(LITEV_AGGRESSIVE_SKIP)
endif()
if(LITEV_SPU_FAST_INTERP)
    add_compile_definitions(LITEV_SPU_FAST_INTERP)
endif()
if(LITEV_WIDE_SCHEDULER)
    add_compile_definitions(LITEV_WIDE_SCHEDULER)
endif()
if(LITEV_PBO_UPLOAD)
    add_compile_definitions(LITEV_PBO_UPLOAD)
endif()
if(LITEV_SINGLE_INSTANCE_CURRENT)
    add_compile_definitions(LITEV_SINGLE_INSTANCE_CURRENT)
endif()
if(LITEV_HEADLESS)
    add_compile_definitions(LITEV_HEADLESS)
endif()
if(LITEV_PROFILE)
    add_compile_definitions(LITEV_PROFILE=1)
else()
    add_compile_definitions(LITEV_PROFILE=0)
endif()
if(LITEV_ARM7_PROFILE)
    add_compile_definitions(LITEV_ARM7_PROFILE)
endif()

# PGO support: pass -fprofile-generate / -fprofile-use to the whole build.
if(LITEV_PGO STREQUAL "instrument")
    add_compile_options(-fprofile-generate)
    add_link_options(-fprofile-generate)
elseif(LITEV_PGO STREQUAL "use")
    add_compile_options(-fprofile-use -fprofile-correction)
    add_link_options(-fprofile-use)
endif()

add_compile_definitions(MELONDS_GL_HEADER=<GLES3/gl32.h>)
add_compile_definitions(EGL_EGLEXT_PROTOTYPES)

set(CORE-LIB ../melonDS-android-lib)
add_subdirectory(${CORE-LIB} ./melonDS-android-lib)
include_directories(${CORE-LIB}/src/android)

add_library(
        melonDS-android-frontend

        SHARED

        src/main/cpp/AndroidMelonEventMessenger.cpp
        src/main/cpp/EmulatorMessageQueueJNI.cpp
        src/main/cpp/MelonDSAndroidJNI.cpp
        src/main/cpp/MelonDSAndroidConfiguration.cpp
        src/main/cpp/MelonDSAndroidInterface.cpp
        src/main/cpp/MelonDSNandJNI.cpp
        src/main/cpp/NativeGlContext.cpp
        src/main/cpp/UriFileHandler.cpp
        src/main/cpp/JniEnvHandler.cpp
        src/main/cpp/MelonDSAndroidCameraHandler.cpp
        src/main/cpp/RetroAchievementsMapper.cpp
        src/main/cpp/RomIconBuilder.cpp
)

target_link_libraries(melonDS-android-frontend melonDS-lib)
