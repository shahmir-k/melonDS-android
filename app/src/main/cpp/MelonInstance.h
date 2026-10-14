#ifndef MELONINSTANCE_H
#define MELONINSTANCE_H

#include <atomic>
#include <mutex>

#include <string>
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
#include "net/NetplayInput.h"

using namespace melonDS;

namespace MelonDSAndroid
{

class MelonInstance
{

public:
    // A staged/remote console supplies its own firmware save path before Reset() can write.
    // Empty keeps the normal owner path for the visible console.
    MelonInstance(int instanceId, std::shared_ptr<EmulatorConfiguration> configuration, std::unique_ptr<melonDS::NDSArgs> args, std::shared_ptr<Net> net, std::unique_ptr<ScreenshotRenderer> screenshotRenderer, int consoleType, std::string firmwareSavePathOverride = {});
    ~MelonInstance();

    int getInstanceId() { return instanceId; };
    const melonDS::NetplayRom& loadedRomDescription() const { return loadedRom; }
    // Live Just Play handoff: the visible source keeps its exact DS state while
    // the joining device changes its Netplay/MP slot from 0 to 1.
    bool reassignInstanceId(int newId);

    bool loadRom(std::string romPath, std::string sramPath);
    bool loadGbaRom(std::string romPath, std::string sramPath);
    void loadRumblePak();
    void loadGbaMemoryExpansion();
    bool bootFirmware();
    void start();
    void reset();
    melonDS::u32 runFrame();
#ifdef LITEV_AGGRESSIVE_SKIP
    void setFrameskipTarget(int target);
#endif
    bool skipRepeatAuto = false;   // set by the "Skip repeated frames: Auto" controller (emu thread)
    void stop();

    melonDS::NDS* getNds() { return nds; }

    // Netplay: input is only recorded here; the session applies every player's input to every
    // console a few frames later, identically on every device.
    void setInputDeferred(bool deferred);
    void setRewindSuspendedForNetplay(bool suspended);
    // the last runFrame's timings (record mode's per-frame log)
    struct FrameStats { double loopMs = 0, runFrameMs = 0, emuCpuMs = 0; bool drawn = true; };
    const FrameStats& lastFrameStats() const { return frameStats; }
    int getFrame() const { return frame; }
    melonDS::u32 getInputMask();
    bool getTouch(u16& x, u16& y);
    // Netplay: another player's console. Emulates a frame with no presentation (its screens are
    // not shown), rendering exactly like the local console so both stay identical.
    void runFrameHeadless();

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

    Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline, bool* isNew = nullptr);

    void updateConfiguration(std::shared_ptr<EmulatorConfiguration> newConfiguration);
    void requestNdsSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    // game save writes go to this file from now on (a replay: never the player's .sav)
    void redirectNdsSave(const std::string& path) { if (ndsSave) ndsSave->SetPath(path, false); }
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
    void updateBattery();

private:
    void updateRenderer();
    void blitAcceleratedFrame(melonDS::u32 srcArrayTex, melonDS::u32 dstTex, int dstWidth, int dstHeight);
    void setBatteryLevels();
    int batteryTarget();
    int appliedBattery = 100;
    void setDateTime();
    void saveRewindState(RewindSaveState* rewindSaveState);

private:
    int skipRepeatMode = 1;        // debug.litev.skiprepeat value in force (polled in runFrame)
    bool skipRepeatMp = false;     // Netplay or LAN running
    int instanceId;
    melonDS::NetplayRom loadedRom;
    int consoleType;
    NDS* nds;
    std::shared_ptr<Net> net;

    std::unique_ptr<RetroAchievements::RetroAchievementsManager> retroAchievementsManager;
    std::unique_ptr<SaveManager> ndsSave;
    std::unique_ptr<SaveManager> gbaSave;
    std::unique_ptr<SaveManager> firmwareSave;
    u32 inputMask;
    bool inputDeferred = false;
    bool deferredLidClosed = false;
    std::mutex inputMutex;
    std::atomic_bool netplayRewindSuspended{false};
    FrameStats frameStats;
    u32 lastRetiredPresented = 0;   // SoftRenderer::RetiredFrames() at the last present
    bool touching = false;
    u16 touchX = 0, touchY = 0;
    bool headlessRendererSet = false;

    std::shared_ptr<EmulatorConfiguration> currentConfiguration;
    FrameQueue frameQueue;
    std::unique_ptr<ScreenshotRenderer> screenshotRenderer;
    RewindManager rewindManager;
    Renderer currentRenderer;
    // the scale updateRenderer actually applied (1 for software): the ONLY source for
    // the presentation size, so the blit can never disagree with what was rendered
    int currentScale = 1;
    bool isRenderConfigurationDirty;
    int frame;
    // FBOs used to blit the accelerated renderer's array-texture output into
    // the app's stacked 2D frame texture (created lazily, 0 = uninitialised).
    melonDS::u32 blitReadFBO = 0;
    melonDS::u32 blitDrawFBO = 0;
};

}

#endif
