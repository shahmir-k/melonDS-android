#include <ctime>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <sys/system_properties.h>
#include <sched.h>
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
#include "OpenGLContext.h"
#include "NDS.h"
#include "NDSCart.h"
#include "net/Net_Slirp.h"
#include "Platform.h"
#include "SDCardArgsBuilder.h"
#include "MelonLog.h"

// liteDS-v2: pin the calling thread to a dedicated CPU core. Without this the
// scheduler may co-locate the hot emu + render threads on one A55 core, so R4's
// "overlap" degrades to time-slicing on a single core (render busy ~doubles under
// overlap — measured). DraStic pins its emu/render/raster threads (teardown 07).
static void litevPinThread(int cpu)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

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

    // ===================== R4 FBHASH correctness gate =====================
    // Per-frame framebuffer checksum. Enable with:
    //   adb shell setprop debug.litev.fbhash 1
    // After the final composite/blit (glSubmitPresent, render-thread context), we
    // glReadPixels the renderer's final output array texture (layer 0 = top screen,
    // layer 1 = bottom screen) and hash the RGBA bytes. This reads the GL framebuffer
    // DIRECTLY, so it works on the hardware-overlay SurfaceView that screencap can't
    // capture. For a deterministic scene the SERIAL (rtserial=1) and THREADED (rt=1)
    // per-frame hashes must be identical (under the fixed pipeline offset). Any
    // divergence is a render-thread data race — including sub-visible corruption.
    int litevFbHashEnabled = -1;
    void litevRefreshFbHashEnabled() {
        char buf[8] = {0};
        litevFbHashEnabled = (__system_property_get("debug.litev.fbhash", buf) > 0 && atoi(buf) != 0) ? 1 : 0;
    }
    // Post-savestate-load frame index. Reset to 0 at each loadState so the SERIAL
    // and THREADED runs (each a fresh load of the same deterministic savestate) are
    // compared by identical post-load frame numbers (offset 0). Also queried by
    // setDateTime to decide whether to pin the RTC for reload determinism.
    int  litevFbHashFrame = 0;
    void litevFbHashResetFrame() { litevFbHashFrame = 0; }
    bool litevFbHashOn() {
        if (litevFbHashEnabled < 0) litevRefreshFbHashEnabled();
        return litevFbHashEnabled == 1;
    }
    inline uint64_t litevFnv1a(const uint8_t* p, size_t n) {
        uint64_t h = 1469598103934665603ULL;      // FNV offset basis
        for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628257ULL; }
        return h;
    }
    // Read+hash both layers of the composited output array texture. Runs on the GL
    // context that owns the framebuffer (render thread). Restores prior read-FBO.
    void litevFbHash(GLuint arrayTex, int w, int perScreenH, int frameId) {
        static GLuint fbo = 0;
        if (!fbo) glGenFramebuffers(1, &fbo);
        GLint prevRead = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        size_t sz = (size_t)w * (size_t)perScreenH * 4;
        static std::vector<uint8_t> buf;
        if (buf.size() < sz) buf.resize(sz);
        uint64_t htop = 0, hbot = 0;
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arrayTex, 0, 0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glReadPixels(0, 0, w, perScreenH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
            htop = litevFnv1a(buf.data(), sz);
        }
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, arrayTex, 0, 1);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
            glReadPixels(0, 0, w, perScreenH, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
            hbot = litevFnv1a(buf.data(), sz);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, prevRead);
        LOG_INFO("LITEV_FBHASH", "frame=%d top=0x%016llx bot=0x%016llx",
                 frameId, (unsigned long long)htop, (unsigned long long)hbot);
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
#ifdef LITEV_RENDER_THREAD
    stopRenderThread();
#endif
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
    blitAcceleratedFrameFBO(srcArrayTex, dstTex, dstWidth, dstHeight, blitReadFBO, blitDrawFBO);
}

void MelonInstance::blitAcceleratedFrameFBO(u32 srcArrayTex, u32 dstTex, int dstWidth, int dstHeight, u32& blitReadFBO, u32& blitDrawFBO)
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
#ifdef LITEV_RENDER_THREAD
    // Drain the render thread so it is not reading emulation/renderer state while
    // Reset clears the geometry banks / re-derives renderer state (design §5.2).
    drainRenderThread();
#endif
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
#ifdef LITEV_RENDER_THREAD
    // Pin the emu thread to a dedicated core (once), separate from the render
    // thread's core (2), so RunFrame and the GL SubmitFrame run truly parallel.
    { static bool _pinned = false; if (!_pinned) { litevPinThread(3); _pinned = true; } }

    // Decide the thread topology once (design §6) and start the render thread
    // BEFORE the first updateRenderer, so the renderer's per-context GL objects
    // (FBOs/VAOs — not shared across EGL contexts on this Mali driver) are created
    // on the render thread's context, where SubmitFrame later uses them.
    if (!rtTopologyDecided) { rtUse = renderThreadWanted(); rtTopologyDecided = true; }
    if (rtUse) startRenderThread();
