// LITEV_HYB_DIRECT_PRESENT: the GL renderers' present thread (hyb-present / glhi-present) draws
// the finished frame into the game window itself, right after the merge, instead of handing it
// to the Kotlin FrameRenderThread. Only for what that thread's default screen shader does (no
// filter or linear filter, no background, no text overlay); the FrameRenderThread decides when
// it applies and hands the window surface over (debug.litev.directpresent=0: off).
#ifndef DIRECTPRESENT_H
#define DIRECTPRESENT_H

#include <EGL/egl.h>
#include <GLES3/gl3.h>

namespace DirectPresent
{
// compiled in and not switched off
bool Available();
// FrameRenderThread: hand over the window surface (EGL_NO_SURFACE: take it back; returns once
// the present thread no longer uses it). vertices: 5 floats per vertex (x, y, u, v, alpha)
void Set(EGLSurface surface, const float* vertices, int count, bool linear, int width, int height);
bool Active();
// present thread, after the frame's merge: draw tex into the window and swap; false: not active
bool Present(GLuint tex);
}

#endif
