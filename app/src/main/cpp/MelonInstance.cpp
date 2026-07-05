#include <ctime>
#include <chrono>
#include <cstdlib>
#include <sys/system_properties.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <filesystem>
#include <GLES3/gl3.h>
#include "Args.h"
#include "Configuration.h"
#include "DSi.h"
#include "DSiSupport.h"
#include "DSi_I2C.h"
#include "GPU.h"
#include "GPU_Soft.h"
#include "GPU_OpenGL.h"
#include "MelonDS.h"
#include "MelonInstance.h"
#include "NDS.h"
#include "NDSCart.h"
#include "net/Net_Slirp.h"
#include "Platform.h"
#include "SDCardArgsBuilder.h"
#include "MelonLog.h"

// ---- liteDS-v2 frame-phase profiler (gated by a debug property at runtime) ----
// Enable with:  adb shell setprop debug.litev.prof 1
// Emits a per-60-frame LITEV_PROF logcat line splitting the emulator frame into
// present-fence wait / RunFrame (core emulation + GL submit) / blit / other, plus
// a GPU TIME_ELAPSED reading. Default off => zero overhead in normal play.
#include <GLES2/gl2ext.h>
namespace {
    bool litevProfEnabled = false;
    void litevRefreshProfEnabled() {
        char buf[8] = {0};
        litevProfEnabled = (__system_property_get("debug.litev.prof", buf) > 0 && atoi(buf) != 0);
    }
    inline double litevNowMs() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
    }
    // GPU TIME_ELAPSED query (GL_EXT_disjoint_timer_query), deferred read.
    typedef void (GL_APIENTRYP LITEV_PFNGENQUERIES)(GLsizei, GLuint*);
    typedef void (GL_APIENTRYP LITEV_PFNBEGINQUERY)(GLenum, GLuint);
    typedef void (GL_APIENTRYP LITEV_PFNENDQUERY)(GLenum);
    typedef void (GL_APIENTRYP LITEV_PFNGETQOBJUI64)(GLuint, GLenum, GLuint64*);
    typedef void (GL_APIENTRYP LITEV_PFNGETQOBJUIV)(GLuint, GLenum, GLuint*);
    LITEV_PFNGENQUERIES  litevGenQueries  = nullptr;
    LITEV_PFNBEGINQUERY  litevBeginQuery  = nullptr;
    LITEV_PFNENDQUERY    litevEndQuery    = nullptr;
    LITEV_PFNGETQOBJUI64 litevGetQObjUI64 = nullptr;
    LITEV_PFNGETQOBJUIV  litevGetQObjUIV  = nullptr;
    bool  litevGpuInit = false;
    bool  litevGpuOk   = false;
    GLuint litevQ[2] = {0, 0};
    int    litevQSlot = 0;
    bool   litevQPending[2] = {false, false};
    static const GLenum LITEV_TIME_ELAPSED = 0x88BF;
    static const GLenum LITEV_QUERY_RESULT = 0x8866;
    static const GLenum LITEV_QUERY_RESULT_AVAILABLE = 0x8867;
    void litevGpuEnsure() {
        if (litevGpuInit) return;
        litevGpuInit = true;
        litevGenQueries  = (LITEV_PFNGENQUERIES) eglGetProcAddress("glGenQueriesEXT");
        litevBeginQuery  = (LITEV_PFNBEGINQUERY) eglGetProcAddress("glBeginQueryEXT");
        litevEndQuery    = (LITEV_PFNENDQUERY)   eglGetProcAddress("glEndQueryEXT");
        litevGetQObjUI64 = (LITEV_PFNGETQOBJUI64) eglGetProcAddress("glGetQueryObjectui64vEXT");
        litevGetQObjUIV  = (LITEV_PFNGETQOBJUIV)  eglGetProcAddress("glGetQueryObjectuivEXT");
        if (litevGenQueries && litevBeginQuery && litevEndQuery && litevGetQObjUI64 && litevGetQObjUIV) {
            litevGenQueries(2, litevQ);
            litevGpuOk = (litevQ[0] != 0 && litevQ[1] != 0);
        }
    }
}
// ---- end profiler ----

using namespace std;
using namespace melonDS;
using namespace melonDS::Platform;

