#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__   // AMediaCodec input surfaces are API 26+; checked at run time
#include "RecordVideo.h"
#include <fcntl.h>
#include <unistd.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/native_window.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkMediaMuxer.h>
#include "../MelonLog.h"

namespace RecordVideo
{

static constexpr int kWidth = 256, kHeight = 384;
static constexpr int kBitrate = 200000;            // ~1.5 MB a minute
static constexpr int kSegmentFrames = 3600;        // one file a minute

struct Encoder
{
    AMediaCodec* codec = nullptr;
    AMediaMuxer* muxer = nullptr;
    ANativeWindow* window = nullptr;
    EGLSurface surface = EGL_NO_SURFACE;
    int fd = -1;
    ssize_t track = -1;
    int firstFrame = 0, lastFrame = -1;
    std::string dir;
};

static Encoder enc;
static bool unavailable = false;    // the device can't do it: don't retry every frame
static GLuint program = 0, vao = 0, sampler = 0;
static GLint uScale = -1;
static PFNEGLPRESENTATIONTIMEANDROIDPROC presentationTime = nullptr;

static void drain(bool endOfStream)
{
    for (int spins = 0; spins < 200;)
    {
        AMediaCodecBufferInfo info;
        ssize_t i = AMediaCodec_dequeueOutputBuffer(enc.codec, &info, endOfStream ? 10000 : 0);
        if (i == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
        {
            AMediaFormat* format = AMediaCodec_getOutputFormat(enc.codec);
            enc.track = AMediaMuxer_addTrack(enc.muxer, format);
            AMediaFormat_delete(format);
            AMediaMuxer_start(enc.muxer);
            continue;
        }
        if (i < 0)
        {
            if (!endOfStream) return;
            spins++;
            continue;
        }
        size_t size;
        uint8_t* data = AMediaCodec_getOutputBuffer(enc.codec, i, &size);
        if (data && info.size > 0 && enc.track >= 0 && !(info.flags & AMEDIACODEC_BUFFER_FLAG_CODEC_CONFIG))
            AMediaMuxer_writeSampleData(enc.muxer, enc.track, data + info.offset, &info);
        AMediaCodec_releaseOutputBuffer(enc.codec, i, false);
        if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) return;
    }
}

void Stop()
{
    if (!enc.codec) return;
    if (__builtin_available(android 26, *)) AMediaCodec_signalEndOfInputStream(enc.codec);
    drain(true);
    if (enc.track >= 0) AMediaMuxer_stop(enc.muxer);
    AMediaMuxer_delete(enc.muxer);
    AMediaCodec_stop(enc.codec);
    AMediaCodec_delete(enc.codec);
    EGLDisplay display = eglGetCurrentDisplay();
    if (enc.surface != EGL_NO_SURFACE) eglDestroySurface(display, enc.surface);
    if (enc.window) ANativeWindow_release(enc.window);
    close(enc.fd);
    enc = Encoder {};
}

static bool setupGl()
{
    if (program) return true;
    const char* vs = "#version 300 es\nin vec2 p; out vec2 uv;\nvoid main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }";
    // the frame texture holds the top screen in rows 0..192 and the bottom one from row 194
    // (in DS pixels; uScale = texture size in DS pixels)
    const char* fs = "#version 300 es\nprecision mediump float; in vec2 uv; out vec4 color; uniform sampler2D tex; uniform vec2 uScale;\n"
                     "void main() { float y = (1.0 - uv.y) * 384.0; y = y < 192.0 ? y : y + 2.0;\n"
                     "  color = vec4(texture(tex, vec2(uv.x * 256.0, y) / uScale).rgb, 1.0); }";
    auto compile = [](GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        return s;
    };
    program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(program, 0, "p");
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) { program = 0; return false; }
    uScale = glGetUniformLocation(program, "uScale");
    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    GLuint vbo;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    presentationTime = (PFNEGLPRESENTATIONTIMEANDROIDPROC) eglGetProcAddress("eglPresentationTimeANDROID");
    return true;
}

