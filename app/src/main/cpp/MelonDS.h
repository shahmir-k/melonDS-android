#ifndef MELONDS_MELONDS_H
#define MELONDS_MELONDS_H

#include <functional>
#include <future>
#include <list>
#include <array>
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
namespace melonDS { class AmbientLAN; struct NetplayRom; struct NetplayLiveCheckpoint; class NetplayLiveLink; }

namespace MelonDSAndroid {
    struct PreparedLiveReplica;
    class LiveCheckpointTransfer;
    struct LiveRom;
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
    // the running game's code ("" = none: firmware boot)
    extern std::string romGameCode();
    // The currently loaded NDS file/SAF URI, for exact Just Play ROM identity checks.
    // Read on the emulator thread; empty after firmware boot or a failed ROM load.
    extern std::string currentRomPath();
    extern melonDS::NetplayRom currentRomDescription();
    extern bool netplayLiveSourceIdle();
    extern bool netplayLiveSourceIsMPClient();
    // live (handed-off) Netplay's end: the frontend's way back to discovery, called on the emu thread
    extern void netplaySetLiveExitHandler(std::function<void()> handler);
    extern bool netplayLocalWifiOn();
    extern void netplayRequestLiveLeave();
    extern void netplayFreezeLiveInput();
    extern void netplayResumeLiveInput();
    extern bool netplayCaptureLive(melonDS::AmbientLAN& ambient,
                                   const melonDS::NetplayRom& rom, int player,
                                   melonDS::NetplayLiveCheckpoint& checkpoint,
                                   std::string& error);
    extern bool netplayBeginLiveTransfer(LiveCheckpointTransfer& transfer,
                                         melonDS::NetplayLiveCheckpoint local,
                                         const melonDS::NetplayRom& peerRom,
                                         std::shared_future<LiveRom> peerRomData,
                                         melonDS::NetplayLiveLink& link,
                                         std::string& error, int peerPlayer);
    // this console's state now (warm, no cache reset): the baseline of the cut's checkpoint delta
    extern bool netplayCaptureBaseline(std::vector<melonDS::u8>& state);
    // Runs two reversible Lockstep frames on disposable copies of both saved
    // consoles, then reloads the peer candidate from its original checkpoint.
    // The source Ambient console never advances. Hashes are ordered by frame,
    // then player and must match the remote device before commit.
    extern void netplayPrepareOwnProofCopy(const melonDS::NetplayLiveCheckpoint& local);
    extern void netplayPrepareOwnProofShell(int player);
    extern void netplayPreparePeerShell(int player, const LiveRom& rom);
    // Every other member of the live handoff: its checkpoint, its exact prepared replica, its IPv4.
    struct LivePeer
    {
        std::shared_ptr<melonDS::NetplayLiveCheckpoint> Checkpoint;
        std::unique_ptr<PreparedLiveReplica> Replica;
        std::string IPv4;
    };
    // The proof on N consoles (this one's copy and every peer replica): hashes[frame * N + player].
    extern bool netplayProveScratchSet(const melonDS::NetplayLiveCheckpoint& local,
                                       std::vector<LivePeer>& peers,
                                       std::vector<u64>& hashes,
                                       std::string& error);
    // Stage the exact peer replicas, input socket and wireless backend while the
    // original Ambient console remains frozen. This does not install the new
    // backend or run a Netplay frame. Only the emulator/GL thread may call it.
    // Ports: Netplay input UDP kLiveInputPort + player, frame-zero proof kLiveStartPort + player.
    constexpr int kLiveInputPort = 7100, kLiveStartPort = 7150, kLiveLinkPort = 7200;
    extern bool netplayStageLive(const melonDS::NetplayLiveCheckpoint& local,
                                 std::vector<LivePeer> peers,
                                 bool hosted, std::string& error);
    // Netplay quits a live session (this player's inputs carry the leave from the next one on)
    extern void netplayQuitLive();
    // Both calls require the frozen source and run on the emulator thread.
    // prove exchanges frame-zero input but advances neither console. Commit
    // installs the staged backend and starts the already-prepared peer console.
    extern bool netplayProveStagedLive(std::string& error);
    extern bool netplayCommitStagedLive(std::string& error);
    extern void netplayDiscardStagedLive();
    // Just Play (LITEV_AMBIENT_LAN): the console MAC from the device id, false = off
    extern bool ambientFirmwareMac(u8* mac);
    // The next game started runs in Netplay as `player` (the LAN lobby id; 0 = host) of `players`;
    // guests pass the host's IP as `host` (the host passes "").
    // library: where another player's game may be; cacheDir: where games received from the others
    // go (cacheMaxBytes: its size cap, least recently used first)
    extern void netplayPrepare(int player, int players, std::string host, bool hosted, std::vector<std::string> library,
                               std::string cacheDir, u64 cacheMaxBytes);
    // "" when not in Netplay, else "Netplay", "waiting for other player" or "DESYNC"
    extern std::string netplayStatus();
    extern std::string netplaySpeedLabel();  // the session speed: "" normal, else "FF 2x by P2" / "uncapped by P1"
    extern int netplayKind();   // 0 none, 1 Netplay, 2 Hosted Netplay
#ifdef LITEV_NP_SPEED
    // Netplay session speed (plain and Hosted): ONE state; netplayToggleSpeed = a press of the
    // fast-forward key (toggles normal <-> `multiplier`, <= 0: uncapped; returns what the key shows,
    // 1 fast / 0 normal, -1 = not in Netplay); netplaySpeedShown = what the key shows now (same codes);
    // netplaySpeed = the session's speed now (0 = uncapped); netplaySkipAllowed = frameskip may run
    // (not in Netplay, or a plain session is fast-forwarding)
    extern int netplayToggleSpeed(float multiplier);
    extern int netplaySpeedShown();
    extern bool netplaySpeed(float& multiplier);
    extern bool netplaySkipAllowed();
#endif
    // Ends every Netplay wait (stopping: the emulator thread may be waiting on a player who left)
    extern void netplayAbort();
    // Record mode (single player): "record DIR", "replay DIR" or "stop", run at the next frame
    extern void recordQueue(std::string request);
    extern int recordMode();            // 0 none, 1 recording, 2 replaying
    // host battery shown to the guest in single player (percent < 0: always full); see MelonInstance::batteryTarget
    extern void setHostBattery(int percent, bool charging);
    extern int recordMark();            // flag this moment (L2); the frame, or -1 when not recording
    extern bool recordRtc(int* out);    // the recording's clock (6 ints), while one is active
    extern bool recordVideoTarget(const Frame* frame, std::string& dir, std::string& prefix, int& recFrame);
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
#ifdef LITEV_FF_HEADLESS3D
    extern void setFastForwardHeadless(bool on);
#endif
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
