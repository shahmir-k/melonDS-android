#ifndef MELONINSTANCE_H
#define MELONINSTANCE_H

#include <string>
#ifdef LITEV_RENDER_THREAD
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#endif
#include "Args.h"
#include "Configuration.h"
#include "NDS.h"
#include "MelonDS.h"
#include "SaveManager.h"
#include "RewindManager.h"
#include "renderer/FrameQueue.h"
#include "renderer/Renderer.h"
#include "renderer/ScreenshotRenderer.h"
#include "retroachievements/RetroAchievementsManager.h"
#include "net/Net.h"

using namespace melonDS;

namespace MelonDSAndroid
{

class MelonInstance
{

public:
    MelonInstance(int instanceId, std::shared_ptr<EmulatorConfiguration> configuration, std::unique_ptr<melonDS::NDSArgs> args, std::shared_ptr<Net> net, std::unique_ptr<ScreenshotRenderer> screenshotRenderer, int consoleType);
    ~MelonInstance();

    int getInstanceId() { return instanceId; };

    bool loadRom(std::string romPath, std::string sramPath);
    bool loadGbaRom(std::string romPath, std::string sramPath);
    void loadRumblePak();
    void loadGbaMemoryExpansion();
    bool bootFirmware();
    void start();
    void reset();
    melonDS::u32 runFrame();
    void stop();

    void touchScreen(u16 x, u16 y);
    void releaseScreen();
    void pressKey(u32 key);
    void releaseKey(u32 key);
    int readAudioOutput(s16* buffer, int length);
    void setAudioOutputSkew(double skew);
    bool takeScreenshot();
    void loadCheats(std::list<Cheat> cheats);
    int sendNetPacket(u8* data, int length);
    int receiveNetPacket(u8* data);

    Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline);

    void updateConfiguration(std::shared_ptr<EmulatorConfiguration> newConfiguration);
    void requestNdsSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    void requestGbaSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    void requestFirmwareSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    bool saveState(Savestate* state);
    bool loadState(Savestate* state);
    RewindWindow getRewindWindow();
    bool loadRewindState(RewindSaveState rewindSaveState);
    void setupAchievements(
        std::list<RetroAchievements::RAAchievement> achievements,
        std::list<RetroAchievements::RALeaderboard> leaderboards,
        std::optional<std::string> richPresenceScript
    );
    void unloadRetroAchievementsData();
    std::string getRichPresenceStatus();
    std::vector<RetroAchievements::RARuntimeAchievement> getRuntimeAchievements();

private:
    void updateRenderer();
    void blitAcceleratedFrame(melonDS::u32 srcArrayTex, melonDS::u32 dstTex, int dstWidth, int dstHeight);
    void blitAcceleratedFrameFBO(melonDS::u32 srcArrayTex, melonDS::u32 dstTex, int dstWidth, int dstHeight, melonDS::u32& readFBO, melonDS::u32& drawFBO);
    void setBatteryLevels();
    void setDateTime();
    void saveRewindState(RewindSaveState* rewindSaveState);

#ifdef LITEV_RENDER_THREAD
    // ---- R4 STEP 3: render-thread offload (docs/r4-render-thread-design.md §4) ----
    // The emu thread (this runFrame) runs NDS::RunFrame (GL-free) + per-scanline
    // capture, publishes a depth-1 packet (the log/shadow bank id), and immediately
    // runs the next frame. A dedicated render thread owns a GL context in the emu
    // context's share group and replays the frame's GL submission (SubmitFrame ->
    // ReplayLog), blits and presents. It releases the geometry bank EARLY (design
    // §4.2), right after the 3D geometry upload — which is also after the whole 2D
    // command log has been replayed, so the emu thread's next-frame DrawScanline
    // cannot race the 2D config members the replay reads.
    bool renderThreadActive();          // flag ON + prop + accelerated + deferred
    bool renderThreadWanted();          // topology: accelerated config + renderthread prop
    void startRenderThread();           // lazy: creates the render GL context
    void stopRenderThread();
    void renderThreadLoop();
    // Run a GL closure synchronously on the render thread (the sole GL-context
    // owner). Drains first. Used for renderer (re)creation, capture frames and
    // screenshot capture — all of which must issue GL on the render context where
    // the renderer's per-context objects (FBOs/VAOs) live.
    void dispatchToRenderThread(std::function<void()> fn);
    // The GL submit+blit+present block, run on whichever context is current (the
    // render thread normally; the emu thread for the drained capture fallback).
    Frame* glSubmitPresent(int bank, bool deferred, bool sleeping, int screenW, int screenH, bool onRenderThread);
    void onBankReleased();              // early-release callback (from core, on render thread)
    void drainRenderThread();           // block until the render thread is fully idle

