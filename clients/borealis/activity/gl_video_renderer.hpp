// GLVideoRenderer - render NV12 frames via a custom GLSL shader.
//
// Phase 2A: doing the NV12->RGB conversion in a GPU shader saves the NEON CPU
// pass (~5-8ms/frame) and shrinks the upload (3.5MB RGBA -> 1.4MB for Y+UV
// kept separate).
//
// Pipeline:
//   1. Y plane -> R8 texture (1280x720)
//   2. UV plane -> RG8 texture (640x360, sampled as U+V through .r/.g)
//   3. Fragment shader: sample Y and UV, convert with the matrix and range
//      chosen per frame (COL1 - gl_video_renderer.cpp)
//   4. Render a fullscreen quad (letterboxed to the aspect ratio)

#pragma once

#include <cstdint>

class GLVideoRenderer {
public:
    GLVideoRenderer() = default;
    ~GLVideoRenderer();

    // Lazy init on the first render (needs an active GL context).
    bool init();
    void cleanup();

    // Render one NV12 frame into the pixel rectangle (dst_x, dst_y, dst_w,
    // dst_h) of a (vp_w, vp_h) pixel viewport. Letterboxing is the caller's job.
    // Y plane : `y_data`, stride `y_stride`, dimensions w x h.
    // UV plane : `uv_data`, stride `uv_stride`, dimensions (w/2) x (h/2) in
    //            pixels (each UV pixel = 2 interleaved bytes -> byte stride = w).
    // Returns true on success.
    //
    // HO-1 (2026-09-11): `frame_seq` is the caller's number for the picture it
    // hands in - a new number means new pixels. The planes are uploaded only
    // when it changes (or the size, the chroma mode, or after a re-init): the
    // view draws ~3 times per new picture, and every draw used to re-send both
    // planes. kSeqUnknown keeps the old behaviour, an upload on every call.
    static constexpr uint64_t kSeqUnknown = ~(uint64_t)0;
    bool render(int frame_w, int frame_h,
                const uint8_t* y_data, int y_stride,
                const uint8_t* uv_data, int uv_stride,
                float dst_x, float dst_y, float dst_w, float dst_h,
                int vp_w, int vp_h,
                int chroma_pleine = 0,
                uint64_t frame_seq = kSeqUnknown);

private:
    bool initialized = false;
    unsigned int program = 0;
    unsigned int y_tex = 0;
    unsigned int uv_tex = 0;
    unsigned int vao = 0;
    unsigned int vbo = 0;
    int u_dst_loc = -1;
    int u_y_loc = -1;
    int u_uv_loc = -1;
    int u_range_loc = -1, u_kr_loc = -1, u_kb_loc = -1;   /* COL1 */
    int last_matrix = -2, last_full = -2;                  /* COL1 - log on change */
    uint64_t uploaded_seq_ = 0;     /* HO-1: the picture the textures hold */
    bool     tex_valid_    = false; /* HO-1: they hold one at the current size */
    int last_y_w = 0, last_y_h = 0;
    int last_uv_w = 0, last_uv_h = 0;
};
