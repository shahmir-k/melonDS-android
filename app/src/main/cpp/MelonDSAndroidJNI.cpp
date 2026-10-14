#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <jni.h>
#include <algorithm>
#include <chrono>
#include <array>
#include <deque>
#include <random>
#include <string>
#include <sstream>
#include <mutex>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <time.h>
#include <sched.h>
#include <sys/system_properties.h>
#include <arpa/inet.h>
#include <MelonDS.h>
#include "renderer/RecordVideo.h"
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
#ifdef LITEV_AMBIENT_LAN
#include "net/AmbientLAN.h"
#endif
#include "net/NetplayInput.h"
#include "LitevCores.h"
#include "SkipRepeatAuto.h"
#include "sereneds_build_id.h"

enum GbaSlotType {
    NONE = 0,
    GBA_ROM = 1,
    RUMBLE_PAK = 2,
    MEMORY_EXPANSION = 3,
};

void* emulate(void*);
static void lanEndAll();
static void ambientInstall();
static void ambientHandoffService();
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
#ifdef LITEV_NP_SPEED
// the fast-forward speed setting: in Netplay the session's speed drives the variables above,
// and the fast-forward key asks the session for this
static float ffSetting = 2.0f;
#endif
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
#ifdef LITEV_FF_ADAPTIVE_SKIP
// Fast-forward below its target speed skips more frames, up to the max frameskip setting: a
// shown frame costs the emulator ~1 ms more than a skipped one (2D snapshots, the full 3D bank),
// and with 30 Hz 3D a deeper skip leaves more banks unwatched (FF_HEADLESS3D). PW town at 2.5x:
// skip 2/3/4 = 118/125/129 game fps. Judged per 30-frame window of loop work vs the target
// frame period: one level up per slow window, one down after 4 windows under 85%.
// debug.litev.ffadapt=0 turns it off.
static int ffAdaptiveSkip(int base, int max, double workMs, double stepMs)
{
    static const bool on = [] { char b[PROP_VALUE_MAX] = {}; return !(__system_property_get("debug.litev.ffadapt", b) > 0 && atoi(b) == 0); }();
    static int extra = 0, n = 0, calm = 0;
    static double sum = 0;
    if (base < 0 || !on) { extra = n = calm = 0; sum = 0; return base; }
    sum += workMs;
    if (++n >= 30)
    {
        const double avg = sum / n;
        n = 0; sum = 0;
        if (avg > stepMs * 1.03 && base + extra < max) { extra++; calm = 0; }
        else if (avg < stepMs * 0.85 && extra > 0) { if (++calm >= 4) { extra--; calm = 0; } }
        else calm = 0;
    }
    return std::max(base, std::min(base + extra, max));
}
#endif
#endif
#ifdef LITEV_AUTO_FRAMESKIP
// Adaptive frameskip (user setting) that holds real-time speed instead of slow-mo when a
// scene can't sustain 60fps. Distinct from fast-forward; only active when FF is OFF.
bool autoFrameskipEnabled = false;
static int    autoFsSkip     = 0;    // current adaptive raster-skip level (0..3)
static double autoFsEmaMs    = 0.0;  // FAST EMA of per-frame wall time (attack signal)
static int    autoFsCooldown = 0;    // frames to wait after a change (let it settle)
static int    autoFsHeadroom = 0;    // consecutive low-load frames (slow-release counter)
static int    autoFsRelease  = 30;   // headroom frames needed to release a level (backs off on flapping)
static int    autoFsSinceRelease = 1000;   // frames since the last release
#endif

#ifdef LITEV_SKIP_REPEAT_FRAMES
static SkipRepeatAuto skipRepeatAuto;   // "Skip repeated frames: Auto" at 1x (emu thread)
static bool skipRepeatAutoLogged = false;
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
#ifdef LITEV_NP_SPEED
    ffSetting = fastForwardSpeedMultiplier;
#endif
#ifdef LITEV_AUTO_FRAMESKIP
    autoFrameskipEnabled = finalEmulatorConfiguration.autoFrameskipEnabled;
    // New emulator session (the emu thread is not running yet): start from no skip.
    autoFsSkip = 0; autoFsEmaMs = 0.0; autoFsCooldown = 0; autoFsHeadroom = 0; autoFsRelease = 30; autoFsSinceRelease = 1000;
