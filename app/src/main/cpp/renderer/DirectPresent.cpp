#include "DirectPresent.h"
#include <mutex>
#include <vector>
#include <atomic>
#include <cstdlib>
#include <sys/system_properties.h>
#include <android/log.h>

namespace DirectPresent
{
namespace
{
std::mutex M;
Layout L;
std::atomic<bool> On {false};
std::atomic<bool> Broken {false};
unsigned Gen = 0;   // layout generation: the present thread re-uploads its vertex buffers

// the present thread's own vertex arrays (not shared between contexts)
struct Mesh { GLuint Vao = 0, Vbo = 0; GLsizei N = 0; GLuint Prog = 0; };
Mesh Screens, Bg, Ovl;
unsigned UploadedGen = ~0u;

// vertices with pos(2) uv(2) [alpha(1)] into m, attributes as the program names them
void Upload(Mesh& m, GLuint prog, const std::vector<float>& v, int stride)
{
    m.Prog = prog;
    m.N = (GLsizei)(v.size() / stride);
    if (!prog || v.empty()) return;
    if (!m.Vao) { glGenVertexArrays(1, &m.Vao); glGenBuffers(1, &m.Vbo); }
    glBindVertexArray(m.Vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.Vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    const GLint pos = glGetAttribLocation(prog, "vPos"), uv = glGetAttribLocation(prog, "vUV"), al = glGetAttribLocation(prog, "vAlpha");
    if (pos >= 0) { glEnableVertexAttribArray(pos); glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)0); }
    if (uv >= 0) { glEnableVertexAttribArray(uv); glVertexAttribPointer(uv, 2, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(2 * sizeof(float))); }
    if (al >= 0 && stride == 5) { glEnableVertexAttribArray(al); glVertexAttribPointer(al, 1, GL_FLOAT, GL_FALSE, stride * sizeof(float), (void*)(4 * sizeof(float))); }
    glBindVertexArray(0);
}

void Draw(const Mesh& m, GLuint tex)
{
    if (!m.Prog || !m.N) return;
    glUseProgram(m.Prog);
    glUniform1i(glGetUniformLocation(m.Prog, "tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(m.Vao);
    glDrawArrays(GL_TRIANGLES, 0, m.N);
}
}

bool Available()
{
#ifdef LITEV_HYB_DIRECT_PRESENT
    static const bool on = [] { char b[PROP_VALUE_MAX] = {}; return !(__system_property_get("debug.litev.directpresent", b) > 0 && atoi(b) == 0); }();
    return on && !Broken;   // a window the present thread could not use: back to the FrameRenderThread
#else
    return false;
#endif
}

void Set(Layout layout)
{
    std::lock_guard<std::mutex> l(M);   // waits for a present in progress
    const bool on = layout.Surface != EGL_NO_SURFACE && layout.Program && !layout.Verts.empty();
    L = std::move(layout);
    Gen++;
    On.store(on, std::memory_order_release);
}

bool Active() { return On.load(std::memory_order_acquire); }

bool Present(GLuint tex)
{
    if (!On.load(std::memory_order_acquire) || Broken) return false;
    std::lock_guard<std::mutex> l(M);
    if (L.Surface == EGL_NO_SURFACE) return false;
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(dpy, L.Surface, L.Surface, ctx))
    {
        __android_log_print(ANDROID_LOG_ERROR, "melonDS", "LITEV_DIRECTPRESENT: window not usable here (EGL 0x%x): off", eglGetError());
        Broken = true;
        eglMakeCurrent(dpy, draw, read, ctx);
        return false;
    }
    static bool logged = false;
    if (!logged) { __android_log_print(ANDROID_LOG_INFO, "melonDS", "LITEV_DIRECTPRESENT: on"); logged = true; }
    eglSwapInterval(dpy, 0);   // as the FrameRenderThread: no vsync wait (per surface; cheap)
    if (UploadedGen != Gen)
    {
        Upload(Screens, L.Program, L.Verts, 5);
        Upload(Bg, L.BgProgram, L.BgVerts, 4);
        Upload(Ovl, L.OvlProgram, L.OvlVerts, 4);
        UploadedGen = Gen;
    }
    // DSRenderer.drawFrame
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, L.Width, L.Height);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_STENCIL_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    glBindSampler(0, 0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    Draw(Bg, L.BgTexture);   // its shader writes alpha 1
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, L.Filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, L.Filter);
    Draw(Screens, tex);
    if (Ovl.Prog)
    {
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);   // premultiplied text
        Draw(Ovl, L.OvlTexture);
    }
    glDisable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(0);
    eglSwapBuffers(dpy, L.Surface);
    eglMakeCurrent(dpy, draw, read, ctx);
    return true;
}
}