namespace MelonDSAndroid
{

const int kRewindBufferSize = 1024 * 1024 * 20; // Use 20MB per savestate
const int kRewindScreenshotSize = 256 * 384 * 4;

MelonInstance::MelonInstance(int instanceId, std::shared_ptr<EmulatorConfiguration> configuration, std::unique_ptr<melonDS::NDSArgs> args, std::shared_ptr<Net> net, std::unique_ptr<ScreenshotRenderer> screenshotRenderer, int consoleType) :
    instanceId(instanceId),
    currentConfiguration(configuration),
    net(net),
    screenshotRenderer(std::move(screenshotRenderer)),
    consoleType(consoleType),
    rewindManager(configuration->rewindEnabled, configuration->rewindLengthSeconds, configuration->rewindCaptureSpacingSeconds, kRewindBufferSize, kRewindScreenshotSize)
{
    // Software renderer is always used during initialisation. Actual renderer will be set of first frame run
    currentRenderer = Renderer::Software;
    isRenderConfigurationDirty = true;
    inputMask = 0xFFF;
    frame = 0;

    net->RegisterInstance(instanceId);

    if (consoleType == 1)
    {
        melonDS::DSiArgs &dsiArgs = static_cast<melonDS::DSiArgs &>(*args);
        nds = new DSi(std::move(dsiArgs), this);
    }
    else
    {
        nds = new NDS(std::move(*args), this);
    }

    if (configuration->userInternalFirmwareAndBios)
    {
        std::filesystem::path firmwarePath = MelonDSAndroid::internalFilesDir;
        firmwarePath /= "wfcsettings.bin";
        firmwareSave = std::make_unique<SaveManager>(firmwarePath);
    }
    else
    {
        std::string firmwarePathString;
        if (consoleType == 1)
            firmwarePathString = configuration->dsiFirmwarePath;
        else
            firmwarePathString = configuration->dsFirmwarePath;

        firmwareSave = std::make_unique<SaveManager>(firmwarePathString);
    }

    // All instances have a RetroAchievements manager, but only the first instance will actually load achievements
    retroAchievementsManager = std::make_unique<RetroAchievements::RetroAchievementsManager>(nds);

    nds->Reset();
    setBatteryLevels();
    setDateTime();
}

MelonInstance::~MelonInstance()
{
    frameQueue.clear();
    if (blitReadFBO) glDeleteFramebuffers(1, &blitReadFBO);
    if (blitDrawFBO) glDeleteFramebuffers(1, &blitDrawFBO);
    net->UnregisterInstance(instanceId);
    delete nds;
}

// Blit the accelerated renderer's 2-layer array texture (layer 0 = top screen,
// layer 1 = bottom screen, each screenWidth x 192*scale) into the app's stacked
// frame texture, matching the software renderer's vertical layout (top at y=0,
// bottom at y=(192+2)*scale).
void MelonInstance::blitAcceleratedFrame(u32 srcArrayTex, u32 dstTex, int dstWidth, int dstHeight)
{
    if (!blitReadFBO) glGenFramebuffers(1, &blitReadFBO);
    if (!blitDrawFBO) glGenFramebuffers(1, &blitDrawFBO);

    int scale = dstWidth / 256;
    if (scale < 1) scale = 1;
    int perScreenH = 192 * scale;
    int gap = 2 * scale;

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, blitDrawFBO);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, blitReadFBO);

    // Top screen (layer 0)
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, srcArrayTex, 0, 0);
    glBlitFramebuffer(0, 0, dstWidth, perScreenH,
                      0, 0, dstWidth, perScreenH,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    // Bottom screen (layer 1)
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, srcArrayTex, 0, 1);
    glBlitFramebuffer(0, 0, dstWidth, perScreenH,
                      0, perScreenH + gap, dstWidth, perScreenH + gap + perScreenH,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
}

