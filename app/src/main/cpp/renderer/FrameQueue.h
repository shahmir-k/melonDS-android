#ifndef FRAMEQUEUE_H
#define FRAMEQUEUE_H

#include <array>
#include <chrono>
#include <queue>
#include <atomic>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include "OpenGLContext.h"
#include "types.h"

using namespace melonDS;

// 9 frames should allow the emulator to run up 8x speed. This includes 8 frames ready to present, plus one frame currently being rendered to.
constexpr std::size_t FRAME_QUEUE_SIZE = 9;

// rendered frames never presented (newest-wins drops), all queues (debug.litev.presentlog)
extern std::atomic<int> g_presentDrops;
// emulated frames whose drawing frameskip skipped (never shown)
extern std::atomic<int> g_framesHidden;
// emulated frames skip-repeat did not draw because nothing visible changed (debug.litev.presentlog)
extern std::atomic<int> g_framesRepeat;

struct Frame {
    GLuint frameTexture{};
    u32 width{};
    u32 height{};
    int emuFrame{};     // MelonInstance frame number it shows (record mode footage)
    int64_t readyNs{};  // steady clock when it was pushed for presentation (debug.litev.presentlog)
    EGLSyncKHR renderFence{};
    EGLSyncKHR presentFence{};
    // LITEV_PRESENT_SAME: per screen (top in the high 32 bits, bottom low) the renderer's serial
    // of the merged picture (equal serial = identical pixels); 0 = unknown (always redraw)
    int64_t screenSerials{};
};

class FrameQueue
{
public:
    FrameQueue();
    Frame* getRenderFrame();
    // isNew (optional): false when it returns the frame it already returned last time
    Frame* getPresentFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline, bool* isNew = nullptr);
    void validateRenderFrame(Frame* frame, int requiredWidth, int requiredHeight);
    void pushRenderedFrame(Frame* frame);
    void discardRenderedFrame(Frame* frame);
    void clear();

private:
    std::mutex frameLock;
    std::condition_variable presentFrameReadyCondition;
    std::condition_variable renderFrameFreeCondition;   // a frame went back to a queue getRenderFrame takes from
    std::array<Frame, FRAME_QUEUE_SIZE> frames{};
    std::queue<Frame*> freeQueue{};
    std::deque<Frame*> presentQueue{};
    Frame* previousFrame = nullptr;
};

#endif //FRAMEQUEUE_H
