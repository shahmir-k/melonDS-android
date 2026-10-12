#include "FrameQueue.h"
#include <Platform.h>
#include <sys/system_properties.h>
#include <cstdlib>

std::atomic<int> g_presentDrops{0};
std::atomic<int> g_framesHidden{0};

FrameQueue::FrameQueue()
{
    for (auto& frame : frames)
    {
        freeQueue.push(&frame);
    }
}

Frame* FrameQueue::getRenderFrame()
{
    std::unique_lock lock(frameLock);

    Frame* frame;

    // Every frame can be in flight in queued async presents (the hybrid renderer's present
    // thread holds them until it hands them over), e.g. when the emulator outruns the present
    // thread with frameskip: wait for one instead of taking a frame from an empty queue.
    renderFrameFreeCondition.wait(lock, [this] { return !freeQueue.empty() || !presentQueue.empty(); });

    // If there are no free frames, use oldest present frame
    if (freeQueue.empty())
    {
        frame = presentQueue.back();
        presentQueue.pop_back();
        g_presentDrops++;
    }
    else
    {
        frame = freeQueue.front();
        freeQueue.pop();
    }

    return frame;
}

Frame* FrameQueue::getPresentFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline, bool* isNew)
{
    std::unique_lock lock(frameLock);
    if (isNew) *isNew = false;

    if (presentQueue.empty()) {
        bool hasNewFrame = false;
        if (deadline.has_value())
            hasNewFrame = presentFrameReadyCondition.wait_until(lock, *deadline, [&]{ return !presentQueue.empty(); });

        if (!hasNewFrame)
            return previousFrame;
    }

    if (previousFrame)
    {
        freeQueue.push(previousFrame);
        previousFrame = nullptr;
    }

    // Show the OLDEST waiting frame and keep at most one more for the next vsync. The emulator
    // paces on its own clock and a frame's work time varies (2-11 ms in PW), so when its finish
    // time sits near the vsync deadline two frames land in one vsync and none in the next:
    // taking the newest (upstream) held one vsync and then dropped a frame, ~1-30 times a
    // second, while the counter read 60. One frame of slack absorbs that jitter (it adds up to
    // one vsync of latency while frames arrive in pairs). Fast-forward / catch-up bursts still
    // drop down to the newest two. debug.litev.presentfifo=0: newest only (upstream).
    static const bool fifo = [] { char b[92] = {}; return !(__system_property_get("debug.litev.presentfifo", b) > 0 && atoi(b) == 0); }();
    const size_t keep = fifo ? 2 : 1;   // presentQueue: front = newest
    while (presentQueue.size() > keep)
    {
        freeQueue.push(presentQueue.back());
        presentQueue.pop_back();
        g_presentDrops++;
    }
    Frame* frame = presentQueue.back();
    presentQueue.pop_back();
    previousFrame = frame;
    renderFrameFreeCondition.notify_one();
    if (isNew) *isNew = true;
    return frame;
}

void FrameQueue::validateRenderFrame(Frame* frame, int requiredWidth, int requiredHeight)
{
    if (frame->width != requiredWidth || frame->height != requiredHeight)
    {
        // Update frame texture to have the required size
        if (!frame->frameTexture)
        {
            glGenTextures(1, &frame->frameTexture);
            glBindTexture(GL_TEXTURE_2D, frame->frameTexture);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        else
        {
            glBindTexture(GL_TEXTURE_2D, frame->frameTexture);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, requiredWidth, requiredHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        frame->width = requiredWidth;
        frame->height = requiredHeight;
    }
}

void FrameQueue::pushRenderedFrame(Frame* frame)
{
    frame->readyNs = std::chrono::steady_clock::now().time_since_epoch().count();
    std::unique_lock lock(frameLock);
    presentQueue.push_front(frame);
    presentFrameReadyCondition.notify_one();
    renderFrameFreeCondition.notify_one();
}

void FrameQueue::discardRenderedFrame(Frame* frame)
{
    std::unique_lock lock(frameLock);
    freeQueue.push(frame);
    renderFrameFreeCondition.notify_one();
}

void FrameQueue::clear()
{
    std::unique_lock lock(frameLock);

    for (auto f : presentQueue)
    {
        freeQueue.push(f);
    }

    presentQueue.clear();
    previousFrame = nullptr;

    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    for (auto& frame : frames) {
        glDeleteTextures(1, &frame.frameTexture);
        eglDestroySyncKHR(currentDisplay, frame.renderFence);
        eglDestroySyncKHR(currentDisplay, frame.presentFence);
        frame.frameTexture = 0;
        frame.width = 0;
        frame.height = 0;
        frame.renderFence = 0;
        frame.presentFence = 0;
    }
}