#endif
#ifdef LITEV_SKIP_REPEAT_FRAMES
    skipRepeatAuto.reset();
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
    ambientInstall();       // (before the emulator thread runs)
    pthread_create(&emuThread, NULL, emulate, NULL);
    pthread_setname_np(emuThread, "EmulatorThread");

    started = true;
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_litevProp(JNIEnv* env, jobject thiz, jstring name, jint def)
{
    const char* n = env->GetStringUTFChars(name, nullptr);
    std::string key = std::string("debug.litev.") + n;
    env->ReleaseStringUTFChars(name, n);
    char prop[PROP_VALUE_MAX] = {0};
    return __system_property_get(key.c_str(), prop) > 0 ? atoi(prop) : def;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_presentFrame(JNIEnv* env, jobject thiz, jlong deadlineNs, jobject renderFrameCallback)
{
    // This runs on the Kotlin "FrameRenderThread". Left unpinned, the scheduler
    // puts it on the emulator's core as well (~1.85 ms/frame measured on the RG DS), where
    // its run-queue time deschedules the emulator. Pin it to the other cores once,
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
#ifdef LITEV_TOPO_PIN
                melonDS::LitevTopo::PinSelf(melonDS::LitevTopo::CoreRole::RenderCritical);
#else
                const cpu_set_t& set = melonDS::LitevCores::Get().OtherSet;
                sched_setaffinity(0, sizeof(set), &set);
#endif
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

    bool isNew = false;
    Frame* presentationFrame = MelonDSAndroid::getPresentationFrame(deadlineTime, &isNew);
    // debug.litev.presentlog=1: one record per vsync callback (what the screen shows), dumped
    // 40 at a time: "dt_ms,new,emuFrame step,drops,age_ms,deadline_ms,repeats" (age = ready -> taken;
    // deadline = how long it would wait for a frame, -1 none; repeats = frames skip-repeat left
    // undrawn since the last record, identical to the picture shown). tools/record/presents.py
    {
        static const bool presLog = [] { char b[PROP_VALUE_MAX] = {}; return __system_property_get("debug.litev.presentlog", b) > 0 && atoi(b) != 0; }();
        if (presLog)
        {
            static char line[4000]; static int n = 0, len = 0, lastEmu = 0, lastDrops = 0, lastRep = 0;
            static int64_t lastNs = 0;
            const int64_t now = std::chrono::steady_clock::now().time_since_epoch().count();
            const int drops = g_presentDrops.load(std::memory_order_relaxed);
            const int rep = g_framesRepeat.load(std::memory_order_relaxed);
            const int emu = presentationFrame ? presentationFrame->emuFrame : 0;
            if (n == 0) len = snprintf(line, sizeof(line), "t=%lld", (long long)(now / 1000000));
            len += snprintf(line + len, sizeof(line) - len, " %.1f,%d,%d,%d,%.1f,%.1f,%d", lastNs ? (now - lastNs) / 1e6 : 0.0, isNew ? 1 : 0,
                            emu - lastEmu, drops - lastDrops, isNew && presentationFrame ? (now - presentationFrame->readyNs) / 1e6 : 0.0,
                            deadlineNs > 0 ? (deadlineNs - now) / 1e6 : -1.0, rep - lastRep);
            lastNs = now; lastEmu = emu; lastDrops = drops; lastRep = rep;
            if (++n >= 40) { LOG_INFO("LITEV_PRES", "%s", line); n = 0; }
        }
    }
    // Nothing new since the last present (paused, or the game runs under 60 fps): keep what is on
    // screen instead of redrawing and swapping every display again. That GPU/DRAM traffic slows
    // the emulator threads on the RG DS (a paused app made a two-console emulation ~40% slower).
    // Still redraw every 30th vsync, so layout or setting changes show up while paused.
    // Escape hatch for A/B: debug.litev.presentall=1 presents every vsync as before.
    static const bool presentAll = [] {
        char prop[8] = {0};
        return __system_property_get("debug.litev.presentall", prop) > 0 && atoi(prop) == 1;
    }();
    static int stale = 0;
    if (!presentAll && presentationFrame != nullptr && !isNew && ++stale < 30)
        return;
    stale = 0;
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
        // record mode footage, on this thread's GL context (the hardware encoder does the rest)
        {
            std::string dir, prefix;
            int recFrame;
            if (isNew && MelonDSAndroid::recordVideoTarget(presentationFrame, dir, prefix, recFrame))
                RecordVideo::Present(presentationFrame, recFrame, dir, prefix, 15);
            else if (!isNew) {}
            else
                RecordVideo::Stop();
        }
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

        // Netplay: the emulator thread may be waiting for a player who has left
        MelonDSAndroid::netplayAbort();
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
#ifdef LITEV_NP_SPEED
    // Netplay: the session's speed drives fast-forward (the key: netplayToggleFastForward)
    if (MelonDSAndroid::netplaySpeedShown() >= 0) return;
#endif
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

#ifdef LITEV_NP_SPEED
    ffSetting = newConfiguration.fastForwardSpeedMultiplier;
    // plain Netplay: the session's speed drives fast-forward; the setting is what the key asks for next
    float npSpeed;
    const bool npDriven = MelonDSAndroid::netplaySpeed(npSpeed);
    if (!npDriven)
#endif
    fastForwardSpeedMultiplier = newConfiguration.fastForwardSpeedMultiplier;
#ifdef LITEV_AUTO_FRAMESKIP
    autoFrameskipEnabled = newConfiguration.autoFrameskipEnabled;
#endif
#ifdef LITEV_AGGRESSIVE_SKIP
    ffMaxFrameskip = newConfiguration.fastForwardMaxFrameskip;
#endif

    MelonDSAndroid::updateEmulatorConfiguration(std::make_unique<MelonDSAndroid::EmulatorConfiguration>(std::move(newConfiguration)));

#ifdef LITEV_NP_SPEED
    if (npDriven) return;
#endif
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

static void ambientUninstall();
static void lanEndAll()
{
    ambientUninstall();     // (the lobby / a session replaces Just Play while it is open)
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

// One row per player: "id \t maxPlayers \t name \t status \t ping \t isLocal \t ip"
static std::vector<std::string> lanPlayerRows(melonDS::LAN& l)
{
    std::vector<std::string> rows;
    int maxPlayers = l.GetMaxPlayers();
    for (const auto& p : l.GetPlayerList())
    {
        // player addresses are network-order (first octet in the low byte)
        uint32_t ip = p.Address;
        char row[160];
        snprintf(row, sizeof(row), "%d\t%d\t%.32s\t%d\t%u\t%d\t%u.%u.%u.%u",
                 p.ID, maxPlayers, p.Name, (int) p.Status, p.Ping, p.IsLocalPlayer ? 1 : 0,
                 ip & 0xFF, (ip >> 8) & 0xFF, (ip >> 16) & 0xFF, ip >> 24);
        rows.emplace_back(row);
    }
    return rows;
}

// ---- Just Play local wireless (LITEV_AMBIENT_LAN) ----------------------------------------------
// Installed as the MPInterface when a game starts (unless the setting is "Lobby only", or Netplay runs). The LAN lobby
// replaces it while open (lanEndAll) and it comes back when the lobby closes (lanLeave). Installs / removals happen
// with the emulator thread parked or not running; ambientMu guards the pointer the app's other threads use (NSD peers,
// foreground, the nearby list, the counters). The device id comes from the app's per-install identity key.
#ifdef LITEV_AMBIENT_LAN
static std::mutex ambientMu;
static melonDS::AmbientLAN* ambientPtr = nullptr;       // owned by MPInterface while installed
static melonDS::AmbientLAN::Config ambientCfg;
static int ambientMode = -1;                            // -1 off ("Lobby only" / not configured), else AmbientLAN::Visibility
static bool ambientFg = true;
static std::vector<std::pair<u32, u16>> ambientPeers;   // NSD finds, kept across installs
enum class AmbientHandoffRequestKind { Offer, Respond, Cancel };
struct AmbientHandoffRequest
{
    AmbientHandoffRequestKind Kind;
    u64 Epoch = 0;
    int Mode = 0;
    bool Accept = false;
};
static std::deque<AmbientHandoffRequest> ambientHandoffRequests;
static std::string ambientHandoffStatus = "none";
static u64 ambientLastOfferEpoch = 0;
static u64 ambientOwnOfferEpoch = 0;
static std::array<u8, 16> ambientOwnOfferToken{};
static u64 ambientRomEpoch = 0;
static melonDS::NetplayRom ambientOwnRom{};
static bool ambientRomPublished = false;

static void ambientUninstall()
{
    std::lock_guard<std::mutex> g(ambientMu);
    if (!ambientPtr) return;
    ambientPtr = nullptr;
    ambientHandoffRequests.clear();
    ambientHandoffStatus = "none";
    ambientOwnOfferEpoch = 0;
    ambientOwnOfferToken = {};
    ambientRomEpoch = 0;
    ambientOwnRom = {};
    ambientRomPublished = false;
    if (melonDS::MPInterface::GetType() == melonDS::MPInterface_Ambient)
        melonDS::MPInterface::Set(melonDS::MPInterface_Dummy);
}

static void ambientInstall()
{
    std::lock_guard<std::mutex> g(ambientMu);
    if (ambientPtr || ambientMode < 0 || MelonDSAndroid::netplayActive() || melonDS::MPInterface::GetType() != melonDS::MPInterface_Dummy)
        return;
    std::string code = MelonDSAndroid::romGameCode();
    if (code.size() != 4) return;       // (firmware boot: no game)
    melonDS::AmbientLAN::Config c = ambientCfg;
    memcpy(c.GameCode, code.data(), 4);
    c.Vis = ambientMode;
    c.AllowHandoff = SERENEDS_HANDOFF_BUILD_CLEAN != 0;
    c.RequireHandoffOffer = true;
    auto a = std::make_unique<melonDS::AmbientLAN>(c);
    if (!a->Ok()) return;
    a->SetForeground(ambientFg);
    for (auto& [ip, port] : ambientPeers) a->AddPeerAddress(ip, port);
    ambientPtr = a.get();
    ambientHandoffRequests.clear();
    ambientHandoffStatus = "none";
    ambientOwnOfferEpoch = 0;
    ambientOwnOfferToken = {};
    ambientRomEpoch = 0;
    ambientOwnRom = {};
    ambientRomPublished = false;
    melonDS::MPInterface::Set(std::move(a), melonDS::MPInterface_Ambient);
    lanApplyRecvTimeout();
}

// All Ambient offer/control methods are emulator-thread-only. UI callbacks
// only enqueue a request; this service runs immediately before the next frame
// and publishes a copy of the offer state for non-emulator threads to read.
static void ambientHandoffService()
{
    std::unique_lock<std::mutex> g(ambientMu);
    if (!ambientPtr) return;
    while (!ambientHandoffRequests.empty())
    {
        const auto request = ambientHandoffRequests.front();
        ambientHandoffRequests.pop_front();
        if (request.Kind == AmbientHandoffRequestKind::Offer)
        {
            std::array<u8, 16> token{};
            std::random_device random;
            for (u8& byte : token) byte = static_cast<u8>(random());
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            const u64 next = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
            const u64 epoch = std::max(next, ambientLastOfferEpoch + 1);
            if (ambientPtr->ProposeHandoff(epoch, token,
                    static_cast<melonDS::AmbientLAN::HandoffMode>(request.Mode)))
            {
                ambientLastOfferEpoch = ambientOwnOfferEpoch = epoch;
                ambientOwnOfferToken = token;
            }
            else ambientHandoffStatus = "failed\t0\t0\t0";
        }
        else if (request.Kind == AmbientHandoffRequestKind::Respond)
        {
            if (!ambientPtr->RespondToHandoffOffer(request.Epoch, request.Accept))
                ambientHandoffStatus = "failed\t0\t0\t0";
        }
        else
        {
            ambientPtr->CancelHandoffOffer();
            ambientOwnOfferEpoch = 0;
            ambientOwnOfferToken = {};
            ambientRomEpoch = 0;
            ambientOwnRom = {};
            ambientRomPublished = false;
            ambientHandoffStatus = "none";
        }
    }
    melonDS::AmbientLAN::HandoffOfferStatus status{};
    if (ambientPtr->GetHandoffOfferStatus(status))
    {
        if (status.Accepted && ambientRomEpoch != status.Epoch)
        {
            // Hashing a ROM on external SD can take time. Keep the live source
            // running and let UI readers observe the offer while the mutex is free.
            const std::string path = MelonDSAndroid::currentRomPath();
            ambientRomEpoch = status.Epoch;
            ambientOwnRom = {};
            ambientRomPublished = false;
            g.unlock();
            const melonDS::NetplayRom described = melonDS::NetplayDescribeRom(path);
            g.lock();
            melonDS::AmbientLAN::HandoffOfferStatus current{};
            if (!ambientPtr || ambientRomEpoch != status.Epoch ||
                !ambientPtr->GetHandoffOfferStatus(current) || !current.Accepted ||
                current.Epoch != status.Epoch)
                return;
            if (!described.Size || described.Size > (512u << 20))
            {
                ambientPtr->CancelHandoffOffer();
                ambientOwnOfferEpoch = 0;
                ambientOwnOfferToken = {};
                ambientRomEpoch = 0;
                ambientHandoffStatus = "failed\t0\t0\t0";
                return;
            }
            ambientOwnRom = described;
        }
        if (status.Accepted && ambientOwnRom.Size && !ambientRomPublished)
            ambientRomPublished = ambientPtr->PublishHandoffRom(ambientOwnRom);
        const char* phase = status.Accepted ? "accepted" : status.Outgoing ? "outgoing" : "incoming";
        char row[96];
        snprintf(row, sizeof(row), "%s\t%llu\t%d\t%llu", phase,
                 (unsigned long long)status.Epoch, (int)status.Mode,
                 (unsigned long long)status.PeerID);
        ambientHandoffStatus = row;
    }
    else if (ambientOwnOfferEpoch)
    {
        ambientOwnOfferEpoch = 0;
        ambientOwnOfferToken = {};
        ambientRomEpoch = 0;
        ambientOwnRom = {};
        ambientRomPublished = false;
        ambientHandoffStatus = "closed\t0\t0\t0";
    }
    else if (ambientRomEpoch)
    {
        ambientRomEpoch = 0;
        ambientOwnRom = {};
        ambientRomPublished = false;
        ambientHandoffStatus = "closed\t0\t0\t0";
    }
}

static bool ambientHasLinks()
{
    std::lock_guard<std::mutex> g(ambientMu);
    return ambientPtr && ambientPtr->HasLinks();
}

namespace MelonDSAndroid
{
// the console MAC for Just Play (unique per install; two consoles with one MAC cannot see each other)
bool ambientFirmwareMac(u8* mac)
{
    std::lock_guard<std::mutex> g(ambientMu);
    // (not in Netplay: every device builds every console there, their MACs must not depend on the device)
    if (ambientMode < 0 || !ambientCfg.DeviceID || netplayActive()) return false;
    melonDS::AmbientLAN::MacFromID(ambientCfg.DeviceID, mac);
    return true;
}
}
#else
static void ambientUninstall() {}
static void ambientInstall() {}
static void ambientHandoffService() {}
static bool ambientHasLinks() { return false; }
#endif

// ---- Group: a Netplay lobby kept connected while its session runs -----------------------------
// At the host's Start every device keeps its lobby link (a LAN, port 7064; the session itself uses
// 7100) as the group's control channel, not installed as the MPInterface: the host sends group
// commands over it (switch mode, change the Hosted server, end the session) and every device
// follows. Pumped by the app (groupTake) from its own thread, so it has its own lock.
static std::unique_ptr<melonDS::MPInterface> groupLink;
static std::mutex groupMutex;
static std::vector<std::string> groupLeftRows;   // "id \t name" of players whose link closed (groupLeft)
#define grp() ((melonDS::LAN&) *groupLink)

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
    if (MelonDSAndroid::netplayActive() || groupLink || !lanLockParked())   // end the session / group first
        return JNI_FALSE;
    std::string name = lanJString(env, playerName);
    lanEndAll();
    melonDS::MPInterface::Set(melonDS::MPInterface_LAN);
    lanApplyRecvTimeout();
    bool ok = lan().StartHost(name.c_str(), maxPlayers);
    if (ok)
        lanMode = LanHosting;
    else
    {
        melonDS::MPInterface::Set(melonDS::MPInterface_Dummy);
        ambientInstall();
    }
    pthread_mutex_unlock(&emuThreadMutex);
    return ok;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_lanStartDiscovery(JNIEnv* env, jobject thiz)
{
    if (MelonDSAndroid::netplayActive() || groupLink || !lanLockParked())   // end the session / group first
        return JNI_FALSE;
    lanEndAll();
    melonDS::MPInterface::Set(melonDS::MPInterface_LAN);
    lanApplyRecvTimeout();
    bool ok = lan().StartDiscovery();
    if (ok)
        lanMode = LanDiscovering;
    else
    {
        melonDS::MPInterface::Set(melonDS::MPInterface_Dummy);
        ambientInstall();
    }
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
    if (MelonDSAndroid::netplayActive() || groupLink || !lanLockParked())   // end the session / group first
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
        ambientInstall();
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
            rows = lanPlayerRows(lan());
        pthread_mutex_unlock(&emuThreadMutex);
    }
    return lanStringArray(env, rows);
}

// Netplay start over the lobby. Host: sends it to every client. Client: -1 until the host's
// start arrives, then (hosted << 8) | players.
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_lanStartSession(JNIEnv* env, jobject thiz, jboolean hosted, jint players, jint server)
{
    if (!lanLockParked())
        return;
    if (lanMode == LanHosting)
        lan().HostStartSession(hosted ? 1 : 0, (u8) players, (u8) server);
    pthread_mutex_unlock(&emuThreadMutex);
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_lanGetStartRequest(JNIEnv* env, jobject thiz)
{
    int r = -1;
    if (lanLockParked())
    {
        if (lanMode == LanJoined)
            r = lan().TakeStartRequest();
        pthread_mutex_unlock(&emuThreadMutex);
    }
    return r;
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

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_netplayKind(JNIEnv* env, jobject thiz)
{
    return MelonDSAndroid::netplayKind();
}

// Netplay: a press of the fast-forward key toggles the session's speed for every player; returns
// what the key shows (1 fast, 0 normal), -1 = not in Netplay (the key is local)
JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_netplayToggleFastForward(JNIEnv* env, jobject thiz)
{
#ifdef LITEV_NP_SPEED
    return MelonDSAndroid::netplayToggleSpeed(ffSetting);
#else
    return -1;
#endif
}

// Netplay: what the fast-forward key shows (the session's speed, or this player's press in flight)
JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_netplayFastForwardShown(JNIEnv* env, jobject thiz)
{
#ifdef LITEV_NP_SPEED
    return MelonDSAndroid::netplaySpeedShown();
#else
    return -1;
#endif
}

// Netplay: the session speed ("" normal, else "FF 2x by P2" / "uncapped by P1")
JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_netplaySpeedLabel(JNIEnv* env, jobject thiz)
{
    return env->NewStringUTF(MelonDSAndroid::netplaySpeedLabel().c_str());
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_netplayStatus(JNIEnv* env, jobject thiz)
{
    return env->NewStringUTF(MelonDSAndroid::netplayStatus().c_str());
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_recordQueue(JNIEnv* env, jobject thiz, jstring request)
{
    const char* r = env->GetStringUTFChars(request, JNI_FALSE);
    MelonDSAndroid::recordQueue(r);
    env->ReleaseStringUTFChars(request, r);
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_recordMark(JNIEnv* env, jobject thiz)
{
    return MelonDSAndroid::recordMark();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setHostBattery(JNIEnv* env, jobject thiz, jint percent, jboolean charging)
{
    MelonDSAndroid::setHostBattery(percent, charging);
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_recordMode(JNIEnv* env, jobject thiz)
{
    return MelonDSAndroid::recordMode();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_netplayPrepare(JNIEnv* env, jobject thiz, jint player, jint players, jstring host, jboolean hosted, jobjectArray library,
                                                    jstring cacheDir, jlong cacheMaxBytes)
{
    std::vector<std::string> paths;
    for (jsize i = 0, n = env->GetArrayLength(library); i < n; i++)
    {
        auto path = (jstring)env->GetObjectArrayElement(library, i);
        const char* p = env->GetStringUTFChars(path, nullptr);
        paths.emplace_back(p);
        env->ReleaseStringUTFChars(path, p);
        env->DeleteLocalRef(path);
    }
    const char* h = env->GetStringUTFChars(host, nullptr);
    const char* c = env->GetStringUTFChars(cacheDir, nullptr);
    MelonDSAndroid::netplayPrepare(player, players, h, hosted, std::move(paths), c, (u64)cacheMaxBytes);
    env->ReleaseStringUTFChars(cacheDir, c);
    env->ReleaseStringUTFChars(host, h);
}

// session setup's ROM transfer: "state\nbytes\ntotal\ntitle\nquestion" (NetplayXferStatus)
JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_netplayTransferStatus(JNIEnv* env, jobject thiz)
{
    melonDS::NetplayXferStatus st = melonDS::NetplayGetXferStatus();
    std::string s = std::to_string(st.State) + "\n" + std::to_string(st.Bytes) + "\n" + std::to_string(st.Total) + "\n" + st.Title + "\n" + st.Question;
    return env->NewStringUTF(s.c_str());
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_netplayAnswer(JNIEnv* env, jobject thiz, jboolean yes)
{
    melonDS::NetplayAnswer(yes);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_netplayCancelSetup(JNIEnv* env, jobject thiz)
{
    melonDS::NetplayCancelSetup();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_lanLeave(JNIEnv* env, jobject thiz)
{
    if (!lanLockParked())
        return;
    lanEndAll();
    ambientInstall();       // the lobby closed: back to Just Play
    pthread_mutex_unlock(&emuThreadMutex);
}

// The lobby (hosting or joined) becomes the group: kept connected, no longer the MPInterface.
JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_lanToGroup(JNIEnv* env, jobject thiz)
{
    if (!lanLockParked())
        return JNI_FALSE;
    bool ok = lanMode == LanHosting || lanMode == LanJoined;
    if (ok)
    {
        std::lock_guard<std::mutex> lk(groupMutex);
        groupLink = melonDS::MPInterface::Take();
        grp().SetPeerTimeout(10000);    // a dropped device is noticed in 10 s: at 3 s a congested Wi-Fi (200-600 ms round trips, 7-20% loss measured) dropped a live device and ended its session alone
        lanMode = LanNone;
    }
    pthread_mutex_unlock(&emuThreadMutex);
    return ok;
}

// The group becomes this game's LAN session again (the host switched the group to LAN).
JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_groupToLan(JNIEnv* env, jobject thiz)
{
    if (MelonDSAndroid::netplayActive() || !lanLockParked())
        return JNI_FALSE;
    std::lock_guard<std::mutex> lk(groupMutex);
    bool ok = groupLink != nullptr;
    if (ok)
    {
        bool host = false;
        for (const auto& p : grp().GetPlayerList())
            if (p.IsLocalPlayer) host = p.Status == melonDS::LAN::Player_Host;
        lanEndAll();
        melonDS::MPInterface::Set(std::move(groupLink), melonDS::MPInterface_LAN);
        lanApplyRecvTimeout();
        lanMode = host ? LanHosting : LanJoined;
    }
    pthread_mutex_unlock(&emuThreadMutex);
    return ok;
}

// Just Play: mode 0 everyone nearby, 1 only while the game's wireless is on, 2 hidden, 3 lobby only (off). Called
// before a game starts (the MAC comes from the id); a change of mode while running applies its visibility now, and
// "lobby only" / back on from the next game.
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientConfigure(JNIEnv* env, jobject thiz, jlong deviceId, jstring name, jint mode)
{
#ifdef LITEV_AMBIENT_LAN
    std::lock_guard<std::mutex> g(ambientMu);
    ambientCfg.DeviceID = (u64)deviceId;
    ambientCfg.Name = lanJString(env, name);
    // Presence advertises the native compatibility fingerprint, not the Java
    // package version. The full 64-bit value also binds live checkpoints.
    ambientCfg.BuildID = (u32)(SERENEDS_HANDOFF_BUILD_ID ^ (SERENEDS_HANDOFF_BUILD_ID >> 32));
    ambientMode = mode >= 0 && mode <= 2 ? mode : -1;
    if (ambientPtr && ambientMode >= 0) ambientPtr->SetVisibility(ambientMode);
#endif
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientSetForeground(JNIEnv* env, jobject thiz, jboolean fg)
{
#ifdef LITEV_AMBIENT_LAN
    std::lock_guard<std::mutex> g(ambientMu);
    ambientFg = fg;
    if (ambientPtr) ambientPtr->SetForeground(fg);
#endif
}

// a SereneDS device found by NSD (_sereneds._udp): presence goes there by unicast too (routers that drop broadcast)
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientAddPeer(JNIEnv* env, jobject thiz, jstring ip, jint port)
{
#ifdef LITEV_AMBIENT_LAN
    in_addr a;
    if (inet_pton(AF_INET, lanJString(env, ip).c_str(), &a) != 1 || port <= 0 || port > 65535) return;
    std::lock_guard<std::mutex> g(ambientMu);
    for (auto& e : ambientPeers) if (e.first == a.s_addr && e.second == (u16)port) return;
    ambientPeers.push_back({a.s_addr, (u16)port});
    if (ambientPtr) ambientPtr->AddPeerAddress(a.s_addr, (u16)port);
#endif
}

// this device's presence port (advertised over NSD), 0 = Just Play not running
JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientPort(JNIEnv* env, jobject thiz)
{
#ifdef LITEV_AMBIENT_LAN
    std::lock_guard<std::mutex> g(ambientMu);
    return ambientPtr ? ambientPtr->GetUnicastPort() : 0;
#else
    return 0;
#endif
}

// nearby devices, one row each: "id \t name \t game \t ip:port \t ggid \t build \t linked"
JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientNearby(JNIEnv* env, jobject thiz)
{
    std::vector<std::string> rows;
#ifdef LITEV_AMBIENT_LAN
    {
        std::lock_guard<std::mutex> g(ambientMu);
        if (ambientPtr) rows = ambientPtr->NearbyRows();
    }
#endif
    return lanStringArray(env, rows);
}

// the counters (cumulative), "key=value" pairs separated by spaces; "" = not running
JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientStats(JNIEnv* env, jobject thiz)
{
    std::string r;
#ifdef LITEV_AMBIENT_LAN
    std::lock_guard<std::mutex> g(ambientMu);
    if (ambientPtr)
    {
        const auto s = ambientPtr->GetStats();
        char b[768];
        snprintf(b, sizeof(b), "frames=%llu nearby=%u links=%u opened=%llu presence_tx=%llu presence_rx=%llu link_tx=%llu link_rx=%llu "
                 "beacons=%llu blocked_us=%llu exchanges=%llu frame_blocked_us=%u frame_exchanges=%u rtt_ms=%u "
                 "slow_fps_x100=%u slow_wait_us=%u slow_exchanges_x100=%u slow_offer=%u "
                 "host_wait_us=%llu host_wait_calls=%llu host_wait_misses=%llu host_wait_timeouts=%llu "
                 "client_wait_us=%llu client_wait_calls=%llu client_wait_misses=%llu",
                 (unsigned long long)s.Frames, s.Nearby, s.Links, (unsigned long long)s.LinksOpened, (unsigned long long)s.PresenceTx,
                 (unsigned long long)s.PresenceRx, (unsigned long long)s.LinkTx, (unsigned long long)s.LinkRx,
                 (unsigned long long)s.BeaconsInjected, (unsigned long long)s.BlockedUs, (unsigned long long)s.Exchanges,
                 s.LastFrameBlockedUs, s.LastFrameExchanges, s.MaxRttMs, s.SlowFpsX100, s.SlowWaitUsPerFrame,
                 s.SlowExchangesPerFrameX100, s.SlowdownPending ? 1u : 0u,
                 (unsigned long long)s.HostWaitUs, (unsigned long long)s.HostWaitCalls, (unsigned long long)s.HostWaitMisses,
                 (unsigned long long)s.HostWaitTimeouts,
                 (unsigned long long)s.ClientWaitUs, (unsigned long long)s.ClientWaitCalls, (unsigned long long)s.ClientWaitMisses);
        r = b;
    }
#endif
    return env->NewStringUTF(r.c_str());
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientDismissSlowdownPrompt(JNIEnv* env, jobject thiz)
{
#ifdef LITEV_AMBIENT_LAN
    std::lock_guard<std::mutex> g(ambientMu);
    if (ambientPtr) ambientPtr->DismissSlowdownPrompt();
#endif
}

// These calls queue user intent only. Ambient's ENet state is touched on the
// emulator thread by ambientHandoffService(), never from Compose/JNI callers.
JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientOfferHandoff(JNIEnv*, jobject, jint mode)
{
#ifdef LITEV_AMBIENT_LAN
    if (mode != melonDS::AmbientLAN::HandoffPlainNetplay &&
        mode != melonDS::AmbientLAN::HandoffHostedNetplay) return JNI_FALSE;
    std::lock_guard<std::mutex> g(ambientMu);
    if (!ambientPtr || ambientHandoffRequests.size() >= 8) return JNI_FALSE;
    ambientHandoffRequests.push_back({AmbientHandoffRequestKind::Offer, 0, mode, false});
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientRespondHandoff(JNIEnv*, jobject, jlong epoch, jboolean accept)
{
#ifdef LITEV_AMBIENT_LAN
    if (epoch <= 0) return JNI_FALSE;
    std::lock_guard<std::mutex> g(ambientMu);
    if (!ambientPtr || ambientHandoffRequests.size() >= 8) return JNI_FALSE;
    ambientHandoffRequests.push_back({AmbientHandoffRequestKind::Respond,
                                      static_cast<u64>(epoch), 0, accept == JNI_TRUE});
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientCancelHandoff(JNIEnv*, jobject)
{
#ifdef LITEV_AMBIENT_LAN
    std::lock_guard<std::mutex> g(ambientMu);
    if (!ambientPtr || ambientHandoffRequests.size() >= 8) return JNI_FALSE;
    ambientHandoffRequests.push_back({AmbientHandoffRequestKind::Cancel});
    return JNI_TRUE;
#else
    return JNI_FALSE;
#endif
}

// "phase\tepoch\tmode\tpeer_id". A queued offer is not yet peer acceptance.
JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_ambientHandoffStatus(JNIEnv* env, jobject)
{
#ifdef LITEV_AMBIENT_LAN
    std::lock_guard<std::mutex> g(ambientMu);
    return env->NewStringUTF(ambientHandoffStatus.c_str());
#else
    return env->NewStringUTF("none");
#endif
}

JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_groupPlayers(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::mutex> lk(groupMutex);
    return lanStringArray(env, groupLink ? lanPlayerRows(grp()) : std::vector<std::string> {});
}

// Pumps the group; returns the next command from the host ((server << 16) | (mode << 8) |
// players), or -1. A client whose host has gone leaves the group.
JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_groupTake(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::mutex> lk(groupMutex);
    if (!groupLink)
        return -1;
    {   // the group's links time out after 3 s unpumped: say when the pump itself was late
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        long gap = (long)std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count();
        if (gap > 400) Platform::Log(Platform::LogLevel::Warn, "Group: pumped after %ld ms\n", gap);
        last = now;
    }
    grp().Process();
    for (const auto& p : grp().TakeLeft())
        groupLeftRows.push_back(std::to_string(p.ID) + "\t" + std::string(p.Name));
    bool lostHost = false;
    for (const auto& p : grp().GetPlayerList())
        if (p.ID == 0 && !p.IsLocalPlayer && p.Status == melonDS::LAN::Player_Disconnected) lostHost = true;
    int r = grp().TakeStartRequest();
    if (lostHost && r < 0)
    {
        grp().EndSession();
        groupLink = nullptr;
    }
    return r;
}

// The players who left the group (lobby link closed or timed out) since the last call: "id \t name"
JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_groupLeft(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::mutex> lk(groupMutex);
    std::vector<std::string> rows;
    rows.swap(groupLeftRows);
    return lanStringArray(env, rows);
}

// Host: sends a group command to every client
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_groupSend(JNIEnv* env, jobject thiz, jint mode, jint players, jint server)
{
    std::lock_guard<std::mutex> lk(groupMutex);
    if (groupLink)
        grp().HostStartSession((u8) mode, (u8) players, (u8) server);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_groupLeave(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::mutex> lk(groupMutex);
    if (groupLink)
        grp().EndSession();
    groupLink = nullptr;
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

        ambientHandoffService();
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

#ifdef LITEV_NP_SPEED
        // Netplay: the session's speed (plain: the same frame on every device) is the fast-forward
        // state: pacing, frameskip, skip-repeat and the sound follow it as for local fast-forward.
        {
            static bool npDriven = false;
            float m;
            if (MelonDSAndroid::netplaySpeed(m))
            {
                npDriven = true;
                isFastForwardEnabled = m != 1.0f;
                fastForwardSpeedMultiplier = m == 0 ? -1.0f : m;   // <= 0 = unlimited
                limitFps = m != 0;
                targetFps = m > 0 ? (int)(60 * m) : 60;
            }
            else if (npDriven)
            {
                npDriven = false;
                isFastForwardEnabled = false;
                fastForwardSpeedMultiplier = ffSetting;
                limitFps = true;
                targetFps = 60;
            }
        }
#endif
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

        // debug.litev.ffhold=<multiplier>:<frames> (measurement): fast-forward is held at that
        // multiplier for the first <frames> frames after launch, as if the player held the key
        // (devreplay of the real fast-forward path, then its release). Read once.
        {
            static float holdM = 0; static int holdN = 0, holdFrame = 0; static bool holdOn = false;
            static const bool holdRead = [] { char b[PROP_VALUE_MAX] = {}; if (__system_property_get("debug.litev.ffhold", b) > 0) sscanf(b, "%f:%d", &holdM, &holdN); return true; }();
            (void)holdRead;
            const bool want = holdM != 0 && holdFrame++ < holdN;
            if (want != holdOn)
            {
                holdOn = want;
                if (want) fastForwardSpeedMultiplier = holdM;
                isFastForwardEnabled = want;
                limitFps = !want || fastForwardSpeedMultiplier > 0;
                targetFps = want ? 60 * fastForwardSpeedMultiplier : 60;
                LOG_INFO("LITEV_FFHOLD", "fast-forward %s at frame %d", want ? "held" : "released", holdFrame);
            }
        }
#ifdef LITEV_FF_HEADLESS3D
        {
            // only banks that no shown frame renders are built unwatched (GPU::FFHeadlessDecide);
            // debug.litev.ffheadless=0 turns it off
            static const bool ffHeadless = [] { char b[92] = {}; return !(__system_property_get("debug.litev.ffheadless", b) > 0 && atoi(b) == 0); }();
            MelonDSAndroid::setFastForwardHeadless(ffHeadless && (noLimit || (isFastForwardEnabled && fastForwardSpeedMultiplier != 1.0f)));
        }
#endif
        if (limitFps && !noLimit)
        {
            frameLimitError += frameTimeStep - delay;
#ifdef LITEV_PACE_CATCHUP
            // Up to N frames behind (debug.litev.pacedebt, default 3) are caught up by running the
            // next frames without sleeping, so a one-off slow frame (a JIT compile burst) doesn't
            // permanently drop game time. Upstream keeps only one frame of debt.
            static const double maxDebt = [] { char b[92] = {}; return __system_property_get("debug.litev.pacedebt", b) > 0 ? atof(b) : 3.0; }();
            if (frameLimitError < -maxDebt * frameTimeStep)
                frameLimitError = -maxDebt * frameTimeStep;
#else
            if (frameLimitError < -frameTimeStep)
                frameLimitError = -frameTimeStep;
#endif
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
        // Never during linked multiplayer: network wait makes the controller mistake link latency
        // for CPU pressure. Skipping draws can recover some emulated speed, but it cannot reduce
        // the link's round trip and can hide three out of four displayed frames.
        // Just Play leaves lanMode == LanNone; check its active MP links too. Discovery alone keeps
        // the solo player's adaptive frameskip available.
        static const bool ambientAutoFs = [] { char b[PROP_VALUE_MAX] = {}; return __system_property_get("debug.litev.ambientautofs", b) > 0 && atoi(b) != 0; }();
        const bool ambientLinked = ambientHasLinks();
        if (autoFrameskipEnabled && !isFastForwardEnabled && lanMode == LanNone && (!ambientLinked || ambientAutoFs)) {
            // One frame's sample is clamped to 2x the budget and the EMA is slow (alpha 0.1):
            // a one-off hitch (savestate load, area load, JIT compile burst) can't raise the
            // level by itself. Skipping drawing doesn't shorten those frames anyway; the old
            // controller (alpha 0.3, unclamped, jump 2) latched level 3 on PW's load-in and its
            // 120-frame-per-level release kept the picture at 15 then 30 fps for ~6 s while
            // the work was 5 ms of a 16.7 ms budget and the counter read 60 (RG DS 2, 2026-10-12).
            // Release after 30 frames of headroom; an attack soon after a release doubles the
            // next release wait (30 .. 480 frames) so a scene at the threshold can't flap.
            // debug.litev.autofsold=1: the old controller (A/B).
            static const bool oldFs = [] { char b[PROP_VALUE_MAX] = {}; return __system_property_get("debug.litev.autofsold", b) > 0 && atoi(b) != 0; }();
            const double w = oldFs || delay < frameTimeStep * 2 ? delay : frameTimeStep * 2;
            const double alpha = oldFs ? 0.3 : 0.1;
            autoFsEmaMs = (autoFsEmaMs <= 0.0) ? (oldFs || w < frameTimeStep ? w : frameTimeStep)
                                               : (autoFsEmaMs * (1 - alpha) + w * alpha);
            if (autoFsCooldown > 0) autoFsCooldown--;
            autoFsSinceRelease++;

            if (autoFsEmaMs > frameTimeStep * 1.15) {
                autoFsHeadroom = 0;                        // behind -> attack
                if (autoFsCooldown == 0 && autoFsSkip < 3) {
                    int step = (oldFs && autoFsEmaMs > frameTimeStep * 1.9) ? 2 : 1;
                    autoFsSkip = (autoFsSkip + step > 3) ? 3 : autoFsSkip + step;
                    MelonDSAndroid::setFrameskip(autoFsSkip);
                    if (!oldFs && autoFsSinceRelease < 300)
                        autoFsRelease = autoFsRelease * 2 < 480 ? autoFsRelease * 2 : 480;
                    LOG_INFO("LITEV_AUTOFS", "skip level %d (ema %.2f ms, release %d)", autoFsSkip, autoFsEmaMs, autoFsRelease);
                    autoFsCooldown = oldFs ? 5 : 15;   // let the new level show in the EMA
                }
            } else if (autoFsEmaMs < frameTimeStep * 0.70) {
                if (!oldFs && autoFsSinceRelease >= 300) autoFsRelease = 30;   // the last release held
                if (++autoFsHeadroom >= (oldFs ? 120 : autoFsRelease) && autoFsSkip > 0) {   // headroom -> release
                    autoFsSkip--;
                    MelonDSAndroid::setFrameskip(autoFsSkip);
                    LOG_INFO("LITEV_AUTOFS", "skip level %d (ema %.2f ms)", autoFsSkip, autoFsEmaMs);
                    autoFsHeadroom = 0;
                    autoFsCooldown = 5;
                    autoFsSinceRelease = 0;
                }
            } else {
                autoFsHeadroom = 0;                        // stable band -> hold
            }
        } else if (autoFsSkip != 0) {
            // Setting turned off, or fast-forward engaged, while a skip was applied ->
            // clear it; the controller restarts from 0 when it next runs.
            autoFsSkip = 0; autoFsEmaMs = 0.0; autoFsCooldown = 0; autoFsHeadroom = 0; autoFsRelease = 30; autoFsSinceRelease = 1000;
            MelonDSAndroid::setFrameskip(0);
            LOG_INFO("LITEV_AUTOFS", "skip level 0 (inactive)");
        }
#endif

#ifdef LITEV_SKIP_REPEAT_FRAMES
        // "Skip repeated frames: Auto" (only used when that mode is in force, see runFrame).
        // Judged against the current target speed: fast-forward above 1x (or unlimited) = on,
        // most of its presented frames are dropped anyway; below 1x = off; at 1x the
        // SkipRepeatAuto controller decides from the work time vs the frame period.
        // The debug.litev.nolimit measurement mode counts as unlimited fast-forward.
        {
            bool want;
            if (noLimit || (isFastForwardEnabled && fastForwardSpeedMultiplier != 1.0f)) {
                want = noLimit || !limitFps || fastForwardSpeedMultiplier > 1.0f;
                skipRepeatAuto.reset();
            } else
                want = skipRepeatAuto.update(delay, frameTimeStep);
#ifdef LITEV_AUTO_FRAMESKIP
            if (autoFsSkip > 0)   // the raster frameskip rungs sit above this one
                want = skipRepeatAuto.on = true;
#endif
            MelonDSAndroid::setSkipRepeatAuto(want);
            if (want != skipRepeatAutoLogged) {
                skipRepeatAutoLogged = want;
                LOG_INFO("LITEV_SKIPREP", "auto %s (ema %.2f ms, budget %.2f ms, next hold %d, ff %d)",
                         want ? "on" : "off", skipRepeatAuto.emaMs, frameTimeStep, skipRepeatAuto.hold, isFastForwardEnabled ? 1 : 0);
            }
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
#ifdef LITEV_FF_ADAPTIVE_SKIP
            if (limitFps) ffWant = ffAdaptiveSkip(ffWant, ffMaxFrameskip, delay, frameTimeStep);
#endif
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

        // debug.litev.presentlog=1: per emulated frame "work_ms,sleep_ms", 60 per line
        {
            static const bool presLog = [] { char b[PROP_VALUE_MAX] = {}; return __system_property_get("debug.litev.presentlog", b) > 0 && atoi(b) != 0; }();
            if (presLog)
            {
                static char line[2048]; static int n = 0, len = 0;
                if (n == 0) len = snprintf(line, sizeof(line), "t=%lld", (long long)(std::chrono::steady_clock::now().time_since_epoch().count() / 1000000));
                len += snprintf(line + len, sizeof(line) - len, " %.1f,%.1f",
                                std::chrono::duration_cast<std::chrono::nanoseconds>(frameDuration).count() / 1e6, actualSleepMs);
                if (++n >= 60) { LOG_INFO("LITEV_EMUF", "%s", line); n = 0; }
            }
        }

        observedFrames++;
        if (observedFrames >= 30) {
            // The counter shows frames that reached the screen: emulated frames minus the ones
            // frameskip didn't draw and the drawn ones the presenter replaced before a vsync
            // showed them (skip-repeat frames count: they look the same as the one shown).
            // Fast-forward shows the emulated rate (most frames are meant to be skipped there).
            // debug.litev.fpsemu=1: always the emulated rate.
            static const bool fpsEmu = [] { char b[PROP_VALUE_MAX] = {}; return __system_property_get("debug.litev.fpsemu", b) > 0 && atoi(b) != 0; }();
            static int lastHidden = 0, lastDrops = 0;
            const int hidden = g_framesHidden.load(std::memory_order_relaxed), drops = g_presentDrops.load(std::memory_order_relaxed);
            const int hiddenDelta = hidden - lastHidden, dropDelta = drops - lastDrops;
            int shown = observedFrames;
            if (!fpsEmu && !isFastForwardEnabled && limitFps && !noLimit)
                shown -= hiddenDelta + dropDelta;
            lastHidden = hidden; lastDrops = drops;
            fps = ((shown > 0 ? shown : 0) * 1000.0) / (lastTick - lastMeasureFpsTick);
#ifdef LITEV_AMBIENT_LAN
            static const bool logAmbientView = [] { char b[PROP_VALUE_MAX] = {}; return __system_property_get("debug.litev.ambientview", b) > 0 && atoi(b) != 0; }();
            if (logAmbientView && ambientHasLinks())
#ifdef LITEV_AUTO_FRAMESKIP
                LOG_INFO("LITEV_AMBIENT_VIEW", "emu %.1f shown %.1f hidden %d drops %d auto skip %d", observedFrames * 1000.0 / (lastTick - lastMeasureFpsTick),
                         fps, hiddenDelta, dropDelta, autoFsSkip);
#else
                LOG_INFO("LITEV_AMBIENT_VIEW", "emu %.1f shown %.1f hidden %d drops %d", observedFrames * 1000.0 / (lastTick - lastMeasureFpsTick),
                         fps, hiddenDelta, dropDelta);
#endif
#endif
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
