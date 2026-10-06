#include <ctime>
#include <chrono>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <sched.h>
#include <dirent.h>
#include <sys/resource.h>
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
#include "GPU_Hybrid.h"
#include "MelonDS.h"
#include "MelonInstance.h"
#include "NDS.h"
#include "NDSCart.h"
#include "net/Net_Slirp.h"
#include "Platform.h"
#include "SDCardArgsBuilder.h"
#include "MelonLog.h"

// ---- liteDS frame-phase profiler + correctness gates (runtime-gated by props) ----
// Enable with:  adb shell setprop debug.litev.prof 1
// Emits a per-60-frame LITEV_PROF logcat line splitting the emulator frame into
// present-fence wait / RunFrame / blit / other, plus a GPU TIME_ELAPSED reading
// on GL renderers. Off by default; then the only cost is a few clock reads per
// frame and a prop read every 60 frames.
#include <GLES2/gl2ext.h>
namespace {
    // Only gates diagnostic logging; atomic so a reader on another thread is not
    // a data race.
    std::atomic_bool litevProfEnabled{false};
    void litevRefreshProfEnabled() {
        char buf[8] = {0};
        litevProfEnabled.store(__system_property_get("debug.litev.prof", buf) > 0 && atoi(buf) != 0,
                               std::memory_order_relaxed);
    }
    inline double litevNowMs() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
    }

    // ===================== FBHASH correctness gate =====================
    // Per-frame framebuffer checksum, logged as LITEV_FBHASH. Enable with:
    //   adb shell setprop debug.litev.fbhash 1
    // Hashes the final composited output of each frame (top + bottom screen), read
    // directly from the renderer, so it works where screencap of the secure
    // hardware-overlay surface does not. Two runs of the same savestate (e.g. flag
    // OFF vs ON) are compared frame by frame; any divergence, even sub-visible,
    // shows up as a hash mismatch.
    int litevFbHashEnabled = -1;
    void litevRefreshFbHashEnabled() {
        char buf[8] = {0};
        litevFbHashEnabled = (__system_property_get("debug.litev.fbhash", buf) > 0 && atoi(buf) != 0) ? 1 : 0;
    }
    // Post-savestate-load frame index. Reset to 0 at each loadState so two runs
    // (each a fresh load of the same savestate) are compared by identical
    // post-load frame numbers. Also queried by setDateTime to decide whether to
    // pin the RTC for reload determinism.
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
    // GL renderers: read+hash both layers of the composited output array texture
    // on the current (emulator) GL context. Restores the prior read FBO.
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
    // SOFTWARE-renderer FBHASH: GetFramebuffers returns RAM pointers to the final
    // composited 256x192 RGBA output (top+bottom), so hash them directly on the CPU --
    // the GL path above only covers accelerated renderers (it reads a GL texture). Same
    // log format, so the same OFF-vs-ON diff harness covers software-render changes.
    void litevFbHashRam(const void* top, const void* bottom, int frameId) {
        const size_t sz = (size_t)256 * 192 * 4;
        uint64_t htop = top    ? litevFnv1a((const uint8_t*)top,    sz) : 0;
        uint64_t hbot = bottom ? litevFnv1a((const uint8_t*)bottom, sz) : 0;
        LOG_INFO("LITEV_FBHASH", "frame=%d top=0x%016llx bot=0x%016llx",
                 frameId, (unsigned long long)htop, (unsigned long long)hbot);
    }
    // Bar-scan (debug.litev.barscan): objective black-bar detector for the TOP software framebuffer
    // (256x192 RGBA). A horizontal black bar = a run of fully-black rows, so we log per frame the
    // count of fully-black rows and the longest consecutive run. On a static scene the baseline is
    // steady; a flickering bar shows as a transient spike in maxrun/blackrows. Also logs blackpix so
    // the pixel-count-jump analysis works directly on the log stream (no secure-display capture).
    int litevBarScanEnabled = -1;
    void litevRefreshBarScan() {
        char buf[8] = {0};
        litevBarScanEnabled = (__system_property_get("debug.litev.barscan", buf) > 0 && atoi(buf) != 0) ? 1 : 0;
    }
    void litevBarScan(const void* top, int frameId) {
        if (!top) return;
        const uint8_t* p = (const uint8_t*)top;   // RGBA 256x192, row-major
        // A "dark row" is >=70% pixels with max(R,G,B) < 32 (catches dark, not just pure-black, and
        // partial-width bars). A horizontal bar = a run of dark rows.
        int darkRows = 0, maxRun = 0, run = 0;
        long darkPix = 0;
        for (int y = 0; y < 192; y++) {
            int darkInRow = 0;
            for (int x = 0; x < 256; x++) {
                const uint8_t* px = p + (((size_t)y * 256 + x) * 4);
                int mx = px[0]; if (px[1] > mx) mx = px[1]; if (px[2] > mx) mx = px[2];
                if (mx < 32) { darkInRow++; darkPix++; }
            }
            if (darkInRow >= 179) { darkRows++; if (++run > maxRun) maxRun = run; }
            else run = 0;
        }
        LOG_INFO("LITEV_BARSCAN", "frame=%d darkrows=%d maxrun=%d darkpix=%ld",
                 frameId, darkRows, maxRun, darkPix);
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
static double litevThreadCpuMs()
{
    timespec ts; clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}
// Core 3 belongs to the emu thread. Measured on the RG DS (Shrek race): the emu thread
// spent ~19 % of its time runnable but preempted (schedstat run-queue wait 1.9 s per 10 s,
// 4.6k involuntary switches/s), mostly by this process's own unpinned threads (Mali driver
// backend at nice -10, binder, dispatchers, audio). Every 300 frames: move every other
// thread of the process to cores 0-2 (new threads appear over time), and pin the emu
// thread to core 3 at nice -10. The emu pin is re-asserted each time, not set once:
// a cpuset move (Android reassigns the app's cgroup) resets the thread's affinity, and
// a recreated emu thread starts unpinned. debug.litev.core3=0 disables it (A/B).
static void litevKeepCore3()
{
    static int ctr = 0, on = -1;
    if (on < 0)
    {
        char b[PROP_VALUE_MAX] = {0};
        on = (__system_property_get("debug.litev.core3", b) > 0) ? atoi(b) != 0 : 1;
    }
    if (!on || ctr-- > 0) return;
    ctr = 300;
    const pid_t self = gettid();
    cpu_set_t core3; CPU_ZERO(&core3); CPU_SET(3, &core3);
    sched_setaffinity(0, sizeof(core3), &core3);
    setpriority(PRIO_PROCESS, self, -10);
    cpu_set_t others; CPU_ZERO(&others);
    CPU_SET(0, &others); CPU_SET(1, &others); CPU_SET(2, &others);
    if (DIR* d = opendir("/proc/self/task"))
    {
        while (dirent* e = readdir(d))
        {
            pid_t tid = atoi(e->d_name);
            if (tid > 0 && tid != self)
                sched_setaffinity(tid, sizeof(others), &others);
        }
        closedir(d);
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
    // an async hybrid present on the GL 3D thread still uses frameQueue
    if (auto* hybrid = dynamic_cast<HybridRenderer*>(&nds->GPU.GetRenderer())) hybrid->WaitPresent();
    if (auto* gl = dynamic_cast<GLRenderer*>(&nds->GPU.GetRenderer())) gl->WaitPresent();
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

#ifdef LITEV_AGGRESSIVE_SKIP
// Called from the emu loop (same thread as runFrame) by the adaptive frameskip
// controller and the fast-forward frameskip. A later change of debug.litev.frameskip still overrides it.
void MelonInstance::setFrameskipTarget(int target)
{
    if (nds)
        nds->GPU.SetFrameskipTarget(target);
}
#endif

u32 MelonInstance::runFrame()
{
    // Keep core 3 for the emulator thread (runFrame always runs on it): pins it to
    // core 3 and the process's other threads to cores 0-2, re-asserted every 300 frames.
    litevKeepCore3();

    if (isRenderConfigurationDirty)
    {
        updateRenderer();
        isRenderConfigurationDirty = false;
    }

#ifdef LITEV_AGGRESSIVE_SKIP
    // Runtime frameskip target (skips 2D/3D rasterisation only; CPU, DMA and
    // timers keep running, so gameplay and audio stay full speed). Default 0 =
    // no skip. Polled every 30 frames from: adb shell setprop debug.litev.frameskip <n>
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

    // presentation size: from the scale the renderer was actually configured with
    int screenWidth = 256 * currentScale;
    int screenHeight = (192 + 1) * currentScale;

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

    double litev_c_rf0 = litevThreadCpuMs();
    double litev_t_rf0 = litevNowMs();
    u32 nLines = nds->RunFrame();
    double litev_t_rf1 = litevNowMs();
    double litev_c_rf1 = litevThreadCpuMs();
    retroAchievementsManager->FrameUpdate();

    // Present. Unified renderer API: GetFramebuffers() returns true with RAM
    // pointers (software renderer) or false for accelerated renderers, where
    // *top is a GLuint* handle to a 2-layer GL_TEXTURE_2D_ARRAY (layer 0 = top
    // screen, layer 1 = bottom screen) at scaled resolution.
    void* fbTop = nullptr;
    void* fbBottom = nullptr;
    auto* hybrid = currentRenderer == Renderer::OpenGl ? dynamic_cast<HybridRenderer*>(&nds->GPU.GetRenderer()) : nullptr;
    bool ramFramebuffers = false;
    bool presentedAsync = false;
    if (hybrid)
    {
        // the hybrid merges straight into the frame texture (no output array + blit), on its
        // GL 3D thread: that thread also waits for the frame's previous presentation, then
        // fences the merge and hands the frame to the presenter
        const bool sleeping = nds->CPUStop & CPUStop_Sleep;
        hybrid->PresentIntoAsync(renderFrame->frameTexture, (192 + 2) * currentScale,
            [renderFrame, currentDisplay] {
                if (renderFrame->presentFence) eglWaitSyncKHR(currentDisplay, renderFrame->presentFence, 0);
            },
            [this, renderFrame, currentDisplay, sleeping] {
                if (sleeping) { frameQueue.discardRenderedFrame(renderFrame); return; }
                renderFrame->renderFence = eglCreateSyncKHR(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
                glFlush();
                frameQueue.pushRenderedFrame(renderFrame);
            });
        presentedAsync = true;
    }
    else
        ramFramebuffers = nds->GPU.GetFramebuffers(&fbTop, &fbBottom);
    if (ramFramebuffers)
    {
        if (fbTop && fbBottom)
        {
            glBindTexture(GL_TEXTURE_2D, renderFrame->frameTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, fbTop);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192 + 2, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, fbBottom);
            glBindTexture(GL_TEXTURE_2D, 0);

            // FBHASH / BARSCAN gates for the software renderer (RAM composite).
            {
                static int checkCtr = 0;
                if (--checkCtr <= 0) { checkCtr = 30; litevRefreshFbHashEnabled(); litevRefreshBarScan(); }
                int fid = litevFbHashFrame++;
                if (litevFbHashEnabled == 1)
                    litevFbHashRam(fbTop, fbBottom, fid);
                if (litevBarScanEnabled == 1)
                    litevBarScan(fbTop, fid);
            }
        }
    }
    else if (fbTop)
    {
        GLuint arrayTex = *(GLuint*) fbTop;
        if (auto* gl = dynamic_cast<GLRenderer*>(&nds->GPU.GetRenderer()))
        {
            // copied into the frame texture on the renderer's present thread, which also
            // waits for the frame's previous presentation and hands the frame off
            const bool sleeping = nds->CPUStop & CPUStop_Sleep;
            gl->PresentIntoAsync(renderFrame->frameTexture, screenWidth,
                [renderFrame, currentDisplay] {
                    if (renderFrame->presentFence) eglWaitSyncKHR(currentDisplay, renderFrame->presentFence, 0);
                },
                [this, renderFrame, currentDisplay, sleeping] {
                    if (sleeping) { frameQueue.discardRenderedFrame(renderFrame); return; }
                    renderFrame->renderFence = eglCreateSyncKHR(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
                    glFlush();
                    frameQueue.pushRenderedFrame(renderFrame);
                });
            presentedAsync = true;
        }
        else
            blitAcceleratedFrame(arrayTex, renderFrame->frameTexture, screenWidth, screenHeight);

        // FBHASH gate for the GL renderers (reads the output array texture).
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
    double litev_c_blit1 = litevThreadCpuMs();
    if (litevProfEnabled && litevGpuOk) {
        litevEndQuery(LITEV_TIME_ELAPSED);
        litevQPending[litevQSlot] = true;
        litevQSlot ^= 1;
    }

    bool isSleeping = nds->CPUStop & CPUStop_Sleep;
    if (presentedAsync) {}   // handed off on the GL 3D thread
    else if (!isSleeping) [[likely]]
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
    {
        if (hybrid) hybrid->WaitPresent();   // the frame texture is written on another thread
        if (auto* gl = dynamic_cast<GLRenderer*>(&nds->GPU.GetRenderer())) gl->WaitPresent();
        screenshotRenderer->renderScreenshot(&nds->GPU, currentRenderer, renderFrame);
    }

    if (needsRewindCapture)
    {
        auto nextRewindState = rewindManager.GetNextRewindSaveState(frame);
        saveRewindState(nextRewindState);
    }

    double litev_t_end = litevNowMs();
    // Frame-phase profiler: accumulate and log one LITEV_PROF line per 60 frames
    // when debug.litev.prof=1 (re-read every 60 frames). `submit` is kept in the
    // format for log-parser compatibility; it is always 0 without a render thread.
    {
        static double a_fw = 0, a_rf = 0, a_blit = 0, a_other = 0, a_total = 0, c_rf = 0, c_blit = 0;
        c_rf += litev_c_rf1 - litev_c_rf0;
        c_blit += litev_c_blit1 - litev_c_rf1;
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
                    "60f: cpu_loop=%.2fms (fenceWait=%.2f runFrame=%.2f submit=%.2f blit=%.2f other=%.2f) | thread-cpu runFrame=%.2f blit=%.2f | gpu=%.2fms | wall/frame=%.2fms (%.1f fps)",
                    a_total / n, a_fw / n, a_rf / n, 0.0, a_blit / n, a_other / n, c_rf / n, c_blit / n,
                    gpuAvg, wallSpan / n, (wallSpan > 0 ? 60000.0 / wallSpan : 0));
            }
            a_fw = a_rf = a_blit = a_other = a_total = c_rf = c_blit = 0;
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

    std::atomic_store(&currentConfiguration, newConfiguration);
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
        // FBHASH gate: restart the per-frame index at the load point so two runs
        // from the same savestate are aligned at frame 0.
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
    // updateConfiguration swaps the pointer on the JNI thread
    std::shared_ptr<EmulatorConfiguration> config = std::atomic_load(&currentConfiguration);
    const Renderer configRenderer = config->renderer;
    Renderer newRenderer = configRenderer;

    // A/B overrides (dev props, read when the renderer configuration changes / at launch):
    //   debug.litev.software=1   force the software renderer
    //   debug.litev.renderer=N   force renderer N (0 software, 1 OpenGL hybrid, 2 compute,
    //                            3 OpenGL hi-res 2D)
    //   debug.litev.glscale=N    force the GL internal scale
    char prop[PROP_VALUE_MAX] = {0};
    if (__system_property_get("debug.litev.renderer", prop) > 0)
    {
        int r = atoi(prop);
        if (r >= 0 && r <= 3) newRenderer = static_cast<Renderer>(r);
    }
    if (__system_property_get("debug.litev.software", prop) > 0 && atoi(prop) != 0)
        newRenderer = Renderer::Software;

    // renderSettings is typed by the CONFIGURED renderer; never cast it as another one
    const bool configIsGl = configRenderer == Renderer::OpenGl || configRenderer == Renderer::OpenGlHiRes;
    RendererSettings settings {};
    switch (newRenderer)
    {
        case Renderer::Software:
        {
            settings.ScaleFactor = 1;
            settings.Threaded = true;
            if (configRenderer == Renderer::Software)
            {
                auto& sw = static_cast<SoftwareRenderSettings&>(*config->renderSettings);
                settings.Threaded = sw.threadedRendering;
                settings.Accurate3D = sw.accurate3d;
            }
            if (__system_property_get("debug.litev.softthread", prop) > 0)
                settings.Threaded = atoi(prop) != 0;
            break;
        }
        case Renderer::OpenGl:
        case Renderer::OpenGlHiRes:
        {
            settings.ScaleFactor = 1;
            if (configIsGl)
            {
                auto& gl = static_cast<OpenGlRenderSettings&>(*config->renderSettings);
                settings.ScaleFactor = gl.scale;
                settings.BetterPolygons = gl.betterPolygons;
            }
            break;
        }
        case Renderer::Compute:
        {
            settings.ScaleFactor = 1;
            if (configRenderer == Renderer::Compute)
            {
                auto& cs = static_cast<ComputeRenderSettings&>(*config->renderSettings);
                settings.ScaleFactor = cs.scale;
                settings.HiresCoordinates = cs.highResCoordinates;
            }
            break;
        }
        default: __builtin_unreachable();
    }
    if (newRenderer != Renderer::Software && __system_property_get("debug.litev.glscale", prop) > 0)
    {
        int s = atoi(prop);
        if (s >= 1 && s <= 8) settings.ScaleFactor = s;
    }
    if (settings.ScaleFactor < 1) settings.ScaleFactor = 1;

    // Unified renderer API (upstream GPU rework): a single Renderer owns both the
    // 2D and 3D pipelines.
    if (newRenderer != currentRenderer)
    {
        switch (newRenderer)
        {
            case Renderer::Software:
                nds->GPU.SetRenderer(std::make_unique<SoftRenderer>(*nds));
                break;
            case Renderer::OpenGl:
                nds->GPU.SetRenderer(std::make_unique<HybridRenderer>(*nds));
                break;
            case Renderer::OpenGlHiRes:
                nds->GPU.SetRenderer(std::make_unique<GLRenderer>(*nds, /*compute=*/false));
                break;
            case Renderer::Compute:
                nds->GPU.SetRenderer(std::make_unique<GLRenderer>(*nds, /*compute=*/true));
                break;
            default: __builtin_unreachable();
        }
        currentRenderer = newRenderer;
    }
    nds->GPU.GetRenderer().SetRenderSettings(settings);
    currentScale = settings.ScaleFactor;
    LOG_INFO("LITEV_RENDERER", "renderer=%d scale=%d threaded=%d accurate3d=%d",
             (int)currentRenderer, currentScale, (int)settings.Threaded, (int)settings.Accurate3D);
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
    // FBHASH gate: the real-time clock is the one per-load nondeterminism source
    // that survives a savestate load (SetDateTime runs AFTER DoSavestate). Game
    // content seeded from the RTC (RNG, time-of-day lighting) would differ on every
    // reload and make runs incomparable, so pin the RTC while the gate is on.
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