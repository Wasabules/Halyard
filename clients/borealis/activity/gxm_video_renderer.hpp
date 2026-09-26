// GxmVideoRenderer - display an NV12 picture on the PS Vita, through nanovg.
//
// === WHY THIS IS NOT A PORT OF THE GL RENDERER ===========================
//
// `gl_video_renderer.cpp` does what a desktop does: it LEAVES nanovg
// (`nvgEndFrame`), issues its own GL - a program, two textures, a quad, a
// YUV->RGB fragment shader - and re-opens a frame afterwards. Reproducing that
// on GXM would mean a shader patcher, vertex and fragment programs, USSE
// memory and a render target, all of it duplicating what Borealis already owns
// and all of it able to leave the console's GPU in a state Borealis does not
// expect.
//
// The Vita gets there with none of that, for two reasons that are properties
// of THIS hardware rather than clever tricks:
//
//   1. The texture unit converts YUV itself. `SCE_GXM_TEXTURE_FORMAT_
//      YUV420P2_CSC*` is a two-plane 4:2:0 format - NV12, exactly what
//      `SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER` hands us - and sampling it
//      returns RGB. The conversion the GL path spends a fragment shader on is
//      free here, so there is no shader to write.
//
//   2. Borealis' nanovg backend can adopt a texture we built:
//      `nvgxmCreateImageFromHandle` turns a `SceGxmTexture` into an image id,
//      flagged NODELETE so ownership stays with us. From there the picture is
//      an ordinary `nvgImagePattern` fill, drawn INSIDE Borealis' frame, in the
//      same coordinate space as everything else - which is also why this
//      renderer takes logical coordinates and no viewport, where the GL one
//      needs both and a `windowScale` multiplication (V1).
//
// THREE BUFFERS, and it is the same lesson the audio backend paid for on the
// same day: GXM draws are DEFERRED. The texture is read when the GPU executes
// the command list, well after `render()` returns, so writing next frame's
// pixels into the buffer the GPU is still sampling tears the picture. We cycle.
#pragma once

#include <cstdint>

#include "../device_caps.h"

#if SHADOW_HAS_GXM_VIDEO

struct NVGcontext;

class GxmVideoRenderer {
public:
    ~GxmVideoRenderer();

    // Draws one NV12 picture into the logical rect (dx, dy, dw, dh).
    // `y` / `uv` are the two planes with their strides; `frame_w` x `frame_h`
    // is the DISPLAYABLE size, which is <= the stride (the encoder's alignment
    // rows must not reach the screen - that is the green-band defect).
    //
    // `frame_seq` is the caller's picture number, as in the GL renderer: the
    // planes are copied only when it changes, because the view draws several
    // times per picture.
    static constexpr uint64_t kSeqUnknown = ~(uint64_t)0;
    bool render(NVGcontext* vg,
                int frame_w, int frame_h,
                const uint8_t* y, int y_stride,
                const uint8_t* uv, int uv_stride,
                float dx, float dy, float dw, float dh,
                uint64_t frame_seq = kSeqUnknown);

    void cleanup();

private:
    bool ensure(NVGcontext* vg, int pitch, int height);

    static constexpr int SLOTS = 3;
    int      uid_[SLOTS]   = { -1, -1, -1 };
    uint8_t* mem_[SLOTS]   = { nullptr, nullptr, nullptr };
    int      img_[SLOTS]   = { 0, 0, 0 };
    NVGcontext* vg_        = nullptr;  // kept only to release the image slots
    int      slot_         = 0;      // the one we fill next
    int      shown_        = -1;     // the one the last draw used
    int      pitch_        = 0;
    int      height_       = 0;
    uint64_t uploaded_seq_ = kSeqUnknown;
    bool     said_ok_      = false;
};

#endif  /* SHADOW_HAS_GXM_VIDEO */
