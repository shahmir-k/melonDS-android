#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <jni.h>
#include <string>
#include <sstream>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <time.h>
#include <sched.h>
#include <sys/system_properties.h>
#include <MelonDS.h>
#include <MelonDSAudio.h>
#include <RomGbaSlotConfig.h>
#include <android/asset_manager_jni.h>
#include "UriFileHandler.h"
#include "JniEnvHandler.h"
#include "MelonLog.h"
#include "AndroidMelonEventMessenger.h"
#include "MelonDSAndroidInterface.h"
#include "MelonDSAndroidConfiguration.h"
#include "MelonDSAndroidCameraHandler.h"
#include "RetroAchievementsMapper.h"
#include "performancehint/ThreadSafePerformanceHintSession.h"
#include "performancehint/PerformanceHintManagerFactory.h"

#include "Platform.h"
#include "net/MPInterface.h"
#include "net/LAN.h"

enum GbaSlotType {
    NONE = 0,
    GBA_ROM = 1,
    RUMBLE_PAK = 2,
    MEMORY_EXPANSION = 3,
};

void* emulate(void*);
static void lanEndAll();
MelonDSAndroid::RomGbaSlotConfig* buildGbaSlotConfig(GbaSlotType slotType, const char* romPath, const char* savePath);

pthread_t emuThread;
pthread_mutex_t emuThreadMutex;
pthread_cond_t emuThreadCond;

bool started = false;
bool stop;
bool paused;
std::atomic_bool isThreadReallyPaused = false;
int observedFrames = 0;
float fps = 0;
int targetFps;
float fastForwardSpeedMultiplier;
bool limitFps = true;
bool isFastForwardEnabled = false;
#ifdef LITEV_AGGRESSIVE_SKIP
// Frameskip-based fast-forward ("Fast-forward max frameskip" setting, 0..9, default 0).
// The limiter-only fast-forward can't speed anything up when the device already runs
// below 60fps (no sleep to reclaim); skipping rasterisation (CPU/DMA/timers keep
// running) makes each skipped frame cheap, so the game advances several emulated frames
// per presented frame. 0 = limiter-only fast-forward, every frame rendered.
int ffMaxFrameskip = 0;
static int ffSkipApplied = -1;       // FF frameskip target last applied; -1 = FF not driving

// Speed multiplier -> raster-skip target: render ~1 of M frames, capped by the setting;
// -1 (unlimited) uses the setting. GPU::SetFrameskipTarget renders 1 of N+1 frames
// (and clamps N to LITEV_FRAMESKIP_MAX, 3 in litev-clean).
static int ffFrameskipTarget(float m, int max)
{
    if (max <= 0) return 0;
    if (m < 0) return max;
    int s = (int)(m + 0.5f) - 1;
    if (s < 1) s = 1;
    return s < max ? s : max;
}
#endif
#ifdef LITEV_AUTO_FRAMESKIP
// Adaptive frameskip (user setting) that holds real-time speed instead of slow-mo when a
// scene can't sustain 60fps. Distinct from fast-forward; only active when FF is OFF.
bool autoFrameskipEnabled = false;
static int    autoFsSkip     = 0;    // current adaptive raster-skip level (0..3)
static double autoFsEmaMs    = 0.0;  // FAST EMA of per-frame wall time (attack signal)
static int    autoFsCooldown = 0;    // frames to wait after a change (let it settle)
static int    autoFsHeadroom = 0;    // consecutive low-load frames (slow-release counter)
#endif

jobject globalCameraManager;
MelonDSAndroidCameraHandler* androidCameraHandler;

static const int64_t FRAME_DURATION_60FPS_NS = 16666666;
static const int64_t FRAME_DURATION_1000FPS_NS = 1000000; // 1ms. Used as frame time when fast-forward is enabled
ThreadSafePerformanceHintSession* performanceHintSession = nullptr;