#endif

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

#ifdef LITEV_RENDER_THREAD
    // ===================== R4 STEP 3: render-thread offload =====================
    // The render thread is the sole GL-context owner. This (emu) thread runs the
    // GL-free NDS::RunFrame and hands off a depth-1 packet; the render thread
    // replays the frame's GL submission + blit + present. Frames whose GL must be
    // serialized on the render context (capture, screenshot/rewind) are dispatched
    // there synchronously.
    if (rtUse)
    {
        // Capture frames raster inline INSIDE RunFrame, so their RunFrame must run
        // on the render context. Predict via the previous frame (capture comes in
        // bursts). Screenshot/rewind frames need a freshly-rendered frame for the
        // capture, so they run their submit (and the screenshot) on the render
        // thread too — but RunFrame itself is GL-free for them, so it stays here.
        bool predictCapture = rtCapturePrev;
        bool willScreenshot = rewindManager.ShouldCaptureState(frame + 1)
                              || screenshotRenderer->isScreenshotPending();

        double litev_g0 = 0, litev_g1 = 0, litev_t_rf0 = 0, litev_t_rf1 = 0;
        u32 nLines;

        if (predictCapture)
        {
            // Whole frame on the render thread: RunFrame (inline capture GL) + submit.
            int sw = screenWidth, sh = screenHeight;
            litev_t_rf0 = litevNowMs();
            dispatchToRenderThread([this, sw, sh]{
                rtNLines = nds->RunFrame();
                int bank = nds->GPU.GetLogBuildBank();
                bool deferred = nds->GPU.IsDeferredSubmit();
                bool sleeping = (nds->CPUStop & CPUStop_Sleep) != 0;
                glSubmitPresent(bank, deferred, sleeping, sw, sh, true);
            });
            litev_t_rf1 = litevNowMs();
            nLines = rtNLines;
            rtCapturePrev = nds->GPU.WasCaptureActiveThisFrame();
        }
        else
        {
            // Depth-1 gate (design §4.2): wait until the render thread has released
            // the previously-published job's bank (early release — which fires AFTER
            // BuildPolygons, i.e. after the whole 2D command log was replayed AND the
            // texcache GetTexture reads, so this RunFrame cannot race the geometry
            // bank, the 2D config members, or the texcache Cache the replay reads).
            litev_g0 = litevNowMs();
            {
                // DIAGNOSTIC: debug.litev.rtserial=1 => wait for the render thread to
                // fully finish each frame (no overlap) to distinguish an overlap race
                // from a render-context correctness bug. Default (0) = depth-1 early
                // release.
                static int serialMode = -1;
                if (serialMode < 0) {
                    char b[8] = {0};
                    serialMode = (__system_property_get("debug.litev.rtserial", b) > 0 && atoi(b) != 0) ? 1 : 0;
                }
                std::unique_lock<std::mutex> lk(rtMutex);
                if (serialMode)
                    rtDoneCond.wait(lk, [this]{ return (!rtBusy && !rtHasJob) || rtStop; });
                else
                    rtDoneCond.wait(lk, [this]{ return rtBanksReleased == rtJobsPublished || rtStop; });
            }
            litev_g1 = litevNowMs();

            litev_t_rf0 = litevNowMs();
            nLines = nds->RunFrame();          // GL-free (non-capture)
            litev_t_rf1 = litevNowMs();

            bool captured = nds->GPU.WasCaptureActiveThisFrame();
            bool sleeping = (nds->CPUStop & CPUStop_Sleep) != 0;
            int  bank     = nds->GPU.GetLogBuildBank();
            bool deferred = nds->GPU.IsDeferredSubmit();
            rtCapturePrev = captured;

            if (captured || willScreenshot)
            {
                // Dispatch the submit (and any screenshot) to the render context.
                int sw = screenWidth, sh = screenHeight;
                bool shot = willScreenshot;
                dispatchToRenderThread([this, bank, deferred, sleeping, sw, sh, shot]{
                    Frame* f = glSubmitPresent(bank, deferred, sleeping, sw, sh, true);
                    if (shot) screenshotRenderer->renderScreenshot(&nds->GPU, currentRenderer, f);
                });
            }
            else
            {
                // Publish the depth-1 packet; the render thread does the GL.
                std::unique_lock<std::mutex> lk(rtMutex);
                rtJobBank      = bank;
                rtJobDeferred  = deferred;
                rtJobSleeping  = sleeping;
                rtJobScreenW   = screenWidth;
                rtJobScreenH   = screenHeight;
                rtHasJob       = true;
                rtBusy         = true;
                rtJobsPublished++;
                lk.unlock();
                rtJobCond.notify_one();
            }
        }

        // ---- emu-thread tail (no GL): achievements, saves, rewind savestate ----
        retroAchievementsManager->FrameUpdate();
        if (ndsSave)      ndsSave->CheckFlush();
        if (gbaSave)      gbaSave->CheckFlush();
        if (firmwareSave) firmwareSave->CheckFlush();

        frame++;
        if (rewindManager.ShouldCaptureState(frame))
        {
            auto nextRewindState = rewindManager.GetNextRewindSaveState(frame);
            saveRewindState(nextRewindState);
        }

        double litev_t_end = litevNowMs();
        {
            static double a_rf = 0, a_gate = 0, a_total = 0;
            static int    n = 0;
            static double lastWall = 0;
            a_rf    += (litev_t_rf1 - litev_t_rf0);
            a_gate  += (litev_g1 - litev_g0);
            a_total += (litev_t_end - litev_t0);
            n++;
            if (n >= 60) {
                litevRefreshProfEnabled();
                if (litevProfEnabled) {
                    double wallSpan = (lastWall > 0) ? (litev_t_end - lastWall) : 0;
                    LOG_INFO("LITEV_PROF",
                        "60f THREADED: cpu_loop=%.2fms (gate=%.2f runFrame=%.2f) | wall/frame=%.2fms (%.1f fps)",
                        a_total / n, a_gate / n, a_rf / n,
                        wallSpan / n, (wallSpan > 0 ? 60000.0 / wallSpan : 0));
                }
                a_rf = a_gate = a_total = 0;
                n = 0;
                lastWall = litev_t_end;
            } else if (lastWall == 0) {
                lastWall = litev_t_end;
            }
        }
        return nLines;
    }
