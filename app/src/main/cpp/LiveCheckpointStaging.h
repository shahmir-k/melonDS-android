#ifndef LIVE_CHECKPOINT_STAGING_H
#define LIVE_CHECKPOINT_STAGING_H

#include <memory>
#include <string>
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
    bool Started = false;
};

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
}

#endif
