#include <cstring>
#include <utility>
#include <android/asset_manager.h>
#include <oboe/Oboe.h>
#include "EmulatorArgsBuilder.h"
#include "MelonDS.h"
#include "MelonDSAudio.h"
#include "OboeCallback.h"
#include "MicInputOboeCallback.h"
#include "OpenGLContext.h"
#include "mic_blow.h"
#include "NDS.h"
#include "GPU.h"
#include "GPU3D.h"
#include "GBACart.h"
#include "SPU.h"
#include "Platform.h"
#include "Savestate.h"
#include "MelonInstance.h"
#include "RewindManager.h"
#include "ROMManager.h"
#include "MPInterface.h"
#include "net/LockstepMP.h"
#include "net/NetplayInput.h"
#include "LitevCores.h"
#include <sys/system_properties.h>
#include <sys/stat.h>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include "xxhash/xxhash.h"
#include "AndroidCameraHandler.h"
#include "renderer/ScreenshotRenderer.h"
#include "renderer/FrameQueue.h"
#include "retroachievements/RetroAchievementsManager.h"
#include "net/Net.h"
#include "net/Net_Slirp.h"
#include <fstream>

namespace MelonDSAndroid
{
    OpenGLContext *openGlContext;
    AndroidFileHandler* fileHandler;
    AndroidCameraHandler* cameraHandler;
    std::string internalFilesDir;
    std::shared_ptr<MelonEventMessenger> eventMessenger;
    std::shared_ptr<EmulatorConfiguration> currentConfiguration;
    std::shared_ptr<Net> net;

    std::shared_ptr<MelonInstance> instance;

    // Netplay (two players): this device runs both consoles. The local console (id = local
    // player) runs on the emulator thread as usual and is shown; the other player's console runs
    // headless on its own thread. Each console takes its player's input Delay frames after it was
    // sampled (NetplayInput), and the two talk over the deterministic in-process LockstepMP, so
    // both devices compute exactly the same thing.
    // ponytail: started from the debug.litev.netplay prop ("player=0,peer=IP:PORT[,port=N,delay=D]")
    // until the lobby UI exists.
    struct NetplaySession
    {
        int player = 0, delay = 3, port = 7100;
        std::string peer;
        std::unique_ptr<NetplayInput> input;
        std::shared_ptr<MelonInstance> remote;
        std::vector<u32> remoteScreenshot = std::vector<u32>(256 * 384);
        std::thread thread;
        std::atomic<bool> running {false};
        int frame = 0;
    };
    std::unique_ptr<NetplaySession> netplay;

    bool netplayActive() { return netplay != nullptr; }

    static std::unique_ptr<NetplaySession> netplayFromProp()
    {
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.litev.netplay", prop) <= 0)
            return nullptr;
        auto session = std::make_unique<NetplaySession>();
        std::string spec = prop;
        for (size_t pos = 0; pos <= spec.size();)
        {
            size_t end = spec.find(',', pos);
            if (end == std::string::npos) end = spec.size();
            std::string kv = spec.substr(pos, end - pos);
            size_t eq = kv.find('=');
            if (eq != std::string::npos)
            {
                std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                if (k == "player") session->player = atoi(v.c_str()) & 1;
                else if (k == "delay") session->delay = atoi(v.c_str());
                else if (k == "port") session->port = atoi(v.c_str());
                else if (k == "peer") session->peer = v;
            }
            pos = end + 1;
        }
        if (session->peer.empty())
            return nullptr;
        return session;
    }

    static void netplayApply(MelonInstance& console, const NetplayFrameInput& in)
    {
        NDS* nds = console.getNds();
        nds->SetKeyMask(in.Keys);
        if (in.TouchX >= 0) nds->TouchScreen(in.TouchX, in.TouchY);
        else                nds->ReleaseScreen();
    }

    // Every device must start every console identically, whatever its own settings.
    static void netplayFixConfiguration(EmulatorConfiguration& c)
    {
        c.userInternalFirmwareAndBios = true;
        c.showBootScreen = false;
        c.useJit = true;
        c.consoleType = 0;
        c.rewindEnabled = 0;
        auto& fw = c.firmwareConfiguration;
        strcpy(fw.username, "SereneDS");
        fw.language = 1;            // English
        fw.birthdayMonth = 1;
        fw.birthdayDay = 1;
        fw.favouriteColour = 0;
        fw.message[0] = 0;
        fw.randomizeMacAddress = false;
        fw.macAddress[0] = 0;       // the generated firmware's MAC, + instance id
    }

