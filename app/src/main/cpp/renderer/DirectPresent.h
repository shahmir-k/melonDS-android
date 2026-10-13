// LITEV_HYB_DIRECT_PRESENT: the GL renderers' present thread (hyb-present / glhi-present) draws
// the finished frame into the game window itself, right after the merge, instead of handing it
// to the Kotlin FrameRenderThread. Only for what that thread's default screen shader does (no
// what DSRenderer.drawFrame draws (background, screens with the filter's program, text overlay),
// with that thread's programs and textures (one share group); the FrameRenderThread decides
// when it applies and hands the window surface over (debug.litev.directpresent=0: off).
#ifndef DIRECTPRESENT_H
#define DIRECTPRESENT_H

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <vector>

namespace DirectPresent
{
// compiled in and not switched off
bool Available();
// FrameRenderThread: hand over the window surface (EGL_NO_SURFACE: take it back; returns once
// the present thread no longer uses it)
struct Layout
{
    EGLSurface Surface = EGL_NO_SURFACE;
    int Width = 0, Height = 0;
    GLuint Program = 0; GLint Filter = GL_NEAREST;
    std::vector<float> Verts;            // screens: x, y, u, v, alpha
    GLuint BgProgram = 0, BgTexture = 0;
    std::vector<float> BgVerts;          // x, y, u, v
    GLuint OvlProgram = 0, OvlTexture = 0;
    std::vector<float> OvlVerts;         // x, y, u, v
};
void Set(Layout layout);
bool Active();
// present thread, after the frame's merge: draw tex into the window and swap; false: not active
bool Present(GLuint tex);
}

#endif
