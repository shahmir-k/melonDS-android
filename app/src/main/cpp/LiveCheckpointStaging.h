#ifndef LIVE_CHECKPOINT_STAGING_H
#define LIVE_CHECKPOINT_STAGING_H

#include <atomic>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Configuration.h"
#include "net/NetplayInput.h"

namespace melonDS { class AmbientLAN; class Net; }

namespace MelonDSAndroid
{
class MelonInstance;

// Call on the emulator thread only, after the Ambient fence is complete and
// before the original console is allowed to run another frame. No user-facing
// save-state file is created.
bool captureLiveCheckpoint(MelonInstance& source, melonDS::AmbientLAN& ambient,
                           const melonDS::NetplayRom& rom, int player,
                           melonDS::NetplayLiveCheckpoint& checkpoint,
                           std::string& error);

// Owns the *actual* preloaded console that can later be committed to Netplay.
// Its private Net prevents RegisterInstance(player) from replacing the live
// console's packet queue while both consoles coexist. Its scratch save and
// firmware paths must outlive the console; destruction stops and removes them.
struct PreparedLiveReplica
{
    PreparedLiveReplica() = default;
    PreparedLiveReplica(const PreparedLiveReplica&) = delete;
    PreparedLiveReplica& operator=(const PreparedLiveReplica&) = delete;
    ~PreparedLiveReplica();

    std::shared_ptr<MelonInstance> Console;
    std::shared_ptr<melonDS::Net> PrivateNet;
    std::vector<melonDS::u32> Screenshot;
    std::string ScratchDir;
    std::string RomPath;
    bool Started = false;
};

// The JITS section is part of guest-visible execution timing: a loader from
// another build discards a warm one and diverges on the first frame. Live
// checkpoints are therefore cold (Savestate::ColdCaches: empty JIT and HLE
// caches, build 0); Configuration holds the JIT settings that shape blocks.
struct LiveJitIdentity
{
    melonDS::u64 Build = 0;
    melonDS::u32 Configuration = 0;
    bool operator==(const LiveJitIdentity& other) const
    { return Build == other.Build && Configuration == other.Configuration; }
};
bool readLiveJitIdentity(const std::vector<melonDS::u8>& state,
                         LiveJitIdentity& identity, std::string& error);

// A ROM in memory for a replica: read and identified ahead of the cut (readLiveRom, on a worker,
// once the peer's ROM is found), or the running console's own buffer. Reading and hashing a
// 268 MB ROM file took 2-7 s of the RG DS cut freeze per replica.
struct LiveRom
{
    std::string Path;
    std::shared_ptr<const melonDS::u8[]> Bytes;
    melonDS::u32 Length = 0;
    melonDS::NetplayRom Rom;
};
LiveRom readLiveRom(const std::string& path);
LiveRom liveRomOf(const MelonInstance& console, const std::string& path);

// Must run on the emulator/GL thread while the original Ambient console is
// frozen. ROM bytes are verified, and all savestate writes target scratch
// storage. The caller retains the returned object through PREPARED/ARMED/GO;
// rebuilding a replica after the vote would invalidate that vote.
std::unique_ptr<PreparedLiveReplica> prepareLiveReplica(
    const melonDS::NetplayLiveCheckpoint& checkpoint,
    const LiveRom& rom,
    std::shared_ptr<EmulatorConfiguration> configuration,
    const std::string& internalFilesDir,
    std::string& error);

// A replica before its checkpoint: scratch storage, the ROM loaded, started (any valid firmware;
// the checkpoint's replaces it). Made ahead of the cut, off the emulator thread; completed at the
// cut with the checkpoint (completeLiveReplica: identity checks, firmware, state).
std::unique_ptr<PreparedLiveReplica> prepareLiveReplicaShell(
    int player, const LiveRom& rom, const std::vector<melonDS::u8>& firmware,
    std::shared_ptr<EmulatorConfiguration> configuration,
    const std::string& internalFilesDir, std::string& error);
bool completeLiveReplica(PreparedLiveReplica& replica, const melonDS::NetplayLiveCheckpoint& checkpoint,
                         std::string& error);

// Loads the checkpoint into an already prepared replica again (after the reversible proof ran it),
// without reloading its ROM.
bool reloadLiveReplica(PreparedLiveReplica& replica,
                       const melonDS::NetplayLiveCheckpoint& checkpoint,
                       std::string& error);

// Owns the bounded checkpoint socket exchange and the *actual* preloaded peer
// replica. Start after the Ambient fence is complete, then call
// serviceOnEmulatorThread() while keeping Ambient::Process() alive and the
// visible source frozen. The network worker waits in its prepare callback;
// it never constructs an NDS or touches GL itself. Destruction cancels/joins
// the worker and discards the scratch replica unless take() transferred it.
// start/service/take/cancel/destruction all belong on the emulator/GL thread.
class LiveCheckpointTransfer
{
public:
    LiveCheckpointTransfer() = default;
    LiveCheckpointTransfer(const LiveCheckpointTransfer&) = delete;
    LiveCheckpointTransfer& operator=(const LiveCheckpointTransfer&) = delete;
    ~LiveCheckpointTransfer();

    bool start(melonDS::NetplayLiveCheckpoint local,
               const melonDS::NetplayRom& expectedPeerRom,
               std::shared_future<LiveRom> peerRom,
               std::shared_ptr<EmulatorConfiguration> configuration,
               const std::string& internalFilesDir,
               melonDS::NetplayLiveLink& link,
               std::string& error,
               std::future<std::unique_ptr<PreparedLiveReplica>> peerShell = {});
    bool serviceOnEmulatorThread();
    bool finished() const { return Done.load(std::memory_order_acquire); }
    bool take(melonDS::NetplayLiveCheckpoint& peer,
              std::unique_ptr<PreparedLiveReplica>& replica,
              std::string& error);
    void cancel();

private:
    melonDS::NetplayLivePrepareBridge Bridge;
    std::atomic<bool> Cancelled{false};
    std::atomic<bool> Done{false};
    std::thread Worker;
    melonDS::NetplayLiveCheckpoint Local, Peer;
    melonDS::NetplayRom ExpectedPeerRom;
    std::unique_ptr<PreparedLiveReplica> Replica;
    std::shared_ptr<EmulatorConfiguration> Configuration;
    std::shared_future<LiveRom> PeerRom;
    std::future<std::unique_ptr<PreparedLiveReplica>> PeerShell;   // (made ahead; the cut completes it)
    std::string InternalFilesDir, Error;
    melonDS::NetplayLiveLink* Link = nullptr;
    bool Started = false;
    bool Ok = false;
    bool Taken = false;
};
}

#endif
