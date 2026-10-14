#ifndef LIVE_CHECKPOINT_STAGING_H
#define LIVE_CHECKPOINT_STAGING_H

#include <atomic>
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

// Must run on the emulator/GL thread while the original Ambient console is
// frozen. ROM bytes are verified, and all savestate writes target scratch
// storage. The caller retains the returned object through PREPARED/ARMED/GO;
// rebuilding a replica after the vote would invalidate that vote.
std::unique_ptr<PreparedLiveReplica> prepareLiveReplica(
    const melonDS::NetplayLiveCheckpoint& checkpoint,
    const std::string& romPath,
    std::shared_ptr<EmulatorConfiguration> configuration,
    const std::string& internalFilesDir,
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
               const std::string& peerRomPath,
               std::shared_ptr<EmulatorConfiguration> configuration,
               const std::string& internalFilesDir,
               const std::string& peerIPv4, melonDS::u16 tcpPort,
               std::string& error);
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
    std::string PeerRomPath, InternalFilesDir, PeerIPv4, Error;
    melonDS::u16 TcpPort = 0;
    bool Started = false;
    bool Ok = false;
    bool Taken = false;
};
}

#endif
