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
#ifdef LITEV_HOSTED_NETPLAY
#include "net/HostedMP.h"
#endif
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
#include <zlib.h>
#include <thread>

extern bool isFastForwardEnabled;   // MelonDSAndroidJNI.cpp (record mode logs it per frame)
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
        bool lostHost = false;                      // Hosted guest: the host went silent; the session ended
        bool waiting = false;
        // Hosted Netplay (docs/HYBRID-NETPLAY.md, design F): the host (player 0) runs every console
        // and records what the link returns to each; every guest runs only its own console, whose
        // link replays the host's record of it. The guests' inputs travel to the host only.
        bool hosted = false;
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

    bool netplayActive() { return netplay != nullptr; }

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
    bool recordVideoTarget(const Frame* frame, std::string& dir, int& recFrame)
    {
        std::lock_guard<std::mutex> l(recordLock);
        if (!recording || recording->replay || !frame || frame->emuFrame < recording->startEmuFrame) return false;
        dir = recording->dir;
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
                    const char* h = strrchr(line, ',');
                    if (h && sscanf(h + 1, "%llx", &hash) == 1) r->hashes[frame] = hash;
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
                fclose(f);
            }
            rec.inputs = fopen((dir + "/inputs.txt").c_str(), "w");
        }
        char name[64];
        snprintf(name, sizeof(name), rec.replay ? "/replay-%ld.csv" : "/frames.csv", (long) time(nullptr));
        rec.log = fopen((dir + name).c_str(), "w");
        if (rec.log) fprintf(rec.log, "frame,period_ms,loop_ms,runframe_ms,emu_cpu_ms,drawn,ff,hash\n");
        char prop[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.litev.rechash", prop) > 0 && atoi(prop) == 1)
        {
            snprintf(name, sizeof(name), rec.replay ? "/parts-replay-%ld.csv" : "/parts.csv", (long) time(nullptr));
            rec.parts = fopen((dir + name).c_str(), "w");
            if (rec.parts) fprintf(rec.parts, "frame,timestamp,arm9regs,arm7regs,mainram,vram,wram\n");
        }
        instance->setInputDeferred(true);
#ifdef LITEV_AGGRESSIVE_SKIP
        nds->GPU.KeepCaptures = true;
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
        u32 lines = instance->runFrame();
        NDS& nds = *instance->getNds();
        u64 hash = ((r.frame + 1) % 60) == 0 ? hostedState(nds, in, r.frame) : 0;
        if (r.log)
        {
            const auto& s = instance->lastFrameStats();
            fprintf(r.log, "%d,%.3f,%.3f,%.3f,%.3f,%d,%d,", r.frame, msSince(r.prev, t0), s.loopMs, s.runFrameMs, s.emuCpuMs,
                    s.drawn ? 1 : 0, ::isFastForwardEnabled ? 1 : 0);
            if (hash) fprintf(r.log, "%016llx\n", (unsigned long long) hash); else fputs("\n", r.log);
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
        s.record->EndFrame(player, frame, hostedState(*console.getNds(), in, frame), in);
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
        if (netplay->replay) { netplay->replay->SetClock(std::move(clock)); return; }
#endif
        ((LockstepMP&) MPInterface::Get()).SetClock(player, std::move(clock));
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

    int netplayKind() { return !netplay ? 0 : netplay->hosted ? 2 : 1; }

    std::string netplayStatus()
    {
        if (!netplay) return recordStatus();
        if (netplay->lostHost) return "HOST LEFT";
        if (netplay->desyncFrame >= 0) return "DESYNC";
#ifdef LITEV_HOSTED_NETPLAY
        if (netplay->record)
            for (int p = 1; p < netplay->players; p++)
                if (netplay->input && netplay->input->Dropped(p)) return "Hosted Netplay (a player left)";
#endif
        return netplay->waiting ? "waiting for other player" : netplay->hosted ? "Hosted Netplay" : "Netplay";
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
            // a dropped player's Get() returns at once: keep its console paced with ours
            while (s.running && s.input->Dropped(r->player) && f > __atomic_load_n(&s.frame, __ATOMIC_RELAXED))
                usleep(2000);
            if (!s.running) break;
            netplayRecord(s, r->player, f, in);
            netplayApply(*r->console, in);
            r->console->runFrameHeadless();
            hostedEndFrame(*r->console, r->player, f, in);
            netplayHash(*r->console, r->player, f + 1);
        }
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
        netplay->running = false;
        netplayStopLink();
        netplay->input = nullptr;   // wakes a Get() waiting for the peer
        for (auto& r : netplay->remotes)
        {
            if (r->thread.joinable())
                r->thread.join();
            r->console->stop();
        }
        for (FILE* f : netplay->rec) if (f) fclose(f);
#ifdef LITEV_HOSTED_NETPLAY
        netplay->client = nullptr;
        netplay->server = nullptr;
#endif
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
    static bool netplayAddRemote(int player, const std::string& romPath, const std::vector<u8>& save, const std::string& tag)
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
        netplaySetClock(player, nds);
        netplayLoadScript("debug.litev.npscript2", r->script, player);
        bool ok = r->console->loadRom(romPath, netplaySavePath(player, save, tag));
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

    int loadRom(std::string romPath, std::string sramPath, RomGbaSlotConfig* gbaSlotConfig)
    {
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
            netplay->input = std::make_unique<NetplayInput>(netplay->player, netplay->delay, netplay->port, peers);
            Platform::Log(Platform::LogLevel::Info, "Netplay: %d players, input delay %d frames%s\n", netplay->players,
                          netplay->delay, netplay->input->Ok() ? "" : " (SOCKET FAILED)");
            for (auto& [player, addr] : peers)
                if (!netplayAddRemote(player, roms.count(player) ? roms[player] : romPath, saves[player], tags[player]))
                    return 2;
#ifdef LITEV_HOSTED_NETPLAY
            }
#endif
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
            NetplayFrameInput applied;
#ifdef LITEV_HOSTED_NETPLAY
            if (netplay->record)    // Hosted Netplay host: its own console exists only here, so no delay
            {
                if (netplayScripted(netplay->script, netplay->frame)) local = netplayScriptAt(netplay->script, netplay->frame);
                applied = local;
            }
            else
#endif
            {
            if (netplayScripted(netplay->script, netplay->frame + netplay->delay))   // by applied frame: what is submitted now applies Delay frames later
                local = netplayScriptAt(netplay->script, netplay->frame + netplay->delay);
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
            u32 lines = instance->runFrame();
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
            if (netplay->replay && netplay->frame % 600 == 0)
                Platform::Log(Platform::LogLevel::Info, "Netplay: Hosted replica f%d, waited %.0f ms for the host so far\n",
                              netplay->frame, netplay->replay->StallMs());
#endif
            netplayHash(*instance, netplay->player, netplay->frame);
            if (netplay->frame % 60 == 0) netplayCheck(*netplay);
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