bool MelonInstance::loadRom(std::string romPath, std::string sramPath)
{
    unique_ptr<u8[]> romData;
    unique_ptr<u8[]> sramData;
    u32 romFileLength = 0;
    u32 sramFileLength = 0;

    // ROM file loading
    Platform::FileHandle* romFile = Platform::OpenFile(romPath, FileMode::Read);
    if (!romFile)
        return false;

    u64 length = Platform::FileLength(romFile);
    if (length > 0x40000000)
    {
        Platform::CloseFile(romFile);
        return false;
    }

    romFileLength = (u32) length;
    Platform::FileRewind(romFile);
    romData = make_unique<u8[]>(romFileLength);
    size_t nread = Platform::FileRead(romData.get(), (size_t) romFileLength, 1, romFile);
    Platform::CloseFile(romFile);
    if (nread != 1)
    {
        return false;
    }

    // SRAM file loading
    FileHandle* sramFile = Platform::OpenFile(sramPath, FileMode::Read);
    if (!sramFile)
    {
        return false;
    }
    else if (!Platform::CheckFileWritable(sramPath))
    {
        return false;
    }

    sramFileLength = (u32) Platform::FileLength(sramFile);

    FileRewind(sramFile);
    sramData = std::make_unique<u8[]>(sramFileLength);
    FileRead(sramData.get(), sramFileLength, 1, sramFile);
    CloseFile(sramFile);

    NDSCart::NDSCartArgs cartargs{
        // Don't load the SD card itself yet, because we don't know if
        // the ROM is homebrew or not.
        // So this is the card we *would* load if the ROM were homebrew.
        .SDCard = std::nullopt, // getSDCardArgs("DLDI"), // TODO: Re-enable this
        .SRAM = std::move(sramData),
        .SRAMLength = sramFileLength,
    };

    auto cart = NDSCart::ParseROM(std::move(romData), romFileLength, this, std::move(cartargs));
    if (!cart)
    {
        return false;
    }

    nds->SetNDSCart(std::move(cart));
    ndsSave = std::make_unique<SaveManager>(sramPath);

    return true;
}

bool MelonInstance::loadGbaRom(std::string romPath, std::string sramPath)
{
    unique_ptr<u8[]> romData;
    unique_ptr<u8[]> sramData = nullptr;
    u32 romFileLength = 0;
    u32 sramFileLength = 0;

    // ROM file loading
    Platform::FileHandle* romFile = Platform::OpenFile(romPath, FileMode::Read);
    if (!romFile)
        return false;

    u64 length = Platform::FileLength(romFile);
    if (length > 0x40000000)
    {
        Platform::CloseFile(romFile);
        return false;
    }

    romFileLength = length;
    Platform::FileRewind(romFile);
    romData = make_unique<u8[]>(romFileLength);
    size_t nread = Platform::FileRead(romData.get(), (size_t) romFileLength, 1, romFile);
    Platform::CloseFile(romFile);
    if (nread != 1)
    {
        return false;
    }

    FileHandle* saveFile = Platform::OpenFile(sramPath, FileMode::Read);
    if (!saveFile)
    {
        return false;
    }
    else if (!Platform::CheckFileWritable(sramPath))
    {
        return false;
    }

    sramFileLength = (u32) FileLength(saveFile);

    if (sramFileLength > 0)
    {
        FileRewind(saveFile);
        sramData = std::make_unique<u8[]>(sramFileLength);
        FileRead(sramData.get(), sramFileLength, 1, saveFile);
    }
    CloseFile(saveFile);

    auto cart = GBACart::ParseROM(std::move(romData), romFileLength, std::move(sramData), sramFileLength, this);
    if (!cart)
    {
        return false;
    }

    nds->SetGBACart(std::move(cart));
    gbaSave = std::make_unique<SaveManager>(sramPath);

    return true;
}

void MelonInstance::loadRumblePak()
{
    auto rumblePakCart = GBACart::LoadAddon(GBAAddon_RumblePak, this);
    nds->SetGBACart(std::move(rumblePakCart));
}

void MelonInstance::loadGbaMemoryExpansion()
{
    auto memoryExpansionCart = GBACart::LoadAddon(GBAAddon_RAMExpansion, this);
    nds->SetGBACart(std::move(memoryExpansionCart));
}

bool MelonInstance::bootFirmware()
{
    if (nds->NeedsDirectBoot())
        return false;

    return true;
}

