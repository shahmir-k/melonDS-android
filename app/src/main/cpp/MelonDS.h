#ifndef MELONDS_MELONDS_H
#define MELONDS_MELONDS_H

#include <list>
#include <vector>
#include "AndroidFileHandler.h"
#include "AndroidCameraHandler.h"
#include "Configuration.h"
#include "MelonEventMessenger.h"
#include "RewindManager.h"
#include "RomGbaSlotConfig.h"
#include "retroachievements/RAAchievement.h"
#include "retroachievements/RALeaderboard.h"
#include "renderer/FrameQueue.h"
#include "types.h"
#include "../GPU.h"
#include <android/asset_manager.h>

using namespace melonDS;

namespace MelonDSAndroid {
    typedef struct {
        std::vector<u32> code;
    } Cheat;

    typedef enum {
        ROM,
        FIRMWARE
    } RunMode;

    extern OpenGLContext *openGlContext;
    extern AndroidFileHandler* fileHandler;
    extern AndroidCameraHandler* cameraHandler;
    extern std::string internalFilesDir;
    extern std::shared_ptr<MelonEventMessenger> eventMessenger;

    // Netplay: this device also emulates the other players' consoles; only inputs cross the network.
    extern bool netplayActive();
    // The next game started runs in Netplay as `player` (the LAN lobby id; 0 = host) of `players`;
    // guests pass the host's IP as `host` (the host passes "").
    // library: where another player's game may be; cacheDir: where games received from the others
    // go (cacheMaxBytes: its size cap, least recently used first)
    extern void netplayPrepare(int player, int players, std::string host, bool hosted, std::vector<std::string> library,
                               std::string cacheDir, u64 cacheMaxBytes);
    // "" when not in Netplay, else "Netplay", "waiting for other player" or "DESYNC"
    extern std::string netplayStatus();
    extern int netplayKind();   // 0 none, 1 Netplay, 2 Hosted Netplay
    // Ends every Netplay wait (stopping: the emulator thread may be waiting on a player who left)
    extern void netplayAbort();
    // Record mode (single player): "record DIR", "replay DIR" or "stop", run at the next frame
    extern void recordQueue(std::string request);
    extern int recordMode();            // 0 none, 1 recording, 2 replaying
    extern int recordMark();            // flag this moment (L2); the frame, or -1 when not recording
    extern bool recordRtc(int* out);    // the recording's clock (6 ints), while one is active
    extern bool recordVideoTarget(const Frame* frame, std::string& dir, int& recFrame);
    extern void setConfiguration(EmulatorConfiguration emulatorConfiguration);
    extern void setup(AndroidCameraHandler* androidCameraHandler, std::shared_ptr<MelonEventMessenger> androidEventMessenger, u32* screenshotBufferPointer, int instanceId);
    extern void setCodeList(std::list<Cheat> cheats);
    extern void setupAchievements(std::list<RetroAchievements::RAAchievement> achievements, std::list<RetroAchievements::RALeaderboard> leaderboards, std::optional<std::string> richPresenceScript);
    extern void unloadRetroAchievementsData();
    extern std::string getRichPresenceStatus();
    extern std::vector<RetroAchievements::RARuntimeAchievement> getRuntimeAchievements();
    extern void updateEmulatorConfiguration(std::unique_ptr<EmulatorConfiguration> emulatorConfiguration);
#ifdef LITEV_AGGRESSIVE_SKIP
    extern void setFrameskip(int target);
#endif
#ifdef LITEV_SKIP_REPEAT_FRAMES
    // Skip repeated frames "Auto": the emu loop's controller sets whether it is on (emu thread)
    extern void setSkipRepeatAuto(bool on);
#endif

    /**
     * Loads the NDS ROM and, optionally, the GBA ROM.
     *
     * @param romPath The path to the NDS rom
     * @param sramPath The path to the rom's SRAM file
     * @param gbaSlotConfig The config to be used for the GBA slot
     * @return The load result. 0 if everything was loaded successfully, 1 if the NDS ROM was loaded but the GBA ROM
     * failed to load, 2 if the NDS ROM failed to load
     */
    extern int loadRom(std::string romPath, std::string sramPath, RomGbaSlotConfig* gbaSlotConfig);
    extern int bootFirmware();
    extern void touchScreen(u16 x, u16 y);
    extern void releaseScreen();
    extern void pressKey(u32 key);
    extern void releaseKey(u32 key);
    extern void start();
    extern u32 loop();
    extern Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline, bool* isNew = nullptr);
    extern void pause();
    extern void resume();
    extern void reset();
    extern bool saveState(const char* path);
    extern bool loadState(const char* path);
    extern bool loadRewindState(melonDS::RewindSaveState rewindSaveState);
    extern RewindWindow getRewindWindow();
    extern bool takeScreenshot();
    extern void stop();
    extern void cleanup();
}

#endif //MELONDS_MELONDS_H
