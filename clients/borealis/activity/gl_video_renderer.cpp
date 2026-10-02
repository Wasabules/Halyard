// GLVideoRenderer - implementation. See gl_video_renderer.hpp.

#include "clients/borealis/activity/gl_video_renderer.hpp"

extern "C" {
#include "../../../core/services/log.h"
#include "../../../core/media/h264_decoder.h"   /* COL1 - the colorimetry the stream declares */
}

#include "../gl_compat.h"

#if SHADOW_HAVE_GLAD
#include <glad/glad.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

/* S81 - the log category is DECLARED here, not inferred from the text of the
 * messages. `glog` stays at INFO: the existing call sites do not disappear.
 * `gdbg` is there for the high-volume lines, which move over to it one at a
 * time. */
#define glog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
#define gdbg(...) JOURNAL_DEBUG_(JOURNAL_CAT_VIDEO, __VA_ARGS__)
// Vertex shader: maps the [0,1] quad coords to NDC through u_dst (pixel rect).
static const char *VERT_SRC = R"glsl(
#version 330 core
layout(location = 0) in vec2 a_pos;       // [0,1] quad coords
layout(location = 1) in vec2 a_tex;       // [0,1] texture coords
out vec2 v_tex;
uniform vec4 u_dst;  // x, y, w, h en NDC [-1,1]
void main() {
    vec2 ndc = u_dst.xy + a_pos * u_dst.zw;
    gl_Position = vec4(ndc, 0.0, 1.0);
    v_tex = a_tex;
}
)glsl";

// Fragment shader: NV12 -> RGB; matrix and range come in as uniforms (COL1).
// Y texture : R8, single channel, sampled in .r within [0,1].
// UV texture : RG8, two channels, sampled .r=U, .g=V within [0,1].
static const char *FRAG_SRC = R"glsl(
#version 330 core
in vec2 v_tex;
uniform sampler2D u_y;
uniform sampler2D u_uv;
out vec4 fragColor;

// u_range : 0 = echelle limitee (TV, 16-235), 1 = pleine echelle (PC, 0-255).
// u_kr,u_kb: the matrix coefficients (BT.601 or BT.709).
// COL1 2026-09-10: the Shadow stream measures as LIMITED range, BT.709 - see
// the comment above compile_shader, which also says where e72d02d2's
// full-range reading went wrong.
uniform int   u_range;
uniform float u_kr;
uniform float u_kb;
void main() {
    float y = texture(u_y, v_tex).r;
    float u = texture(u_uv, v_tex).r - 0.5;
    float v = texture(u_uv, v_tex).g - 0.5;
    if (u_range == 0) {            // limitee -> normalise 16..235 / 16..240
        y = (y * 255.0 - 16.0) / 219.0;
        u = u * 255.0 / 224.0;
        v = v * 255.0 / 224.0;
    }
    float kr = u_kr, kb = u_kb, kg = 1.0 - kr - kb;
    float r = y + 2.0 * (1.0 - kr) * v;
    float b = y + 2.0 * (1.0 - kb) * u;
    float g = (y - kr * r - kb * b) / kg;
    fragColor = vec4(clamp(r, 0.0, 1.0),
                     clamp(g, 0.0, 1.0),
                     clamp(b, 0.0, 1.0),
                     1.0);
}
)glsl";

/* === COL1 2026-09-10 - THE COLOUR UNIFORMS WERE NEVER SET ===
 *
 * e72d02d2 (2026-08-22) gave the shader `u_range`, `u_kr` and `u_kb`, meaning
 * full range + BT.709 by default, steerable by SHADOW_COLOR_RANGE and
 * SHADOW_COLOR_MATRIX - its message says so. Only the shader half landed:
 * nothing ever called glUniform on them, and neither variable was read
 * anywhere. An unset uniform is 0: every picture since was converted with
 * u_range = 0 (limited) and kr = kb = 0, which turns the green channel into a
 * copy of the luma - greens darkened, reds pushed towards orange, greys right.
 *
 * MEASURED on the live stream (captures of the client window showing a known
 * UI on the VM, 3x3 averages):
 *
 *                     true colour    before (kr=kb=0)  full, 709     limited, 709
 *   Facebook button   24,119,242      1,107,254        35,118,228    24,119,244 (*)
 *   white button     255,255,255    255,255,254       235,235,235   255,255,255
 *   black button       0,  0,  0      1,  0,  1        17, 16, 17     0,  0,  0
 *
 * (*) computed from the full-range capture, inverted exactly. White at 235 and
 * black at 16 are the signature of a LIMITED-range stream shown as full: the
 * RANGE half of e72d02d2's diagnosis is SUPERSEDED. It had read luma spanning
 * 0..255 - probably overshoot at sharp edges, which a limited stream has too;
 * the bulk of the picture is what says the range. Its MATRIX default holds:
 * BT.601 would put that red at 33 instead of 24.
 *
 * The stream declares neither (the "[colour] the stream declares" line reads
 * colorspace=2 color_range=0: both unspecified), so the defaults are what was
 * measured: LIMITED range, BT.709. Field by field: the variable when set, else
 * what the stream declares, else that default. Read once: experiment
 * switches, not session state. */