#endif

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
                if (litev_gpuSamples >= 60) {
                    LOG_INFO("LITEV_GPU", "gpu_hw=%.2fms/frame", litev_gpuMsAccum / litev_gpuSamples);
                    litev_gpuMsAccum = 0.0; litev_gpuSamples = 0;
                }
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
    double litev_t_submit0 = litevNowMs();
#ifdef LITEV_RENDER_THREAD
    // R4 submit phase (single-thread this tranche): replay the frame's deferred
    // GL submission on the emu thread, right after RunFrame and before the frame
    // texture is read below. No-op unless the renderer is in deferred mode and
    // the frame actually deferred (non-capture). This is the seam a later tranche
    // moves onto a dedicated render thread.
    if (nds->GPU.IsDeferredSubmit())
        nds->GPU.SubmitFrame();
#endif
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

        // FBHASH gate on the INLINE path (renderthread=0 => non-deferred direct render,
        // the ground-truth flag-OFF reference; also renderthread=1 rtUse=false fallback).
        {
            static int checkCtr = 0;
            if (--checkCtr <= 0) { checkCtr = 30; litevRefreshFbHashEnabled(); }
            int fid = litevFbHashFrame++;
            if (litevFbHashEnabled == 1) {
                int scale = screenWidth / 256; if (scale < 1) scale = 1;
                litevFbHash(arrayTex, screenWidth, 192 * scale, fid);
            }
        }
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
        static double a_submit = 0;
        static int    n = 0;
        static double lastWall = 0;
        double wall = litev_t_end;
        a_fw    += (litev_t_fw1 - litev_t_fw0);
        a_rf    += (litev_t_submit0 - litev_t_rf0);
        a_submit += (litev_t_rf1 - litev_t_submit0);
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
                    "60f: cpu_loop=%.2fms (fenceWait=%.2f runFrame=%.2f submit=%.2f blit=%.2f other=%.2f) | gpu=%.2fms | wall/frame=%.2fms (%.1f fps)",
                    a_total / n, a_fw / n, a_rf / n, a_submit / n, a_blit / n, a_other / n,
                    gpuAvg, wallSpan / n, (wallSpan > 0 ? 60000.0 / wallSpan : 0));