    // Fresh, empty save per player: identical on both devices.
    // ponytail: no save exchange yet, so games start from no save.
    static std::string netplaySavePath(int player)
    {
        std::string dir = internalFilesDir + "/netplay";
        mkdir(dir.c_str(), 0700);
        std::string path = dir + "/p" + std::to_string(player) + ".sav";
        if (FILE* f = fopen(path.c_str(), "wb")) fclose(f);
        return path;
    }

    // Desync check: both devices log the same lines for the same console if they agree.
    static void netplayLogHash(MelonInstance& console, int player, int frames)
    {
        if (frames % 60) return;
        NDS* nds = console.getNds();
        Platform::Log(Platform::LogLevel::Info, "NETPLAY_HASH p%d f%d sys=%llu ram=%016llx\n", player, frames,
                      (unsigned long long)nds->GetSysTimestamp(),
                      (unsigned long long)XXH3_64bits(nds->MainRAM, nds->MainRAMMask + 1));
    }

    static void netplayRemoteLoop()
    {
        // off the emulator's core (it competes there with the tile workers)
        const auto& cores = LitevCores::Get();
        if (!cores.Others.empty())
        {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(cores.Others[0], &set);
            sched_setaffinity(0, sizeof(set), &set);
        }
        pthread_setname_np(pthread_self(), "NetplayRemote");

        NetplaySession& s = *netplay;
        int other = 1 - s.player;
        for (int f = 0; s.running; f++)
        {
            NetplayFrameInput in = s.input->Get(other, f);
            if (!s.running) break;
            netplayApply(*s.remote, in);
            s.remote->runFrameHeadless();
            netplayLogHash(*s.remote, other, f + 1);
        }
    }

    static void netplayStop()
    {
        if (!netplay)
            return;
        netplay->running = false;
        ((LockstepMP&) MPInterface::Get()).Stop();
        netplay->input = nullptr;   // wakes a Get() waiting for the peer
        if (netplay->thread.joinable())
            netplay->thread.join();
        if (netplay->remote)
            netplay->remote->stop();
        netplay = nullptr;
        MPInterface::Set(MPInterface_Dummy);
    }

    bool setupOpenGlContext();
    void cleanupOpenGlContext();

    /**
     * Used to set the emulator's initial configuration, before boot. To update the configuration during runtime, use @updateEmulatorConfiguration.
     *
     * @param emulatorConfiguration The emulator configuration during the next emulator run
     */
    void setConfiguration(EmulatorConfiguration emulatorConfiguration) {
        currentConfiguration = std::make_shared<EmulatorConfiguration>(std::move(emulatorConfiguration));
        internalFilesDir = currentConfiguration->internalFilesDir;

        net = std::make_shared<Net>();
        net->SetDriver(std::make_unique<Net_Slirp>([](const u8* data, int len) {
            net->RXEnqueue(data, len);
        }));
    }

    void setup(AndroidCameraHandler* androidCameraHandler, std::shared_ptr<MelonEventMessenger> androidEventMessenger, u32* screenshotBufferPointer, int instanceId)
    {
        cameraHandler = androidCameraHandler;
        eventMessenger = androidEventMessenger;
        RetroAchievements::RetroAchievementsManager::EventMessenger = androidEventMessenger;

        netplay = netplayFromProp();
        if (netplay)
        {
            netplayFixConfiguration(*currentConfiguration);
            instanceId = netplay->player;
            netplay->input = std::make_unique<NetplayInput>(netplay->player, netplay->delay, netplay->port, netplay->peer);
            MPInterface::Set(MPInterface_Netplay);
            Platform::Log(Platform::LogLevel::Info, "Netplay: player %d, peer %s, port %d, delay %d frames%s\n",
                          netplay->player, netplay->peer.c_str(), netplay->port, netplay->delay, netplay->input->Ok() ? "" : " (SOCKET FAILED)");
        }

        auto instanceArgs = BuildArgsFromConfiguration(*currentConfiguration, instanceId);
        if (!instanceArgs.has_value())
        {
            // TODO: Handle this somehow?
            instance = nullptr;
            return;
        }
        instance = std::make_shared<MelonInstance>(
            instanceId,
            currentConfiguration,
            std::move(instanceArgs.value()),
            net,
            std::make_unique<ScreenshotRenderer>(screenshotBufferPointer),
            currentConfiguration->consoleType
        );

        setupAudio(currentConfiguration->audioSettings);
        setAudioActiveInstance(instance);

        if (netplay)
        {
            int other = 1 - netplay->player;
            auto remoteArgs = BuildArgsFromConfiguration(*currentConfiguration, other);
            netplay->remote = std::make_shared<MelonInstance>(
                other,
                currentConfiguration,
                std::move(remoteArgs.value()),
                net,
                std::make_unique<ScreenshotRenderer>(netplay->remoteScreenshot.data()),
                0
            );
            instance->setInputDeferred(true);
            auto& link = (LockstepMP&) MPInterface::Get();
            NDS* local = instance->getNds();
            NDS* remote = netplay->remote->getNds();
            link.SetClock(netplay->player, [local] { return local->GetSysTimestamp(); });
            link.SetClock(other, [remote] { return remote->GetSysTimestamp(); });
        }
    }

