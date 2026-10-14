#include "LiveCheckpointStaging.h"

#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <unistd.h>

#include "EmulatorArgsBuilder.h"
#include "GPU.h"
#include "GPU3D.h"
#include "MelonInstance.h"
#include "Savestate.h"
#include "SPU.h"
#include "SPI_Firmware.h"
#include "net/AmbientLAN.h"
#include "net/Net.h"
#include "renderer/ScreenshotRenderer.h"
#include "sereneds_build_id.h"

namespace MelonDSAndroid
{
using namespace melonDS;

static bool readLiveJitIdentityBytes(const u8* bytes, size_t length,
                                     LiveJitIdentity& identity, std::string& error)
{
    identity = {};
    if (!bytes || !length || length > UINT32_MAX)
        { error = "live checkpoint has no valid savestate"; return false; }
    Savestate state(const_cast<u8*>(bytes), static_cast<u32>(length), false);
    if (state.Error || length < 16)
        { error = "live checkpoint has invalid savestate header"; return false; }
    // The peer supplied these bytes. Do not use Savestate::FindSection here:
    // a corrupt zero section length can leave that legacy scanner looping.
    for (size_t offset = 16; offset < length;)
    {
        if (length - offset < 16)
            { error = "live checkpoint has truncated section header"; return false; }
        u32 sectionLength = 0;
        memcpy(&sectionLength, bytes + offset + 4, sizeof(sectionLength));
        if (sectionLength < 16 || sectionLength > length - offset)
            { error = "live checkpoint has invalid section length"; return false; }
        if (memcmp(bytes + offset, "JITS", 4) == 0)
        {
            if (sectionLength < 32)
                { error = "live checkpoint has truncated JIT profile"; return false; }
            memcpy(&identity.Build, bytes + offset + 16, sizeof(identity.Build));
            memcpy(&identity.Configuration, bytes + offset + 24, sizeof(identity.Configuration));
            u32 present = 0;
            memcpy(&present, bytes + offset + 28, sizeof(present));
            if (!identity.Build || !identity.Configuration || present != 1)
                { error = "live checkpoint has no restorable JIT profile"; return false; }
            return true;
        }
        offset += sectionLength;
    }
    error = "live checkpoint has no JIT profile section";
    return false;
}

bool readLiveJitIdentity(const std::vector<u8>& bytes,
                         LiveJitIdentity& identity, std::string& error)
{
    return readLiveJitIdentityBytes(bytes.data(), bytes.size(), identity, error);
}

bool captureLiveCheckpoint(MelonInstance& source, AmbientLAN& ambient,
                           const NetplayRom& rom, int player,
                           NetplayLiveCheckpoint& checkpoint, std::string& error)
{
    checkpoint = {};
    error.clear();
    if (!SERENEDS_HANDOFF_BUILD_CLEAN || !SERENEDS_HANDOFF_BUILD_ID)
        { error = "native handoff build is not clean"; return false; }
    if (player < 0 || player > 1 || !rom.Size || source.loadedRomDescription() != rom ||
        !ambient.HandoffFenceComplete() || ambient.HandoffFenceAborted() ||
        ambient.HandoffQueuedFrames())
        { error = "Ambient handoff fence is not quiescent"; return false; }
    NDS* nds = source.getNds();
    if (!nds || nds->ConsoleType != 0 || nds->Wifi.HandoffBusyMask())
        { error = "source DS is not safe to checkpoint"; return false; }

    NetplayLiveCheckpoint captured;
    captured.Epoch = ambient.GetHandoffFenceStats().Epoch;
    captured.Build = SERENEDS_HANDOFF_BUILD_ID;
    captured.RawClock = nds->GetSysTimestamp();
    captured.Frame = source.getFrame();
    captured.Player = static_cast<u8>(player);
    captured.Rom = rom;
    if (!captured.Epoch || !ambient.GetHandoffFenceToken(captured.FenceToken))
        { error = "Ambient handoff identity is missing"; return false; }

    const Firmware& firmware = nds->GetFirmware();
    if (!firmware.Buffer() || !firmware.Length() || firmware.Length() > (1u << 20))
        { error = "source firmware is unavailable"; return false; }
    captured.Firmware.assign(firmware.Buffer(), firmware.Buffer() + firmware.Length());

    Savestate state;
    if (!nds->DoSavestate(&state) || state.Error || state.Length() == 0 ||
        state.Length() > (64u << 20))
        { error = "source savestate capture failed"; return false; }
    captured.State.assign(static_cast<const u8*>(state.Buffer()),
                          static_cast<const u8*>(state.Buffer()) + state.Length());
    LiveJitIdentity jit;
    if (!readLiveJitIdentity(captured.State, jit, error)) return false;
    if (ambient.HandoffFenceAborted() || ambient.HandoffQueuedFrames() ||
        nds->Wifi.HandoffBusyMask())
        { error = "Ambient link changed during checkpoint capture"; return false; }
    checkpoint = std::move(captured);
    return true;
}

PreparedLiveReplica::~PreparedLiveReplica()
{
    if (Console)
    {
        if (Started) Console->stop();
        Console.reset();
    }
    PrivateNet.reset();
    if (!ScratchDir.empty())
    {
        unlink((ScratchDir + "/console.sav").c_str());
        unlink((ScratchDir + "/console.firmware").c_str());
        rmdir(ScratchDir.c_str());
    }
}

std::unique_ptr<PreparedLiveReplica> prepareLiveReplica(
    const NetplayLiveCheckpoint& checkpoint, const std::string& romPath,
    std::shared_ptr<EmulatorConfiguration> configuration,
    const std::string& internalFilesDir, std::string& error)
{
    error.clear();
    if (!SERENEDS_HANDOFF_BUILD_CLEAN || !SERENEDS_HANDOFF_BUILD_ID ||
        checkpoint.Build != SERENEDS_HANDOFF_BUILD_ID ||
        checkpoint.Player > 1 || !checkpoint.Epoch ||
        checkpoint.FenceToken == std::array<u8, 16>{} ||
        checkpoint.State.empty() || checkpoint.State.size() > (64u << 20) ||
        checkpoint.Firmware.empty() || checkpoint.Firmware.size() > (1u << 20) ||
        !checkpoint.Rom.Size || romPath.empty() || internalFilesDir.empty() ||
        !configuration || configuration->consoleType != 0)
        { error = "unsupported or incompatible live checkpoint"; return nullptr; }
    if (!(NetplayDescribeRom(romPath, true) == checkpoint.Rom))
        { error = "scratch replica ROM does not match the checkpoint"; return nullptr; }

    LiveJitIdentity sourceJit;
    if (!readLiveJitIdentity(checkpoint.State, sourceJit, error)) return nullptr;

    auto replica = std::make_unique<PreparedLiveReplica>();
    const std::string parent = internalFilesDir + "/netplay";
    if (mkdir(parent.c_str(), 0700) != 0 && errno != EEXIST)
        { error = "cannot create private Netplay storage"; return nullptr; }
    struct stat parentStat {};
    if (lstat(parent.c_str(), &parentStat) != 0 || !S_ISDIR(parentStat.st_mode))
        { error = "private Netplay storage is not a directory"; return nullptr; }
    std::string name = parent + "/live-XXXXXX";
    if (!mkdtemp(name.data()))
        { error = "cannot create live checkpoint scratch directory"; return nullptr; }
    replica->ScratchDir = std::move(name);
    replica->RomPath = romPath;
    const std::string savePath = replica->ScratchDir + "/console.sav";
    FILE* save = fopen(savePath.c_str(), "wb");
    if (!save)
        { error = "cannot create scratch save"; return nullptr; }
    if (fclose(save) != 0)
        { error = "cannot close scratch save"; return nullptr; }

    auto args = BuildArgsFromConfiguration(
        *configuration, checkpoint.Player,
        Firmware(checkpoint.Firmware.data(), static_cast<u32>(checkpoint.Firmware.size())));
    if (!args)
        { error = "cannot configure scratch DS"; return nullptr; }
    replica->PrivateNet = std::make_shared<Net>();
    replica->Screenshot.resize(256 * 384);
    replica->Console = std::make_shared<MelonInstance>(
        checkpoint.Player, configuration, std::move(*args), replica->PrivateNet,
        std::make_unique<ScreenshotRenderer>(replica->Screenshot.data()), 0,
        replica->ScratchDir + "/console.firmware");
    NDS* nds = replica->Console->getNds();
    nds->GPU.Headless = true;
    nds->GPU.GPU3D.Headless = true;
    nds->SPU.Silent = true;
    replica->Console->setInputDeferred(true);
    if (!replica->Console->loadRom(romPath, savePath))
        { error = "cannot load scratch replica ROM"; return nullptr; }
    if (replica->Console->loadedRomDescription() != checkpoint.Rom)
        { error = "scratch replica loaded different ROM bytes"; return nullptr; }
    replica->Console->start();
    replica->Started = true;

    {
        Savestate scratchState;
        if (!nds->DoSavestate(&scratchState) || scratchState.Error || !scratchState.Length())
            { error = "cannot inspect scratch JIT configuration"; return nullptr; }
        LiveJitIdentity scratchJit;
        if (!readLiveJitIdentityBytes(static_cast<const u8*>(scratchState.Buffer()),
                                      scratchState.Length(), scratchJit, error)) return nullptr;
        if (!(sourceJit == scratchJit))
            { error = "live checkpoint JIT build or configuration differs from this device"; return nullptr; }
    }

    Savestate state(const_cast<u8*>(checkpoint.State.data()),
                    static_cast<u32>(checkpoint.State.size()), false);
    if (state.Error || !nds->DoSavestate(&state) || state.Error)
        { error = "scratch replica savestate load failed"; return nullptr; }
    const Firmware& loaded = nds->GetFirmware();
    if (loaded.Length() != checkpoint.Firmware.size() ||
        memcmp(loaded.Buffer(), checkpoint.Firmware.data(), loaded.Length()) != 0 ||
        nds->GetSysTimestamp() != checkpoint.RawClock)
        { error = "scratch replica state differs from checkpoint"; return nullptr; }
    nds->GPU.Headless = true;
    nds->GPU.GPU3D.Headless = true;
    nds->SPU.Silent = true;
    return replica;
}

LiveCheckpointTransfer::~LiveCheckpointTransfer()
{
    cancel();
}

bool LiveCheckpointTransfer::start(NetplayLiveCheckpoint local,
                                   const NetplayRom& expectedPeerRom,
                                   const std::string& peerRomPath,
                                   std::shared_ptr<EmulatorConfiguration> configuration,
                                   const std::string& internalFilesDir,
                                   const std::string& peerIPv4, u16 tcpPort,
                                   std::string& error)
{
    error.clear();
    in_addr parsed{};
    if (Started || local.Player > 1 || !local.Epoch || !local.Build ||
        !local.Rom.Size || local.State.empty() || local.Firmware.empty() ||
        !expectedPeerRom.Size || peerRomPath.empty() || !configuration ||
        internalFilesDir.empty() || !tcpPort || tcpPort == UINT16_MAX ||
        inet_pton(AF_INET, peerIPv4.c_str(), &parsed) != 1)
    { error = "invalid live checkpoint transfer parameters"; return false; }
    Started = true;
    Local = std::move(local);
    ExpectedPeerRom = expectedPeerRom;
    PeerRomPath = peerRomPath;
    Configuration = std::move(configuration);
    InternalFilesDir = internalFilesDir;
    PeerIPv4 = peerIPv4;
    TcpPort = tcpPort;
    Worker = std::thread([this] {
        const std::string address = PeerIPv4 + ":" + std::to_string(TcpPort);
        Ok = NetplayExchangeLiveCheckpoints(
            TcpPort, address, PeerIPv4, Local, ExpectedPeerRom,
            [this](const NetplayLiveCheckpoint& candidate, std::string& reason) {
                LiveJitIdentity own, theirs;
                if (!readLiveJitIdentity(Local.State, own, reason) ||
                    !readLiveJitIdentity(candidate.State, theirs, reason)) return false;
                if (!(own == theirs))
                { reason = "peer JIT build or configuration differs from this device"; return false; }
                return Bridge.Request(candidate, reason, Cancelled);
            }, Cancelled, Peer, Error);
        Done.store(true, std::memory_order_release);
    });
    return true;
}

bool LiveCheckpointTransfer::serviceOnEmulatorThread()
{
    if (!Started || Done.load(std::memory_order_acquire)) return false;
    return Bridge.Service([this](const NetplayLiveCheckpoint& candidate, std::string& error) {
        Replica = prepareLiveReplica(candidate, PeerRomPath, Configuration,
                                     InternalFilesDir, error);
        return Replica != nullptr;
    });
}

bool LiveCheckpointTransfer::take(NetplayLiveCheckpoint& peer,
                                  std::unique_ptr<PreparedLiveReplica>& replica,
                                  std::string& error)
{
    error.clear();
    if (!Started || Taken || !finished()) return false;
    if (Worker.joinable()) Worker.join();
    Taken = true;
    error = Error;
    if (!Ok || Cancelled.load(std::memory_order_acquire) || !Replica)
    {
        if (error.empty()) error = "live checkpoint transfer did not prepare a replica";
        Replica.reset();
        return false;
    }
    peer = std::move(Peer);
    replica = std::move(Replica);
    return true;
}

void LiveCheckpointTransfer::cancel()
{
    Cancelled.store(true, std::memory_order_release);
    Bridge.Cancel();
    if (Worker.joinable()) Worker.join();
    Replica.reset();
}
}
