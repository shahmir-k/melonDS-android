#include <chrono>
#include <cstdint>
#include <ctime>
#include <cstring>
#include <utility>
#include <android/asset_manager.h>
#include <oboe/Oboe.h>
#include "EmulatorArgsBuilder.h"
#include "MelonDS.h"
#include "LiveCheckpointStaging.h"
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
#ifdef LITEV_JIT_SHARED_CODE
#include "ARMJIT_Global.h"
#endif
#ifdef LITEV_HOSTED_NETPLAY
#include "net/HostedMP.h"
#endif
#include "LitevCores.h"
#include <sys/system_properties.h>
#include "sereneds_build_id.h"
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
namespace melonDS { extern std::uint64_t JitCompileCount, JitProtectCalls, JitProtectFaults; }
#ifdef LITEV_JIT_COMPILE_STATS
#include "ARMJIT_Internal.h"
#endif
#if defined(LITEV_SLOWMEM_HIST) || defined(LITEV_ACCESS_STATS)
#include <android/log.h>
#endif
#ifdef LITEV_SLOWMEM_HIST
namespace melonDS { void LitevSlowHistLine(int h, char* buf, int len); }
#endif
#ifdef LITEV_ACCESS_STATS
namespace melonDS { extern std::uint64_t LitevAccess[6][0x10000]; }
#include <algorithm>
#include <vector>
#endif
#include <sched.h>
#include <zlib.h>
#include <future>
#include <thread>
#include <exception>
#include <atomic>

extern bool isFastForwardEnabled;   // MelonDSAndroidJNI.cpp (record mode logs it per frame)
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
    static std::string activeRomPath;

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
        std::unique_ptr<PreparedLiveReplica> liveOwner; // keeps adopted save/firmware scratch paths alive
        std::shared_ptr<MelonInstance> console;
        std::vector<u32> screenshot = std::vector<u32>(256 * 384);
        std::map<int, NetplayFrameInput> script;    // debug.litev.npscript2: replaces the network
        std::thread thread;
        std::map<int, u64> hashes;                  // desync check: our copy, by frame (hashLock)
        std::atomic<bool> left {false};             // a live session's partial exit: this player left
#ifdef LITEV_NP_SPEED
        std::atomic<double> cpuMs {0};              // this console's thread CPU so far (the report)
#endif
    };
    struct NetplaySession
    {
        int player = 0, players = 2, delay = 3, port = 7100;
        std::string peer;                           // the host's IP:port (exchange=0: the other player's)
        bool exchange = true;                       // session setup with the host (exchange=0: testing, 2 players)
        bool autoDelay = true;                      // input delay from the measured round trips
        NetFaults faults;                           // testing: latency=,jitter=,loss= on received inputs
        std::chrono::steady_clock::time_point created = std::chrono::steady_clock::now();
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
        bool lostHost = false;                      // Hosted guest: the host went silent; the session ended
        bool waiting = false;
        bool live = false;
        bool liveFirstInputSubmitted = false;
        NetplayFrameInput liveFirstInput;
        // A live session ends by the game (NetplayLiveExit: every device leaves after the same
        // frame) or when the other player is gone (liveLeave requested from outside); the local
        // console then plays on alone in discovery (netplayLeaveLive), never reloaded.
        std::unique_ptr<NetplayLiveExit> liveExit;
        std::atomic<bool> liveEnded {false}, liveLeave {false};
        // a partial exit of this device (its console quit / its game left the others / it was left
        // alone): the others play on; our inputs up to liveLastFrame must still reach them
        bool livePartial = false;
        int liveLastFrame = -1;
        // Hosted Netplay (docs/HYBRID-NETPLAY.md, design F): the host (player 0) runs every console
        // and records what the link returns to each; every guest runs only its own console, whose
        // link replays the host's record of it. The guests' inputs travel to the host only.
        bool hosted = false;
#ifdef LITEV_NP_SPEED
        // Session speed: ONE state for the whole session; any player's fast-forward key toggles it
        // (NetplayInput::ToggleSpeed: a press travels with that player's input). `speed` = the
        // session's speed at the frame just run (plain Netplay: the same frame on every device; Hosted:
        // the host decides, its guests follow its frame markers), `speedWho` = whose press set it
        // (NetplayInput player bits; the Hosted host = kHostedServerId).
        std::atomic<int> speed {0};
        std::atomic<u32> speedWho {0};
        // the per-device report (NETPLAY_SPEED, every 600 frames)
        double runMs = 0;                           // the local console's runFrame (wall, incl. link waits)
        std::chrono::steady_clock::time_point reportAt = std::chrono::steady_clock::now();
        double reportCpu = 0, reportWait = 0;
#endif
#ifdef LITEV_HOSTED_NETPLAY
        RecordMP* record = nullptr;                 // host: the link (owned by MPInterface)
        ReplayMP* replay = nullptr;                 // guest: the link (owned by MPInterface)
        std::unique_ptr<HostedServer> server;
        std::unique_ptr<HostedClient> client;
#endif
    };
#ifdef LITEV_HOSTED_NETPLAY
    // the host's NetplayInput player index (the consoles are 0..14)
    static constexpr int kHostedServerId = NetplayInput::kMaxPlayers - 1;