#ifdef LITEV_RENDER_THREAD
                // R4 RIR counter proof (recipe §8): replay = converted GL sites
                // routed through record+replay; inlineGL = converted sites forced
                // inline (should be 0 in RIR mode). Cumulative since emu start.
                LOG_INFO("LITEV_RIR", "rir: replayed=%llu inlineGL=%llu",
                    (unsigned long long) nds->GPU.GetRIRReplayCount(),
                    (unsigned long long) nds->GPU.GetRIRInlineGL());
                // R4 Phase 3 prep-decomposition (TEMP): per-frame ms inside RunFrame
                // spent in VRAM flatten (MakeVRAMFlat_*) vs config compute
                // (UpdateLayerConfig/UpdateScanlineConfig/UpdateOAM). Delta of the
                // cumulative core ns counters across this 60-frame window / n.
                {
                    static unsigned long long prevFlat = 0, prevCfg = 0, prev2D = 0, prevSpr = 0;
                    unsigned long long curFlat = (unsigned long long) nds->GPU.GetPrepFlattenNs();
                    unsigned long long curCfg  = (unsigned long long) nds->GPU.GetPrepCfgNs();
                    unsigned long long cur2D   = (unsigned long long) nds->GPU.GetPrep2DNs();
                    unsigned long long curSpr  = (unsigned long long) nds->GPU.GetPrepSpritesNs();
                    double flatMs = (double)(curFlat - prevFlat) / 1.0e6 / n;
                    double cfgMs  = (double)(curCfg  - prevCfg)  / 1.0e6 / n;
                    double d2dMs  = (double)(cur2D   - prev2D)   / 1.0e6 / n;
                    double sprMs  = (double)(curSpr  - prevSpr)  / 1.0e6 / n;
                    LOG_INFO("LITEV_PREP",
                        "prep: full2D=%.2fms (sprites=%.2f) flatten=%.2fms cfg=%.2fms (of runFrame=%.2fms)",
                        d2dMs, sprMs, flatMs, cfgMs, a_rf / n);
                    prevFlat = curFlat;
                    prevCfg  = curCfg;
                    prev2D   = cur2D;
                    prevSpr  = curSpr;
                }
#endif
            }
            a_fw = a_rf = a_blit = a_other = a_total = 0;
            a_submit = 0;
            litev_gpuMsAccum = 0; litev_gpuSamples = 0;
            n = 0;
            lastWall = wall;
        } else if (lastWall == 0) {
            lastWall = wall;
        }
    }

    return nLines;
}

#ifdef LITEV_RENDER_THREAD
// ======================= R4 STEP 3: render-thread offload =======================

bool MelonInstance::renderThreadActive()
{
    // The render thread offloads only the accelerated deferred-submit path.
    // IsDeferredSubmit() already encodes the debug.litev.renderthread property
    // (applied at updateRenderer). Software renderer / RIR / flag-off => false.
    return (currentRenderer == Renderer::OpenGl || currentRenderer == Renderer::Compute)
           && nds->GPU.IsDeferredSubmit();
}

bool MelonInstance::renderThreadWanted()
{
    // Topology decision (design §6), taken once before the first updateRenderer.
    if (!(currentConfiguration->renderer == Renderer::OpenGl
          || currentConfiguration->renderer == Renderer::Compute))
        return false;
    char buf[8] = {0};
    bool on = true;   // default ON when the flag is compiled in
    if (__system_property_get("debug.litev.renderthread", buf) > 0)
        on = (atoi(buf) != 0);
    return on;
}

void MelonInstance::dispatchToRenderThread(std::function<void()> fn)
{
    if (!rtStarted)
    {
        // No render thread (software / flag-off / not yet started): run inline on
        // whatever GL context is current on this thread.
        fn();
        return;
    }
    std::unique_lock<std::mutex> lk(rtMutex);
    // Drain any in-flight offload frame so the task has exclusive use of the context.
    rtDoneCond.wait(lk, [this]{ return (!rtBusy && !rtHasJob && !rtTaskPending) || rtStop; });
    if (rtStop) { lk.unlock(); fn(); return; }
    rtTask = std::move(fn);
    rtTaskPending = true;
    rtTaskDone = false;
    lk.unlock();
    rtJobCond.notify_one();
    lk.lock();
    rtDoneCond.wait(lk, [this]{ return rtTaskDone || rtStop; });
}

void MelonInstance::startRenderThread()
{
    if (rtStarted) return;
    rtStarted = true;
    rtStop = false;
    rtHasJob = false;
    rtBusy = false;
    rtJobsPublished = 0;
    rtBanksReleased = 0;
    rtReleasePending = false;
    renderThread = std::thread([this]{ renderThreadLoop(); });
}

void MelonInstance::stopRenderThread()
{
    if (!rtStarted) return;
    {
        std::unique_lock<std::mutex> lk(rtMutex);
        rtStop = true;
    }
    rtJobCond.notify_all();
    rtDoneCond.notify_all();
    if (renderThread.joinable()) renderThread.join();
    rtStarted = false;
}