void MelonInstance::start()
{
    auto cart = nds->NDSCartSlot.GetCart();
    if (nds->ConsoleType == 1 && cart != nullptr && cart->GetHeader().IsDSiWare() && !currentConfiguration->showBootScreen)
    {
        auto dsi = (DSi*) nds;
        DSiSupport::SetupDSiDirectBoot(dsi);
    }
    else if (!currentConfiguration->showBootScreen || nds->NeedsDirectBoot())
    {
        // This seems to be unused, but it's required
        std::string romName;
        nds->SetupDirectBoot(romName);
    }
    nds->Start();

    screenshotRenderer->init();
}

void MelonInstance::reset()
{
    nds->Reset();
    setBatteryLevels();
    setDateTime();

    // If there is a cart inserted, check if direct boot is required
    if (nds->GetNDSCart())
    {
        if (!currentConfiguration->showBootScreen || nds->NeedsDirectBoot())
        {
            // This seems to be unused, but it's required
            std::string romName;
            nds->SetupDirectBoot(romName);
        }
    }

    rewindManager.Reset();
    retroAchievementsManager->Reset();
    nds->Start();
}

u32 MelonInstance::runFrame()
{
    if (isRenderConfigurationDirty)
    {
        updateRenderer();
        isRenderConfigurationDirty = false;
    }

#ifdef LITEV_AGGRESSIVE_SKIP
    // Runtime-controllable frameskip target (skips rasterisation only; CPU/DMA/
    // timers keep running so gameplay/audio stay full-speed). Default 0.
    // Controllable via: adb shell setprop debug.litev.frameskip <0..3>
    // A UI setting can drive the same GPU::SetFrameskipTarget entry point.
    {
        static int cachedSkip = -1;
        static int checkCounter = 0;
        if (--checkCounter <= 0)
        {
            checkCounter = 30;
            char buf[8] = {0};
            int target = 0;
            if (__system_property_get("debug.litev.frameskip", buf) > 0)
                target = atoi(buf);
            if (target != cachedSkip)
            {
                cachedSkip = target;
                nds->GPU.SetFrameskipTarget(target);
            }
        }
    }
#endif

    int screenWidth;
    int screenHeight;
    if (currentRenderer == Renderer::OpenGl)
    {
        // ScaleFactor is no longer queryable from the unified renderer; use the
        // same value the config feeds into SetRenderSettings.
        int scale = static_cast<OpenGlRenderSettings&>(*currentConfiguration->renderSettings).scale;
        screenWidth = 256 * scale;
        screenHeight = (192 + 1) * scale;
    }
    else if (currentRenderer == Renderer::Compute)
    {
        auto computeRenderSettings = static_cast<ComputeRenderSettings&>(*currentConfiguration->renderSettings);
        int scale = computeRenderSettings.scale;
        screenWidth = 256 * scale;
        screenHeight = (192 + 1) * scale;
    }
    else
    {
        screenWidth = 256;
        screenHeight = 192 + 1;
    }

    double litev_t0 = litevNowMs();

    Frame* renderFrame = frameQueue.getRenderFrame();

    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    // Delete old render fence
    if (renderFrame->renderFence)
    {
        eglDestroySyncKHR(currentDisplay, renderFrame->renderFence);
        renderFrame->renderFence = 0;
    }

    double litev_t_fw0 = litevNowMs();
    // Ensure presentation is finished
    if (renderFrame->presentFence)
    {
        eglWaitSyncKHR(currentDisplay, renderFrame->presentFence, 0);
    }
    double litev_t_fw1 = litevNowMs();

    // Validate frame after ensuring that the frame has finished presenting
    frameQueue.validateRenderFrame(renderFrame, screenWidth, screenHeight * 2);

    // --- GPU timer: read previous frame's TIME_ELAPSED (deferred, non-stalling) ---
    static double litev_gpuMsAccum = 0.0;
    static int    litev_gpuSamples = 0;
    if (litevProfEnabled) litevGpuEnsure();
    if (litevProfEnabled && litevGpuOk) {
        int prev = litevQSlot ^ 1;
        if (litevQPending[prev]) {
            GLuint avail = 0;
            litevGetQObjUIV(litevQ[prev], LITEV_QUERY_RESULT_AVAILABLE, &avail);
            if (avail) {
                GLuint64 ns = 0;
                litevGetQObjUI64(litevQ[prev], LITEV_QUERY_RESULT, &ns);
                litev_gpuMsAccum += ns / 1000000.0;
                litev_gpuSamples++;
                litevQPending[prev] = false;
            }
        }
        litevBeginQuery(LITEV_TIME_ELAPSED, litevQ[litevQSlot]);
    }

    [[unlikely]] if (nds->GPU.GetRenderer().NeedsShaderCompile())
    {
        // Compile all required shaders at once
        do
        {
            int currentShader;
            int shadersCount;
            nds->GPU.GetRenderer().ShaderCompileStep(currentShader, shadersCount);
        }
        while (nds->GPU.GetRenderer().NeedsShaderCompile());
    }

    double litev_t_rf0 = litevNowMs();
    u32 nLines = nds->RunFrame();
    double litev_t_rf1 = litevNowMs();
    retroAchievementsManager->FrameUpdate();

    // Present. Unified renderer API: GetFramebuffers() returns true with RAM
    // pointers (software renderer) or false for accelerated renderers, where
    // *top is a GLuint* handle to a 2-layer GL_TEXTURE_2D_ARRAY (layer 0 = top
    // screen, layer 1 = bottom screen) at scaled resolution.
    void* fbTop = nullptr;
    void* fbBottom = nullptr;
    bool ramFramebuffers = nds->GPU.GetFramebuffers(&fbTop, &fbBottom);
    if (ramFramebuffers)
    {
        if (fbTop && fbBottom)
        {
            glBindTexture(GL_TEXTURE_2D, renderFrame->frameTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, fbTop);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192 + 2, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, fbBottom);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
    }
    else if (fbTop)
    {
        GLuint arrayTex = *(GLuint*) fbTop;
        blitAcceleratedFrame(arrayTex, renderFrame->frameTexture, screenWidth, screenHeight);
    }

    double litev_t_blit1 = litevNowMs();
    if (litevProfEnabled && litevGpuOk) {
        litevEndQuery(LITEV_TIME_ELAPSED);
        litevQPending[litevQSlot] = true;
        litevQSlot ^= 1;
    }

    bool isSleeping = nds->CPUStop & CPUStop_Sleep;
    if (!isSleeping) [[likely]]
    {
        renderFrame->renderFence = eglCreateSyncKHR(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
        glFlush();
        frameQueue.pushRenderedFrame(renderFrame);
    }
    else
    {
        frameQueue.discardRenderedFrame(renderFrame);
    }

    if (ndsSave)
        ndsSave->CheckFlush();

    if (gbaSave)
        gbaSave->CheckFlush();

    if (firmwareSave)
        firmwareSave->CheckFlush();

    frame++;
    bool needsRewindCapture = rewindManager.ShouldCaptureState(frame);
    bool needsScreenshot = screenshotRenderer->isScreenshotPending();

    if (needsRewindCapture || needsScreenshot) [[unlikely]]
        screenshotRenderer->renderScreenshot(&nds->GPU, currentRenderer, renderFrame);

    if (needsRewindCapture)
    {
        auto nextRewindState = rewindManager.GetNextRewindSaveState(frame);
        saveRewindState(nextRewindState);
    }

    double litev_t_end = litevNowMs();
    // Accumulate phase timings and log every 60 frames.
    {
        static double a_fw = 0, a_rf = 0, a_blit = 0, a_other = 0, a_total = 0;
        static int    n = 0;
        static double lastWall = 0;
        double wall = litev_t_end;
        a_fw    += (litev_t_fw1 - litev_t_fw0);
        a_rf    += (litev_t_rf1 - litev_t_rf0);
        a_blit  += (litev_t_blit1 - litev_t_rf1);
        a_other += (litev_t_end - litev_t0) - (litev_t_fw1 - litev_t_fw0)
                   - (litev_t_rf1 - litev_t_rf0) - (litev_t_blit1 - litev_t_rf1);
        a_total += (litev_t_end - litev_t0);
        n++;
        if (n >= 60) {
            litevRefreshProfEnabled();
            if (litevProfEnabled) {
                double wallSpan = (lastWall > 0) ? (wall - lastWall) : 0;
                double gpuAvg = (litev_gpuSamples > 0) ? (litev_gpuMsAccum / litev_gpuSamples) : -1.0;
                LOG_INFO("LITEV_PROF",
                    "60f: cpu_loop=%.2fms (fenceWait=%.2f runFrame=%.2f blit=%.2f other=%.2f) | gpu=%.2fms | wall/frame=%.2fms (%.1f fps)",
                    a_total / n, a_fw / n, a_rf / n, a_blit / n, a_other / n,
                    gpuAvg, wallSpan / n, (wallSpan > 0 ? 60000.0 / wallSpan : 0));
            }
            a_fw = a_rf = a_blit = a_other = a_total = 0;
            litev_gpuMsAccum = 0; litev_gpuSamples = 0;
            n = 0;
            lastWall = wall;
        } else if (lastWall == 0) {
            lastWall = wall;
        }
    }

    return nLines;
}

void MelonInstance::stop()
{
    retroAchievementsManager = nullptr;
    screenshotRenderer->cleanup();
}

void MelonInstance::touchScreen(u16 x, u16 y)
{
    nds->TouchScreen(x, y);
}

void MelonInstance::releaseScreen()
{
    nds->ReleaseScreen();
}

void MelonInstance::pressKey(u32 key)
{
    // Special handling for Lid input
    if (key == 16 + 7)
    {
        nds->SetLidClosed(true);
    }
    else
    {
        inputMask &= ~(1 << key);
        nds->SetKeyMask(inputMask);
    }
}

void MelonInstance::releaseKey(u32 key)
{
    // Special handling for Lid input
    if (key == 16 + 7)
    {
        nds->SetLidClosed(false);
    }
    else
    {
        inputMask |= (1 << key);
        nds->SetKeyMask(inputMask);
    }
}

int MelonInstance::readAudioOutput(s16* buffer, int length)
{
    return nds->SPU.ReadOutput(buffer, length);
}

void MelonInstance::setAudioOutputSkew(double skew)
{
    nds->SPU.SetOutputSkew(skew);
}

bool MelonInstance::takeScreenshot()
{
    return screenshotRenderer->takeScreenshot();
}

void MelonInstance::loadCheats(std::list<Cheat> cheats)
{
    std::vector<ARCode> codeList;

    for (auto cheat : cheats)
    {
        ARCode arCode {
            .Enabled = true,
            .Code = cheat.code,
        };
        codeList.push_back(arCode);
    }

    nds->AREngine.Cheats = codeList;
}

int MelonInstance::sendNetPacket(u8* data, int length)
{
    return net->SendPacket(data, length, instanceId);
}

int MelonInstance::receiveNetPacket(u8* data)
{
    return net->RecvPacket(data, instanceId);
}

Frame* MelonInstance::getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline)
{
    return frameQueue.getPresentFrame(deadline);
}

