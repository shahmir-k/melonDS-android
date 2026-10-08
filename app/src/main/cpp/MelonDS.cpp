#include <ctime>
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
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <poll.h>
#include <netinet/tcp.h>
#include <cmath>
#include <algorithm>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <map>
#include <mutex>
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

    // Netplay (N players, 2..16): this device runs every player's console. The local console
    // (id = local player) runs on the emulator thread as usual and is shown; every other player's
    // console runs headless on its own thread. Each console takes its player's input Delay frames
    // after it was sampled (NetplayInput), and they talk over the deterministic in-process
    // LockstepMP, so every device computes exactly the same thing.
    // Started from the multiplayer lobby (netplayPrepare, then the game restarts), or for testing
    // from the debug.litev.netplay prop ("player=P[,players=N],peer=HOSTIP:PORT[,port=N,delay=D]";
    // the host, player 0, needs no peer).
    struct NetplayRemote
    {
        int player = 0;
        std::shared_ptr<MelonInstance> console;
        std::vector<u32> screenshot = std::vector<u32>(256 * 384);
        std::map<int, NetplayFrameInput> script;    // debug.litev.npscript2: replaces the network
        std::thread thread;
        std::map<int, u64> hashes;                  // desync check: our copy, by frame (hashLock)
    };
    struct NetplaySession
    {
        int player = 0, players = 2, delay = 3, port = 7100;
        std::string peer;                           // the host's IP:port (exchange=0: the other player's)
        bool exchange = true;                       // session setup with the host (exchange=0: testing, 2 players)
        bool autoDelay = true;                      // input delay from the measured round trips
        // Test/replay input, by applied frame: debug.litev.npscript (local player)
        std::map<int, NetplayFrameInput> script;
        // Every session records every player's applied inputs (netplay/rec_ID_pN.txt) in the same
        // format, so a failure can be replayed on one device: frame-exact, no network.
        FILE* rec[NetplayInput::kMaxPlayers] {};
        long recId = (long)time(nullptr);           // one recording per session
        NetplayFrameInput recLast[NetplayInput::kMaxPlayers];
        std::unique_ptr<NetplayInput> input;
        std::vector<std::unique_ptr<NetplayRemote>> remotes;   // every other player's console
        std::atomic<bool> running {false};
        int frame = 0;
        std::mutex hashLock;
        std::atomic<int> desyncFrame {-1};
        bool waiting = false;
    };
    std::unique_ptr<NetplaySession> netplay;

    bool netplayActive() { return netplay != nullptr; }

    static std::string netplayPending; // set by the lobby, used by the next setup()

    void netplayPrepare(int player, int players, std::string host)
    {
        netplayPending = "player=" + std::to_string(player) + ",players=" + std::to_string(players);
        if (!host.empty()) netplayPending += ",peer=" + host + ":7100";
    }

    static std::unique_ptr<NetplaySession> netplayFromProp()
    {
        std::string spec = std::move(netplayPending);
        netplayPending.clear();
        char prop[PROP_VALUE_MAX] = {0};
        if (spec.empty() && __system_property_get("debug.litev.netplay", prop) > 0)
            spec = prop;
        if (spec.empty())
            return nullptr;
        auto session = std::make_unique<NetplaySession>();
        for (size_t pos = 0; pos <= spec.size();)
        {
            size_t end = spec.find(',', pos);
            if (end == std::string::npos) end = spec.size();
            std::string kv = spec.substr(pos, end - pos);
            size_t eq = kv.find('=');
            if (eq != std::string::npos)
            {
                std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
                if (k == "player") session->player = std::clamp(atoi(v.c_str()), 0, NetplayInput::kMaxPlayers - 1);
                else if (k == "players") session->players = std::clamp(atoi(v.c_str()), 2, NetplayInput::kMaxPlayers);
                else if (k == "delay") { session->delay = atoi(v.c_str()); session->autoDelay = false; }
                else if (k == "port") session->port = atoi(v.c_str());
                else if (k == "peer") session->peer = v;
                else if (k == "exchange") session->exchange = atoi(v.c_str()) != 0;
            }
            pos = end + 1;
        }
        // a guest needs the host's address; without session setup (testing) only two players,
        // and the peer is the other one
        if ((session->peer.empty() && (session->player != 0 || !session->exchange)) ||
            (!session->exchange && (session->players != 2 || session->player > 1)))
            return nullptr;
        return session;
    }

    // Test input: the harness script format, "FRAME KEY[,KEY...]" / "FRAME NONE" / "FRAME T:x:y"
    // per line, each held until the next line.
    // Also reads the recording format: "FRAME 0xKEYS TX TY".
    // `prop` holds the path; a "%d" in it becomes `player` (debug.litev.npscript2: one script
    // per remote player, or the same one for all without %d).
    static void netplayLoadScript(const char* prop, std::map<int, NetplayFrameInput>& script, int player = 0)
    {
        char spec[PROP_VALUE_MAX] = {0};
        if (__system_property_get(prop, spec) <= 0 || !spec[0]) return;
        std::string path = spec;
        if (size_t at = path.find("%d"); at != std::string::npos) path.replace(at, 2, std::to_string(player));
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return;
        static const char* names[12] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            int frame;
            char rest[200] = {0};
            if (sscanf(line, "%d %199s", &frame, rest) != 2) continue;
            NetplayFrameInput in;
            int x, y;
            unsigned keys;
            if (sscanf(line, "%*d 0x%x %d %d", &keys, &x, &y) == 3) { in.Keys = keys; in.TouchX = x; in.TouchY = y; }
            else if (sscanf(rest, "T:%d:%d", &x, &y) == 2) { in.TouchX = x; in.TouchY = y; }
            else for (char* k = strtok(rest, ","); k; k = strtok(nullptr, ","))
                for (int b = 0; b < 12; b++)
                    if (!strcmp(k, names[b])) in.Keys &= ~(1u << b);
            script[frame] = in;
        }
        fclose(f);
        Platform::Log(Platform::LogLevel::Info, "Netplay: scripted input, %zu lines from %s\n", script.size(), path.c_str());
    }

    static NetplayFrameInput netplayScriptAt(const std::map<int, NetplayFrameInput>& script, int frame)
    {
        auto it = script.upper_bound(frame);
        return it == script.begin() ? NetplayFrameInput {} : std::prev(it)->second;
    }

    static void netplayRecord(NetplaySession& s, int player, int frame, const NetplayFrameInput& in)
    {
        NetplayFrameInput& last = s.recLast[player];
        if (frame > 0 && in.Keys == last.Keys && in.TouchX == last.TouchX && in.TouchY == last.TouchY) return;
        last = in;
        if (!s.rec[player])
        {
            std::string dir = internalFilesDir + "/netplay";
            mkdir(dir.c_str(), 0700);
            s.rec[player] = fopen((dir + "/rec_" + std::to_string(s.recId) + "_p" + std::to_string(player) + ".txt").c_str(), "w");
            if (!s.rec[player]) return;
        }
        fprintf(s.rec[player], "%d 0x%03x %d %d\n", frame, in.Keys, in.TouchX, in.TouchY);
        fflush(s.rec[player]);
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

    // Scratch save for another player's console (it starts from their save, sent at session start).
    // Our own console uses our real save, so progress made in Netplay persists as usual.
    static std::string netplaySavePath(int player, const std::vector<u8>& data)
    {
        std::string dir = internalFilesDir + "/netplay";
        mkdir(dir.c_str(), 0700);
        std::string path = dir + "/p" + std::to_string(player) + ".sav";
        if (FILE* f = fopen(path.c_str(), "wb"))
        {
            if (!data.empty()) fwrite(data.data(), 1, data.size(), f);
            fclose(f);
        }
        return path;
    }

    static std::vector<u8> netplayReadFile(const std::string& path, size_t maxLen = SIZE_MAX)
    {
        std::vector<u8> data;
        if (Platform::FileHandle* f = Platform::OpenFile(path, Platform::FileMode::Read))
        {
            data.resize(std::min<u64>(Platform::FileLength(f), maxLen));
            Platform::FileRewind(f);
            if (!data.empty() && Platform::FileRead(data.data(), data.size(), 1, f) != 1) data.clear();
            Platform::CloseFile(f);
        }
        return data;
    }

    // Desync check, every 60 frames: each device sends the hash of its own console to everyone;
    // the others compare it with their copy of that console. Also logged (NETPLAY_HASH) for
    // offline diffs.
    static void netplayHash(MelonInstance& console, int player, int frames)
    {
        if (frames % 60) return;
        NDS* nds = console.getNds();
        u64 hash = XXH3_64bits(nds->MainRAM, nds->MainRAMMask + 1) ^ nds->GetSysTimestamp();
        Platform::Log(Platform::LogLevel::Info, "NETPLAY_HASH p%d f%d sys=%llu ram=%016llx\n", player, frames,
                      (unsigned long long)nds->GetSysTimestamp(),
                      (unsigned long long)XXH3_64bits(nds->MainRAM, nds->MainRAMMask + 1));
        NetplaySession& s = *netplay;
        if (player == s.player)
        {
            s.input->SendHash(frames, hash);
            return;
        }
        for (auto& r : s.remotes)
            if (r->player == player)
            {
                std::lock_guard<std::mutex> lk(s.hashLock);
                r->hashes[frames] = hash;
                while (r->hashes.size() > 64) r->hashes.erase(r->hashes.begin());
            }
    }

    static void netplayCheck(NetplaySession& s)
    {
        std::lock_guard<std::mutex> lk(s.hashLock);
        for (auto& r : s.remotes)
            for (auto& [f, mine] : r->hashes)
            {
                u64 theirs;
                if (s.desyncFrame < 0 && s.input->PeerHash(r->player, f, theirs) && theirs != mine)
                {
                    s.desyncFrame = f;
                    Platform::Log(Platform::LogLevel::Error, "Netplay: DESYNC at frame %d (player %d's console)\n", f, r->player);
                }
            }
        s.waiting = s.input->MsSincePeer() > 500;   // a peer went quiet (each re-sends every 10 ms)
    }

    std::string netplayStatus()
    {
        if (!netplay) return "";
        if (netplay->desyncFrame >= 0) return "DESYNC";
        return netplay->waiting ? "waiting for other player" : "Netplay";
    }

    static void netplayRemoteLoop(NetplayRemote* r)
    {
        // Off the emulator's core, above the render threads (nice -10): the local console waits
        // for this one every frame, so it must not queue behind rendering.
#ifdef LITEV_TOPO_PIN
        LitevTopo::PinSelf(LitevTopo::CoreRole::RemoteConsole);
#else
        const auto& cores = LitevCores::Get();
        sched_setaffinity(0, sizeof(cores.OtherSet), &cores.OtherSet);
#endif
        int nice = -16;
        while (setpriority(PRIO_PROCESS, 0, nice) != 0 && nice < 0) nice++;
        Platform::Log(Platform::LogLevel::Info, "Netplay: player %d's console thread at nice %d\n", r->player, nice);
        pthread_setname_np(pthread_self(), "NetplayRemote");

        NetplaySession& s = *netplay;
        for (int f = 0; s.running; f++)
        {
            NetplayFrameInput in = r->script.empty() ? s.input->Get(r->player, f) : netplayScriptAt(r->script, f);
            if (!s.running) break;
            netplayRecord(s, r->player, f, in);
            netplayApply(*r->console, in);
            r->console->runFrameHeadless();
            netplayHash(*r->console, r->player, f + 1);
        }
    }

    void netplayAbort()
    {
        if (!netplay)
            return;
        netplay->running = false;
        ((LockstepMP&) MPInterface::Get()).Stop();
        if (netplay->input)
            netplay->input->Abort();
    }

    static void netplayStop()
    {
        if (!netplay)
            return;
        netplay->running = false;
        ((LockstepMP&) MPInterface::Get()).Stop();
        netplay->input = nullptr;   // wakes a Get() waiting for the peer
        for (auto& r : netplay->remotes)
        {
            if (r->thread.joinable())
                r->thread.join();
            r->console->stop();
        }
        for (FILE* f : netplay->rec) if (f) fclose(f);
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
            netplayLoadScript("debug.litev.npscript", netplay->script);
            MPInterface::Set(MPInterface_Netplay);
            Platform::Log(Platform::LogLevel::Info, "Netplay: player %d of %d, host %s, port %d\n",
                          netplay->player, netplay->players, netplay->peer.c_str(), netplay->port);
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
            // the other players' consoles are created by loadRom, once the session setup has
            // said who they are
            instance->setInputDeferred(true);
            NDS* local = instance->getNds();
            ((LockstepMP&) MPInterface::Get()).SetClock(netplay->player, [local] { return local->GetSysTimestamp(); });
        }
    }

    // Another player's console, booting `romPath` from that player's save.
    static bool netplayAddRemote(int player, const std::string& romPath, const std::vector<u8>& save)
    {
        auto r = std::make_unique<NetplayRemote>();
        r->player = player;
        auto args = BuildArgsFromConfiguration(*currentConfiguration, player);
        if (!args.has_value())
            return false;
        r->console = std::make_shared<MelonInstance>(
            player,
            currentConfiguration,
            std::move(args.value()),
            net,
            std::make_unique<ScreenshotRenderer>(r->screenshot.data()),
            0
        );
        NDS* nds = r->console->getNds();
        nds->GPU.Headless = true;   // its screens are not shown
        nds->SPU.Silent = true;     // nor its sound heard
        nds->GPU.GPU3D.Headless = true;
        ((LockstepMP&) MPInterface::Get()).SetClock(player, [nds] { return nds->GetSysTimestamp(); });
        netplayLoadScript("debug.litev.npscript2", r->script, player);
        bool ok = r->console->loadRom(romPath, netplaySavePath(player, save));
        netplay->remotes.push_back(std::move(r));   // even on failure: netplayStop stops it
        return ok;
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
            std::vector<u8> head = netplayReadFile(romPath, 0x1000);
            u64 romId = XXH3_64bits(head.data(), head.size());
            if (head.empty())
                return 2;
            std::vector<std::pair<int, std::string>> peers {{1 - netplay->player, netplay->peer}};
            std::map<int, std::vector<u8>> saves;
            if (netplay->exchange)
            {
                NetplaySetup setup;
                setup.Player = netplay->player;
                setup.NumPlayers = netplay->players;
                setup.Port = netplay->port;
                setup.Host = netplay->peer;
                setup.RomId = romId;
                setup.Save = netplayReadFile(sramPath);
                setup.Delay = netplay->autoDelay ? 0 : netplay->delay;
                bool ok = NetplayHandshake(setup);
                for (size_t pos = 0, end; (end = setup.Log.find('\n', pos)) != std::string::npos; pos = end + 1)
                    Platform::Log(ok ? Platform::LogLevel::Info : Platform::LogLevel::Error, "%s\n", setup.Log.substr(pos, end - pos).c_str());
                if (!ok)
                    return 2;
                netplay->delay = setup.Delay;
                netplay->players = (int)setup.Peers.size() + 1;
                peers = std::move(setup.Peers);
                saves = std::move(setup.Saves);
            }
            // created only now: every device has agreed on the players and the input delay
            netplay->input = std::make_unique<NetplayInput>(netplay->player, netplay->delay, netplay->port, peers);
            Platform::Log(Platform::LogLevel::Info, "Netplay: %d players, input delay %d frames%s\n", netplay->players,
                          netplay->delay, netplay->input->Ok() ? "" : " (SOCKET FAILED)");
            for (auto& [player, addr] : peers)
                if (!netplayAddRemote(player, romPath, saves[player]))
                    return 2;
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
            netplay->running = true;
            for (auto& r : netplay->remotes)
            {
                r->console->start();
                r->thread = std::thread(netplayRemoteLoop, r.get());
            }
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
            if (!netplay->script.empty())   // by applied frame: what is submitted now applies Delay frames later
                local = netplayScriptAt(netplay->script, netplay->frame + netplay->delay);
            netplay->input->SubmitLocal(netplay->frame, local);
            NetplayFrameInput applied = netplay->input->Get(netplay->player, netplay->frame);
            netplayRecord(*netplay, netplay->player, netplay->frame, applied);
            netplayApply(*instance, applied);
            netplay->frame++;
            if (netplay->frame % 60 == 0)
            {   // TEMP diagnostic: debug.litev.npnodraw=1 stops the local console drawing (A/B mid-race)
                char p[PROP_VALUE_MAX] = {0};
                int v = __system_property_get("debug.litev.npnodraw", p) > 0 ? atoi(p) : 0;
                instance->getNds()->GPU.Headless = v == 1;
                instance->getNds()->GPU.DiagNoDraw = v == 2 ? 1 : v == 3 ? 2 : 0;   // 2: no 2D, 3: no 3D
            }
            u32 lines = instance->runFrame();
            netplayHash(*instance, netplay->player, netplay->frame);
            if (netplay->frame % 60 == 0) netplayCheck(*netplay);
            return lines;
        }
        return instance->runFrame();
    }

    Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline, bool* isNew)
    {
        if (!instance)
            return nullptr;

        return instance->getPresentationFrame(deadline, isNew);
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