static bool start(const std::string& dir, int firstFrame, int fps)
{
    if (!__builtin_available(android 26, *)) return false;
    std::string path = dir + "/video-" + std::to_string(firstFrame) + ".mp4";
    enc.fd = open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (enc.fd < 0) return false;
    enc.dir = dir;
    enc.firstFrame = firstFrame;
    enc.muxer = AMediaMuxer_new(enc.fd, AMEDIAMUXER_OUTPUT_FORMAT_MPEG_4);
    enc.codec = AMediaCodec_createEncoderByType("video/avc");
    AMediaFormat* format = AMediaFormat_new();
    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, kWidth);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, kHeight);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_BIT_RATE, kBitrate);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_I_FRAME_INTERVAL, 2);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_COLOR_FORMAT, 0x7F000789);  // COLOR_FormatSurface
    bool ok = enc.muxer && enc.codec
           && AMediaCodec_configure(enc.codec, format, nullptr, nullptr, AMEDIACODEC_CONFIGURE_FLAG_ENCODE) == AMEDIA_OK
           && AMediaCodec_createInputSurface(enc.codec, &enc.window) == AMEDIA_OK
           && AMediaCodec_start(enc.codec) == AMEDIA_OK;
    AMediaFormat_delete(format);
    if (ok)
    {
        EGLDisplay display = eglGetCurrentDisplay();
        EGLContext context = eglGetCurrentContext();
        EGLint id = 0, n = 0;
        EGLConfig config = nullptr;
        eglQueryContext(display, context, EGL_CONFIG_ID, &id);
        const EGLint byId[] = { EGL_CONFIG_ID, id, EGL_NONE };
        const EGLint recordable[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                                      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RECORDABLE_ANDROID, 1, EGL_NONE };
        if (!(id > 0 && eglChooseConfig(display, byId, &config, 1, &n) && n == 1))
            eglChooseConfig(display, recordable, &config, 1, &n);
        enc.surface = n == 1 ? eglCreateWindowSurface(display, config, enc.window, nullptr) : EGL_NO_SURFACE;
        ok = enc.surface != EGL_NO_SURFACE;
    }
    if (!ok)
    {
        LOG_ERROR("RecordVideo", "no hardware video for %s", path.c_str());
        Stop();
        unlink(path.c_str());
    }
    return ok;
}

void Present(const Frame* frame, int recFrame, const std::string& dir, int fps)
{
    if (unavailable || !frame) return;
    if (enc.codec && (dir != enc.dir || recFrame < enc.lastFrame || recFrame - enc.firstFrame >= kSegmentFrames))
        Stop();
    if (enc.codec && recFrame - enc.lastFrame < 60 / fps) return;   // e.g. 30 fps: every other frame
    if (!enc.codec && !(setupGl() && start(dir, recFrame, fps)))
    {
        unavailable = true;
        return;
    }
    enc.lastFrame = recFrame;

    EGLDisplay display = eglGetCurrentDisplay();
    EGLContext context = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW), read = eglGetCurrentSurface(EGL_READ);
    GLint prevProgram, prevVao, prevTex, prevActive, prevSampler, prevFbo, viewport[4];
    glGetIntegerv(GL_CURRENT_PROGRAM, &prevProgram);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prevVao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prevActive);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_SAMPLER_BINDING, &prevSampler);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_VIEWPORT, viewport);

    eglMakeCurrent(display, enc.surface, enc.surface, context);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, kWidth, kHeight);
    glUseProgram(program);
    glUniform2f(uScale, 256.0f, frame->height * 256.0f / frame->width);
    glBindVertexArray(vao);
    glBindTexture(GL_TEXTURE_2D, frame->frameTexture);
    glBindSampler(0, sampler);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (presentationTime) presentationTime(display, enc.surface, (EGLnsecsANDROID) (recFrame - enc.firstFrame) * 1000000000LL / 60);
    eglSwapBuffers(display, enc.surface);

    eglMakeCurrent(display, draw, read, context);
    glBindSampler(0, prevSampler);
    glBindTexture(GL_TEXTURE_2D, prevTex);
    glActiveTexture(prevActive);
    glBindVertexArray(prevVao);
    glUseProgram(prevProgram);
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    drain(false);
}

}