    void setCodeList(std::list<Cheat> cheats)
    {
        if (netplay) return; // the other device would not run them
        instance->loadCheats(std::move(cheats));
    }

    void setupAchievements(
        std::list<RetroAchievements::RAAchievement> achievements,
        std::list<RetroAchievements::RALeaderboard> leaderboards,
        std::optional<std::string> richPresenceScript
    )
    {
        instance->setupAchievements(std::move(achievements), std::move(leaderboards), std::move(richPresenceScript));
    }

    void unloadRetroAchievementsData()
    {
        instance->unloadRetroAchievementsData();
    }

    std::string getRichPresenceStatus()
    {
        return instance->getRichPresenceStatus();
    }

    std::vector<RetroAchievements::RARuntimeAchievement> getRuntimeAchievements()
    {
        return instance->getRuntimeAchievements();
    }

    /**
     * Used to update the emulator's configuration during runtime. Will only update the configurations that can actually change during runtime without causing issues,
     *
     * @param emulatorConfiguration The new emulator configuration
     */
    void updateEmulatorConfiguration(std::unique_ptr<EmulatorConfiguration> emulatorConfiguration) {
        std::shared_ptr<EmulatorConfiguration> sharedConfig = std::move(emulatorConfiguration);
        instance->updateConfiguration(sharedConfig);
        updateAudioSettings(sharedConfig->audioSettings);

        currentConfiguration = sharedConfig;
    }

#ifdef LITEV_AGGRESSIVE_SKIP
    void setFrameskip(int target) {
        if (instance)
            instance->setFrameskipTarget(target);
    }
#endif

    int loadRom(std::string romPath, std::string sramPath, RomGbaSlotConfig* gbaSlotConfig)
    {
        if (netplay)
        {
            if (!netplay->remote->loadRom(romPath, netplaySavePath(1 - netplay->player)))
                return 2;
            sramPath = netplaySavePath(netplay->player);
        }
        if (!instance->loadRom(std::move(romPath), std::move(sramPath)))
            return 2;
        if (netplay)
            return 0; // no GBA slot: the other device may not have the same cartridge

        if (gbaSlotConfig->type == GBA_ROM)
        {
            RomGbaSlotConfigGbaRom* gbaRomConfig = (RomGbaSlotConfigGbaRom*) gbaSlotConfig;
            if (!instance->loadGbaRom(gbaRomConfig->romPath, gbaRomConfig->savePath))
                return 1;
        }
        else if (gbaSlotConfig->type == RUMBLE_PAK)
        {
            instance->loadRumblePak();
        }
        else if (gbaSlotConfig->type == MEMORY_EXPANSION)
        {
            instance->loadGbaMemoryExpansion();
        }

        return 0;
    }

    int bootFirmware()
    {
        // TODO: Maybe validate BIOS and firmware?
        if (instance->bootFirmware())
            return ROMManager::SUCCESS;
        else
            return ROMManager::FIRMWARE_NOT_BOOTABLE;
    }

    void touchScreen(u16 x, u16 y)
    {
        if (instance)
            instance->touchScreen(x, y);
    }

    void releaseScreen()
    {
        if (instance)
            instance->releaseScreen();
    }

    void pressKey(u32 key)
    {
        if (instance)
            instance->pressKey(key);
    }

    void releaseKey(u32 key)
    {
        if (instance)
            instance->releaseKey(key);
    }

    void start()
    {
        startAudio();
        setupOpenGlContext();

        instance->start();
        if (netplay)
        {
            netplay->remote->start();
            netplay->running = true;
            netplay->thread = std::thread(netplayRemoteLoop);
        }
    }

