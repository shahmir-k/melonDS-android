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

    // Netplay (two players): this device runs both consoles. The local console (id = local
    // player) runs on the emulator thread as usual and is shown; the other player's console runs
    // headless on its own thread. Each console takes its player's input Delay frames after it was
    // sampled (NetplayInput), and the two talk over the deterministic in-process LockstepMP, so
    // both devices compute exactly the same thing.
    // Started from the multiplayer lobby (netplayPrepare, then the game restarts), or for testing
    // from the debug.litev.netplay prop ("player=0,peer=IP:PORT[,port=N,delay=D]").
    struct NetplaySession
    {
        int player = 0, delay = 3, port = 7100;
        std::string peer;
        bool exchange = true;                       // swap saves at start (exchange=0: testing)
        bool autoDelay = true;                      // input delay from the measured round trip
        // Test/replay input, by applied frame: debug.litev.npscript (local player),
        // debug.litev.npscript2 (other player: replaces the network)
        std::map<int, NetplayFrameInput> script, script2;
        // Every session records both players' applied inputs (netplay/rec_pN.txt) in the same
        // format, so a failure can be replayed on one device: frame-exact, no network.
        FILE* rec[2] {};
        long recId = (long)time(nullptr);           // one recording per session
        NetplayFrameInput recLast[2];
        std::unique_ptr<NetplayInput> input;
        std::shared_ptr<MelonInstance> remote;
        std::vector<u32> remoteScreenshot = std::vector<u32>(256 * 384);
        std::thread thread;
        std::atomic<bool> running {false};
        int frame = 0;
        // desync check: our copy of the other console, by frame (written by its thread)
        std::mutex hashLock;
        std::map<int, u64> remoteHashes;
        std::atomic<int> desyncFrame {-1};
        bool waiting = false;
    };
    std::unique_ptr<NetplaySession> netplay;

    bool netplayActive() { return netplay != nullptr; }

    static std::string netplayPending; // set by the lobby, used by the next setup()

    void netplayPrepare(int player, std::string peer)
    {
        netplayPending = "player=" + std::to_string(player) + ",peer=" + peer + ":7100";
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
                if (k == "player") session->player = atoi(v.c_str()) & 1;
                else if (k == "delay") { session->delay = atoi(v.c_str()); session->autoDelay = false; }
                else if (k == "port") session->port = atoi(v.c_str());
                else if (k == "peer") session->peer = v;
                else if (k == "exchange") session->exchange = atoi(v.c_str()) != 0;
            }
            pos = end + 1;
        }
        if (session->peer.empty())
            return nullptr;
        return session;
    }

    // Test input: the harness script format, "FRAME KEY[,KEY...]" / "FRAME NONE" / "FRAME T:x:y"
    // per line, each held until the next line.
    // Also reads the recording format: "FRAME 0xKEYS TX TY".
    static void netplayLoadScript(const char* prop, std::map<int, NetplayFrameInput>& script)
    {
        char path[PROP_VALUE_MAX] = {0};
        if (__system_property_get(prop, path) <= 0 || !path[0]) return;
        FILE* f = fopen(path, "r");
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
        Platform::Log(Platform::LogLevel::Info, "Netplay: scripted input, %zu lines from %s\n", script.size(), path);
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

    // Scratch save for the other player's console (it starts from their save, sent at session start).
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

    static bool sendAll(int fd, const void* p, size_t n)
    {
        for (const u8* b = (const u8*)p; n; )
        {
            ssize_t k = send(fd, b, n, MSG_NOSIGNAL);
            if (k <= 0) return false;
            b += k; n -= k;
        }
        return true;
    }

    static bool recvAll(int fd, void* p, size_t n)
    {
        for (u8* b = (u8*)p; n; )
        {
            ssize_t k = recv(fd, b, n, 0);
            if (k <= 0) return false;
            b += k; n -= k;
        }
        return true;
    }

    // Session start: both devices check they run the same game and swap saves, so every console
    // boots from its owner's save. TCP on the Netplay port + 1; player 0 listens, player 1
    // connects (whoever starts first waits up to 60 s for the other).
    static bool netplayExchange(NetplaySession& s, u64 romId, const std::vector<u8>& mine, std::vector<u8>& theirs)
    {
        std::string ip = s.peer.substr(0, s.peer.rfind(':'));
        int fd = -1;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        if (s.player == 0)
        {
            int ls = socket(AF_INET, SOCK_STREAM, 0);
            int one = 1;
            setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            sockaddr_in a {};
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_ANY);
            a.sin_port = htons(s.port + 1);
            if (bind(ls, (sockaddr*)&a, sizeof(a)) == 0 && listen(ls, 1) == 0)
            {
                pollfd pf {ls, POLLIN, 0};
                if (poll(&pf, 1, 60000) == 1) fd = accept(ls, nullptr, nullptr);
            }
            close(ls);
        }
        else
        {
            while (fd < 0 && std::chrono::steady_clock::now() < deadline)
            {
                fd = socket(AF_INET, SOCK_STREAM, 0);
                sockaddr_in a {};
                a.sin_family = AF_INET;
                inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
                a.sin_port = htons(s.port + 1);
                if (connect(fd, (sockaddr*)&a, sizeof(a)) != 0)
                {
                    close(fd);
                    fd = -1;
                    usleep(500000);
                }
            }
        }
        if (fd < 0)
        {
            Platform::Log(Platform::LogLevel::Error, "Netplay: could not reach the other player\n");
            return false;
        }
        timeval tv {30, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        u64 theirRom = 0;
        u32 len = (u32)mine.size(), theirLen = 0;
        bool ok = sendAll(fd, &romId, 8) && sendAll(fd, &len, 4) && (!len || sendAll(fd, mine.data(), len))
               && recvAll(fd, &theirRom, 8) && recvAll(fd, &theirLen, 4) && theirLen <= (32u << 20);
        if (ok)
        {
            theirs.resize(theirLen);
            ok = !theirLen || recvAll(fd, theirs.data(), theirLen);
        }
        // Input delay: the host times a few round trips and picks the smallest delay that covers
        // one way (90th percentile) plus a frame of margin, then tells the guest, so both use the
        // same one. (A late input only stalls; a too-long delay only adds input lag.)
        constexpr int kPings = 12;
        u8 delay = (u8)s.delay;
        if (ok && s.player == 0)
        {
            std::vector<double> rtt;
            for (int i = 0; ok && i < kPings; i++)
            {
                u8 b = (u8)i;
                auto t0 = std::chrono::steady_clock::now();
                ok = sendAll(fd, &b, 1) && recvAll(fd, &b, 1);
                rtt.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            }
            if (ok && s.autoDelay)
            {
                std::sort(rtt.begin(), rtt.end());
                double p90 = rtt[rtt.size() * 9 / 10];
                delay = (u8)std::clamp((int)std::ceil(p90 / 2 / (1000.0 / 60)) + 1, 1, 8);
                Platform::Log(Platform::LogLevel::Info, "Netplay: round trip median %.1f ms, p90 %.1f ms -> input delay %d frames\n",
                              rtt[rtt.size() / 2], p90, delay);
            }
            ok = ok && sendAll(fd, &delay, 1);
        }
        else if (ok)
        {
            for (int i = 0; ok && i < kPings; i++)
            {
                u8 b;
                ok = recvAll(fd, &b, 1) && sendAll(fd, &b, 1);
            }
            ok = ok && recvAll(fd, &delay, 1) && delay >= 1 && delay <= 8;
        }
        if (ok) s.delay = delay;
        close(fd);
        if (!ok)
            Platform::Log(Platform::LogLevel::Error, "Netplay: save exchange failed\n");
        else if (theirRom != romId)
        {
            Platform::Log(Platform::LogLevel::Error, "Netplay: the other player runs a different game\n");
            ok = false;
        }
        else
            Platform::Log(Platform::LogLevel::Info, "Netplay: saves exchanged (ours %u bytes, theirs %u bytes)\n", len, theirLen);
        return ok;
    }

    // Desync check, every 60 frames: each device sends the hash of its own console; the other
    // compares it with its copy of that console. Also logged (NETPLAY_HASH) for offline diffs.
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
        std::lock_guard<std::mutex> lk(s.hashLock);
        s.remoteHashes[frames] = hash;
        while (s.remoteHashes.size() > 64) s.remoteHashes.erase(s.remoteHashes.begin());
    }

    static void netplayCheck(NetplaySession& s)
    {
        std::lock_guard<std::mutex> lk(s.hashLock);
        for (auto& [f, mine] : s.remoteHashes)
        {
            u64 theirs;
            if (s.desyncFrame < 0 && s.input->PeerHash(f, theirs) && theirs != mine)
            {
                s.desyncFrame = f;
                Platform::Log(Platform::LogLevel::Error, "Netplay: DESYNC at frame %d\n", f);
            }
        }
        s.waiting = s.input->MsSincePeer() > 500;   // the peer went quiet (it re-sends every 10 ms)
    }

    std::string netplayStatus()
    {
        if (!netplay) return "";
        if (netplay->desyncFrame >= 0) return "DESYNC";
        return netplay->waiting ? "waiting for other player" : "Netplay";
    }

    static void netplayRemoteLoop()
    {
        // Off the emulator's core, above the render threads (nice -10): the local console waits
        // for this one every frame, so it must not queue behind rendering.
        const auto& cores = LitevCores::Get();
        sched_setaffinity(0, sizeof(cores.OtherSet), &cores.OtherSet);
        int nice = -16;
        while (setpriority(PRIO_PROCESS, 0, nice) != 0 && nice < 0) nice++;
        Platform::Log(Platform::LogLevel::Info, "Netplay: remote console thread at nice %d\n", nice);
        pthread_setname_np(pthread_self(), "NetplayRemote");

        NetplaySession& s = *netplay;
        int other = 1 - s.player;
        for (int f = 0; s.running; f++)
        {
            NetplayFrameInput in = s.script2.empty() ? s.input->Get(other, f) : netplayScriptAt(s.script2, f);
            if (!s.running) break;
            netplayRecord(s, other, f, in);
            netplayApply(*s.remote, in);
            s.remote->runFrameHeadless();
            netplayHash(*s.remote, other, f + 1);
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
        if (netplay->thread.joinable())
            netplay->thread.join();
        if (netplay->remote)
            netplay->remote->stop();
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
            netplayLoadScript("debug.litev.npscript2", netplay->script2);
            MPInterface::Set(MPInterface_Netplay);
            Platform::Log(Platform::LogLevel::Info, "Netplay: player %d, peer %s, port %d\n",
                          netplay->player, netplay->peer.c_str(), netplay->port);
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
            netplay->remote->getNds()->GPU.Headless = true; // its screens are not shown
            netplay->remote->getNds()->SPU.Silent = true;   // nor its sound heard
            netplay->remote->getNds()->GPU.GPU3D.Headless = true;
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
            std::vector<u8> head = netplayReadFile(romPath, 0x1000);
            u64 romId = XXH3_64bits(head.data(), head.size());
            std::vector<u8> mine = netplayReadFile(sramPath), theirs;
            if (head.empty() || (netplay->exchange && !netplayExchange(*netplay, romId, mine, theirs)))
                return 2;
            // created only now: both devices have agreed on the input delay
            netplay->input = std::make_unique<NetplayInput>(netplay->player, netplay->delay, netplay->port, netplay->peer);
            Platform::Log(Platform::LogLevel::Info, "Netplay: input delay %d frames%s\n", netplay->delay,
                          netplay->input->Ok() ? "" : " (SOCKET FAILED)");
            if (!netplay->remote->loadRom(romPath, netplaySavePath(1 - netplay->player, theirs)))
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

