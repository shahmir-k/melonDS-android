#ifndef RECORDVIDEO_H
#define RECORDVIDEO_H

#include <string>
#include "FrameQueue.h"

// Record mode footage: the presented frames, scaled to the DS's 256x384 (top over bottom), into
// the hardware H.264 encoder through its input surface. The GPU does the scaling and the encoder
// block the compression, so the emulator threads pay nothing. Runs on the present thread.
// One MP4 per minute (video-<first frame>.mp4); its time t is recording frame first + t * 60.
namespace RecordVideo
{
    // `frame` was just presented; it is recording frame `recFrame` of the recording in `dir`
    void Present(const Frame* frame, int recFrame, const std::string& dir, int fps);
    // no recording: finish the open file, if any
    void Stop();
}

#endif