void MelonInstance::updateConfiguration(std::shared_ptr<EmulatorConfiguration> newConfiguration)
{
    if (nds)
    {
        nds->SPU.SetInterpolation(static_cast<AudioInterpolation>(newConfiguration->audioSettings.audioInterpolation));
        nds->SPU.SetDegrade10Bit(static_cast<AudioBitDepth>(newConfiguration->audioSettings.audioBitrate));
    }

    rewindManager.UpdateRewindSettings(newConfiguration->rewindEnabled, newConfiguration->rewindLengthSeconds, newConfiguration->rewindCaptureSpacingSeconds);

    currentConfiguration = newConfiguration;
    isRenderConfigurationDirty = true;
}

void MelonInstance::requestNdsSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (ndsSave)
        ndsSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

void MelonInstance::requestGbaSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (gbaSave)
        gbaSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

void MelonInstance::requestFirmwareSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (firmwareSave)
        firmwareSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

bool MelonInstance::saveState(Savestate* state)
{
    if (!retroAchievementsManager->DoSavestate(state))
        return false;

    return nds->DoSavestate(state);
}

bool MelonInstance::loadState(Savestate* state)
{
    if (!retroAchievementsManager->DoSavestate(state))
        return false;

    if (nds->DoSavestate(state))
    {
        setBatteryLevels();
        setDateTime();
        return true;
    }
    else
    {
        return false;
    }
}