void MelonInstance::onBankReleased()
{
    // r4-fix: invoked from glSubmitPresent (render thread) right AFTER SubmitFrame()
    // returns — every emu-state read (geometry, 2D config replay, texture VRAM) is done,
    // so the emu thread may resume frame N+1 while the GPU blit/present runs. rtMutex is
    // NOT held by the caller; rtReleasePending is render-thread-owned (set at job pickup)
    // so this never collides with the emu, and no-ops on the dispatched task paths.
    std::unique_lock<std::mutex> lk(rtMutex);
    if (rtReleasePending)
    {
        rtReleasePending = false;
        rtBanksReleased++;
        lk.unlock();
        rtDoneCond.notify_all();
    }
}

void MelonInstance::drainRenderThread()
{
    if (!rtStarted) return;
    std::unique_lock<std::mutex> lk(rtMutex);
    rtDoneCond.wait(lk, [this]{ return (!rtBusy && !rtHasJob) || rtStop; });
}

void MelonInstance::renderThreadLoop()
{
    // Own GL context in the emu context's share group + a private pbuffer. All
    // melonDS GL objects (renderer FBOs/VAOs/textures/shaders, FrameQueue frame
    // textures) live in the share group and are visible on both contexts.
    renderGlContext = new OpenGLContext();
    bool ok = renderGlContext->InitContext((long) MelonDSAndroid::openGlContext->GetContext())
              && renderGlContext->Use();
    if (!ok)
    {
        LOG_ERROR("LITEV_RT", "render thread failed to create/current shared GL context");
        renderGlContext->DeInit(); delete renderGlContext; renderGlContext = nullptr;
        // Wake any emu-thread waiter so the depth-1 gate can't deadlock on a dead
        // render thread; the frame(s) already published simply go unrendered.
        std::unique_lock<std::mutex> lk(rtMutex);
        rtStop = true; rtBusy = false; rtHasJob = false;
        rtBanksReleased = rtJobsPublished;   // balance the depth-1 gate
        lk.unlock();
        rtDoneCond.notify_all();
        return;
    }

    // r4-fix: EARLY vs DELAYED bank release, prop-gated (debug.litev.earlyrelease).
    //
    // DELAYED (default, prop=0): the emu thread is released explicitly in glSubmitPresent
    // AFTER nds->GPU.SubmitFrame() returns — i.e. after ALL emu-state reads complete
    // (geometry, 2D config replay, texture VRAM). Only the GPU blit/present overlaps emu
    // frame N+1. Guaranteed race-free, ~40-45fps cooled.
    //
    // EARLY (prop=1): the geometry bank is released from ReplayLog right after
    // RenderFrameBodyGeometry/BuildPolygons has consumed all geometry into render-private
    // VBOs (design §4.2). The emu thread then overlaps the 2D command-log replay + 3D
    // raster + blit/present — the ~55fps overlap win. This is now SAFE because:
    //   - geometry: RenderSceneChunk reads only render-private snapshots (3e6dac43);
    //   - 2D config: RIRReplay stages LayerConfig/ScanlineConfig/SpriteConfig from the
    //     REPLAY-bank log arena into render-private Rpl.* copies (STEP A, ed8062d8);
    //   - texture VRAM + render registers: STEP-2 A/B banked to the replay bank;
    //   - log arena: ReplaySrc() reads the replay bank, emu records the other (4492c0e1).
    // The FBHASH gate proves threaded==serial before this is enabled by default.
    bool earlyRelease = true;   // default ON: FBHASH-gate-proven race-free (threaded==serial
                                // 444/444, ==flag-OFF golden 397/397). Overridable for A/B.
    {
        char b[8] = {0};
        if (__system_property_get("debug.litev.earlyrelease", b) > 0)
            earlyRelease = (atoi(b) != 0);
    }
    if (earlyRelease)
        nds->GPU.SetBankReleaseCallback([this]{ onBankReleased(); });
    else
        nds->GPU.SetBankReleaseCallback(nullptr);
    // Re-init the screenshot renderer on THIS context so renderScreenshot (dispatched
    // here for rewind/user screenshots) uses render-context FBO/VAO objects.
    screenshotRenderer->init();

    // Pin the render thread to a dedicated core so it runs truly parallel to the
    // emu thread (pinned to a different core in runFrame), not time-sliced with it.
    litevPinThread(2);

    for (;;)
    {
        int bank; bool deferred; bool sleeping; int sw; int sh;
        std::function<void()> task;
        auto _rtw0 = std::chrono::steady_clock::now();   // start idle-wait
        {
            std::unique_lock<std::mutex> lk(rtMutex);
            rtJobCond.wait(lk, [this]{ return rtHasJob || rtTaskPending || rtStop; });
            if (rtStop) break;
            if (rtTaskPending)
            {
                rtTaskPending = false;
                task = std::move(rtTask);
                lk.unlock();
                task();                     // GL closure on the render context
                lk.lock();
                rtTaskDone = true;
                lk.unlock();
                rtDoneCond.notify_all();
                continue;
            }
            rtHasJob = false;          // consume the depth-1 slot
            rtReleasePending = true;   // this job's bank release not yet counted
            bank = rtJobBank; deferred = rtJobDeferred; sleeping = rtJobSleeping;
            sw = rtJobScreenW; sh = rtJobScreenH;
        }

        auto _rtw1 = std::chrono::steady_clock::now();   // job picked up (idle ended)
        glSubmitPresent(bank, deferred, sleeping, sw, sh, /*onRenderThread*/true);
        {
            auto _rtb1 = std::chrono::steady_clock::now();
            static double aIdle = 0, aBusy = 0; static int rn = 0;
            aIdle += std::chrono::duration_cast<std::chrono::nanoseconds>(_rtw1 - _rtw0).count() / 1e6;
            aBusy += std::chrono::duration_cast<std::chrono::nanoseconds>(_rtb1 - _rtw1).count() / 1e6;
            if (++rn >= 60) {
                LOG_INFO("LITEV_RT", "render: idle=%.2fms busy=%.2fms (cycle=%.2fms)",
                         aIdle / 60, aBusy / 60, (aIdle + aBusy) / 60);
                aIdle = aBusy = 0; rn = 0;
            }
        }

        // Guarantee the bank release is counted even for frames with no 3D record
        // (RenderFrameIdentical / zero polygons never call the early-release hook),
        // and mark the frame fully done.
        {
            std::unique_lock<std::mutex> lk(rtMutex);
            if (rtReleasePending) { rtReleasePending = false; rtBanksReleased++; }
            rtBusy = false;
            lk.unlock();
            rtDoneCond.notify_all();
        }
    }

    nds->GPU.SetBankReleaseCallback(nullptr);
    renderGlContext->DeInit();
    delete renderGlContext;
    renderGlContext = nullptr;
}