namespace {
int g_env_color_range  = -2;   /* -2 unread, -1 unset, else 0 / 1 */
int g_env_color_matrix = -2;   /* -2 unread, -1 unset, else 601 / 709 / 2020 */
int g_env_upload_every = -2;   /* HO-1: SHADOW_UPLOAD_EVERY_DRAW, -1 unset */

int color_env(const char *name, int *cache)
{
    if (*cache == -2) {
        const char *e = std::getenv(name);
        *cache = (e && *e) ? std::atoi(e) : -1;
    }
    return *cache;
}
}  // namespace

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        glog("gl_video: shader compile FAIL (%s): %s",
             type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLVideoRenderer::~GLVideoRenderer() {
    cleanup();
}

bool GLVideoRenderer::init() {
    if (initialized) return true;

    GLuint vs = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, FRAG_SRC);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return false;
    }
    program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        glog("gl_video: program link FAIL: %s", log);
        glDeleteProgram(program); program = 0;
        return false;
    }

    u_dst_loc = glGetUniformLocation(program, "u_dst");
    u_y_loc   = glGetUniformLocation(program, "u_y");
    u_uv_loc  = glGetUniformLocation(program, "u_uv");
    u_range_loc = glGetUniformLocation(program, "u_range");   /* COL1 */
    u_kr_loc    = glGetUniformLocation(program, "u_kr");
    u_kb_loc    = glGetUniformLocation(program, "u_kb");

    // Quad: 4 vertices, triangle strip.
    // (a_pos, a_tex): pos in [0,1]^2, tex in [0,1]^2 (Y flipped for the OpenGL
    // orientation).
    static const float quad[] = {
        // x, y,  u, v
        0.0f, 0.0f,  0.0f, 1.0f,  // bottom-left -> top of texture (GL Y flip)
        1.0f, 0.0f,  1.0f, 1.0f,  // bottom-right
        0.0f, 1.0f,  0.0f, 0.0f,  // top-left
        1.0f, 1.0f,  1.0f, 0.0f,  // top-right
    };
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                           (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    glGenTextures(1, &y_tex);
    glBindTexture(GL_TEXTURE_2D, y_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &uv_tex);
    glBindTexture(GL_TEXTURE_2D, uv_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);
    /* HO-1: fresh textures hold nothing and have no storage yet. */
    tex_valid_ = false;
    last_y_w = last_y_h = last_uv_w = last_uv_h = 0;
    initialized = true;
    glog("gl_video: initialized (program=%u y_tex=%u uv_tex=%u vao=%u)",
         program, y_tex, uv_tex, vao);
    /* HO-1 - the log never named the GPU, and the upload saving depends on it:
     * the HO-1/R2 benches found the NVIDIA driver can absorb part of it in a
     * spin-wait, so process CPU is not the metric there. */
    {
        const GLubyte *rdr = glGetString(GL_RENDERER);
        glog("gl_video: renderer=%s", rdr ? (const char *)rdr : "?");
    }
    return true;
}

void GLVideoRenderer::cleanup() {
    if (!initialized) return;
    if (program) glDeleteProgram(program);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (y_tex) glDeleteTextures(1, &y_tex);
    if (uv_tex) glDeleteTextures(1, &uv_tex);
    program = vao = vbo = y_tex = uv_tex = 0;
    /* HO-1: the textures are gone and so is what they held. Without resetting
     * the size cache too, a re-init would glTexSubImage2D into textures with no
     * storage (latent: only the destructor calls cleanup() today). */
    tex_valid_ = false;
    last_y_w = last_y_h = last_uv_w = last_uv_h = 0;
    initialized = false;
}