RewindWindow MelonInstance::getRewindWindow()
{
    return RewindWindow {
        .currentFrame = frame,
        .rewindStates = rewindManager.GetRewindWindow(),
    };
}

bool MelonInstance::loadRewindState(RewindSaveState rewindSaveState)
{
    Savestate* savestate = new Savestate(rewindSaveState.buffer, rewindSaveState.bufferContentSize, false);
    if (savestate->Error)
    {
        delete savestate;
        return false;
    }

    bool result = loadState(savestate);
    if (result)
    {
        frame = rewindSaveState.frame;
        rewindManager.OnRewindFromState(rewindSaveState);
    }

    delete savestate;

    return result;
}

void MelonInstance::setupAchievements(
    std::list<RetroAchievements::RAAchievement> achievements,
    std::list<RetroAchievements::RALeaderboard> leaderboards,
    std::optional<std::string> richPresenceScript
)
{
    if (instanceId == 0)
    {
        retroAchievementsManager->LoadAchievements(achievements);
        retroAchievementsManager->LoadLeaderboards(leaderboards);
        if (richPresenceScript)
            retroAchievementsManager->SetupRichPresence(*richPresenceScript);
    }
}

void MelonInstance::unloadRetroAchievementsData()
{
    retroAchievementsManager->UnloadEverything();
}