Frame* MelonInstance::glSubmitPresent(int bank, bool deferred, bool sleeping, int screenWidth, int screenHeight, bool onRenderThread)
{
    auto _sp0 = std::chrono::steady_clock::now();
    Frame* renderFrame = frameQueue.getRenderFrame();

    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    if (renderFrame->renderFence)
    {
        eglDestroySyncKHR(currentDisplay, renderFrame->renderFence);
        renderFrame->renderFence = 0;
    }
    if (renderFrame->presentFence)
        eglWaitSyncKHR(currentDisplay, renderFrame->presentFence, 0);
    auto _sp1 = std::chrono::steady_clock::now();  // after getRenderFrame + presentFence wait

    frameQueue.validateRenderFrame(renderFrame, screenWidth, screenHeight * 2);

    // Shaders compile lazily on the thread that owns the GL context (design §5.7).
    [[unlikely]] if (nds->GPU.GetRenderer().NeedsShaderCompile())
    {
        do
        {
            int currentShader, shadersCount;
            nds->GPU.GetRenderer().ShaderCompileStep(currentShader, shadersCount);
        }
        while (nds->GPU.GetRenderer().NeedsShaderCompile());
    }

    // Replay the deferred GL command log for the published bank. Under the render
    // thread the packet carries the bank the emu thread recorded into; the emu
    // thread is meanwhile recording bank 1-r for the next frame.
    if (deferred)
    {
        nds->GPU.SetSubmitReplayBank(bank);
        nds->GPU.SubmitFrame();
    }

    // r4-fix: release the emu thread HERE — after SubmitFrame() has finished every read
    // of emu-owned state (geometry, the 2D config replay, the texture VRAM). Everything
    // below (GetFramebuffers, blit, present, fbhash readback) touches only render-thread
    // GL objects, never emu state, so emu frame N+1 may now run concurrently with the
    // GPU blit/present. onBankReleased() no-ops for the dispatched capture/screenshot
    // paths (rtReleasePending is only set on the depth-1 packet path).
    onBankReleased();
    // Kick the GPU to start the just-submitted 3D raster + 2D composite NOW, so the
    // blit below (which reads that output) waits less for GPU completion. debug.litev.
    // flushaftersubmit=1 to A/B this.
    { static int _fa = -1; if (_fa < 0) { char b[8]={0}; _fa = (__system_property_get("debug.litev.flushaftersubmit", b) > 0 && atoi(b)!=0) ? 1 : 0; } if (_fa) glFlush(); }
    {
        auto _sp2 = std::chrono::steady_clock::now();
        static double aWait = 0, aSub = 0; static int sn = 0;
        aWait += std::chrono::duration_cast<std::chrono::nanoseconds>(_sp1 - _sp0).count() / 1e6;
        aSub  += std::chrono::duration_cast<std::chrono::nanoseconds>(_sp2 - _sp1).count() / 1e6;
        if (++sn >= 60) {
            LOG_INFO("LITEV_SUBMIT", "60f: presentWait=%.2fms submitToRelease=%.2fms",
                     aWait / 60, aSub / 60);
            aWait = aSub = 0; sn = 0;
        }
    }

    auto _bl0 = std::chrono::steady_clock::now();
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
        // DIAGNOSTIC: debug.litev.noblit=1 skips the blit (garbage output) to measure
        // whether the ~9ms blit is separable eliminable GPU cost vs an inseparable wait.
        static int _nb = -1; if (_nb < 0) { char b[8]={0}; _nb = (__system_property_get("debug.litev.noblit", b) > 0 && atoi(b)!=0) ? 1 : 0; }
        if (!_nb) {
        if (onRenderThread)
            blitAcceleratedFrameFBO(arrayTex, renderFrame->frameTexture, screenWidth, screenHeight, rtBlitReadFBO, rtBlitDrawFBO);
        else
            blitAcceleratedFrameFBO(arrayTex, renderFrame->frameTexture, screenWidth, screenHeight, blitReadFBO, blitDrawFBO);
        }

        // FBHASH gate: hash the final composited output (both screens). One line/frame.
        // frameId is a monotonic render counter — both rtserial=1 and rt=1 render every
        // emulated frame exactly once in order, so the sequences align (offset ~0).
        {
            static int checkCtr = 0;
            if (--checkCtr <= 0) { checkCtr = 30; litevRefreshFbHashEnabled(); }
            int fid = litevFbHashFrame++;
            if (litevFbHashEnabled == 1) {
                int scale = screenWidth / 256; if (scale < 1) scale = 1;
                litevFbHash(arrayTex, screenWidth, 192 * scale, fid);
                LOG_INFO("LITEV_FBHASH", "  diag frame=%d bank=%d deferred=%d", fid, bank, (int)deferred);
            }
        }
    }

    auto _bl1 = std::chrono::steady_clock::now();   // after blit
    if (!sleeping) [[likely]]
    {
        renderFrame->renderFence = eglCreateSyncKHR(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
        glFlush();
        frameQueue.pushRenderedFrame(renderFrame);
    }
    else
    {
        frameQueue.discardRenderedFrame(renderFrame);
    }
    {
        auto _bl2 = std::chrono::steady_clock::now();
        static double aBlit = 0, aPush = 0; static int bn = 0;
        aBlit += std::chrono::duration_cast<std::chrono::nanoseconds>(_bl1 - _bl0).count() / 1e6;
        aPush += std::chrono::duration_cast<std::chrono::nanoseconds>(_bl2 - _bl1).count() / 1e6;
        if (++bn >= 60) {
            LOG_INFO("LITEV_BLIT", "60f: blit=%.2fms fence+push=%.2fms", aBlit / 60, aPush / 60);
            aBlit = aPush = 0; bn = 0;
        }
    }
    return renderFrame;
}
#endif // LITEV_RENDER_THREAD