#endif
    std::unique_ptr<NetplaySession> netplay;

    // Prepared without disturbing the singleton Ambient MPInterface or the
    // visible source. The session owns its input/Hosted sockets; Backend is
    // destroyed last, after a HostedClient has stopped calling ReplayMP.
    struct LiveNetplayCandidate
    {
        std::unique_ptr<MPInterface> Backend;
        std::unique_ptr<PreparedLiveReplica> PeerReplica;   // (a Hosted guest's peer copy: validated, dropped at commit)
        std::unique_ptr<NetplaySession> Session;
        NetplayLiveCheckpoint Local;
        std::vector<std::pair<int, u64>> PeerClocks;        // every peer's player and raw clock at the cut
        bool ProofDone = false;
    };
    static std::unique_ptr<LiveNetplayCandidate> liveCandidate;



    bool netplayStageLive(const NetplayLiveCheckpoint& local, std::vector<LivePeer> peers,
                          bool hosted, std::string& error)
    {
        error.clear();
        // (each clause named: 'does not match' alone could not be diagnosed from a device log)
        const char* mismatch =
            (liveCandidate) ? "liveCandidate" :
            (netplay) ? "netplay" :
            (MPInterface::GetType() != MPInterface_Ambient) ? "MPInterface::GetType() != MPInterface_Ambient" :
            (!instance) ? "!instance" :
            (!instance->getNds()) ? "!instance->getNds()" :
            (peers.empty() || peers.size() >= (size_t)NetplayInput::kMaxPlayers) ? "peer count" :
            (hosted && peers.size() != 1) ? "Hosted Netplay of more than two players" :
            (local.Player >= NetplayInput::kMaxPlayers) ? "local.Player out of range" :
            (!local.Epoch) ? "!local.Epoch" :
            (local.Build != NetplayHandoffCompat()) ? "local.Build != NetplayHandoffCompat()" :
            (local.FenceToken == std::array<u8, 16>{}) ? "local.FenceToken == std::array<u8, 16>{}" :
            (local.State.empty()) ? "local.State.empty()" :
            (local.Firmware.empty()) ? "local.Firmware.empty()" :
            (currentRomDescription() != local.Rom) ? "currentRomDescription() != local.Rom" :
            (instance->getFrame() != local.Frame) ? "instance->getFrame() != local.Frame" :
            (instance->getNds()->GetSysTimestamp() != local.RawClock) ? "instance->getNds()->GetSysTimestamp() != local.RawClock" :
            (instance->getNds()->Wifi.HandoffBusyMask() != 0) ? "instance->getNds()->Wifi.HandoffBusyMask() != 0" :
            nullptr;
        bool seen[NetplayInput::kMaxPlayers] = {};
        seen[local.Player < NetplayInput::kMaxPlayers ? local.Player : 0] = true;
        for (const LivePeer& p : peers)
        {
            if (mismatch) break;
            in_addr parsed{};
            const NetplayLiveCheckpoint* c = p.Checkpoint.get();
            mismatch =
                (!c || !p.Replica || !p.Replica->Started || !p.Replica->Console || !p.Replica->Console->getNds()) ? "peer replica missing" :
                (c->Player >= NetplayInput::kMaxPlayers || seen[c->Player]) ? "peer player invalid or repeated" :
                (c->Epoch != local.Epoch) ? "peer epoch != local epoch" :
                (c->Build != local.Build) ? "peer build != local build" :
                (c->FenceToken != local.FenceToken) ? "peer fence token != local" :
                (c->State.empty() || c->Firmware.empty()) ? "peer checkpoint empty" :
                (p.Replica->Console->getInstanceId() != c->Player) ? "peer replica instance id" :
                (p.Replica->Console->loadedRomDescription() != c->Rom) ? "peer replica ROM" :
                (p.Replica->Console->getNds()->GetSysTimestamp() != c->RawClock) ? "peer replica clock" :
                (p.Replica->Console->getNds()->Wifi.HandoffBusyMask() != 0) ? "peer replica busy" :
                (inet_pton(AF_INET, p.IPv4.c_str(), &parsed) != 1) ? "peer IPv4" : nullptr;
            if (!mismatch) seen[c->Player] = true;
        }
        if (mismatch)
        { error = std::string("live Netplay candidate does not match the frozen cut: ") + mismatch; return false; }
#ifndef LITEV_HOSTED_NETPLAY
        if (hosted) { error = "Hosted Netplay is not available in this build"; return false; }
#endif

        constexpr int kInputDelay = 3;
        auto candidate = std::make_unique<LiveNetplayCandidate>();
        candidate->Local = local;
        for (const LivePeer& p : peers) candidate->PeerClocks.push_back({p.Checkpoint->Player, p.Checkpoint->RawClock});
        candidate->Session = std::make_unique<NetplaySession>();
        auto& session = *candidate->Session;
        session.player = local.Player;
        session.players = (int)peers.size() + 1;
        session.delay = kInputDelay;
        session.port = kLiveInputPort + local.Player;
        session.peer = peers[0].IPv4 + ":" + std::to_string(kLiveInputPort + peers[0].Checkpoint->Player);
        session.exchange = false;
        session.autoDelay = false;
        session.hosted = hosted;
        session.live = true;
        int maxPlayer = local.Player;
        for (const LivePeer& p : peers) maxPlayer = std::max(maxPlayer, (int)p.Checkpoint->Player);
        session.liveExit = std::make_unique<NetplayLiveExit>(maxPlayer + 1);

        int inputPlayer = local.Player;
        std::vector<std::pair<int, std::string>> inputPeers, startPeers;
        for (const LivePeer& p : peers)
        {
            int peerInputPlayer = p.Checkpoint->Player;
#ifdef LITEV_HOSTED_NETPLAY
            if (hosted && peerInputPlayer == 0) peerInputPlayer = kHostedServerId;
#endif
            inputPeers.push_back({peerInputPlayer, p.IPv4 + ":" + std::to_string(kLiveInputPort + p.Checkpoint->Player)});
            startPeers.push_back({peerInputPlayer, p.IPv4 + ":" + std::to_string(kLiveStartPort + p.Checkpoint->Player)});
        }
#ifdef LITEV_HOSTED_NETPLAY
        if (hosted && local.Player == 0) inputPlayer = kHostedServerId;
#endif
        session.input = std::make_unique<NetplayInput>(inputPlayer, kInputDelay, session.port, inputPeers);
        if (!session.input->Ok() ||
            !session.input->ArmHandoffSession(local.Epoch, local.FenceToken, kLiveStartPort + local.Player, startPeers))
        { error = "cannot stage session-bound Netplay input"; return false; }

#ifdef LITEV_HOSTED_NETPLAY
        const std::string& peerIPv4 = peers[0].IPv4;
        if (hosted && local.Player == 0)
        {
            auto record = std::make_unique<RecordMP>(std::make_unique<LockstepMP>());
            session.record = record.get();
            candidate->Backend = std::move(record);
            session.server = std::make_unique<HostedServer>(session.port + 2);
            if (!session.server->Ok())
            { error = "cannot stage Hosted Netplay record server"; return false; }
            session.server->DrainMs = 500;
            session.input->DropAfterMs = 3000;
        }
        else if (hosted)
        {
            auto replay = std::make_unique<ReplayMP>(local.Player);
            session.replay = replay.get();
            candidate->Backend = std::move(replay);
            ReplayMP* sink = session.replay;
            NetplayInput* input = session.input.get();
            sink->SetServerSilence([input] { return input->MsSincePeer(); });
            session.client = std::make_unique<HostedClient>(local.Player,
                peerIPv4 + ":" + std::to_string(kLiveInputPort + peers[0].Checkpoint->Player + 2),
                [sink](const u8* data, size_t size) { sink->Feed(data, size); });
            if (!session.client->Ok())
            { error = "cannot stage Hosted Netplay record client"; return false; }
        }
        else
#endif
            candidate->Backend = std::make_unique<LockstepMP>();
        if (!hosted || local.Player == 0)
            for (LivePeer& p : peers)
            {
                auto remote = std::make_unique<NetplayRemote>();
                remote->player = p.Checkpoint->Player;
                remote->liveOwner = std::move(p.Replica);
                remote->console = remote->liveOwner->Console;
                session.remotes.push_back(std::move(remote));
            }
        else candidate->PeerReplica = std::move(peers[0].Replica);
        liveCandidate = std::move(candidate);
        return true;
    }

    bool netplayProveStagedLive(std::string& error)
    {
        error.clear();
        if (!liveCandidate || liveCandidate->ProofDone || !instance ||
            MPInterface::GetType() != MPInterface_Ambient ||
            instance->getFrame() != liveCandidate->Local.Frame ||
            instance->getNds()->GetSysTimestamp() != liveCandidate->Local.RawClock)
        { error = "source changed before live Netplay proof"; return false; }
        NetplayFrameInput first;
        first.Keys = instance->getInputMask() & 0xFFF;
        u16 x, y;
        if (instance->getTouch(x, y)) { first.TouchX = x; first.TouchY = y; }
#ifdef LITEV_NP_SPEED
        liveCandidate->Session->input->StampSpeed(first);
#endif
        liveCandidate->Session->liveFirstInput = first;
        liveCandidate->Session->input->SubmitLocal(0, first);
        if (!liveCandidate->Session->input->WaitForHandoffPeerInput(5000))
        { error = "peer frame-zero input was not proven"; return false; }
        liveCandidate->Session->liveFirstInputSubmitted = true;
        liveCandidate->ProofDone = true;
        return true;
    }

    bool netplayActive() { return netplay != nullptr; }

    std::string romGameCode()
    {
        melonDS::NDS* nds = instance ? instance->getNds() : nullptr;
        if (!nds || !nds->NDSCartSlot.GetCart()) return "";
        return std::string(nds->NDSCartSlot.GetCart()->GetHeader().GameCode, 4);
    }
    std::string currentRomPath() { return activeRomPath; }
    NetplayRom currentRomDescription()
    {
        return instance && !activeRomPath.empty() ? instance->loadedRomDescription() : NetplayRom{};
    }
    bool netplayLiveSourceIsMPClient()
    {
        return instance && instance->getNds() && instance->getNds()->Wifi.IsMPClientMode();
    }
    bool netplayLiveSourceIdle()
    {
        return instance && instance->getNds() && instance->getNds()->ConsoleType == 0 &&
               instance->getNds()->Wifi.HandoffBusyMask() == 0;
    }
    void netplayFreezeLiveInput()
    {
        if (instance) instance->setInputDeferred(true);
    }
    void netplayResumeLiveInput()
    {
        if (instance && !netplay) instance->setInputDeferred(false);
    }
    bool netplayCaptureLive(AmbientLAN& ambient, const NetplayRom& rom, int player,
                            NetplayLiveCheckpoint& checkpoint, std::string& error)
    {
        if (!instance) { error = "no visible source console"; return false; }
        return captureLiveCheckpoint(*instance, ambient, rom, player, checkpoint, error);
    }
    // replicas made ahead (makeShellAhead below)
    static std::future<std::unique_ptr<PreparedLiveReplica>> ownProofShell;
    static std::map<int, std::future<std::unique_ptr<PreparedLiveReplica>>> peerShells;   // by player
    bool netplayBeginLiveTransfer(LiveCheckpointTransfer& transfer,
                                  NetplayLiveCheckpoint local,
                                  const NetplayRom& peerRom,
                                  std::shared_future<LiveRom> peerRomData,
                                  NetplayLiveLink& link,
                                  std::string& error, int peerPlayer)
    {
        std::future<std::unique_ptr<PreparedLiveReplica>> shell;
        if (auto it = peerShells.find(peerPlayer); it != peerShells.end()) { shell = std::move(it->second); peerShells.erase(it); }
        return transfer.start(std::move(local), peerRom, std::move(peerRomData),
                              currentConfiguration, internalFilesDir, link, error, std::move(shell));
    }
    bool netplayCaptureBaseline(std::vector<u8>& state)
    {
        if (!instance || !instance->getNds()) return false;
        Savestate s;
        if (!instance->getNds()->DoSavestate(&s) || s.Error || !s.Length()) return false;
        state.assign((const u8*)s.Buffer(), (const u8*)s.Buffer() + s.Length());
        return true;
    }

    // the proof's copy of this console, made while the checkpoints cross (the emu thread waits then)
    static std::unique_ptr<PreparedLiveReplica> ownProofCopy;
    // the proof's own copy made ahead (at the offer, on a worker): the cut only loads its state
    void netplayDiscardStagedLive() { liveCandidate.reset(); ownProofCopy.reset(); ownProofShell = {}; peerShells.clear(); }

    // A replica made ahead on a worker (scratch storage, ROM loaded, started); the cut gives it the
    // checkpoint's firmware and state. Own: the proof's copy of this console; peer: its replica, once
    // its ROM is read (this console's firmware stands in until the cut).
    static std::future<std::unique_ptr<PreparedLiveReplica>> makeShellAhead(int player, LiveRom rom)
    {
        const Firmware& fw = instance->getNds()->GetFirmware();
        std::vector<u8> firmware(fw.Buffer(), fw.Buffer() + fw.Length());
        std::packaged_task<std::unique_ptr<PreparedLiveReplica>()> task(
            [player, rom = std::move(rom), firmware = std::move(firmware),
             configuration = currentConfiguration, dir = internalFilesDir] {
                std::string error;
                auto shell = prepareLiveReplicaShell(player, rom, firmware, configuration, dir, error);
                if (!shell) Platform::Log(Platform::LogLevel::Warn, "Just Play: replica not made ahead: %s\n", error.c_str());
                return shell;
            });
        auto future = task.get_future();
        std::thread(std::move(task)).detach();
        return future;
    }
    void netplayPreparePeerShell(int player, const LiveRom& rom)
    {
        if (instance && player >= 0 && player < NetplayInput::kMaxPlayers && rom.Bytes) peerShells[player] = makeShellAhead(player, rom);
    }

    void netplayPrepareOwnProofShell(int player)
    {
        if (!instance || player < 0 || player >= NetplayInput::kMaxPlayers) return;
        ownProofShell = makeShellAhead(player, liveRomOf(*instance, activeRomPath));
    }
    void netplayPrepareOwnProofCopy(const NetplayLiveCheckpoint& local)
    {
        std::string error;
        std::unique_ptr<PreparedLiveReplica> shell = ownProofShell.valid() ? ownProofShell.get() : nullptr;
        if (shell && shell->Console->getInstanceId() == local.Player && completeLiveReplica(*shell, local, error))
            ownProofCopy = std::move(shell);
        else
            ownProofCopy = instance ? prepareLiveReplica(local, liveRomOf(*instance, activeRomPath), currentConfiguration,
                                                         internalFilesDir, error) : nullptr;
        if (instance) instance->getNds()->JIT.JitEnableExecute();
    }

    bool netplayProveScratchSet(const NetplayLiveCheckpoint& local, std::vector<LivePeer>& peers,
                                std::vector<u64>& hashes, std::string& error)
    {
        hashes.clear();
        error.clear();
        int n = local.Player + 1;
        for (const LivePeer& p : peers) if (p.Checkpoint) n = std::max(n, p.Checkpoint->Player + 1);
        if (netplay || MPInterface::GetType() != MPInterface_Ambient ||
            !instance || !instance->getNds() || n != (int)peers.size() + 1 ||
            instance->getFrame() != local.Frame ||
            instance->getNds()->GetSysTimestamp() != local.RawClock ||
            currentRomDescription() != local.Rom)
        { error = "source changed before reversible live proof"; return false; }
        for (const LivePeer& p : peers)
            if (!p.Checkpoint || !p.Replica || !p.Replica->Console) { error = "a peer replica is missing for the proof"; return false; }
        const u64 sourceRam = XXH3_64bits(instance->getNds()->MainRAM,
                                           instance->getNds()->MainRAMMask + 1);
        struct RestoreSourceJitExecute
        {
            NDS* Source;
            ~RestoreSourceJitExecute() { Source->JIT.JitEnableExecute(); }
        } sourceJitExecute {instance->getNds()};
        // (the running console's own ROM buffer: no second read of the file)
        auto own = ownProofCopy && ownProofCopy->Console &&
                   ownProofCopy->Console->getNds()->GetSysTimestamp() == local.RawClock ? std::move(ownProofCopy) :
                   prepareLiveReplica(local, liveRomOf(*instance, activeRomPath), currentConfiguration,
                                      internalFilesDir, error);
        ownProofCopy.reset();
        if (!own) return false;
        std::vector<MelonInstance*> consoles(n, nullptr);
        std::vector<NDS*> nds(n, nullptr);
        std::vector<u64> clocks(n, 0);
        consoles[local.Player] = own->Console.get();
        clocks[local.Player] = local.RawClock;
        for (LivePeer& p : peers) { consoles[p.Checkpoint->Player] = p.Replica->Console.get(); clocks[p.Checkpoint->Player] = p.Checkpoint->RawClock; }
        for (int i = 0; i < n; i++)
        {
            if (!consoles[i]) { error = "live proof: a player has no console"; return false; }
            nds[i] = consoles[i]->getNds();
        }
        // (the lib's proof, the same on every platform: NetplayRunLiveProofN)
        const bool proven = NetplayRunLiveProofN(nds, clocks, [&](int player) { consoles[player]->runFrameHeadless(); }, hashes, error);
        own.reset();
        if (!proven ||
            instance->getFrame() != local.Frame ||
            instance->getNds()->GetSysTimestamp() != local.RawClock ||
            XXH3_64bits(instance->getNds()->MainRAM,
                         instance->getNds()->MainRAMMask + 1) != sourceRam)
        {
            if (error.empty()) error = "reversible live proof did not preserve the source";
            for (LivePeer& p : peers) p.Replica.reset();
            return false;
        }
        // The proof advanced the peer copies; the adopted Netplay replicas must
        // instead begin at the exact frozen (cold) checkpoints: load them again.
        for (LivePeer& p : peers)
            if (!reloadLiveReplica(*p.Replica, *p.Checkpoint, error)) { for (LivePeer& q : peers) q.Replica.reset(); return false; }
        std::string text;
        for (u64 h : hashes) { char b[20]; snprintf(b, sizeof(b), " %016llx", (unsigned long long)h); text += b; }
        Platform::Log(Platform::LogLevel::Info, "Just Play reversible proof of %d consoles:%s\n", n, text.c_str());
        return true;
    }

    void netplayQuitLive()
    {
        if (netplay && netplay->live && netplay->input && !netplay->hosted)
        {
            netplay->input->RequestLeave();
            Platform::Log(Platform::LogLevel::Info, "Netplay: this player quits the live session (frame %d)\n", netplay->frame);
        }
    }
