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
EGLSurface Surface = EGL_NO_SURFACE;
std::vector<float> Verts;
bool Linear = false;
int Width = 0, Height = 0;
std::atomic<bool> On {false};
unsigned Gen = 0;   // layout generation: the present thread re-uploads its vertex buffer

// per present-thread GL objects (the thread's context; one present thread per session)
GLuint Prog = 0, Vbo = 0, Vao = 0;
GLint LocPos = -1, LocUv = -1, LocAlpha = -1, LocTex = -1;
unsigned UploadedGen = ~0u;
std::atomic<bool> Broken {false};

// ShaderProgramSource.NoFilterShader / LinearShader (the filter is the sampler state)
const char* kVS =
    "attribute vec2 vUV;\n"
    "attribute vec2 vPos;\n"
    "attribute float vAlpha;\n"
    "varying vec2 uv;\n"
    "varying float alpha;\n"
    "void main()\n"
    "{\n"
    "    gl_Position = vec4(vPos, 0.0, 1.0);\n"
    "    uv = vUV;\n"
    "    alpha = vAlpha;\n"
    "}";
const char* kFS =
    "precision mediump float;\n"
    "uniform sampler2D tex;\n"
    "varying vec2 uv;\n"
    "varying float alpha;\n"
    "void main()\n"
    "{\n"
    "    vec4 color = texture2D(tex, uv);\n"
    "    gl_FragColor = vec4(color.bgr, alpha);\n"
    "}";

GLuint Compile(GLenum type, const char* src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    return ok ? s : 0;
}

bool Setup()
{
    if (Prog) return true;
    GLuint vs = Compile(GL_VERTEX_SHADER, kVS), fs = Compile(GL_FRAGMENT_SHADER, kFS);
    if (!vs || !fs) return false;
    Prog = glCreateProgram();
    glAttachShader(Prog, vs); glAttachShader(Prog, fs);
    glLinkProgram(Prog);
    glDeleteShader(vs); glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(Prog, GL_LINK_STATUS, &ok);
    if (!ok) return false;
    LocPos = glGetAttribLocation(Prog, "vPos");
    LocUv = glGetAttribLocation(Prog, "vUV");
    LocAlpha = glGetAttribLocation(Prog, "vAlpha");
    LocTex = glGetUniformLocation(Prog, "tex");
    glGenBuffers(1, &Vbo);
    glGenVertexArrays(1, &Vao);
    return true;
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

void Set(EGLSurface surface, const float* vertices, int count, bool linear, int width, int height)
{
    std::lock_guard<std::mutex> l(M);   // waits for a present in progress
    Surface = surface;
    Verts.assign(vertices ? vertices : nullptr, vertices ? vertices + count * 5 : nullptr);
    Linear = linear; Width = width; Height = height;
    Gen++;
    On.store(surface != EGL_NO_SURFACE && count > 0, std::memory_order_release);
}

bool Active() { return On.load(std::memory_order_acquire); }

bool Present(GLuint tex)
{
    if (!On.load(std::memory_order_acquire) || Broken) return false;
    std::lock_guard<std::mutex> l(M);
    if (Surface == EGL_NO_SURFACE || Verts.empty()) return false;
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLContext ctx = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    if (!eglMakeCurrent(dpy, Surface, Surface, ctx))
    {
        __android_log_print(ANDROID_LOG_ERROR, "melonDS", "LITEV_DIRECTPRESENT: window not usable here (EGL 0x%x): off", eglGetError());
        Broken = true;
        eglMakeCurrent(dpy, draw, read, ctx);
        return false;
    }
    static bool interval = false;
    if (!interval) { eglSwapInterval(dpy, 0); interval = true; }   // as the FrameRenderThread: no vsync wait
    if (!Setup()) { Broken = true; eglMakeCurrent(dpy, draw, read, ctx); return false; }
    glBindVertexArray(Vao);
    glBindBuffer(GL_ARRAY_BUFFER, Vbo);
    if (UploadedGen != Gen)
    {
        glBufferData(GL_ARRAY_BUFFER, Verts.size() * sizeof(float), Verts.data(), GL_STATIC_DRAW);
        glEnableVertexAttribArray(LocPos); glEnableVertexAttribArray(LocUv); glEnableVertexAttribArray(LocAlpha);
        glVertexAttribPointer(LocPos, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
        glVertexAttribPointer(LocUv, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(2 * sizeof(float)));
        glVertexAttribPointer(LocAlpha, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(4 * sizeof(float)));
        UploadedGen = Gen;
    }
    // what DSRenderer.drawFrame does for this case
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, Width, Height);
    glDisable(GL_SCISSOR_TEST); glDisable(GL_DEPTH_TEST); glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(Prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindSampler(0, 0);
    const GLint f = Linear ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glUniform1i(LocTex, 0);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)(Verts.size() / 5));
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    eglSwapBuffers(dpy, Surface);
    eglMakeCurrent(dpy, draw, read, ctx);
    return true;
}
}