void MelonInstance::stop()
{
#ifdef LITEV_RENDER_THREAD
    stopRenderThread();
#endif
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
#ifdef LITEV_RENDER_THREAD
    drainRenderThread();   // §5.2: no render read during savestate serialization
#endif
    if (!retroAchievementsManager->DoSavestate(state))
        return false;

    return nds->DoSavestate(state);
}

bool MelonInstance::loadState(Savestate* state)
{
#ifdef LITEV_RENDER_THREAD
    drainRenderThread();   // §5.2: no render read while emulation state is replaced
#endif
    if (!retroAchievementsManager->DoSavestate(state))
        return false;

    if (nds->DoSavestate(state))
    {
        setBatteryLevels();
        setDateTime();
        // FBHASH gate: restart the per-frame index at the load point so a SERIAL and
        // a THREADED run (each a fresh load of the same deterministic savestate) are
        // aligned at frame=0 with no offset guessing.
        litevFbHashResetFrame();
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
    // DIAGNOSTIC: debug.litev.software=1 forces the software renderer (native, threaded)
    // to gate M6.6 feasibility — measure its raw speed vs the GL renderer's 3x cost.
    { char _sw[8] = {0}; if (__system_property_get("debug.litev.software", _sw) > 0 && atoi(_sw) != 0)
        newRenderer = Renderer::Software; }
    bool swap = (newRenderer != currentRenderer);

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
            // DIAGNOSTIC: debug.litev.scale overrides internal resolution (1..8) to test
            // whether the render/SubmitFrame cost is GPU-fragment-bound (resolution) vs CPU.
            { char _sb[8] = {0}; if (__system_property_get("debug.litev.scale", _sb) > 0) {
                int _s = atoi(_sb); if (_s >= 1 && _s <= 8) settings.ScaleFactor = _s; } }
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

    // Unified renderer API (upstream GPU rework): a single Renderer owns both the
    // 2D and 3D pipelines.
    auto createAndConfigureRenderer = [this, newRenderer, swap, &settings]{
        if (swap)
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
#ifdef LITEV_RENDER_THREAD
            // Re-register the EARLY-release callback (SetRenderer nulled it — the root
            // cause of the wasted overlap). MEASURED: the render thread is IDLE ~17.6ms/
            // frame (busy only 14.2ms) — it is NOT the bottleneck. The 8ms gate is the emu
            // waiting for the FULL SubmitFrame under delayed release; early release lets the
            // emu proceed after RenderFrameBodyGeometry (~0.8ms) so the 2D replay + 3D raster
            // overlap emu frame N+1. Gated on debug.litev.earlyrelease (default ON here).
            {
                char _erb[8] = {0};
                bool _er = true;
                if (__system_property_get("debug.litev.earlyrelease", _erb) > 0)
                    _er = (atoi(_erb) != 0);
                nds->GPU.SetBankReleaseCallback(_er ? std::function<void()>([this]{ onBankReleased(); })
                                                    : std::function<void()>(nullptr));
            }
#endif
        }
        nds->GPU.GetRenderer().SetRenderSettings(settings);
    };
#ifdef LITEV_RENDER_THREAD
    // Create/replace the renderer and upload settings ON THE RENDER CONTEXT — its GL
    // objects (FBOs/VAOs) are per-context and are consumed there by SubmitFrame.
    // dispatchToRenderThread drains the render thread first (so it is not mid-
    // SubmitFrame against the renderer being replaced) and runs inline when there is
    // no render thread (software / renderthread=0).
    dispatchToRenderThread(createAndConfigureRenderer);
#else
    createAndConfigureRenderer();
#endif
    if (swap) currentRenderer = newRenderer;

#ifdef LITEV_RENDER_THREAD
    // R4 (docs/r4-render-thread-design.md §6): select deferred submission once at
    // renderer (re)creation. Accelerated renderers only; the software renderer
    // issues no GL and is a no-op. Runtime toggle: `debug.litev.renderthread`
    // (default 1 when the flag is compiled in). Toggling takes effect on the next
    // renderer re-init, which is exactly when this runs.
    if (newRenderer == Renderer::OpenGl || newRenderer == Renderer::Compute)
    {
        char litevbuf[8] = {0};
        bool deferOn = true;
        if (__system_property_get("debug.litev.renderthread", litevbuf) > 0)
            deferOn = (atoi(litevbuf) != 0);
        nds->GPU.SetDeferredSubmit(deferOn);

        // R4 RIR (recipe §8): route converted per-scanline GL sites through the
        // command log with immediate replay. Independent of deferred submit; used
        // to prove the record/replay plumbing bit-exact on device. Default off.
        char rirbuf[8] = {0};
        bool rirOn = false;
        if (__system_property_get("debug.litev.rir", rirbuf) > 0)
            rirOn = (atoi(rirbuf) != 0);
        nds->GPU.SetRIRMode(rirOn);

        // R4 decisive-split (TEMP): debug.litev.norender=1 skips ALL GL render
        // calls (emulation/SPU/events still run) so runFrame reads the true in-app
        // UNMOVABLE emulation floor. Applied at renderer (re)init = game load,
        // after the protocol sets the prop. Default off.
        char nrbuf[8] = {0};
        bool nrOn = false;
        if (__system_property_get("debug.litev.norender", nrbuf) > 0)
            nrOn = (atoi(nrbuf) != 0);
        nds->GPU.SetLitevNoRender(nrOn);
        // (The bank-release callback is bound inside the render-thread dispatch
        // above, on the freshly created renderer.)
    }
#endif
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
    // FBHASH gate: the real-time clock is the one per-load nondeterminism source that
    // survives a savestate load (SetDateTime is called AFTER DoSavestate). Any game
    // content seeded from the RTC (RNG, time-of-day lighting) would then differ every
    // reload and make serial-vs-threaded incomparable. When the gate is on, pin the
    // RTC to a fixed instant so reloads of the same savestate are bit-deterministic.
    if (litevFbHashOn())
    {
        nds->RTC.SetDateTime(2026, 1, 1, 0, 0, 0);
        return;
    }

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