bool GLVideoRenderer::render(int frame_w, int frame_h,
                              const uint8_t* y_data, int y_stride,
                              const uint8_t* uv_data, int uv_stride,
                              float dst_x, float dst_y, float dst_w, float dst_h,
                              int vp_w, int vp_h,
                              int chroma_pleine, uint64_t frame_seq) {
    if (!initialized && !init()) return false;
    if (frame_w <= 0 || frame_h <= 0) return false;

    /* K17 - in 4:4:4 the chroma planes are the same size as the Y plane. The
     * shader samples in normalised coordinates, so it has nothing to be told:
     * uploading the texture at the right size is enough. */
    int uv_w = chroma_pleine ? frame_w : frame_w / 2;
    int uv_h = chroma_pleine ? frame_h : frame_h / 2;

    /* === HO-1 2026-09-11 - UPLOAD A PICTURE ONCE, DRAW IT ON EVERY DRAW ===
     * The view draws at the display's rate (~144 draws/s on the Windows
     * baseline) while ~45 new pictures arrive per second, and this call re-sent
     * both planes every time: [L5] video/televerse n=1440 per 10 s against
     * video/file-aff n~450 - 65-74 % of uploads re-sent identical bytes
     * (~137 MB/s). The textures keep what they were given; a draw with no new
     * picture only has to sample them. Benches (HO-1, R2, VI3; real renderer,
     * real decoded pictures, A/B interleaved): UI-thread time in render()
     * x0.43-0.49, GPU time -29 to -53 %; the benches saw no change in the draw
     * cadence. Live A/B (3 interleaved pairs; 60 Hz display that day; GL on an
     * Intel UHD 630, CUDA decode on the NVIDIA): video/televerse mean 0.5 ->
     * 0.3 ms, p50 0.5 -> 0.2; and, not foreseen, video/cadence p90 24.6 ->
     * 17.4 ms and video/bout p90 20.5 -> 18.4 ms, video/decode unchanged.
     * The key is the caller's picture NUMBER: the per-draw "got a new picture"
     * flag left a stale picture 40/40 times when a pop landed on a skipped
     * draw, and a buffer ADDRESS goes stale when the allocator reuses it.
     * SHADOW_UPLOAD_EVERY_DRAW=1 restores an upload on every draw. */
    const bool need_upload =
        color_env("SHADOW_UPLOAD_EVERY_DRAW", &g_env_upload_every) > 0
        || frame_seq == kSeqUnknown || !tex_valid_ || frame_seq != uploaded_seq_
        || last_y_w != frame_w || last_y_h != frame_h
        || last_uv_w != uv_w || last_uv_h != uv_h;

    // === Minimal state save (so nanovg is not broken afterwards) ===
    GLint prev_program = 0, prev_vao = 0, prev_active = 0, prev_tex0 = 0, prev_tex1 = 0;
    GLint prev_blend = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &prev_program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &prev_vao);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &prev_active);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex0);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex1);
    prev_blend = glIsEnabled(GL_BLEND);
    /* Mixing raw GL with nanovg means neutralising the state nanovg leaves
     * behind (it enables the scissor test to clip its views), then restoring it
     * exactly as it was. */
    GLboolean prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean prev_depth   = glIsEnabled(GL_DEPTH_TEST);
    GLboolean prev_stencil = glIsEnabled(GL_STENCIL_TEST);
    GLboolean prev_cull    = glIsEnabled(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);

    // === Upload Y plane ===
    /* HO-1: the texture units are bound on every draw (the quad samples
     * them); only the transfers wait for a new picture. */
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, y_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   /* unconditional: same state left behind */
    if (need_upload) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, y_stride);  // accept padded rows
        if (last_y_w != frame_w || last_y_h != frame_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, frame_w, frame_h, 0,
                         GL_RED, GL_UNSIGNED_BYTE, y_data);
            last_y_w = frame_w; last_y_h = frame_h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame_w, frame_h,
                            GL_RED, GL_UNSIGNED_BYTE, y_data);
        }
    }

    // === Upload UV plane (half resolution, RG8 = U+V interleaved) ===
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, uv_tex);
    if (need_upload) {
        glPixelStorei(GL_UNPACK_ROW_LENGTH, uv_stride / 2);  // RG8 = 2 bytes/pixel
        if (last_uv_w != uv_w || last_uv_h != uv_h) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, uv_w, uv_h, 0,
                         GL_RG, GL_UNSIGNED_BYTE, uv_data);
            last_uv_w = uv_w; last_uv_h = uv_h;
        } else {
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uv_w, uv_h,
                            GL_RG, GL_UNSIGNED_BYTE, uv_data);
        }
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        uploaded_seq_ = frame_seq;
        tex_valid_    = true;
    }

    // === Compute NDC rect (pixel coords -> NDC [-1,1]) ===
    // Pixel origin = top-left, GL NDC origin = bottom-left, hence the Y flip.
    float ndc_x = 2.0f * dst_x / vp_w - 1.0f;
    float ndc_y = 1.0f - 2.0f * (dst_y + dst_h) / vp_h;  // bottom-left of dst
    float ndc_w = 2.0f * dst_w / vp_w;
    float ndc_h = 2.0f * dst_h / vp_h;

    // === Render ===
    glDisable(GL_BLEND);
    glUseProgram(program);
    glUniform4f(u_dst_loc, ndc_x, ndc_y, ndc_w, ndc_h);
    glUniform1i(u_y_loc, 0);
    glUniform1i(u_uv_loc, 1);
    {   /* COL1 - see above compile_shader */
        int decl_matrix = -1, decl_full = -1;
        h264_decoder_colorimetry(&decl_matrix, &decl_full);
        const int env_r = color_env("SHADOW_COLOR_RANGE", &g_env_color_range);
        const int env_m = color_env("SHADOW_COLOR_MATRIX", &g_env_color_matrix);
        const bool env_m_ok = env_m == 601 || env_m == 709 || env_m == 2020;
        const int full   = env_r >= 0 ? (env_r != 0 ? 1 : 0)
                         : decl_full >= 0 ? decl_full : 0;   /* measured: LIMITED */
        const int matrix = env_m_ok ? env_m : decl_matrix > 0 ? decl_matrix : 709;
        float kr = 0.2126f, kb = 0.0722f;                       /* BT.709 */
        if (matrix == 601)  { kr = 0.299f;  kb = 0.114f;  }
        if (matrix == 2020) { kr = 0.2627f; kb = 0.0593f; }
        glUniform1i(u_range_loc, full);
        glUniform1f(u_kr_loc, kr);
        glUniform1f(u_kb_loc, kb);
        if (matrix != last_matrix || full != last_full) {
            glog("[couleur] rendu : matrice BT.%d (%s), echelle %s (%s)", matrix,
                 env_m_ok ? "SHADOW_COLOR_MATRIX" : decl_matrix > 0 ? "flux" : "defaut",
                 full ? "pleine" : "limitee",
                 env_r >= 0 ? "SHADOW_COLOR_RANGE" : decl_full >= 0 ? "flux" : "defaut");
            last_matrix = matrix;
            last_full   = full;
        }
    }
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    // === Restore state for nanovg ===
    glUseProgram(prev_program);
    glBindVertexArray(prev_vao);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, prev_tex1);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, prev_tex0);
    glActiveTexture(prev_active);
    if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
    /* S11: restore, exactly as they were, the states neutralised above. */
    if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (prev_depth)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
    if (prev_stencil) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
    if (prev_cull)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);


    return true;
}