#ifndef LITEV_AMBIENT_LAN
    bool ambientFirmwareMac(u8*) { return false; }
#endif

    static std::string netplayPending; // set by the lobby, used by the next setup()
    // the user's ROM library: where another player's console finds that player's game
    static std::vector<std::string> netplayLibrary;
    // where ROMs received from the other players go, and its size cap
    static std::string netplayCacheDir;
    static u64 netplayCacheMax = 2ull << 30;

    void netplayPrepare(int player, int players, std::string host, bool hosted, std::vector<std::string> library,
                        std::string cacheDir, u64 cacheMaxBytes)
    {
        netplayLibrary = std::move(library);
        netplayCacheDir = std::move(cacheDir);
        netplayCacheMax = cacheMaxBytes;
        netplayPending = "player=" + std::to_string(player) + ",players=" + std::to_string(players);
        if (!host.empty()) netplayPending += ",peer=" + host + ":7100";
        if (hosted) netplayPending += ",hosted=1";
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
                else if (k == "latency") session->faults.LatencyMs = atoi(v.c_str());
                else if (k == "jitter") session->faults.JitterMs = atoi(v.c_str());
                else if (k == "loss") session->faults.LossPct = atoi(v.c_str());
#ifdef LITEV_HOSTED_NETPLAY
                else if (k == "hosted") session->hosted = atoi(v.c_str()) != 0;
#endif
            }
            pos = end + 1;
        }
        // a guest needs the host's address; without session setup (testing) only two players,
        // and the peer is the other one
        if ((session->peer.empty() && (session->player != 0 || !session->exchange)) ||
            (!session->exchange && (session->players != 2 || session->player > 1 || session->hosted)))
            return nullptr;
        return session;
    }

    // Test input: the harness script format, "FRAME KEY[,KEY...]" / "FRAME NONE" / "FRAME T:x:y"
    // per line, each held until the next line.
    // Also reads the recording format: "FRAME 0xKEYS TX TY".
    // `prop` holds the path; a "%d" in it becomes `player` (debug.litev.npscript2: one script
    // per remote player, or the same one for all without %d).
    static void netplayParseScript(const std::string& path, std::map<int, NetplayFrameInput>& script);
    static void netplayLoadScript(const char* prop, std::map<int, NetplayFrameInput>& script, int player = 0)
    {
        char spec[PROP_VALUE_MAX] = {0};
        if (__system_property_get(prop, spec) <= 0 || !spec[0]) return;
        std::string path = spec;
        if (size_t at = path.find("%d"); at != std::string::npos) path.replace(at, 2, std::to_string(player));
        netplayParseScript(path, script);
    }

    static void netplayParseScript(const std::string& path, std::map<int, NetplayFrameInput>& script)
    {
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
            else for (char* k = strtok(rest, ","); k; k = strtok(nullptr, ","))
            {
                if (sscanf(k, "T:%d:%d", &x, &y) == 2) { in.TouchX = x; in.TouchY = y; continue; }
                for (int b = 0; b < 12; b++)
                    if (!strcmp(k, names[b])) in.Keys &= ~(1u << b);
            }
            script[frame] = in;
        }
        fclose(f);
        Platform::Log(Platform::LogLevel::Info, "Netplay: scripted input, %zu lines from %s\n", script.size(), path.c_str());
    }

    // a test script drives the local console up to its last line; after that the real controls do
    static bool netplayScripted(const std::map<int, NetplayFrameInput>& script, int frame)
    {
        return !script.empty() && frame <= script.rbegin()->first;
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

    // Hosted Netplay frame marker state: the input applied this frame, the clock, and every 60
    // frames the RAM. The host's copy of a console and the guest's must compute the same.
    // (Same as the headless harness's HostedState, tools/headless/VerifyTrace.cpp.)
    static u64 hostedState(NDS& nds, const NetplayFrameInput& in, int frame)
    {
        u64 v[4] = { in.Keys | ((u64)(u16)in.TouchX << 32) | ((u64)(u16)in.TouchY << 48), nds.GetSysTimestamp(),
                     ((frame + 1) % 60) == 0 ? XXH3_64bits(nds.MainRAM, nds.MainRAMMask + 1) : 0, (u64)frame };
        return XXH3_64bits(v, sizeof(v));
    }

    // Record mode (single player). A recording is a folder:
    //   start.mln   the state it starts from. Saved and loaded straight back, so recording and
    //               replay both begin from a fresh load (same JIT reset, same pinned clock).
    //   start.sav   the game save at that moment (start.mln holds it too)
    //   inputs.txt  every input change, "FRAME KEY,KEY,T:x:y" or "FRAME NONE", held until the
    //               next line (the headless --input-script format; frames count from start.mln)
    //   frames.csv  per frame: frame period, emulator loop, RunFrame and its thread CPU (ms),
    //               whether the frame was drawn, fast-forward on, and every 60 frames a state hash
    //   meta.txt    game, clock, renderer
    // Replay loads start.mln, applies inputs.txt frame by frame, writes replay_<time>.csv and
    // compares its hashes with the recording's: "REPLAY OK" or "REPLAY DIFFERS at frame N".
    // Requests come from the UI thread and run at the top of the next emulator frame.
    struct Recording
    {
        std::string dir;
        bool replay = false;
        int frame = 0, frames = 0;                  // replay: frames recorded
        int rtc[6] {};
        FILE* inputs = nullptr;
        FILE* log = nullptr;
        FILE* parts = nullptr;                      // debug.litev.rechash=1: per-frame part hashes
        FILE* marks = nullptr;                      // marks.txt: frames the player flagged (L2)
        NetplayFrameInput last;
        std::map<int, NetplayFrameInput> script;    // replay
        std::map<int, u64> hashes;                  // replay: the recording's
        int firstDiff = -1;
        int startEmuFrame = 0;                      // MelonInstance frame number of recording frame 0
        timespec prev {};
    };
    static std::unique_ptr<Recording> recording;
    static std::mutex recordLock;                   // the request and the status text
    static std::string recordRequest;               // "record DIR", "replay DIR" or "stop"
    static std::string recordResult;                // last finished replay, for the status line
    int recordMode();
    static std::atomic<int> recordMarks {0};        // L2 presses not yet written (UI thread -> emu thread)

    // the player flags this moment as worth investigating; lands on the next recorded frame
    int recordMark()
    {
        if (recordMode() != 1) return -1;
        recordMarks++;
        std::lock_guard<std::mutex> l(recordLock);
        return recording ? recording->frame : -1;
    }

    bool recordRtc(int* out)
    {
        if (!recording) return false;
        memcpy(out, recording->rtc, sizeof(recording->rtc));
        return true;
    }

    void recordQueue(std::string request)
    {
        std::lock_guard<std::mutex> l(recordLock);
        recordRequest = std::move(request);
    }

    static std::string recordStatus()
    {
        std::lock_guard<std::mutex> l(recordLock);
        if (!recording) return recordResult;
        return recording->replay ? "REPLAY " + std::to_string(recording->frame) + "/" + std::to_string(recording->frames) : "REC";
    }

    // present thread: the recording a presented frame belongs to (footage of recordings only)
    bool recordVideoTarget(const Frame* frame, std::string& dir, std::string& prefix, int& recFrame)
    {
        std::lock_guard<std::mutex> l(recordLock);
        if (!recording || !frame || frame->emuFrame < recording->startEmuFrame) return false;
        // debug.litev.recvideo=0: no replay footage (it costs the present thread ~0.4 ms a frame)
        static const bool video = [] { char b[PROP_VALUE_MAX] = {}; return !(__system_property_get("debug.litev.recvideo", b) > 0 && atoi(b) == 0); }();
        if (recording->replay && !video) return false;
        dir = recording->dir;
        prefix = recording->replay ? "replay-video-" : "video-";
        recFrame = frame->emuFrame - recording->startEmuFrame;
        return true;
    }

    // 0 none, 1 recording, 2 replaying
    int recordMode() { return !recording ? 0 : recording->replay ? 2 : 1; }

    static double msSince(const timespec& a, const timespec& b)
    {
        return (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
    }

    static void recordEnd()
    {
        Recording& r = *recording;
        std::string result;
        if (r.replay)
        {
            result = r.firstDiff >= 0 ? "REPLAY DIFFERS at frame " + std::to_string(r.firstDiff)
                   : r.frame < r.frames ? "REPLAY STOPPED at frame " + std::to_string(r.frame)
                   : "REPLAY OK";
#ifdef LITEV_SLOWMEM_HIST
            for (int h = 0; h < 4; h++) { char line[600]; melonDS::LitevSlowHistLine(h, line, sizeof(line)); __android_log_print(ANDROID_LOG_INFO, "melonDS", "%s", line); }
#endif
#ifdef LITEV_ACCESS_STATS
            {
                static const char* kinds[6] = {"R16", "R32", "W16", "W32", "R8", "W8"};
                std::vector<std::pair<unsigned long long, int>> v;
                for (int k = 0; k < 6; k++)
                    for (int a = 0; a < 0x10000; a++)
                        if (melonDS::LitevAccess[k][a]) v.push_back({melonDS::LitevAccess[k][a], (k << 16) | a});
                std::sort(v.rbegin(), v.rend());
                for (size_t i = 0; i < v.size() && i < 40; i++)
                {
                    int k = v[i].second >> 16, a = v[i].second & 0xFFFF;
                    if (a >= 0xF000) __android_log_print(ANDROID_LOG_INFO, "melonDS", "ACCESS %s region %02X:%X %llu", kinds[k], (a >> 4) & 0xFF, a & 0xF, v[i].first);
                    else __android_log_print(ANDROID_LOG_INFO, "melonDS", "ACCESS %s io %08X %llu", kinds[k], a >= 0x2000 ? 0x04100000 | (a & 0xFF) : 0x04000000 | a, v[i].first);
                }
            }
#endif
            if (FILE* f = fopen((r.dir + "/replay-result.txt").c_str(), "w"))
            {
                fprintf(f, "%s\nframes %d of %d, %zu hashes\n", result.c_str(), r.frame, r.frames, r.hashes.size());
                fclose(f);
            }
        }
        else if (FILE* f = fopen((r.dir + "/meta.txt").c_str(), "a"))
        {
            fprintf(f, "frames %d\n", r.frame);
            fclose(f);
        }
        if (r.inputs) fclose(r.inputs);
        if (r.log) fclose(r.log);
        if (r.marks) fclose(r.marks);
        if (r.parts) fclose(r.parts);
        Platform::Log(Platform::LogLevel::Info, "Record: %s %s, %d frames\n", r.replay ? "replay" : "recording", r.dir.c_str(), r.frame);
        instance->setInputDeferred(false);
        instance->getNds()->SetKeyMask(instance->getInputMask());
#ifdef LITEV_AGGRESSIVE_SKIP
        instance->getNds()->GPU.KeepCaptures = false;
#endif
        std::lock_guard<std::mutex> l(recordLock);
        recording.reset();
        recordResult = result;
    }

    static bool recordBegin(const std::string& kind, const std::string& dir)
    {
        auto r = std::make_unique<Recording>();
        r->dir = dir;
        r->replay = kind == "replay";
        NDS* nds = instance->getNds();
        char gameCode[5] = {0};
        if (auto* cart = nds->NDSCartSlot.GetCart()) memcpy(gameCode, cart->GetHeader().GameCode, 4);
        if (r->replay)
        {
            FILE* m = fopen((dir + "/meta.txt").c_str(), "r");
            if (!m) return false;
            char line[256], code[8] = {0};
            while (fgets(line, sizeof(line), m))
            {
                sscanf(line, "game %7s", code);
                sscanf(line, "rtc %d %d %d %d %d %d", &r->rtc[0], &r->rtc[1], &r->rtc[2], &r->rtc[3], &r->rtc[4], &r->rtc[5]);
                sscanf(line, "frames %d", &r->frames);
            }
            fclose(m);
            if (strcmp(code, gameCode) != 0)
            {
                Platform::Log(Platform::LogLevel::Error, "Record: %s is a recording of %s, not %s\n", dir.c_str(), code, gameCode);
                return false;
            }
            if (FILE* f = fopen((dir + "/frames.csv").c_str(), "r"))
            {
                char line[256];
                int frame, last = -1;
                unsigned long long hash;
                while (fgets(line, sizeof(line), f))
                {
                    if (sscanf(line, "%d", &frame) != 1) continue;
                    last = frame;
                    // frames.csv keeps the state hash in column 8. Profiling may append
                    // jit/mprotect/rewrites columns, so the final field is not the hash.
                    const char* h = line;
                    for (int column = 0; column < 7 && h; ++column)
                    {
                        h = strchr(h, ',');
                        if (h) ++h;
                    }
                    if (h && sscanf(h, "%llx", &hash) == 1) r->hashes[frame] = hash;
                }
                fclose(f);
                if (!r->frames) r->frames = last + 1;   // a recording cut off without its "frames" line
            }
            // inputs.txt is the Netplay test-script format
            if (access((dir + "/inputs.txt").c_str(), R_OK) != 0) return false;
            netplayParseScript(dir + "/inputs.txt", r->script);
        }
        else
        {
            mkdir(dir.c_str(), 0755);
            time_t t = time(nullptr);
            tm* now = localtime(&t);
            int v[6] = { now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec };
            memcpy(r->rtc, v, sizeof(v));
        }
        {
            std::lock_guard<std::mutex> l(recordLock);
            recording = std::move(r);
            recordResult.clear();
        }
        Recording& rec = *recording;
        std::string state = dir + "/start.mln";
        if (rec.replay && access(state.c_str(), R_OK) != 0)
        {
            // stored gzipped: unpack to the cache for the load
            state = internalFilesDir + "/replay-start.mln";
            gzFile in = gzopen((dir + "/start.mln.gz").c_str(), "rb");
            FILE* out = in ? fopen(state.c_str(), "wb") : nullptr;
            bool ok = in && out;
            std::vector<char> buf(1 << 20);
            int n;
            while (ok && (n = gzread(in, buf.data(), (unsigned) buf.size())) > 0)
                ok = fwrite(buf.data(), 1, n, out) == (size_t) n;
            if (in) gzclose(in);
            if (out) fclose(out);
            if (!ok)
            {
                std::lock_guard<std::mutex> l(recordLock);
                recording.reset();
                recordResult = "REPLAY FAILED (no start state)";
                return false;
            }
        }
        // the clock first, so start.mln holds the clock replay pins (loadState re-pins it)
        // a replay's state load writes the recording's save memory through the save manager (and
        // the game may save during it): keep it off the player's .sav on the SD card
        if (rec.replay) instance->redirectNdsSave(internalFilesDir + "/replay.sav");
        nds->RTC.SetDateTime(rec.rtc[0], rec.rtc[1], rec.rtc[2], rec.rtc[3], rec.rtc[4], rec.rtc[5]);
        if ((!rec.replay && !saveState(state.c_str())) || !loadState(state.c_str()))
        {
            std::lock_guard<std::mutex> l(recordLock);
            recording.reset();
            recordResult = "RECORD FAILED";
            return false;
        }
        nds = instance->getNds();
        rec.startEmuFrame = instance->getFrame();
        if (rec.replay && state != dir + "/start.mln") unlink(state.c_str());   // the unpacked copy
        if (!rec.replay)
        {
            // start.mln is ~19 MB, mostly zeros: gzip it (level 1, ~1.4 MB) off the emulator thread
            std::thread([state] {
                if (gzFile out = gzopen((state + ".gz.part").c_str(), "wb1"))
                {
                    bool ok = false;
                    if (FILE* in = fopen(state.c_str(), "rb"))
                    {
                        std::vector<char> buf(1 << 20);
                        size_t n;
                        ok = true;
                        while ((n = fread(buf.data(), 1, buf.size(), in)) > 0)
                            ok &= gzwrite(out, buf.data(), (unsigned) n) == (int) n;
                        fclose(in);
                    }
                    ok &= gzclose(out) == Z_OK;
                    if (ok && rename((state + ".gz.part").c_str(), (state + ".gz").c_str()) == 0)
                        unlink(state.c_str());
                }
            }).detach();
            if (FILE* f = fopen((dir + "/start.sav").c_str(), "wb"))
            {
                if (nds->GetNDSSave()) fwrite(nds->GetNDSSave(), 1, nds->GetNDSSaveLength(), f);
                fclose(f);
            }
            if (FILE* f = fopen((dir + "/meta.txt").c_str(), "w"))
            {
                fprintf(f, "game %s\nrtc %d %d %d %d %d %d\nrenderer %d\njit %d\n", gameCode,
                        rec.rtc[0], rec.rtc[1], rec.rtc[2], rec.rtc[3], rec.rtc[4], rec.rtc[5],
                        (int) currentConfiguration->renderer, (int) currentConfiguration->useJit);
                // the build and every debug.litev.* override, so the replay can use the same ones
                fprintf(f, "build %s\n", SERENEDS_BUILD_ID);
                __system_property_foreach([](const prop_info* pi, void* file) {
                    char name[PROP_NAME_MAX], value[PROP_VALUE_MAX];
                    __system_property_read(pi, name, value);   // minSdk 24: no read_callback
                    if (!strncmp(name, "debug.litev.", 12) && value[0])
                        fprintf((FILE*) file, "prop %s %s\n", name, value);
                }, f);
                fclose(f);
            }
            rec.inputs = fopen((dir + "/inputs.txt").c_str(), "w");
            recordMarks = 0;
        }
        char name[64];
        snprintf(name, sizeof(name), rec.replay ? "/replay-%ld.csv" : "/frames.csv", (long) time(nullptr));
        rec.log = fopen((dir + name).c_str(), "w");
        if (rec.log) fprintf(rec.log, "frame,period_ms,loop_ms,runframe_ms,emu_cpu_ms,drawn,ff,hash,jit,mprotect,rewrites\n");
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.litev.rechash", prop) > 0 && atoi(prop) == 1)
        {
            snprintf(name, sizeof(name), rec.replay ? "/parts-replay-%ld.csv" : "/parts.csv", (long) time(nullptr));
            rec.parts = fopen((dir + name).c_str(), "w");
            if (rec.parts) fprintf(rec.parts, "frame,timestamp,arm9regs,arm7regs,mainram,vram,wram\n");
        }
        instance->setInputDeferred(true);
#ifdef LITEV_AGGRESSIVE_SKIP
        // debug.litev.reckeepcap=0 (measurement only): frameskip skips capture frames as in normal
        // play, to measure what fast-forward gives outside a recording (the replay may differ)
        nds->GPU.KeepCaptures = !(__system_property_get("debug.litev.reckeepcap", prop) > 0 && atoi(prop) == 0);
        nds->GPU.KeepCapturesSeen = false;
#endif
        clock_gettime(CLOCK_MONOTONIC, &rec.prev);
        Platform::Log(Platform::LogLevel::Info, "Record: %s %s\n", rec.replay ? "replaying" : "recording", dir.c_str());
        return true;
    }

    static void recordWriteInput(FILE* f, int frame, const NetplayFrameInput& in)
    {
        static const char* names[12] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L", "X", "Y"};
        std::string keys;
        for (int b = 0; b < 12; b++)
            if (!(in.Keys & (1u << b))) keys += (keys.empty() ? "" : ",") + std::string(names[b]);
        if (in.TouchX >= 0) keys += (keys.empty() ? "T:" : ",T:") + std::to_string(in.TouchX) + ":" + std::to_string(in.TouchY);
        fprintf(f, "%d %s\n", frame, keys.empty() ? "NONE" : keys.c_str());
    }

    // one emulator frame while recording or replaying
    static u32 recordFrame()
    {
        Recording& r = *recording;
        timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        NetplayFrameInput in;
        if (r.replay)
            in = netplayScriptAt(r.script, r.frame);
        else
        {
            in.Keys = instance->getInputMask() & 0xFFF;
            u16 x, y;
            if (instance->getTouch(x, y)) { in.TouchX = x; in.TouchY = y; }
            if (r.frame == 0 || in.Keys != r.last.Keys || in.TouchX != r.last.TouchX || in.TouchY != r.last.TouchY)
            {
                if (r.inputs) recordWriteInput(r.inputs, r.frame, in);
                r.last = in;
            }
        }
        netplayApply(*instance, in);
        // debug.litev.markframes=<a>-<b>: the emulator thread is named "markframes" while it runs
        // recording frames a..b, so a profile can sort samples by thread name (simpleperf --sort comm)
        // (a comma-separated list of ranges works too: "437-441,7525-7570")
        static int markA[8], markB[8], markN = -1;
        if (markN < 0)
        {
            markN = 0;
            char v[PROP_VALUE_MAX] = {};
            if (__system_property_get("debug.litev.markframes", v) > 0)
                for (const char* q = v; markN < 8 && sscanf(q, "%d-%d", &markA[markN], &markB[markN]) == 2; markN++)
                    if (!(q = strchr(q, ','))) { markN++; break; } else q++;
        }
        bool marked = false;
        for (int k = 0; k < markN; k++) marked |= r.frame >= markA[k] && r.frame <= markB[k];
        if (marked) pthread_setname_np(pthread_self(), "markframes");
        u32 lines = instance->runFrame();
        if (marked) pthread_setname_np(pthread_self(), "EmulatorThread");
        NDS& nds = *instance->getNds();
        u64 hash = ((r.frame + 1) % 60) == 0 ? hostedState(nds, in, r.frame) : 0;
        if (r.log)
        {
            const auto& s = instance->lastFrameStats();
            fprintf(r.log, "%d,%.3f,%.3f,%.3f,%.3f,%d,%d,", r.frame, msSince(r.prev, t0), s.loopMs, s.runFrameMs, s.emuCpuMs,
                    s.drawn ? 1 : 0, ::isFastForwardEnabled ? 1 : 0);
            if (hash) fprintf(r.log, "%016llx", (unsigned long long) hash);
            // JIT blocks compiled, code-protection mprotect calls, and faulting stores rewritten
            // to the slow path during this frame (docs/JIT-CODE-PROTECTION-MPROTECT.md)
            static std::uint64_t lastJit = 0, lastProt = 0, lastFlt = 0;
            fprintf(r.log, ",%llu,%llu,%llu", (unsigned long long)(melonDS::JitCompileCount - lastJit),
                    (unsigned long long)(melonDS::JitProtectCalls - lastProt), (unsigned long long)(melonDS::JitProtectFaults - lastFlt));
#ifdef LITEV_JIT_COMPILE_STATS
            {   // + JIT compile time per phase this frame, us (lib src/ARMJIT_Internal.h JitPhase)
                static std::uint64_t lastPh[melonDS::JitPhase_Count];
                static const double us = 1e6 / melonDS::JitTicksPerSec();
                for (int p = 0; p < melonDS::JitPhase_Count; p++)
                {
                    fprintf(r.log, ",%.0f", (melonDS::JitCompileTicks[p] - lastPh[p]) * us);
                    lastPh[p] = melonDS::JitCompileTicks[p];
                }
            }
#endif
            fprintf(r.log, "\n");
            lastJit = melonDS::JitCompileCount; lastProt = melonDS::JitProtectCalls; lastFlt = melonDS::JitProtectFaults;
        }
        if (r.parts)
        {
            XXH3_state_t* st = XXH3_createState();
            XXH3_64bits_reset(st);
            for (int b = 0; b < 9; b++) XXH3_64bits_update(st, nds.GPU.VRAM[b], nds.GPU.VRAMMask[b] + 1);
            u64 vram = XXH3_64bits_digest(st);
            XXH3_64bits_reset(st);
            XXH3_64bits_update(st, nds.SharedWRAM, 0x8000);
            XXH3_64bits_update(st, nds.ARM7WRAM, nds.ARM7WRAMSize);
            u64 wram = XXH3_64bits_digest(st);
            XXH3_freeState(st);
            fprintf(r.parts, "%d,%llu,%016llx,%016llx,%016llx,%016llx,%016llx\n", r.frame, (unsigned long long) nds.GetSysTimestamp(),
                    (unsigned long long) XXH3_64bits(nds.ARM9.R, sizeof(nds.ARM9.R)), (unsigned long long) XXH3_64bits(nds.ARM7.R, sizeof(nds.ARM7.R)),
                    (unsigned long long) XXH3_64bits(nds.MainRAM, nds.MainRAMMask + 1), (unsigned long long) vram, (unsigned long long) wram);
        }
        r.prev = t0;
        if (r.replay && hash && r.firstDiff < 0)
        {
            auto it = r.hashes.find(r.frame);
            if (it != r.hashes.end() && it->second != hash)
            {
                r.firstDiff = r.frame;
                Platform::Log(Platform::LogLevel::Error, "Record: replay differs from the recording at frame %d\n", r.frame);
            }
        }
        for (int m = recordMarks.exchange(0); m > 0 && !r.replay; m--)
        {
            if (!r.marks && (r.marks = fopen((r.dir + "/marks.txt").c_str(), "w")))
                fprintf(r.marks, "# frame  seconds  (player pressed L2: investigate here)\n");
            if (r.marks) { fprintf(r.marks, "%d %.2f\n", r.frame, r.frame / 60.0); fflush(r.marks); }
        }
        r.frame++;
        if (r.frame % 60 == 0)
        {
            if (r.inputs) fflush(r.inputs);
            if (r.log) fflush(r.log);
        }
        if (r.replay && r.frame >= r.frames)
            recordEnd();
        return lines;
    }

    // at the top of an emulator frame: start or stop what the UI asked for
    static void recordService()
    {
        std::string request;
        {
            std::lock_guard<std::mutex> l(recordLock);
            // a load into a console that has not run a frame yet runs differently under the JIT
            // than every later load (upstream; not found yet), so recordings and replays start
            // only once the game has run (a replay asked for at launch waits one frame)
            if (recordRequest.empty() || instance->getFrame() == 0) return;
            request.swap(recordRequest);
        }
        if (recording) recordEnd();
        size_t sp = request.find(' ');
        if (sp != std::string::npos && !netplay)
            recordBegin(request.substr(0, sp), request.substr(sp + 1));
    }

    // Hosted Netplay host: console `player` finished frame `frame`; its records go to the guest
    // that runs it (none for the host's own console). On that console's thread.
    static void hostedEndFrame(MelonInstance& console, int player, int frame, const NetplayFrameInput& in)
    {
#ifdef LITEV_HOSTED_NETPLAY
        NetplaySession& s = *netplay;
        if (!s.record) return;
#ifdef LITEV_NP_SPEED
        s.record->EndFrame(player, frame, hostedState(*console.getNds(), in, frame), in, (u32)s.speed.load() | s.speedWho.load() << 8);
#else
        s.record->EndFrame(player, frame, hostedState(*console.getNds(), in, frame), in);
#endif
        thread_local std::vector<u8> out;
        out.clear();
        s.record->TakeRecords(player, out);
        if (player != s.player && s.server) s.server->Push(player, out.data(), out.size());
#endif
    }

    static void netplaySetClock(int player, NDS* nds)
    {
        std::function<u64()> clock = [nds] { return nds->GetSysTimestamp(); };
#ifdef LITEV_HOSTED_NETPLAY
        if (netplay->record) { netplay->record->SetClock(player, std::move(clock)); netplay->record->Link().SetWake(player, *nds); return; }
        if (netplay->replay) { netplay->replay->SetClock(std::move(clock)); netplay->replay->SetWake(*nds); return; }
#endif
#ifdef LITEV_MP_FASTPOLL
        ((LockstepMP&) MPInterface::Get()).SetClockSource(player, nds->SysTimestampPtr());   // (a plain read, several per client poll)
#else
        ((LockstepMP&) MPInterface::Get()).SetClock(player, std::move(clock));
#endif
        ((LockstepMP&) MPInterface::Get()).SetWake(player, *nds);
    }

    // ends every wait on the link
    static void netplayStopLink()
    {
#ifdef LITEV_HOSTED_NETPLAY
        if (netplay->record) { netplay->record->Link().Stop(); return; }
        if (netplay->replay) { netplay->replay->Stop(); return; }
#endif
        ((LockstepMP&) MPInterface::Get()).Stop();
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
    // Our own console uses our real save, so progress made in Netplay persists as usual. Named
    // after the console's game too (`tag`: the start of its ROM's SHA-256), so one game's scratch
    // save is never another's.
    static std::string netplaySavePath(int player, const std::vector<u8>& data, const std::string& tag)
    {
        std::string dir = internalFilesDir + "/netplay";
        mkdir(dir.c_str(), 0700);
        std::string path = dir + "/p" + std::to_string(player) + (tag.empty() ? "" : "-" + tag) + ".sav";
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) return {};
        const bool complete = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
        const bool closed = fclose(f) == 0;
        return complete && closed ? path : std::string{};
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

#ifdef LITEV_NP_SPEED
    static double threadCpuMs()
    {
        timespec ts;
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
    }
    static std::string speedName(int code)
    {
        if (code == NetplaySpeed::kUncapped) return "uncapped";
        char b[16];
        snprintf(b, sizeof(b), "%gx", NetplaySpeed::Multiplier(code));
        return b;
    }
    // The fast-forward key in Netplay: toggles the session speed between normal and `multiplier`
    // (<= 0: uncapped). Returns what the key now shows (1 fast, 0 normal), -1 = not in Netplay.
    int netplayToggleSpeed(float multiplier)
    {
        if (!netplay || !netplay->input) return -1;
        return netplay->input->ToggleSpeed(NetplaySpeed::Encode(multiplier)) != 0;
    }
    // What the fast-forward key shows in Netplay (the session speed, or this player's press in
    // flight): 1 fast, 0 normal, -1 = not in Netplay
    int netplaySpeedShown()
    {
        if (!netplay || !netplay->input) return -1;
        return netplay->input->ShownSpeed() != 0;
    }
    // Netplay: the session speed now (multiplier; 0 = uncapped, 1 = normal)
    bool netplaySpeed(float& multiplier)
    {
        if (!netplay || !netplay->input) return false;
        multiplier = NetplaySpeed::Multiplier(netplay->speed);
        return true;
    }
    bool netplaySkipAllowed()
    {
        return !netplay || (!netplay->hosted && netplay->speed != 0);
    }
#endif

    // Opt-in live-cut diagnostic. Frame zero distinguishes a checkpoint/load mismatch from a
    // divergence caused by advancing Netplay. It never sends packets or changes emulated state.
    static void liveHandoffTrace(MelonInstance& console, int player, int frames)
    {
        static const bool enabled = [] {
            char value[PROP_VALUE_MAX] = {0};
            return __system_property_get("debug.litev.handofftrace", value) > 0 && atoi(value) != 0;
        }();
        if (!enabled || !netplay || !netplay->live ||
            (frames > 10 && frames % 10 != 0)) return;
        NDS* nds = console.getNds();
        Platform::Log(Platform::LogLevel::Info,
                      "HANDOFF_TRACE p%d f%d sys=%llu pc9=%08x pc7=%08x ram=%016llx wifi=%d mp=%d\n",
                      player, frames, (unsigned long long)nds->GetSysTimestamp(),
                      nds->ARM9.R[15], nds->ARM7.R[15],
                      (unsigned long long)XXH3_64bits(nds->MainRAM, nds->MainRAMMask + 1),
                      nds->Wifi.IsPoweredOn(), nds->Wifi.IsMultiplayer());
    }

    // Desync check, every 60 frames: each device sends the hash of its own console to everyone;
    // the others compare it with their copy of that console. Also logged (NETPLAY_HASH) for
    // offline diffs.
    static void netplayHash(MelonInstance& console, int player, int frames)
    {
        liveHandoffTrace(console, player, frames);
        if (frames % 60) return;
        NDS* nds = console.getNds();
        u64 hash = XXH3_64bits(nds->MainRAM, nds->MainRAMMask + 1) ^ nds->GetSysTimestamp();
        Platform::Log(Platform::LogLevel::Info, "NETPLAY_HASH p%d f%d sys=%llu ram=%016llx\n", player, frames,
                      (unsigned long long)nds->GetSysTimestamp(),
                      (unsigned long long)XXH3_64bits(nds->MainRAM, nds->MainRAMMask + 1));
        NetplaySession& s = *netplay;
        if (player == s.player)
        {
            if (frames % 600 == 0)
                Platform::Log(Platform::LogLevel::Info, "NETPLAY_NET f%d delay %d rtt %.1f ms stalled %.1f s\n", frames,
                              s.input->CurrentDelay(), s.input->PeerRttMs(), s.input->StallMs() / 1000);
#ifdef LITEV_NP_SPEED
            if (frames % 600 == 0 && !s.hosted)
            {   // per device: achieved fps, its own cost per frame, the time it waited for the others
                auto now = std::chrono::steady_clock::now();
                double cpu = threadCpuMs(), wait = s.input->StallMs(), remote = 0;   // remote consoles waiting for their input
                for (auto& r : s.remotes) remote = std::max(remote, r->cpuMs.load());
                static double lastRemote = 0;
                Platform::Log(Platform::LogLevel::Info, "NETPLAY_SPEED f%d speed %s by 0x%x fps %.1f | local run %.2f cpu %.2f ms/f | "
                              "busiest remote console cpu %.2f ms/f | remote consoles waited for input %.2f ms/f | delay %d rtt %.1f ms\n", frames,
                              speedName(s.speed).c_str(), s.speedWho.load(), 600 / std::chrono::duration<double>(now - s.reportAt).count(),
                              s.runMs / 600, (cpu - s.reportCpu) / 600, (remote - lastRemote) / 600, (wait - s.reportWait) / 600,
                              s.input->CurrentDelay(), s.input->PeerRttMs());
                s.reportAt = now; s.reportCpu = cpu; s.reportWait = wait; s.runMs = 0; lastRemote = remote;
            }
#endif
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

    int netplayKind() { return !netplay ? 0 : netplay->hosted ? 2 : 1; }

    static constexpr double kNetplayPeerLostMs = 10000;
    // The session speed for the fast-forward notice and the status line: "" = normal, else e.g.
    // "FF 2x by P2" / "uncapped by P1". The session's own state, whatever else the status says (a
    // wait, a player who left): the notice once parsed it out of the status text, so a "waiting" or
    // "a player left" status read as normal speed.
    std::string netplaySpeedLabel()
    {
#ifdef LITEV_NP_SPEED
        if (!netplay || netplay->speed == 0) return "";
        std::string who;
        for (int p = 0; p < NetplayInput::kMaxPlayers; p++)
            if ((netplay->speedWho >> p) & 1) who += (who.empty() ? "P" : ",P") + std::to_string(p == kHostedServerId ? 1 : p + 1);
        return (netplay->speed == NetplaySpeed::kUncapped ? std::string("uncapped") : "FF " + speedName(netplay->speed)) + (who.empty() ? "" : " by " + who);
#else
        return "";
#endif
    }

    std::string netplayStatus()
    {
        if (!netplay) return recordStatus();
        if (netplay->lostHost) return "HOST LEFT";
        if (netplay->desyncFrame >= 0) return "DESYNC";
        // plain Netplay: every device waits for every input, so a player gone for good (its app
        // closed, its session ended by a dropped group link) would hold the others forever
        if (!netplay->hosted && netplay->input && netplay->input->MsSincePeer() > kNetplayPeerLostMs
            && std::chrono::steady_clock::now() - netplay->created > std::chrono::seconds(30))
        {
            // a live session never reloads the game: the local console plays on alone (as a DS
            // whose partner walked out of range), back in discovery
            if (netplay->live) { netplayRequestLiveLeave(); return "PEER LEFT"; }
            return "PEER LOST";
        }
        std::string status = netplay->waiting ? "waiting for other player" : netplay->hosted ? "Hosted Netplay" : "Netplay";
#ifdef LITEV_HOSTED_NETPLAY
        if (netplay->record)
            for (int p = 1; p < netplay->players; p++)
                if (netplay->input && netplay->input->Dropped(p)) { status = "Hosted Netplay (a player left)"; break; }
#endif
        // the session speed, next to the fps (toasted on change from netplaySpeedLabel)
        const std::string speed = netplaySpeedLabel();
        return speed.empty() ? status : status + " · " + speed;
    }

    static void netplayRemoteLoop(NetplayRemote* r)
    {
        // Placement: LitevTopo RemoteConsole (debug.litev.npfloat), priority below.
#ifdef LITEV_TOPO_PIN
        LitevTopo::PinSelf(LitevTopo::CoreRole::RemoteConsole);
#else
        const auto& cores = LitevCores::Get();
        sched_setaffinity(0, sizeof(cores.OtherSet), &cores.OtherSet);
#endif
        // debug.litev.npnice: remote consoles' nice. Default -10 = the local emulator's: remotes
        // above it (the old -16) made the local console, which waits for them every frame, queue
        // behind them (headless 8P MKDS, RG DS: 29.8 vs 26 ms/frame wall)
        char nb[8] = {0};
        int nice = __system_property_get("debug.litev.npnice", nb) > 0 ? atoi(nb) : -10;
        while (setpriority(PRIO_PROCESS, 0, nice) != 0 && nice < 0) nice++;
#ifdef LITEV_NP_BATCH
        // SCHED_BATCH (nice kept): never preempts the thread that wakes it. Every lockstep wake
        // (the local console, the in-game host, wakes the others at each exchange) used to hand
        // the waker's core to the woken console: in-app 8P MKDS the local console was runnable
        // but preempted ~40% of its wall time. A woken console still takes an idle core at once.
        static const bool batch = [] { char b[PROP_VALUE_MAX] = {0}; return !(__system_property_get("debug.litev.npbatch", b) > 0 && atoi(b) == 0); }();
        if (batch)
        {
            sched_param sp {};
            const bool ok = sched_setscheduler(0, SCHED_BATCH, &sp) == 0;
            setpriority(PRIO_PROCESS, 0, nice);   // (setscheduler keeps it; re-assert)
            Platform::Log(Platform::LogLevel::Info, "Netplay: player %d's console thread SCHED_BATCH %s\n", r->player, ok ? "on" : "FAILED");
        }
#endif
        Platform::Log(Platform::LogLevel::Info, "Netplay: player %d's console thread at nice %d\n", r->player, nice);
        pthread_setname_np(pthread_self(), "NetplayRemote");

        NetplaySession& s = *netplay;
        for (int f = 0; s.running; f++)
        {
            NetplayFrameInput in = r->script.empty() ? s.input->Get(r->player, f) : netplayScriptAt(r->script, f);
            // a dropped player's Get() returns at once: keep its console paced with ours
            while (s.running && s.input->Dropped(r->player) && f > __atomic_load_n(&s.frame, __ATOMIC_RELAXED))
                usleep(2000);
            if (!s.running) break;
            netplayRecord(s, r->player, f, in);
            netplayApply(*r->console, in);
#ifdef LITEV_REMOTE_GX_SINK
            if (f == 600)
            {
                GPU3D& gx = r->console->getNds()->GPU.GPU3D;
                if (gx.TimingFixed && !GPU3D::SinkVeto.load()) gx.Sink = true;
                Platform::Log(Platform::LogLevel::Info, "Netplay: player %d's console: geometry sink %s\n", r->player, gx.Sink ? "on" : "off");
            }
#endif
            r->console->runFrameHeadless();
#ifdef LITEV_NP_SPEED
            if ((f & 63) == 0) r->cpuMs = threadCpuMs();
#endif
            hostedEndFrame(*r->console, r->player, f, in);
            netplayHash(*r->console, r->player, f + 1);
            if (s.liveExit && s.liveExit->EndFrame(r->player, f, r->console->getNds()->Wifi.MPExchanges, NetplayLeave::Of(in)))
            {
                if (s.liveExit->DroppedAfter(r->player) >= 0)
                {
                    // that player alone left the session (it quit, or its game left the others): its copy
                    // stops here on every device after this same frame; the others play on without it
                    if (auto* link = dynamic_cast<LockstepMP*>(&MPInterface::Get())) link->Leave(r->player);
                    s.input->DropPlayer(r->player, f);
                    r->left = true;
                    Platform::Log(Platform::LogLevel::Info, "Netplay: player %d left the live session after frame %d (%s)\n",
                                  r->player, f, NetplayLeave::Of(in) ? "it quit" : "its game left the others");
                }
                break;   // (else the whole session ends: the local console leaves after this same frame)
            }
        }
    }

    static void netplayStop();

    bool netplayCommitStagedLive(std::string& error)
    {
        error.clear();
        if (!liveCandidate || !liveCandidate->ProofDone || netplay || !instance ||
            MPInterface::GetType() != MPInterface_Dummy ||
            instance->getFrame() != liveCandidate->Local.Frame ||
            instance->getNds()->GetSysTimestamp() != liveCandidate->Local.RawClock)
        { error = "live Netplay candidate is not ready to commit"; return false; }
        const int localPlayer = liveCandidate->Local.Player;
        if (instance->getInstanceId() != localPlayer &&
            (instance->getInstanceId() != 0 || !instance->reassignInstanceId(localPlayer)))
        { error = "cannot remap the visible console to its Netplay seat"; return false; }

        auto candidate = std::move(liveCandidate);
        auto* backend = candidate->Backend.get();
        const auto setOrigin = [&](int player, u64 rawClock) {
#ifdef LITEV_HOSTED_NETPLAY
            if (candidate->Session->record)
                candidate->Session->record->Link().SetClockOrigin(player, rawClock, 1000000000ull);
            else if (!candidate->Session->replay)
#endif
                static_cast<LockstepMP*>(backend)->SetClockOrigin(player, rawClock, 1000000000ull);
        };
        bool hasRemoteConsole = !candidate->Session->hosted;
#ifdef LITEV_HOSTED_NETPLAY
        hasRemoteConsole |= candidate->Session->record != nullptr;
#endif
        if (hasRemoteConsole)
        {
            setOrigin(localPlayer, candidate->Local.RawClock);
            for (const auto& [player, clock] : candidate->PeerClocks) setOrigin(player, clock);
        }
        // A Hosted guest runs only its visible console. Its peer scratch copy
        // was needed to validate PREPARED, but must not outlive the cut.
        candidate->PeerReplica.reset();
        netplay = std::move(candidate->Session);
        MPInterface::Set(std::move(candidate->Backend), MPInterface_Netplay);
        instance->setInputDeferred(true);
        instance->setRewindSuspendedForNetplay(true);
        netplaySetClock(localPlayer, instance->getNds());
        for (auto& remote : netplay->remotes)
            netplaySetClock(remote->player, remote->console->getNds());
        liveHandoffTrace(*instance, localPlayer, 0);
        for (auto& remote : netplay->remotes)
            liveHandoffTrace(*remote->console, remote->player, 0);
        if (instance->getNds()->Wifi.IsPoweredOn())
            MPInterface::Get().Begin(localPlayer);
        for (auto& remote : netplay->remotes)
            if (remote->console->getNds()->Wifi.IsPoweredOn())
                MPInterface::Get().Begin(remote->player);
#ifdef LITEV_NP_ADAPTIVE_DELAY
        netplay->input->Adaptive = !netplay->hosted;
#endif
#ifdef LITEV_NP_QUIET_HB
        netplay->input->QuietHeartbeat = !netplay->hosted;
#endif
        netplay->created = std::chrono::steady_clock::now();
        netplay->running = true;
        try
        {
            for (auto& remote : netplay->remotes)
                remote->thread = std::thread(netplayRemoteLoop, remote.get());
        }
        catch (const std::exception& failure)
        {
            error = std::string("cannot start live peer console: ") + failure.what();
            netplayStop();
            return false;
        }
        Platform::Log(Platform::LogLevel::Info,
                      "Just Play: committed live %s at cut epoch %llu, local player %d\n",
                      netplay->hosted ? "Hosted Netplay" : "Netplay",
                      (unsigned long long)candidate->Local.Epoch, localPlayer);
        return true;
    }

    void netplayAbort()
    {
        NetplayCancelSetup();   // stopping during session setup (a ROM transfer, a consent question)
        if (!netplay)
            return;
        netplay->running = false;
        netplayStopLink();
        if (netplay->input)
            netplay->input->Abort();
    }

    static void netplayStop()
    {
        if (!netplay)
            return;
        const bool wasLive = netplay->live;
        netplay->running = false;
#if defined(LITEV_NP_FLOAT) && defined(LITEV_TOPO_PIN)
        LitevTopo::NetplayFloat() = 0;   // (the emu thread's 300-frame sweep re-masks it, as on the way in)
#endif
        netplayStopLink();
        // Abort first, then join every Get() user before destroying the input
        // object. A live promoted remote uses exactly the same wait path, and
        // Hosted ReplayMP's silence callback also retains this pointer.
        // (A partial exit: the others play on and still need our last inputs: Release only ends the
        // waits; the input link keeps re-sending until it is destroyed on the worker below.)
        const bool partial = wasLive && netplay->livePartial;
        const int partialLast = netplay->liveLastFrame;
        if (netplay->input) { if (partial) netplay->input->Release(); else netplay->input->Abort(); }
        const auto t0 = std::chrono::steady_clock::now();
        for (auto& r : netplay->remotes)
        {
            if (r->thread.joinable())
                r->thread.join();
            if (!r->liveOwner) r->console->stop(); // liveOwner stops its console exactly once
        }
        if (wasLive)
            Platform::Log(Platform::LogLevel::Info, "Netplay: live leave: remote threads joined in %.1f ms\n",
                          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        for (FILE* f : netplay->rec) if (f) fclose(f);
        if (wasLive)
        {
            // A live session's end: the local console plays on in the next frame. The rest - the input
            // link (its receiver's socket wait), the other players' copies, the Netplay MP backend - is
            // released on a worker: ~200 ms of the RG DS return were these destructors. Every thread that
            // used them is joined above; nothing here touches the local console.
            // the link's hooks on the console that plays on go first (the link dies on the worker; any
            // backend - Lockstep, Hosted record or replay - hooks the console)
            if (instance && instance->getNds()) instance->getNds()->DetachMPLink();
            auto old = std::move(netplay);
            auto oldMP = MPInterface::Take();
            MPInterface::Set(MPInterface_Dummy);
            std::thread([old = std::move(old), oldMP = std::move(oldMP), partial, partialLast]() mutable {
#ifdef LITEV_HOSTED_NETPLAY
                old->client = nullptr;
                old->server = nullptr;
#endif
                if (partial && old->input)
                {
                    const bool acked = old->input->WaitAcked(partialLast, 10000);
                    Platform::Log(Platform::LogLevel::Info, "Netplay: live leave: our inputs through frame %d %s\n", partialLast,
                                  acked ? "acknowledged by every player still in the session" : "NOT acknowledged within 10 s");
                }
                old->input = nullptr;
                old.reset();
                oldMP.reset();
            }).detach();
            if (instance)
            {
                instance->setRewindSuspendedForNetplay(false);
                instance->setInputDeferred(false);
            }
            return;
        }
#ifdef LITEV_HOSTED_NETPLAY
        netplay->client = nullptr;
        netplay->server = nullptr;
#endif
        netplay->input = nullptr;
        netplay = nullptr;
        MPInterface::Set(MPInterface_Dummy);
        if (wasLive && instance)
        {
            instance->setRewindSuspendedForNetplay(false);
            instance->setInputDeferred(false);
        }
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
#ifdef LITEV_HOSTED_NETPLAY
            if (netplay->hosted && netplay->player == 0)
            {
                auto record = std::make_unique<RecordMP>(std::make_unique<LockstepMP>());
                netplay->record = record.get();
                MPInterface::Set(std::move(record), MPInterface_Netplay);
            }
            else if (netplay->hosted)
            {
                auto replay = std::make_unique<ReplayMP>(netplay->player);
                netplay->replay = replay.get();
                MPInterface::Set(std::move(replay), MPInterface_Netplay);
            }
            else
#endif
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
            netplaySetClock(netplay->player, local);
        }
    }

    // Another player's console, booting `romPath` from that player's save.
#ifdef LITEV_REMOTE_GX_SINK
    // Netplay, every console (local and remote copies, on every device): fixed polygon timing for
    // games known never to read geometry results, so the remote copies can skip the geometry math
    // (GPU3D::Sink, from frame 600) and still send their frames at the owner's clock.
    // debug.litev.remotegx=0 turns it off: it must be the same on every device of a session.
    static bool netplayGxTiming(NDS* nds)
    {
        static const int prop = [] { char b[8] = {0}; return __system_property_get("debug.litev.remotegx", b) > 0 ? atoi(b) : 1; }();
        const auto* cart = nds->NDSCartSlot.GetCart();
        const bool on = prop != 0 && cart && GPU3D::SinkGameOK(cart->GetHeader().GameCode);
        nds->GPU.GPU3D.TimingFixed = on;
        return on;
    }
#endif

    static bool netplayAddRemote(int player, const std::string& romPath, const std::vector<u8>& save, const std::string& tag)
    {
        auto r = std::make_unique<NetplayRemote>();
        r->player = player;
        // The replica must own both save destinations before its constructor calls NDS::Reset.
        // Savestate loads during a live handoff can write either file immediately.
        const std::string scratchSavePath = netplaySavePath(player, save, tag);
        if (scratchSavePath.empty()) return false;
        auto args = BuildArgsFromConfiguration(*currentConfiguration, player);
        if (!args.has_value())
            return false;
#ifdef LITEV_JIT_SHARED_CODE
        // The remote consoles share one JIT code arena (exact; one copy of the game's code in the
        // shared L3 instead of one per console). debug.litev.jitshared=0: each its own.
        static const bool jitShared = [] { char b[PROP_VALUE_MAX] = {0}; return !(__system_property_get("debug.litev.jitshared", b) > 0 && atoi(b) == 0); }();
        melonDS::ARMJIT_Global::SetSharedCode(jitShared);
#endif
        r->console = std::make_shared<MelonInstance>(
            player,
            currentConfiguration,
            std::move(args.value()),
            net,
            std::make_unique<ScreenshotRenderer>(r->screenshot.data()),
            0,
            scratchSavePath + ".firmware"
        );
#ifdef LITEV_JIT_SHARED_CODE
        melonDS::ARMJIT_Global::SetSharedCode(false);
#endif
        NDS* nds = r->console->getNds();
        nds->GPU.Headless = true;   // its screens are not shown
        nds->SPU.Silent = true;     // nor its sound heard
        nds->GPU.GPU3D.Headless = true;
        netplaySetClock(player, nds);
        netplayLoadScript("debug.litev.npscript2", r->script, player);
        bool ok = r->console->loadRom(romPath, scratchSavePath);
#ifdef LITEV_REMOTE_GX_SINK
        if (ok) netplayGxTiming(nds);
#endif
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
#ifdef LITEV_SKIP_REPEAT_FRAMES
    void setSkipRepeatAuto(bool on) {
        if (instance)
            instance->skipRepeatAuto = on;
    }
#endif
#ifdef LITEV_FF_HEADLESS3D
    // fast-forward: frames whose 3D isn't rendered build geometry unwatched (GPU::FFHeadless3D)
    void setFastForwardHeadless(bool on) {
        if (instance && !netplay)
            instance->getNds()->GPU.FFHeadless3D = on;
    }
#endif

    int loadRom(std::string romPath, std::string sramPath, RomGbaSlotConfig* gbaSlotConfig)
    {
        activeRomPath.clear();
        if (netplay)
        {
            std::vector<std::pair<int, std::string>> peers {{1 - netplay->player, netplay->peer}};
            std::map<int, std::vector<u8>> saves;
            std::map<int, std::string> roms;    // each other console's ROM (cross-game: that player's game)
            std::map<int, std::string> tags;    // each console's ROM, short (scratch save names)
            if (netplay->exchange)
            {
                NetplaySetup setup;
                setup.Player = netplay->player;
                setup.NumPlayers = netplay->players;
                setup.Port = netplay->port;
                setup.Host = netplay->peer;
                setup.RomPath = romPath;
                setup.Rom = NetplayDescribeRom(romPath);
                if (!setup.Rom.Size)
                    return 2;
                std::vector<std::string> library {romPath};
                library.insert(library.end(), netplayLibrary.begin(), netplayLibrary.end());
                // only uncompressed .nds files in the library are searched; a game found nowhere
                // here (zipped ones included) is received from its player, with consent
                setup.FindRom = [library](const NetplayRom& rom) { return NetplayFindRom(rom, library); };
                setup.CacheDir = netplayCacheDir;
                setup.CacheMaxBytes = netplayCacheMax;
                setup.Consent = NetplayAskUser;     // the UI polls the status and answers
                setup.Save = netplayReadFile(sramPath);
                setup.Delay = netplay->autoDelay ? 0 : netplay->delay;
                setup.Hosted = netplay->hosted;
                setup.Join = netplay->hosted && netplay->player != 0;
                bool ok = NetplayHandshake(setup);
                for (size_t pos = 0, end; (end = setup.Log.find('\n', pos)) != std::string::npos; pos = end + 1)
                    Platform::Log(ok ? Platform::LogLevel::Info : Platform::LogLevel::Error, "%s\n", setup.Log.substr(pos, end - pos).c_str());
                if (!ok)
                    return 2;
                netplay->delay = setup.Delay;
                netplay->players = (int)setup.Peers.size() + 1;
                peers = std::move(setup.Peers);
                saves = std::move(setup.Saves);
                roms = std::move(setup.RomPaths);
                for (auto& [player, rom] : setup.Roms) tags[player] = rom.Hex().substr(0, 8);
            }
            // created only now: every device has agreed on the players and the input delay
#ifdef LITEV_HOSTED_NETPLAY
            if (netplay->hosted)
            {
                // host: inputs from every guest (it sends none); guest: its input to the host only.
                // The host's records travel on port + 2.
                if (netplay->record)
                {
                    netplay->input = std::make_unique<NetplayInput>(kHostedServerId, netplay->delay, netplay->port, peers);
                    netplay->server = std::make_unique<HostedServer>(netplay->port + 2);
                    netplay->server->DrainMs = 500;   // stopping: the guests may have left
                    netplay->input->DropAfterMs = 3000; // a guest silent this long is dropped: its console plays on with no input
                    for (auto& [player, addr] : peers)
                        if (!netplayAddRemote(player, roms.count(player) ? roms[player] : romPath, saves[player], tags[player]))
                            return 2;
                }
                else
                {
                    std::string host = netplay->peer.substr(0, netplay->peer.rfind(':'));
                    int hport = atoi(netplay->peer.substr(netplay->peer.rfind(':') + 1).c_str());
                    netplay->input = std::make_unique<NetplayInput>(netplay->player, netplay->delay, netplay->port,
                                                                    std::vector<std::pair<int, std::string>> {{kHostedServerId, netplay->peer}});
                    ReplayMP* replay = netplay->replay;
                    NetplayInput* in = netplay->input.get();
                    replay->SetServerSilence([in] { return in->MsSincePeer(); });   // silent host: end, don't hang
                    netplay->client = std::make_unique<HostedClient>(netplay->player, host + ":" + std::to_string(hport + 2),
                                                                     [replay](const u8* d, size_t l) { replay->Feed(d, l); });
                }
                bool ok = netplay->input->Ok() && (netplay->server ? netplay->server->Ok() : netplay->client->Ok());
                Platform::Log(Platform::LogLevel::Info, "Netplay: Hosted, %s, %d players, input delay %d frames%s\n",
                              netplay->record ? "host runs every console" : "guest replays its own console",
                              netplay->players, netplay->delay, ok ? "" : " (SOCKET FAILED)");
            }
            else
            {
#endif
            netplay->input = std::make_unique<NetplayInput>(netplay->player, netplay->delay, netplay->port, peers,
                                                            netplay->faults.LatencyMs, netplay->faults);
#ifdef LITEV_NP_ADAPTIVE_DELAY
            netplay->input->Adaptive = true;    // the delay follows the round trip from here
#endif
#ifdef LITEV_NP_QUIET_HB
            {   // plain Netplay: heartbeats only where needed; debug.litev.npquiet=0 turns it off
                char b[PROP_VALUE_MAX] = {0};
                netplay->input->QuietHeartbeat = !(__system_property_get("debug.litev.npquiet", b) > 0 && atoi(b) == 0);
            }
#endif
            Platform::Log(Platform::LogLevel::Info, "Netplay: %d players, input delay %d frames%s\n", netplay->players,
                          netplay->delay, netplay->input->Ok() ? "" : " (SOCKET FAILED)");
            for (auto& [player, addr] : peers)
                if (!netplayAddRemote(player, roms.count(player) ? roms[player] : romPath, saves[player], tags[player]))
                    return 2;
#ifdef LITEV_HOSTED_NETPLAY
            }
#endif
        }
        const std::string loadedPath = romPath;
        if (!instance->loadRom(std::move(romPath), std::move(sramPath)))
            return 2;
        activeRomPath = loadedPath;
#ifdef LITEV_REMOTE_GX_SINK
        if (netplay && netplayGxTiming(instance->getNds()))
            Platform::Log(Platform::LogLevel::Info, "Netplay: fixed geometry timing on every console (remote copies sink from frame 600)\n");
#endif
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
        activeRomPath.clear();
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
#if defined(LITEV_NP_SPEED) && defined(LITEV_AGGRESSIVE_SKIP)
            instance->getNds()->GPU.KeepCaptures = !netplay->hosted;    // fast-forward frameskip never skips a capture
            instance->getNds()->GPU.KeepCapturesSeen = false;
#endif
            netplay->running = true;
#if defined(LITEV_NP_FLOAT) && defined(LITEV_TOPO_PIN)
            {   // debug.litev.npfloat: 0 role masks, 1 remote consoles float over every core (default), 2 the
                // local emu too (in-app RG DS: 8P avg +1.7% but worst 1 s -6%, 2P worst 1 s -5%: stays 1)
                char b[8] = {0};
                const int fl = __system_property_get("debug.litev.npfloat", b) > 0 ? atoi(b) : 1;
                LitevTopo::NetplayFloat() = fl;
                Platform::Log(Platform::LogLevel::Info, "Netplay: thread placement npfloat=%d\n", fl);
            }
#endif
            for (auto& r : netplay->remotes)
            {
                r->console->start();
                r->thread = std::thread(netplayRemoteLoop, r.get());
            }
        }
    }

    // A live session's end: the local console - the same object that ran in Netplay - plays on alone,
    // back in discovery. No state moves, nothing reloads: the remote copies are dropped (their
    // scratch saves are never written), the link goes back to Ambient (liveExitHandler, from the
    // frontend), and the inputs already scheduled for the next frames run first.
    static std::vector<NetplayFrameInput> liveScheduled;
    static std::function<void()> liveExitHandler;
    void netplaySetLiveExitHandler(std::function<void()> handler) { liveExitHandler = std::move(handler); }
    bool netplayLocalWifiOn() { return instance && instance->getNds() && instance->getNds()->Wifi.IsPoweredOn(); }
    void netplayRequestLiveLeave()
    {
        // the other player is gone: end the waits now (they never complete), leave on the next frame
        if (!netplay || !netplay->live || netplay->liveLeave.exchange(true)) return;
        if (netplay->liveExit) netplay->liveExit->Cancel();
        netplayStopLink();
        if (netplay->input) netplay->input->Abort();
    }
    static void netplayLeaveLive()
    {
        const auto start = std::chrono::steady_clock::now();
        const bool byGame = netplay->liveEnded && !netplay->liveLeave;
        const bool partial = netplay->livePartial;
        const int last = netplay->frame - 1;
        liveScheduled.clear();
        if (byGame && netplay->input)
            for (int f = netplay->frame; f < netplay->frame + netplay->input->CurrentDelay() &&
                                         netplay->input->HasInput(netplay->player, f); f++)
                liveScheduled.push_back(netplay->input->Get(netplay->player, f));
        if (netplay->liveExit) netplay->liveExit->Cancel();
        const auto ms = [&] { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
        netplayStop();
        const double stopped = ms();
        if (!liveScheduled.empty()) instance->setInputDeferred(true);   // (netplayStop resumed live input)
        if (instance->getInstanceId() != 0) instance->reassignInstanceId(0);
        const double reassigned = ms();
        if (liveExitHandler) liveExitHandler();
        Platform::Log(Platform::LogLevel::Info, "Netplay: live leave: netplayStop %.1f ms, seat %.1f ms, back to Ambient %.1f ms\n",
                      stopped, reassigned - stopped, ms() - reassigned);
        Platform::Log(Platform::LogLevel::Info, "Netplay: live session ended %s after frame %d; playing on alone in "
                      "discovery (%zu scheduled inputs first), swap %.1f ms\n",
                      partial ? "for this device (it left; the others play on)" : byGame ? "by the game" : "(other player gone)",
                      last, liveScheduled.size(),
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }

    u32 loop()
    {
        if (netplay && netplay->live && (netplay->liveEnded || netplay->liveLeave))
            netplayLeaveLive();
        MPInterface::Get().Process();
        if (!netplay) instance->updateBattery();
        if (!netplay && !liveScheduled.empty())
        {
            // the inputs this console's Netplay had already scheduled (the input delay), in order;
            // live input from the frame after the last
            netplayApply(*instance, liveScheduled.front());
            liveScheduled.erase(liveScheduled.begin());
            const u32 lines = instance->runFrame();
            if (liveScheduled.empty()) instance->setInputDeferred(false);
            return lines;
        }
        if (netplay)
        {
            NetplayFrameInput local;
            local.Keys = instance->getInputMask() & 0xFFF;
            u16 x, y;
            if (instance->getTouch(x, y)) { local.TouchX = x; local.TouchY = y; }
            if (netplay->frame == 0 && netplay->liveFirstInputSubmitted)
                local = netplay->liveFirstInput;
            NetplayFrameInput applied;
#ifdef LITEV_HOSTED_NETPLAY
            if (netplay->record)    // Hosted Netplay host: its own console exists only here, so no delay
            {
                if (netplayScripted(netplay->script, netplay->frame)) local = netplayScriptAt(netplay->script, netplay->frame);
                applied = local;
#ifdef LITEV_NP_SPEED
                NetplayFrameInput press;    // the host's key presses (it submits no input)
                netplay->input->StampSpeed(press);
                netplay->input->NoteLocalSpeed(netplay->frame, press);
#endif
            }
            else
#endif
            {
            const int applyAt = netplay->frame + netplay->input->CurrentDelay();
            if (netplayScripted(netplay->script, applyAt))   // by applied frame: what is submitted now applies Delay frames later
                local = netplayScriptAt(netplay->script, applyAt);
#ifdef LITEV_NP_SPEED
            {
                // debug.litev.npspeed=<multiplier> (testing / the uncapped benchmark: -1 or 0 = uncapped,
                // 1 = normal): a change of it is a press for that speed (re-read every 120 frames)
                static std::string propLast;
                if (netplay->frame % 120 == 0)
                {
                    char b[PROP_VALUE_MAX] = {0};
                    __system_property_get("debug.litev.npspeed", b);
                    if (propLast != b)
                    {
                        propLast = b;
                        if (b[0]) netplay->input->RequestSpeed(NetplaySpeed::Encode((float)atof(b)));
                    }
                }
                netplay->input->StampSpeed(local);
            }
#endif
            // debug.litev.npquit=<anything but 0> (tests): this player quits a live session (once); the app's
            // own way to quit is the same call (netplayQuitLive)
            if (netplay->live && netplay->frame % 60 == 0)
            {
                static bool quitDone = false;
                char b[PROP_VALUE_MAX] = {0};
                const bool want = __system_property_get("debug.litev.npquit", b) > 0 && b[0] && strcmp(b, "0") != 0;
                if (want && !quitDone) { quitDone = true; netplayQuitLive(); }
                if (!want) quitDone = false;
            }
            // A live cut sent its exact frame-zero sample during the startup
            // proof. Re-sending frame zero here would append a second input
            // with the same applied frame and could change what the peer sees.
            if (netplay->frame != 0 || !netplay->liveFirstInputSubmitted)
                netplay->input->SubmitLocal(netplay->frame, local);
            applied = netplay->input->Get(netplay->player, netplay->frame);
#ifdef LITEV_HOSTED_NETPLAY
            // guest: past what the host has acknowledged it may have dropped us; apply what it applied
            NetplayFrameInput srv;
            if (netplay->replay && netplay->frame >= netplay->delay && netplay->frame > netplay->input->AckedBy(kHostedServerId)
                && netplay->replay->ServerInput(netplay->frame, srv, true))
                applied = srv;
#endif
            }
            const int frame = netplay->frame;
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
#ifdef LITEV_NP_SPEED
            auto runStart = std::chrono::steady_clock::now();
#endif
            u32 lines = instance->runFrame();
#ifdef LITEV_NP_SPEED
            if (!netplay->replay)   // plain Netplay, or the Hosted host (its guests follow its markers)
            {
                const double runNow = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - runStart).count();
                netplay->runMs += runNow;
                // never waits: the speed decided from the inputs already here (NetplayInput::SpeedAt)
                u32 who = 0;
                int from = -1;
                int code = netplay->input->SpeedAt(frame, &who, &from);
                if (code != netplay->speed || who != netplay->speedWho)
                    Platform::Log(Platform::LogLevel::Info, "NETPLAY_SPEED switch f%d %s by 0x%x (applied at f%d)\n",
                                  from + NetplayInput::kSpeedLag, speedName(code).c_str(), who, frame);
                netplay->speedWho = who;
                netplay->speed = code;
                // the adaptive delay sizes itself in frames of the current period (uncapped: the local work)
                static double workEma = 16.667;
                workEma = workEma * 0.9 + runNow * 0.1;
                const float m = NetplaySpeed::Multiplier(code);
                netplay->input->SetFramePeriodUs(m > 0 ? (int)(1e6 / (60 * m)) : (int)(workEma * 1000));
            }
#endif
            hostedEndFrame(*instance, netplay->player, frame, applied);
#ifdef LITEV_HOSTED_NETPLAY
            // guest: wait for the host's record of this frame and compare
            bool replayOk = !netplay->replay || netplay->replay->EndFrame(frame, hostedState(*instance->getNds(), applied, frame));
            if (!replayOk && netplay->replay->Lost() && !netplay->lostHost)
            {
                netplay->lostHost = true;
                Platform::Log(Platform::LogLevel::Error, "Netplay: Hosted session ended at frame %d: %s\n", frame, netplay->replay->Error().c_str());
            }
            else if (!replayOk && !netplay->lostHost && netplay->desyncFrame < 0)
            {
                netplay->desyncFrame = frame;
                Platform::Log(Platform::LogLevel::Error, "Netplay: DESYNC at frame %d (Hosted replica of console %d): %s\n",
                              frame, netplay->player, netplay->replay->Error().c_str());
            }
#ifdef LITEV_NP_SPEED
            int speedFrame;
            u32 speed;
            if (netplay->replay && netplay->replay->ServerSpeed(speedFrame, speed))
            {   // guest: the session speed is the host's, as sent with its frame markers
                const int code = (int)(speed & 63);
                if (code != netplay->speed || speed >> 8 != netplay->speedWho)
                    Platform::Log(Platform::LogLevel::Info, "NETPLAY_SPEED switch %s by 0x%x (host f%d, here f%d)\n", speedName(code).c_str(), speed >> 8, speedFrame, frame);
                netplay->input->ServerSpeed(speedFrame, code);
                netplay->speedWho = speed >> 8;
                netplay->speed = code;
            }
#endif
            if (netplay->replay && netplay->frame % 600 == 0)
                Platform::Log(Platform::LogLevel::Info, "Netplay: Hosted replica f%d, waited %.0f ms for the host so far\n",
                              netplay->frame, netplay->replay->StallMs());
#endif
            netplayHash(*instance, netplay->player, netplay->frame);
            if (netplay->frame % 60 == 0) netplayCheck(*netplay);
            if (netplay->liveExit && netplay->liveExit->EndFrame(netplay->player, frame, instance->getNds()->Wifi.MPExchanges,
                                                                 NetplayLeave::Of(applied)))
            {
                netplay->liveEnded = true;   // (whole session: every console stops after this frame) leave before the next
                netplay->livePartial = netplay->liveExit->DroppedAfter(netplay->player) >= 0;
                netplay->liveLastFrame = frame;
            }
            else if (netplay->liveExit && netplay->remotes.size() > 1 &&
                     netplay->liveExit->InMask() == (1u << netplay->player))
            {
                // every other player left: alone, this device leaves Netplay too
                netplay->liveEnded = true;
                netplay->livePartial = true;
                netplay->liveLastFrame = frame;
            }
            return lines;
        }
        recordService();
        if (recording) return recordFrame();
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
        netplayDiscardStagedLive(); // scratch consoles must die on this GL-owning thread
        netplayStop();
        instance->stop();
        cleanupOpenGlContext();
    }

    void cleanup()
    {
        cleanupAudio();

        instance = nullptr;
        activeRomPath.clear();
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