    u32 loop()
    {
        MPInterface::Get().Process();
        if (netplay)
        {
            NetplayFrameInput local;
            local.Keys = instance->getInputMask() & 0xFFF;
            u16 x, y;
            if (instance->getTouch(x, y)) { local.TouchX = x; local.TouchY = y; }
            netplay->input->SubmitLocal(netplay->frame, local);
            netplayApply(*instance, netplay->input->Get(netplay->player, netplay->frame));
            netplay->frame++;
            u32 lines = instance->runFrame();
            netplayLogHash(*instance, netplay->player, netplay->frame);
            return lines;
        }
        return instance->runFrame();
    }

    Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline)
    {
        if (!instance)
            return nullptr;

        return instance->getPresentationFrame(deadline);
    }

    void pause()
    {
        pauseAudio();
    }

    void resume()
    {
        startAudio();
    }

    void reset()
    {
        if (netplay) return; // the other device would not reset with us
        instance->reset();
    }

    bool saveState(const char* path)
    {
        Platform::FileHandle* saveStateFile = Platform::OpenFile(path, Platform::FileMode::Write);

        if (!saveStateFile)
            return false;

        Savestate state;
        if (state.Error)
        {
            Platform::CloseFile(saveStateFile);
            return false;
        }

        instance->saveState(&state);

        if (state.Error)
        {
            Platform::CloseFile(saveStateFile);
            return false;
        }

        if (Platform::FileWrite(state.Buffer(), state.Length(), 1, saveStateFile) == 0)
        {
            Platform::Log(Platform::Error, "Failed to write %d-byte savestate to %s\n", state.Length(), path);
            Platform::CloseFile(saveStateFile);
            return false;
        }

        Platform::CloseFile(saveStateFile);
        return true;
    }

    bool loadState(const char* path)
    {
        if (netplay) return false; // ponytail: no state exchange yet; states would desync the devices
        auto saveStateFile = Platform::OpenFile(path, Platform::FileMode::Read);
        if (!saveStateFile)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to open state file \"%s\"\n", path);
            return false;
        }

        std::unique_ptr<Savestate> backup = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
        if (backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to allocate memory for state backup\n");
            Platform::CloseFile(saveStateFile);
            return false;
        }

        if (!instance->saveState(backup.get()) || backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to back up state, aborting load (from \"%s\")\n", path);
            Platform::CloseFile(saveStateFile);
            return false;
        }

        size_t size = Platform::FileLength(saveStateFile);

        // Allocate exactly as much memory as we need for the savestate
        std::vector<u8> buffer(size);
        if (Platform::FileRead(buffer.data(), size, 1, saveStateFile) == 0)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to read %u-byte state file \"%s\"\n", size, path);
            Platform::CloseFile(saveStateFile);
            return false;
        }
        Platform::CloseFile(saveStateFile);

        std::unique_ptr<Savestate> state = std::make_unique<Savestate>(buffer.data(), size, false);

        if (!instance->loadState(state.get()) || state->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to load state file \"%s\" into emulator\n", path);
            // Restore backup
            if (!instance->loadState(backup.get()) || state->Error)
                Platform::Log(Platform::LogLevel::Error, "Failed to load backup state\n", path);
            else
                Platform::Log(Platform::LogLevel::Info, "Backup state loaded\n", path);

            return false;
        }
        return true;
    }

    bool loadRewindState(melonDS::RewindSaveState rewindSaveState)
    {
        if (netplay) return false;
        std::unique_ptr<Savestate> backup = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
        if (backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to allocate memory for state backup");
            return false;
        }

        if (!instance->saveState(backup.get()) || backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to back up state, aborting rewind state load");
            return false;
        }

        bool result = instance->loadRewindState(rewindSaveState);
        if (!result)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to load rewind state");
            // Restore backup
            if (!instance->loadState(backup.get()) || backup->Error)
                Platform::Log(Platform::LogLevel::Error, "Failed to load backup state");
            else
                Platform::Log(Platform::LogLevel::Info, "Backup state loaded");
        }

        return result;
    }

    RewindWindow getRewindWindow()
    {
        return instance->getRewindWindow();
    }

    bool takeScreenshot()
    {
        if (instance)
            return instance->takeScreenshot();

        return false;
    }

    void stop()
    {
        netplayStop();
        instance->stop();
        cleanupOpenGlContext();
    }

    void cleanup()
    {
        cleanupAudio();

        instance = nullptr;
        eventMessenger = nullptr;
    }

    bool setupOpenGlContext()
    {
        if (openGlContext == nullptr)
            return false;

        if (!openGlContext->Use())
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to use OpenGL context");
            return false;
        }

        return true;
    }

    void cleanupOpenGlContext()
    {
        if (openGlContext == nullptr)
            return;

        openGlContext->Release();
    }
}