#else   /* !SHADOW_HAVE_GLAD */

/* === NO GL LOADER: THE PICTURE IS NOT DRAWN, AND SAYS SO ===================
 *
 * The PS Vita renders through GXM, and Borealis drops glad from its own sources
 * there. The 104 GL calls above are a desktop/EGL renderer and cannot be
 * recompiled into GXM: the NV12 shader, the two plane textures and the
 * colour-space uniforms (COL1) all have to be written again against that API,
 * or against vitaGL if the shader can be made to survive vitaShaRK.
 *
 * Until then the class exists, `init()` REFUSES, and the view draws no picture.
 * That is deliberate: a stub that pretended to succeed would leave the caller
 * measuring frames it never showed, which is the family of defect this repo
 * has paid for most often. Everything else on the Vita - the session, the
 * decode, the audio, the input - runs; only the last step is missing.
 *
 * `init()` logs once so the reason is in the session log rather than in a
 * commit message. */
#include "../../../core/services/log.h"

bool GLVideoRenderer::init()
{
    if (!initialized) {
        initialized = true;   /* only to keep this to one line per session */
        journal_uncategorised("video: no GL renderer on this target - the picture is not "
                   "drawn (a GXM/vitaGL renderer is still to be written)");
    }
    return false;
}

void GLVideoRenderer::cleanup() {}

GLVideoRenderer::~GLVideoRenderer() {}

bool GLVideoRenderer::render(int, int, const uint8_t *, int, const uint8_t *, int,
                             float, float, float, float, int, int, int, uint64_t)
{
    return false;
}

#endif  /* SHADOW_HAVE_GLAD */