std::string MelonInstance::getRichPresenceStatus()
{
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRichPresenceStatus();
    else
        return "";
}

std::vector<RetroAchievements::RARuntimeAchievement> MelonInstance::getRuntimeAchievements()
{
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRuntimeAchievements();
    else
        return { };
}

void MelonInstance::updateRenderer()
{
    Renderer newRenderer = currentConfiguration->renderer;

    // Unified renderer API (upstream GPU rework): a single Renderer owns both
    // the 2D and 3D pipelines. SoftRenderer(nds) / GLRenderer(nds, compute).
    if (newRenderer != currentRenderer)
    {
        switch (newRenderer)
        {
            case Renderer::Software:
                nds->GPU.SetRenderer(std::make_unique<SoftRenderer>(*nds));
                break;
            case Renderer::OpenGl:
                nds->GPU.SetRenderer(std::make_unique<GLRenderer>(*nds, /*compute=*/false));
                break;
            case Renderer::Compute:
                nds->GPU.SetRenderer(std::make_unique<GLRenderer>(*nds, /*compute=*/true));
                break;
            default: __builtin_unreachable();
        }
        currentRenderer = newRenderer;
    }

    RendererSettings settings {};
    switch (newRenderer)
    {
        case Renderer::Software:
        {
            auto softwareRenderSettings = static_cast<SoftwareRenderSettings&>(*currentConfiguration->renderSettings);
            settings.ScaleFactor = 1;
            settings.Threaded = softwareRenderSettings.threadedRendering;
            break;
        }
        case Renderer::OpenGl:
        {
            auto glRenderSettings = static_cast<OpenGlRenderSettings&>(*currentConfiguration->renderSettings);
            settings.ScaleFactor = glRenderSettings.scale;
            settings.BetterPolygons = glRenderSettings.betterPolygons;
            break;
        }
        case Renderer::Compute:
        {
            auto computeRenderSettings = static_cast<ComputeRenderSettings&>(*currentConfiguration->renderSettings);
            settings.ScaleFactor = computeRenderSettings.scale;
            settings.HiresCoordinates = computeRenderSettings.highResCoordinates;
            break;
        }
        default: __builtin_unreachable();
    }
    nds->GPU.GetRenderer().SetRenderSettings(settings);
}

void MelonInstance::setBatteryLevels()
{
    if (consoleType == 1)
    {
        auto dsi = static_cast<DSi*>(nds);
        dsi->I2C.GetBPTWL()->SetBatteryLevel(DSi_BPTWL::batteryLevel_Full);
        dsi->I2C.GetBPTWL()->SetBatteryCharging(false);
    }
    else
    {
        nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
    }
}

void MelonInstance::setDateTime()
{
    std::time_t t = std::time(0);
    std::tm* now = std::localtime(&t);

    nds->RTC.SetDateTime(now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec);
}

void MelonInstance::saveRewindState(RewindSaveState* rewindSaveState)
{
    Savestate* savestate = new Savestate(rewindSaveState->buffer, rewindSaveState->bufferSize, true);
    if (saveState(savestate))
    {
        rewindSaveState->bufferContentSize = savestate->Length();
        memcpy(rewindSaveState->screenshot, screenshotRenderer->getScreenshot(), rewindSaveState->screenshotSize);
    }

    delete savestate;
}

}