extern "C"
{
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setupEmulator(JNIEnv* env, jobject thiz, jobject emulatorConfiguration, jobject cameraManager, jobject screenshotBuffer)
{
    MelonDSAndroid::EmulatorConfiguration finalEmulatorConfiguration = MelonDSAndroidConfiguration::buildEmulatorConfiguration(env, emulatorConfiguration);
    fastForwardSpeedMultiplier = finalEmulatorConfiguration.fastForwardSpeedMultiplier;
#ifdef LITEV_AUTO_FRAMESKIP
    autoFrameskipEnabled = finalEmulatorConfiguration.autoFrameskipEnabled;
    // New emulator session (the emu thread is not running yet): start from no skip.
    autoFsSkip = 0; autoFsEmaMs = 0.0; autoFsCooldown = 0; autoFsHeadroom = 0;
#endif
#ifdef LITEV_AGGRESSIVE_SKIP
    ffMaxFrameskip = finalEmulatorConfiguration.fastForwardMaxFrameskip;
    ffSkipApplied = -1;
#endif

    globalCameraManager = env->NewGlobalRef(cameraManager);

    auto androidEventMessenger = std::make_shared<AndroidMelonEventMessenger>();
    androidCameraHandler = new MelonDSAndroidCameraHandler(jniEnvHandler, globalCameraManager);
    u32* screenshotBufferPointer = (u32*) env->GetDirectBufferAddress(screenshotBuffer);

    MelonDSAndroid::setConfiguration(std::move(finalEmulatorConfiguration));
    MelonDSAndroid::setup(androidCameraHandler, std::move(androidEventMessenger), screenshotBufferPointer, 0);
    paused = false;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setupCheats(JNIEnv* env, jobject thiz, jobjectArray cheats)
{
    jsize cheatCount = env->GetArrayLength(cheats);
    if (cheatCount < 1) {
        MelonDSAndroid::setCodeList(std::list<MelonDSAndroid::Cheat>());
        return;
    }

    jclass cheatClass = env->GetObjectClass(env->GetObjectArrayElement(cheats, 0));
    jfieldID codeField = env->GetFieldID(cheatClass, "code", "Ljava/lang/String;");

    std::list<MelonDSAndroid::Cheat> internalCheats;

    for (int i = 0; i < cheatCount; ++i) {
        jobject cheat = env->GetObjectArrayElement(cheats, i);
        jstring code = (jstring) env->GetObjectField(cheat, codeField);
        const char* codeStringPtr = env->GetStringUTFChars(code, JNI_FALSE);
        std::string codeString = codeStringPtr;
        // Since each part of a cheat code has 8 characters (4 bytes), we can add 1 to the length (to ensure that each part has a matching space separator) and divide by 9
        // (part length + space separator) to calculate the total number of parts in the cheat
        size_t codeLength = (codeString.size() + 1) / 9;

        bool isBad = false;
        std::size_t start = 0;
        std::size_t end = 0;

        MelonDSAndroid::Cheat internalCheat;
        internalCheat.code.reserve(codeLength);

        // Split code string into sections separated by a space
        while ((end = codeString.find(' ', start)) != std::string::npos) {
            if (end != start) {
                char* endPointer;
                std::string sectionString = codeString.substr(start, end - start);
                // Each code section must be 4 bytes (8 hex characters)
                if (sectionString.size() != 8) {
                    isBad = true;
                    break;
                }

                unsigned long section = strtoul(sectionString.c_str(), &endPointer, 16);
                if (*endPointer == 0) {
                    internalCheat.code.push_back((u32) section);
                } else {
                    isBad = true;
                    break;
                }
            }
            start = end + 1;
        }

        if (!isBad && end != start) {
            char* endPointer;
            std::string sectionString = codeString.substr(start, end - start);
            if (sectionString.size() != 8) {
                isBad = true;
            } else {
                unsigned long section = strtoul(sectionString.c_str(), &endPointer, 16);
                internalCheat.code.push_back((u32) section);
            }
        }

        env->ReleaseStringUTFChars(code, codeStringPtr);

        if (isBad) {
            continue;
        }

        internalCheats.push_back(internalCheat);
    }

    MelonDSAndroid::setCodeList(internalCheats);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setupAchievements(JNIEnv* env, jobject thiz, jobjectArray achievements, jobjectArray leaderboards, jstring richPresenceScript)
{
    std::list<MelonDSAndroid::RetroAchievements::RAAchievement> internalAchievements;
    std::list<MelonDSAndroid::RetroAchievements::RALeaderboard> internalLeaderboards;
    mapAchievementsFromJava(env, achievements, internalAchievements);
    mapLeaderboardsFromJava(env, leaderboards, internalLeaderboards);

    std::optional<std::string> richPresence = std::nullopt;

    if (richPresenceScript != nullptr)
    {
        jboolean isStringCopy;
        const char* richPresenceString = env->GetStringUTFChars(richPresenceScript, &isStringCopy);
        richPresence = richPresenceString;

        if (isStringCopy)
            env->ReleaseStringUTFChars(richPresenceScript, richPresenceString);
    }

    MelonDSAndroid::setupAchievements(internalAchievements, internalLeaderboards, richPresence);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_unloadRetroAchievementsData(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::unloadRetroAchievementsData();
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_getRichPresenceStatus(JNIEnv* env, jobject thiz)
{
    std::string richPresenceString = MelonDSAndroid::getRichPresenceStatus();
    if (richPresenceString.empty())
        return nullptr;
    else
        return env->NewStringUTF(richPresenceString.c_str());
}

JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_getRuntimeAchievements(JNIEnv* env, jobject thiz)
{
    jclass simpleRuntimeAchievementClass = env->FindClass("me/magnum/melonds/domain/model/retroachievements/RASimpleRuntimeAchievement");
    jmethodID simpleRuntimeAchievementConstructor = env->GetMethodID(simpleRuntimeAchievementClass, "<init>", "(JII)V");

    auto runtimeAchievements = MelonDSAndroid::getRuntimeAchievements();

    jobjectArray achievements = env->NewObjectArray(runtimeAchievements.size(), simpleRuntimeAchievementClass, nullptr);

    int index = 0;
    for (const auto &item: runtimeAchievements)
    {
        jobject simpleRuntimeAchievement = env->NewObject(simpleRuntimeAchievementClass, simpleRuntimeAchievementConstructor, item.id, (jint) item.value, (jint) item.target);
        env->SetObjectArrayElement(achievements, index++, simpleRuntimeAchievement);
    }

    return achievements;
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_loadRomInternal(JNIEnv* env, jobject thiz, jstring romPath, jstring sramPath, jint gbaSlotType, jstring gbaRomPath, jstring gbaSramPath)
{
    jboolean isCopy = JNI_FALSE;
    const char* rom = romPath == nullptr ? nullptr : env->GetStringUTFChars(romPath, &isCopy);
    const char* sram = sramPath == nullptr ? nullptr : env->GetStringUTFChars(sramPath, &isCopy);
    const char* gbaRom = gbaRomPath == nullptr ? nullptr : env->GetStringUTFChars(gbaRomPath, &isCopy);
    const char* gbaSram = gbaSramPath == nullptr ? nullptr : env->GetStringUTFChars(gbaSramPath, &isCopy);

    MelonDSAndroid::RomGbaSlotConfig* gbaSlotConfig = buildGbaSlotConfig((GbaSlotType) gbaSlotType, gbaRom, gbaSram);
    int result = MelonDSAndroid::loadRom(rom, sram, gbaSlotConfig);
    delete gbaSlotConfig;

    if (isCopy == JNI_TRUE) {
        if (romPath) env->ReleaseStringUTFChars(romPath, rom);
        if (sramPath) env->ReleaseStringUTFChars(sramPath, sram);
        if (gbaRomPath) env->ReleaseStringUTFChars(gbaRomPath, gbaRom);
        if (gbaSramPath) env->ReleaseStringUTFChars(gbaSramPath, gbaSram);
    }

    return result;
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_bootFirmwareInternal(JNIEnv* env, jobject thiz) {
    return MelonDSAndroid::bootFirmware();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_startEmulation(JNIEnv* env, jobject thiz)
{
    stop = false;
    isThreadReallyPaused = false;
    limitFps = true;
    targetFps = 60;
    isFastForwardEnabled = false;

    pthread_mutex_init(&emuThreadMutex, NULL);
    pthread_cond_init(&emuThreadCond, NULL);
    pthread_create(&emuThread, NULL, emulate, NULL);
    pthread_setname_np(emuThread, "EmulatorThread");

    started = true;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_presentFrame(JNIEnv* env, jobject thiz, jlong deadlineNs, jobject renderFrameCallback)
{
    // This runs on the Kotlin "FrameRenderThread". Left unpinned, the scheduler
    // puts it on the emulator's core 3 as well (~1.85 ms/frame measured), where
    // its run-queue time deschedules the emulator. Pin it to cores {0,1,2} once,
    // on the first present. Affinity only, no output change. Escape hatch for
    // A/B: debug.litev.pinpresent=0.
    {
        static bool pinned = false;
        if (!pinned)
        {
            pinned = true;
            char prop[8] = {0};
            bool doPin = !(__system_property_get("debug.litev.pinpresent", prop) > 0 && atoi(prop) == 0);
            if (doPin)
            {
                cpu_set_t set;
                CPU_ZERO(&set);
                CPU_SET(0, &set); CPU_SET(1, &set); CPU_SET(2, &set);
                sched_setaffinity(0, sizeof(set), &set);
            }
        }
    }

    jclass presentFrameWrapperClass = env->GetObjectClass(renderFrameCallback);
    jmethodID renderFrameMethodId = env->GetMethodID(presentFrameWrapperClass, "renderFrame", "(ZI)V");

    std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadlineTime;
    if (deadlineNs > 0)
    {
        std::chrono::nanoseconds deadline(deadlineNs);
        deadlineTime = std::make_optional(std::chrono::time_point<std::chrono::steady_clock>(deadline));
    }
    else
    {
        deadlineTime = std::nullopt;
    }

    Frame* presentationFrame = MelonDSAndroid::getPresentationFrame(deadlineTime);
    EGLDisplay currentDisplay = eglGetCurrentDisplay();

    if (presentationFrame != nullptr && presentationFrame->presentFence)
    {
        eglDestroySyncKHR(currentDisplay, presentationFrame->presentFence);
        presentationFrame->presentFence = 0;
    }

    if (presentationFrame != nullptr)
    {
        eglWaitSyncKHR(currentDisplay, presentationFrame->renderFence, 0);
        env->CallVoidMethod(renderFrameCallback, renderFrameMethodId, true, (jint) presentationFrame->frameTexture);
        EGLSyncKHR presentFence = eglCreateSyncKHR(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
        presentationFrame->presentFence = presentFence;
    }
    else
    {
        env->CallVoidMethod(renderFrameCallback, renderFrameMethodId, false, 0);
    }
}

JNIEXPORT jfloat JNICALL
Java_me_magnum_melonds_MelonEmulator_getFPS(JNIEnv* env, jobject thiz)
{
    return fps;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_pauseEmulation(JNIEnv* env, jobject thiz)
{
    if (started) {
        pthread_mutex_lock(&emuThreadMutex);
    }

    if (!stop) {
        paused = true;
    }

    if (started) {
        pthread_mutex_unlock(&emuThreadMutex);
    }

    MelonDSAndroid::pause();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_resumeEmulation(JNIEnv* env, jobject thiz)
{
    if (started) {
        pthread_mutex_lock(&emuThreadMutex);
    }

    if (!stop) {
        paused = false;
        if (started) {
            pthread_cond_broadcast(&emuThreadCond);
        }
    }

    if (started) {
        pthread_mutex_unlock(&emuThreadMutex);
    }

    MelonDSAndroid::resume();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_resetEmulation(JNIEnv* env, jobject thiz) {
    pthread_mutex_lock(&emuThreadMutex);
    if (!stop) {
        if (paused) {
            pthread_mutex_unlock(&emuThreadMutex);
        } else {
            pthread_mutex_unlock(&emuThreadMutex);
            Java_me_magnum_melonds_MelonEmulator_pauseEmulation(env, thiz);
        }

        // Make sure that the thread is really paused to avoid data corruption
        while (!isThreadReallyPaused);
        MelonDSAndroid::reset();
        Java_me_magnum_melonds_MelonEmulator_resumeEmulation(env, thiz);
    } else {
        // If the emulation is stopping, just ignore it
        pthread_mutex_unlock(&emuThreadMutex);
    }
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_saveStateInternal(JNIEnv* env, jobject thiz, jstring path)
{
    const char* saveStatePath = path == nullptr ? nullptr : env->GetStringUTFChars(path, JNI_FALSE);
    return MelonDSAndroid::saveState(saveStatePath);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_loadStateInternal(JNIEnv* env, jobject thiz, jstring path)
{
    const char* saveStatePath = path == nullptr ? nullptr : env->GetStringUTFChars(path, JNI_FALSE);
    return MelonDSAndroid::loadState(saveStatePath);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_loadRewindState(JNIEnv* env, jobject thiz, jobject rewindSaveState) {
    bool result = true;

    pthread_mutex_lock(&emuThreadMutex);
    if (!stop) {
        bool wasPaused = paused;
        if (paused) {
            pthread_mutex_unlock(&emuThreadMutex);
        } else {
            pthread_mutex_unlock(&emuThreadMutex);
            Java_me_magnum_melonds_MelonEmulator_pauseEmulation(env, thiz);
        }

        jclass rewindSaveStateClass = env->FindClass("me/magnum/melonds/ui/emulator/rewind/model/RewindSaveState");
        jfieldID bufferField = env->GetFieldID(rewindSaveStateClass, "buffer", "Ljava/nio/ByteBuffer;");
        jfieldID bufferContentSizeField = env->GetFieldID(rewindSaveStateClass, "bufferContentSize", "J");
        jfieldID screenshotBufferField = env->GetFieldID(rewindSaveStateClass, "screenshotBuffer", "Ljava/nio/ByteBuffer;");
        jfieldID frameField = env->GetFieldID(rewindSaveStateClass, "frame", "I");
        jobject buffer = env->GetObjectField(rewindSaveState, bufferField);
        jlong bufferContentSize = env->GetLongField(rewindSaveState, bufferContentSizeField);
        jobject screenshotBuffer = env->GetObjectField(rewindSaveState, screenshotBufferField);
        jint frame = (int) env->GetIntField(rewindSaveState, frameField);

        // Make sure that the thread is really paused to avoid data corruption
        while (!isThreadReallyPaused);

        melonDS::RewindSaveState state = melonDS::RewindSaveState {
            .buffer = (u8*) env->GetDirectBufferAddress(buffer),
            .bufferSize = (u32) env->GetDirectBufferCapacity(buffer),
            .bufferContentSize = (u32) bufferContentSize,
            .screenshot = (u8*) env->GetDirectBufferAddress(screenshotBuffer),
            .screenshotSize = (u32) env->GetDirectBufferCapacity(screenshotBuffer),
            .frame = frame
        };

        result = MelonDSAndroid::loadRewindState(state);

        // Resume emulation if it was running
        if (!wasPaused) {
            Java_me_magnum_melonds_MelonEmulator_resumeEmulation(env, thiz);
        }
    } else {
        // If the emulation is stopping, just ignore it
        pthread_mutex_unlock(&emuThreadMutex);
    }

    return result;
}

JNIEXPORT jobject JNICALL
Java_me_magnum_melonds_MelonEmulator_getRewindWindow(JNIEnv* env, jobject thiz) {
    auto currentRewindWindow = MelonDSAndroid::getRewindWindow();

    jclass rewindSaveStateClass = env->FindClass("me/magnum/melonds/ui/emulator/rewind/model/RewindSaveState");
    jmethodID rewindSaveStateConstructor = env->GetMethodID(rewindSaveStateClass, "<init>", "(Ljava/nio/ByteBuffer;JLjava/nio/ByteBuffer;I)V");

    jclass listClass = env->FindClass("java/util/ArrayList");
    jmethodID listConstructor = env->GetMethodID(listClass, "<init>", "()V");
    jmethodID listAddMethod = env->GetMethodID(listClass, "add", "(ILjava/lang/Object;)V");
    jobject rewindStateList = env->NewObject(listClass, listConstructor);

    int index = 0;
    for (auto state : currentRewindWindow.rewindStates) {
        jobject stateBuffer = env->NewDirectByteBuffer(state.buffer, state.bufferSize);
        jobject stateScreenshot = env->NewDirectByteBuffer(state.screenshot, state.screenshotSize);
        jobject rewindSaveState = env->NewObject(rewindSaveStateClass, rewindSaveStateConstructor, stateBuffer, (jlong) state.bufferContentSize, stateScreenshot, state.frame);
        env->CallVoidMethod(rewindStateList, listAddMethod, index++, rewindSaveState);
    }

    jclass rewindWindowClass = env->FindClass("me/magnum/melonds/ui/emulator/rewind/model/RewindWindow");
    jmethodID rewindWindowConstructor = env->GetMethodID(rewindWindowClass, "<init>", "(ILjava/util/ArrayList;)V");
    jobject rewindWindow = env->NewObject(rewindWindowClass, rewindWindowConstructor, currentRewindWindow.currentFrame, rewindStateList);
    return rewindWindow;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_stopEmulation(JNIEnv* env, jobject thiz)
{
    if (started)
    {
        pthread_mutex_lock(&emuThreadMutex);
        stop = true;
        paused = false;
        started = false;
        pthread_cond_broadcast(&emuThreadCond);
        pthread_mutex_unlock(&emuThreadMutex);

        pthread_join(emuThread, NULL);
        pthread_mutex_destroy(&emuThreadMutex);
        pthread_cond_destroy(&emuThreadCond);
    }

    // The emulator thread is gone, so the LAN backend can be torn down without the lock.
    lanEndAll();

    MelonDSAndroid::cleanup();

    env->DeleteGlobalRef(globalCameraManager);

    globalCameraManager = nullptr;

    delete androidCameraHandler;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_onScreenTouch(JNIEnv* env, jobject thiz, jint x, jint y)
{
    MelonDSAndroid::touchScreen(x, y);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_onScreenRelease(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::releaseScreen();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_onKeyPress(JNIEnv* env, jobject thiz, jint key)
{
    MelonDSAndroid::pressKey(key);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_onKeyRelease(JNIEnv* env, jobject thiz, jint key)
{
    MelonDSAndroid::releaseKey(key);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_takeScreenshot(JNIEnv* env, jobject thiz)
{
    return MelonDSAndroid::takeScreenshot();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setFastForwardEnabled(JNIEnv* env, jobject thiz, jboolean enabled)
{
    isFastForwardEnabled = enabled;
    if (enabled) {
        limitFps = fastForwardSpeedMultiplier > 0;
        targetFps = 60 * fastForwardSpeedMultiplier;
    } else {
        limitFps = true;
        targetFps = 60;
    }

    if (performanceHintSession != nullptr) {
        if (enabled) {
            if (fastForwardSpeedMultiplier > 0) {
                auto frameDurationNs = static_cast<int64_t>(FRAME_DURATION_60FPS_NS / fastForwardSpeedMultiplier);
                performanceHintSession->updateTargetWorkDuration(frameDurationNs);
            } else {
                performanceHintSession->updateTargetWorkDuration(FRAME_DURATION_1000FPS_NS);
            }
        } else {
            performanceHintSession->updateTargetWorkDuration(FRAME_DURATION_60FPS_NS);
        }
    }
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setMicrophoneEnabled(JNIEnv* env, jobject thiz, jboolean enabled)
{
    if (enabled)
        MelonDSAndroid::userEnableMic();
    else
        MelonDSAndroid::userDisableMic();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_updateEmulatorConfiguration(JNIEnv* env, jobject thiz, jobject emulatorConfiguration)
{
    MelonDSAndroid::EmulatorConfiguration newConfiguration = MelonDSAndroidConfiguration::buildEmulatorConfiguration(env, emulatorConfiguration);

    fastForwardSpeedMultiplier = newConfiguration.fastForwardSpeedMultiplier;
#ifdef LITEV_AUTO_FRAMESKIP
    autoFrameskipEnabled = newConfiguration.autoFrameskipEnabled;
#endif
#ifdef LITEV_AGGRESSIVE_SKIP
    ffMaxFrameskip = newConfiguration.fastForwardMaxFrameskip;
#endif

    MelonDSAndroid::updateEmulatorConfiguration(std::make_unique<MelonDSAndroid::EmulatorConfiguration>(std::move(newConfiguration)));

    if (isFastForwardEnabled) {
        limitFps = fastForwardSpeedMultiplier > 0;
        targetFps = 60 * fastForwardSpeedMultiplier;

        if (performanceHintSession != nullptr) {
            if (fastForwardSpeedMultiplier > 0) {
                auto frameDurationNs = static_cast<int64_t>(FRAME_DURATION_60FPS_NS / fastForwardSpeedMultiplier);
                performanceHintSession->updateTargetWorkDuration(frameDurationNs);
            } else {
                performanceHintSession->updateTargetWorkDuration(FRAME_DURATION_1000FPS_NS);
            }
        }
    }
}
}

MelonDSAndroid::RomGbaSlotConfig* buildGbaSlotConfig(GbaSlotType slotType, const char* romPath, const char* savePath)
{
    if (slotType == GbaSlotType::GBA_ROM && romPath != nullptr)
    {
        MelonDSAndroid::RomGbaSlotConfigGbaRom* gbaSlotConfigGbaRom = new MelonDSAndroid::RomGbaSlotConfigGbaRom {
            .romPath = std::string(romPath),
            .savePath = savePath ? std::string(savePath) : "",
        };
        return (MelonDSAndroid::RomGbaSlotConfig*) gbaSlotConfigGbaRom;
    }
    else if (slotType == GbaSlotType::RUMBLE_PAK)
    {
        return (MelonDSAndroid::RomGbaSlotConfig*) new MelonDSAndroid::RomGbaSlotRumblePak;
    }
    else if (slotType == GbaSlotType::MEMORY_EXPANSION)
    {
        return (MelonDSAndroid::RomGbaSlotConfig*) new MelonDSAndroid::RomGbaSlotConfigMemoryExpansion;
    }
    else
    {
        return (MelonDSAndroid::RomGbaSlotConfig*) new MelonDSAndroid::RomGbaSlotConfigNone;
    }
}

// ---- LAN multiplayer -------------------------------------------------------------------------
// The LAN backend (melonDS's net/LAN, ENet over UDP; discovery on 7063, session on 7064) is
// driven from the in-game multiplayer lobby. Every call below runs with the emulator thread
// parked on emuThreadCond and emuThreadMutex held, so it never races the emulator loop's
// MPInterface::Get().Process(), and MPInterface::Set() never frees a backend that thread is
// using. The lobby is opened from the pause menu, so the game is already paused; while it is
// open, lanTick() pumps the backend in place of the emulator loop.
enum LanMode { LanNone = 0, LanDiscovering = 1, LanHosting = 2, LanJoined = 3 };
static LanMode lanMode = LanNone;

#define lan() ((melonDS::LAN&) melonDS::MPInterface::Get())

// Returns with emuThreadMutex held and the emulator thread parked, or false (mutex not held)
// if no game is running or it is not paused.
static bool lanLockParked()
{
    if (!started)
        return false;

    pthread_mutex_lock(&emuThreadMutex);
    for (;;)
    {
        if (stop || !paused)
        {
            pthread_mutex_unlock(&emuThreadMutex);
            return false;
        }
        if (isThreadReallyPaused)
            return true;
        // the emulator thread sets isThreadReallyPaused under this mutex; let it get there
        pthread_mutex_unlock(&emuThreadMutex);
        usleep(1000);
        pthread_mutex_lock(&emuThreadMutex);
    }
}

// How long the LAN backend blocks waiting for a peer's MP frame (host: client replies; client:
// the host's next frame). melonDS's 25 ms is too short over Wi-Fi: a poll/reply round trip plus
// the peers' frame-pacing phase offset (up to one frame) exceeds it and in-game joins fail with
// a communication error. Measured on two RG DS units (Shrek): 25 ms fails, 50 and 100 ms join and
// race. The waiting side's emulated clock is frozen while it waits, so a longer wait does not
// trip the game's own timeouts. debug.litev.mptimeout overrides it.
static constexpr int kLanRecvTimeoutMs = 50;

static void lanApplyRecvTimeout()
{
    char b[PROP_VALUE_MAX] = {0};
    int ms = __system_property_get("debug.litev.mptimeout", b) > 0 ? atoi(b) : kLanRecvTimeoutMs;
    melonDS::MPInterface::Get().SetRecvTimeout(ms > 0 ? ms : kLanRecvTimeoutMs);
}

static void lanEndAll()
{
    if (lanMode == LanDiscovering)
        lan().EndDiscovery();
    else if (lanMode == LanHosting || lanMode == LanJoined)
        lan().EndSession();
    if (lanMode != LanNone)
        melonDS::MPInterface::Set(melonDS::MPInterface_Dummy);
    lanMode = LanNone;
}

static std::string lanJString(JNIEnv* env, jstring s)
{
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string r(c);
    env->ReleaseStringUTFChars(s, c);
    return r;
}

static jobjectArray lanStringArray(JNIEnv* env, const std::vector<std::string>& rows)
{
    jobjectArray arr = env->NewObjectArray((jsize) rows.size(), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < rows.size(); i++)
    {
        jstring s = env->NewStringUTF(rows[i].c_str());
        env->SetObjectArrayElement(arr, (jsize) i, s);
        env->DeleteLocalRef(s);
    }
    return arr;
}

extern "C"
{
JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_lanGetMode(JNIEnv* env, jobject thiz)
{
    return lanMode;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_lanHost(JNIEnv* env, jobject thiz, jstring playerName, jint maxPlayers)
{
    if (!lanLockParked())
        return JNI_FALSE;
    std::string name = lanJString(env, playerName);
    lanEndAll();
    melonDS::MPInterface::Set(melonDS::MPInterface_LAN);
    lanApplyRecvTimeout();
    bool ok = lan().StartHost(name.c_str(), maxPlayers);
    if (ok)
        lanMode = LanHosting;
    else
        melonDS::MPInterface::Set(melonDS::MPInterface_Dummy);
    pthread_mutex_unlock(&emuThreadMutex);
    return ok;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_lanStartDiscovery(JNIEnv* env, jobject thiz)
{
    if (!lanLockParked())
        return JNI_FALSE;
    lanEndAll();
    melonDS::MPInterface::Set(melonDS::MPInterface_LAN);
    lanApplyRecvTimeout();
    bool ok = lan().StartDiscovery();
    if (ok)
        lanMode = LanDiscovering;
    else
        melonDS::MPInterface::Set(melonDS::MPInterface_Dummy);
    pthread_mutex_unlock(&emuThreadMutex);
    return ok;
}

// One row per discovered session: "ip \t name \t numPlayers \t maxPlayers \t status(0 idle, 1 playing)"
JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_lanGetSessions(JNIEnv* env, jobject thiz)
{
    std::vector<std::string> rows;
    if (lanLockParked())
    {
        if (lanMode == LanDiscovering)
        {
            for (const auto& [key, data] : lan().GetDiscoveryList())
            {
                // discovery keys are host-order addresses (first octet in the top byte)
                char row[160];
                snprintf(row, sizeof(row), "%u.%u.%u.%u\t%.64s\t%u\t%u\t%u",
                         key >> 24, (key >> 16) & 0xFF, (key >> 8) & 0xFF, key & 0xFF,
                         data.SessionName, data.NumPlayers, data.MaxPlayers, data.Status);
                rows.emplace_back(row);
            }
        }
        pthread_mutex_unlock(&emuThreadMutex);
    }
    return lanStringArray(env, rows);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_lanJoin(JNIEnv* env, jobject thiz, jstring playerName, jstring hostAddress)
{
    if (!lanLockParked())
        return JNI_FALSE;
    std::string name = lanJString(env, playerName);
    std::string host = lanJString(env, hostAddress);
    if (lanMode == LanDiscovering)
        lan().EndDiscovery();
    else
    {
        lanEndAll();
        melonDS::MPInterface::Set(melonDS::MPInterface_LAN);
    }
    lanApplyRecvTimeout();
    // blocks while ENet connects to the host (the caller runs this off the UI thread)
    bool ok = lan().StartClient(name.c_str(), host.c_str());
    if (ok)
        lanMode = LanJoined;
    else
    {
        melonDS::MPInterface::Set(melonDS::MPInterface_Dummy);
        lanMode = LanNone;
    }
    pthread_mutex_unlock(&emuThreadMutex);
    return ok;
}

// One row per player: "id \t maxPlayers \t name \t status \t ping \t isLocal \t ip"
// status: 1 client, 2 host, 3 connecting, 4 disconnected (LAN::PlayerStatus)
JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_lanGetPlayers(JNIEnv* env, jobject thiz)
{
    std::vector<std::string> rows;
    if (lanLockParked())
    {
        if (lanMode == LanHosting || lanMode == LanJoined)
        {
            int maxPlayers = lan().GetMaxPlayers();
            for (const auto& p : lan().GetPlayerList())
            {
                // player addresses are network-order (first octet in the low byte)
                uint32_t ip = p.Address;
                char row[160];
                snprintf(row, sizeof(row), "%d\t%d\t%.32s\t%d\t%u\t%d\t%u.%u.%u.%u",
                         p.ID, maxPlayers, p.Name, (int) p.Status, p.Ping, p.IsLocalPlayer ? 1 : 0,
                         ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
                rows.emplace_back(row);
            }
        }
        pthread_mutex_unlock(&emuThreadMutex);
    }
    return lanStringArray(env, rows);
}

// Pumps discovery beacons and ENet events while the lobby holds the game paused.
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_lanTick(JNIEnv* env, jobject thiz)
{
    if (!lanLockParked())
        return;
    if (lanMode != LanNone)
        melonDS::MPInterface::Get().Process();
    pthread_mutex_unlock(&emuThreadMutex);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_lanLeave(JNIEnv* env, jobject thiz)
{
    if (!lanLockParked())
        return;
    lanEndAll();
    pthread_mutex_unlock(&emuThreadMutex);
}
}

double getCurrentMillis() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec * 1000.0) + now.tv_nsec / 1000000.0;
}

void* emulate(void*)
{
    double startTick = getCurrentMillis();
    double lastTick = startTick;
    double lastMeasureFpsTick = startTick;
    double frameLimitError = 0.0;

    // Pacing profiler. Only this outer loop sees the 60 fps limiter that spaces
    // emulation frames apart. Gated by debug.litev.prof (read once per 60
    // frames); logs one LITEV_PACE aggregate per 60 frames.
    double paceWorkMs = 0.0, paceRequestedSleepMs = 0.0, paceActualSleepMs = 0.0;
    double paceIntervalMs = 0.0, paceTargetMs = 0.0;
    int paceN = 0;
    int pacePollFrame = 0;
    bool paceEnabled = false;

    MelonDSAndroid::start();

    auto manager = PerformanceHintManagerFactory::create(jniEnvHandler);
    performanceHintSession = new ThreadSafePerformanceHintSession(std::move(manager));
    if (performanceHintSession != nullptr) {
        performanceHintSession->createSession(gettid(), FRAME_DURATION_60FPS_NS);
    }

    for (;;)
    {
        pthread_mutex_lock(&emuThreadMutex);
        if (paused) {
            isThreadReallyPaused = true;
            while (paused && !stop)
                pthread_cond_wait(&emuThreadCond, &emuThreadMutex);

            frameLimitError = 0;
            lastTick = getCurrentMillis();
            isThreadReallyPaused = false;
        }

        if (stop) {
            pthread_mutex_unlock(&emuThreadMutex);
            break;
        }

        pthread_mutex_unlock(&emuThreadMutex);

        auto frameStart = std::chrono::steady_clock::now();

        u32 nLines = MelonDSAndroid::loop();

        auto frameDuration = std::chrono::steady_clock::now() - frameStart;
        // The ADPF report is a binder call into system_server (which wakes the power HAL),
        // made from the emu thread and landing on its core. Report the mean of every
        // `adpfEvery` frames instead of every frame. debug.litev.adpfevery: unset = 10,
        // 1 = every frame (old behaviour), 0 = no reports. Re-read every 120 frames.
        static int adpfEvery = 10, adpfCtr = 0, adpfN = 0;
        static int64_t adpfSumNs = 0;
        if (--adpfCtr <= 0)
        {
            adpfCtr = 120;
            char b[PROP_VALUE_MAX] = {0};
            adpfEvery = __system_property_get("debug.litev.adpfevery", b) > 0 ? atoi(b) : 10;
        }
        if (performanceHintSession != nullptr && adpfEvery > 0)
        {
            adpfSumNs += std::chrono::nanoseconds(frameDuration).count();
            if (++adpfN >= adpfEvery)
            {
                performanceHintSession->reportActualWorkDuration(adpfSumNs / adpfN);
                adpfSumNs = 0;
                adpfN = 0;
            }
        }

        double previousTick = lastTick;
        double currentTick = getCurrentMillis();
        double delay = currentTick - lastTick;
        double requestedSleepMs = 0.0;
        double actualSleepMs = 0.0;

        // All times are in ms
        double frameTimeStep = (double) nLines / ((float) targetFps * 263.0) * 1000.0;
        if (frameTimeStep < 1)
            frameTimeStep = 1;

        // debug.litev.nolimit=1: limiter off (frameskip untouched) = the uncapped measurement
        // mode of TESTING-METHODOLOGY §1 (same effect as unlimited fast-forward). Re-read
        // every 120 frames.
        static int noLimit = 0, noLimitCtr = 0;
        if (--noLimitCtr <= 0)
        {
            noLimitCtr = 120;
            char b[PROP_VALUE_MAX] = {0};
            noLimit = __system_property_get("debug.litev.nolimit", b) > 0 && atoi(b) != 0;
        }

        if (limitFps && !noLimit)
        {
            frameLimitError += frameTimeStep - delay;
            if (frameLimitError < -frameTimeStep)
                frameLimitError = -frameTimeStep;
            if (frameLimitError > frameTimeStep)
                frameLimitError = frameTimeStep;

            if (round(frameLimitError) > 0.0)
            {
                requestedSleepMs = frameLimitError;
                timespec sleepTime = {
                    .tv_sec = 0,
                    .tv_nsec = (long) (frameLimitError * 1000000),
                };
                clock_nanosleep(CLOCK_MONOTONIC, 0, &sleepTime, nullptr);
                double timeAfterSleep = getCurrentMillis();
                actualSleepMs = timeAfterSleep - currentTick;
                frameLimitError -= actualSleepMs;
                currentTick = timeAfterSleep;
            }

            lastTick = currentTick;
        } else {
            frameLimitError = 0;
            lastTick = getCurrentMillis();
        }

#ifdef LITEV_AUTO_FRAMESKIP
        // Adaptive frameskip to hold REAL-TIME speed, evaluated EVERY frame with a
        // FAST-ATTACK / SLOW-RELEASE controller so a sudden heavy scene doesn't sit in
        // slow-motion waiting for a slow average to catch up.
        //   `delay` = this frame's wall work time (pre-sleep); frameTimeStep = the 60fps
        //   budget (~16.67ms). The EMA (alpha 0.3) reacts in ~3 frames.
        //   ATTACK (fast): while behind (EMA > 1.15x budget), raise the skip level now
        //   (jump 2 if badly behind, >=1.9x), then a 5-frame cooldown lets it settle
        //   before the next step -> reaches the needed level in ~10 frames, not ~60.
        //   RELEASE (slow): only lower the skip after ~120 consecutive low-load frames,
        //   so it doesn't oscillate back into slow-mo at the threshold.
        // Only runs when fast-forward is off. All frameskip changes happen here, on the
        // emu thread, so they never race runFrame.
        // Never during LAN multiplayer: there the frame time is spent waiting on the network,
        // which skipping rendering cannot shorten, and the skips show up as multi-second freezes.
        if (autoFrameskipEnabled && !isFastForwardEnabled && lanMode == LanNone) {
            autoFsEmaMs = (autoFsEmaMs <= 0.0) ? delay : (autoFsEmaMs * 0.7 + delay * 0.3);
            if (autoFsCooldown > 0) autoFsCooldown--;

            if (autoFsEmaMs > frameTimeStep * 1.15) {
                autoFsHeadroom = 0;                        // behind -> attack
                if (autoFsCooldown == 0 && autoFsSkip < 3) {
                    int step = (autoFsEmaMs > frameTimeStep * 1.9) ? 2 : 1;
                    autoFsSkip = (autoFsSkip + step > 3) ? 3 : autoFsSkip + step;
                    MelonDSAndroid::setFrameskip(autoFsSkip);
                    LOG_INFO("LITEV_AUTOFS", "skip level %d (ema %.2f ms)", autoFsSkip, autoFsEmaMs);
                    autoFsCooldown = 5;
                }
            } else if (autoFsEmaMs < frameTimeStep * 0.70) {
                if (++autoFsHeadroom >= 120 && autoFsSkip > 0) {   // sustained headroom -> release
                    autoFsSkip--;
                    MelonDSAndroid::setFrameskip(autoFsSkip);
                    LOG_INFO("LITEV_AUTOFS", "skip level %d (ema %.2f ms)", autoFsSkip, autoFsEmaMs);
                    autoFsHeadroom = 0;
                    autoFsCooldown = 5;
                }
            } else {
                autoFsHeadroom = 0;                        // stable band -> hold
            }
        } else if (autoFsSkip != 0) {
            // Setting turned off, or fast-forward engaged, while a skip was applied ->
            // clear it; the controller restarts from 0 when it next runs.
            autoFsSkip = 0; autoFsEmaMs = 0.0; autoFsCooldown = 0; autoFsHeadroom = 0;
            MelonDSAndroid::setFrameskip(0);
            LOG_INFO("LITEV_AUTOFS", "skip level 0 (inactive)");
        }
#endif

#ifdef LITEV_AGGRESSIVE_SKIP
        // Fast-forward owns the frameskip while engaged: the speed multiplier maps to a
        // raster-skip target capped by the setting; setting 0 forces 0 (FF only lifts
        // the limiter, every frame rendered). On release, any FF skip is cleared and
        // auto frameskip (reset above when FF engaged) restarts from 0. Runs after the
        // auto controller, on the emu thread, so FF wins and nothing races runFrame.
        // Re-evaluated every frame: runtime changes of the setting or multiplier apply
        // at once. The lib clamps targets to GPU::LITEV_FRAMESKIP_MAX.
        {
            int ffWant = isFastForwardEnabled ? ffFrameskipTarget(fastForwardSpeedMultiplier, ffMaxFrameskip) : -1;
            if (ffWant != ffSkipApplied) {
                if (ffWant >= 0 || ffSkipApplied > 0)
                    MelonDSAndroid::setFrameskip(ffWant >= 0 ? ffWant : 0);
                LOG_INFO("LITEV_FFSKIP", "fast-forward skip %d", ffWant);
                ffSkipApplied = ffWant;
            }
        }
#endif

        // Read the property once per 60-frame window.  `work` is the actual
        // MelonDSAndroid::loop duration; `requestedSleep` vs `actualSleep`
        // exposes kernel wake-up overshoot; `interval` is the job cadence that
        // the Choreographer/render thread receives.
        if (pacePollFrame == 0) {
            char prop[8] = {0};
            paceEnabled = (__system_property_get("debug.litev.prof", prop) > 0 && atoi(prop) != 0);
        }
        pacePollFrame = (pacePollFrame + 1) % 60;
        if (paceEnabled) {
            paceWorkMs += std::chrono::duration_cast<std::chrono::nanoseconds>(frameDuration).count() / 1e6;
            paceRequestedSleepMs += requestedSleepMs;
            paceActualSleepMs += actualSleepMs;
            paceIntervalMs += currentTick - previousTick;
            paceTargetMs += frameTimeStep;
            if (++paceN >= 60) {
                LOG_INFO("LITEV_PACE",
                         "60f: work=%.2fms target=%.2fms requestedSleep=%.2fms actualSleep=%.2fms oversleep=%.2fms interval=%.2fms",
                         paceWorkMs / paceN, paceTargetMs / paceN,
                         paceRequestedSleepMs / paceN, paceActualSleepMs / paceN,
                         (paceActualSleepMs - paceRequestedSleepMs) / paceN,
                         paceIntervalMs / paceN);
                paceWorkMs = paceRequestedSleepMs = paceActualSleepMs = 0.0;
                paceIntervalMs = paceTargetMs = 0.0;
                paceN = 0;
            }
        }

        observedFrames++;
        if (observedFrames >= 30) {
            fps = (observedFrames * 1000.0) / (lastTick - lastMeasureFpsTick);
            lastMeasureFpsTick = lastTick;
            observedFrames = 0;
        }
    }

    if (performanceHintSession != nullptr) {
        performanceHintSession->destroySession();

        delete performanceHintSession;
        performanceHintSession = nullptr;
    }

    MelonDSAndroid::stop();
    pthread_exit(NULL);
}