    class OpenGLContext* renderGlContext = nullptr;
    std::thread          renderThread;
    std::mutex           rtMutex;
    std::condition_variable rtJobCond;  // emu -> render: job ready / stop
    std::condition_variable rtDoneCond; // render -> emu: bank released / idle
    bool rtStarted   = false;
    bool rtStop      = false;
    bool rtHasJob    = false;           // a published job awaiting render pickup
    bool rtBusy      = false;           // render actively working (pickup..present done)
    // Depth-1 handshake via monotonic counters (robust against per-job flag reuse):
    // the emu increments rtJobsPublished at publish; the render increments
    // rtBanksReleased exactly once per job at early release. The emu's gate waits
    // rtBanksReleased == rtJobsPublished before the next RunFrame (the previous
    // job's geometry bank + 2D config + texcache reads are all complete).
    unsigned long long rtJobsPublished = 0;
    unsigned long long rtBanksReleased = 0;
    bool rtReleasePending = false;      // render-thread-owned: this job's release not yet counted
    bool rtCapturePrev  = false;        // last frame used display capture (Tier-1 fallback gate)
    bool rtUse          = false;        // topology: this run offloads GL to the render thread
    bool rtTopologyDecided = false;
    // synchronous GL task dispatched to the render thread (updateRenderer/capture/screenshot)
    std::function<void()> rtTask;
    bool rtTaskPending = false;
    bool rtTaskDone    = false;
    melonDS::u32 rtNLines = 0;          // RunFrame result when RunFrame runs on the render thread
    // depth-1 job payload
    int  rtJobBank     = 0;
    bool rtJobDeferred = false;
    bool rtJobSleeping = false;
    int  rtJobScreenW  = 0;
    int  rtJobScreenH  = 0;
    // profiling accumulators (emu thread)
    double rtGateMsAccum = 0.0;
    // Render-thread-private blit FBOs (FBO container objects are per-context in ES;
    // the emu-context sync/capture path uses blitReadFBO/blitDrawFBO instead).
    melonDS::u32 rtBlitReadFBO = 0;
    melonDS::u32 rtBlitDrawFBO = 0;
#endif

private:
    int instanceId;
    int consoleType;
    NDS* nds;
    std::shared_ptr<Net> net;

    std::unique_ptr<RetroAchievements::RetroAchievementsManager> retroAchievementsManager;
    std::unique_ptr<SaveManager> ndsSave;
    std::unique_ptr<SaveManager> gbaSave;
    std::unique_ptr<SaveManager> firmwareSave;
    u32 inputMask;

    std::shared_ptr<EmulatorConfiguration> currentConfiguration;
    FrameQueue frameQueue;
    std::unique_ptr<ScreenshotRenderer> screenshotRenderer;
    RewindManager rewindManager;
    Renderer currentRenderer;
    bool isRenderConfigurationDirty;
    int frame;
    // FBOs used to blit the accelerated renderer's array-texture output into
    // the app's stacked 2D frame texture (created lazily, 0 = uninitialised).
    melonDS::u32 blitReadFBO = 0;
    melonDS::u32 blitDrawFBO = 0;
};

}

#endif
