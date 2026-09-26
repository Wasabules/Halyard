/* GxmVideoRenderer - see gxm_video_renderer.hpp for why this is not a port of
 * the GL renderer. */

#include "gxm_video_renderer.hpp"

#include "../device_caps.h"

#if SHADOW_HAS_GXM_VIDEO

#include <cstdlib>
#include <cstring>

#include <psp2/gxm.h>
#include <psp2/kernel/sysmem.h>

#include <nanovg.h>
#include <nanovg_gxm.h>
#include <nanovg_gxm_utils.h>

extern "C" {
#include "../../../core/services/journal.h"
}

#define vlog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)

namespace {

/* WHICH CONVERSION THE TEXTURE UNIT APPLIES, AND IN WHICH CHROMA ORDER.
 *
 * MEASURED ON HARDWARE 2026-09-13: with `YUV420P2` the picture came out with
 * blue and red exchanged - a global blue->red cast reported from the console.
 * That symptom names its own cause: exchanging Cb and Cr swaps exactly those
 * two channels, where a wrong MATRIX (601 against 709) shifts hues and
 * saturation without ever turning blue into red. So GXM's "YUV" ordering reads
 * the first chroma byte as V, while `SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_
 * RASTER` writes U there - NV12. `YVU420P2` is the same format with the two
 * read the other way round, and it is now the default.
 *
 * The two axes stay independent and each keeps its toggle, because they answer
 * different questions and the first console run only settled ONE of them:
 *
 *   SHADOW_GXM_CHROMA=0   back to YUV ordering (the blue/red swap)
 *   SHADOW_GXM_CSC=0      BT.601 instead of BT.709
 *
 * The matrix is still DEDUCED, not measured: COL1 established the stream is
 * limited-range BT.709 and the format names say CSC1 is the 709 one, but no
 * known colour has been checked through this path. If the picture looks
 * slightly washed out or over-saturated rather than swapped, that is the axis
 * to move, and the answer belongs in KB §9 next to COL1. */
SceGxmTextureFormat csc_format()
{
    static int chroma = -1, matrix = -1;
    if (chroma < 0) {
        const char* e = std::getenv("SHADOW_GXM_CHROMA");
        chroma = e ? std::atoi(e) : 1;          /* 1 = YVU, the measured one */
        const char* m = std::getenv("SHADOW_GXM_CSC");
        matrix = m ? std::atoi(m) : 1;          /* 1 = CSC1 (BT.709) */
    }
    if (chroma) return matrix ? SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1
                              : SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC0;
    return matrix ? SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC1
                  : SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC0;
}

const char* csc_name()
{
    switch (csc_format()) {
        case SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC1: return "YVU/CSC1 (BT.709)";
        case SCE_GXM_TEXTURE_FORMAT_YVU420P2_CSC0: return "YVU/CSC0 (BT.601)";
        case SCE_GXM_TEXTURE_FORMAT_YUV420P2_CSC1: return "YUV/CSC1 (BT.709)";
        default:                                   return "YUV/CSC0 (BT.601)";
    }
}

}  // namespace

GxmVideoRenderer::~GxmVideoRenderer() { cleanup(); }

void GxmVideoRenderer::cleanup()
{
    /* === WAIT FOR THE GPU BEFORE FREEING, AND GIVE THE SLOTS BACK ========
     *
     * Two things this used to get wrong, and the first is the same lesson the
     * audio backend and the triple buffer both paid for on 2026-09-13: GXM
     * draws are DEFERRED. When `cleanup()` runs - on leaving a stream, so
     * immediately after a frame that sampled these textures - the GPU may still
     * be executing the command list that reads them. Unmapping the memory under
     * it is a fault waiting for the right timing, and "leave the stream, come
     * back" is exactly the timing that finds it.
     *
     * And the images: `NVGXM_IMAGE_NODELETE` means nanovg will not free OUR
     * texture, not that the slot costs nothing. Leaving them behind leaked
     * three entries of nanovg's table per session, which a reconnect loop
     * consumes steadily. `nvgDeleteImage` releases the slot and, thanks to that
     * same flag, still leaves the memory to us. */
    if (vg_) {
        for (int i = 0; i < SLOTS; i++)
            if (img_[i] != 0) nvgDeleteImage(vg_, img_[i]);
    }
    if (NVGXMwindow* w = gxmGetWindow()) {
        if (w->context) sceGxmFinish(w->context);
    }
    for (int i = 0; i < SLOTS; i++) {
        if (uid_[i] >= 0) gpu_unmap_free(uid_[i]);
        uid_[i] = -1;
        mem_[i] = nullptr;
        img_[i] = 0;
    }
    vg_    = nullptr;
    pitch_ = height_ = 0;
    shown_ = -1;
    slot_  = 0;
    uploaded_seq_ = kSeqUnknown;
    said_ok_ = false;
}

bool GxmVideoRenderer::ensure(NVGcontext* vg, int pitch, int height)
{
    if (pitch_ == pitch && height_ == height && mem_[0]) return true;
    if (pitch <= 0 || height <= 0) return false;
    cleanup();

    /* Two planes, contiguous: Y is pitch x height, the interleaved chroma is
     * pitch x height/2 right behind it. That IS the YUV420P2 layout, and it is
     * also the layout `h264_decoder_vita.c` asks the decoder for - but we copy
     * rather than point at the decoder's buffer, because the decoder owns a
     * SINGLE picture and would overwrite it under the GPU. */
    const size_t plane_y = (size_t)pitch * (size_t)height;
    const size_t bytes   = plane_y + plane_y / 2;

    for (int i = 0; i < SLOTS; i++) {
        /* LPDDR first, CDRAM if it refuses - the order nanovg's own texture
         * allocator uses (`NVG_IMAGE_LPDDR` is its default). LPDDR is where a
         * buffer the CPU writes every frame belongs; CDRAM is the fallback,
         * not the preference. */
        mem_[i] = (uint8_t*)gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE,
                                          SCE_GXM_MEMORY_ATTRIB_READ, bytes, &uid_[i]);
        if (!mem_[i])
            mem_[i] = (uint8_t*)gpu_alloc_map(SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,
                                              SCE_GXM_MEMORY_ATTRIB_READ, bytes, &uid_[i]);
        if (!mem_[i]) {
            vlog("gxm/video: %u KB REFUSED for slot %d of %d - no picture",
                 (unsigned)(bytes / 1024), i + 1, SLOTS);
            cleanup();
            return false;
        }
        /* Green rather than black on an untouched slot: if a slot is ever
         * DISPLAYED before it is filled, a green flash says so, where black
         * would be indistinguishable from "no video at all" - which is the
         * state this whole renderer exists to leave behind. Y=0,U=V=0 is that
         * green in BT.709 limited range. */
        std::memset(mem_[i], 0, bytes);

        SceGxmTexture tex;
        const int rc = sceGxmTextureInitLinear(&tex, mem_[i], csc_format(),
                                               (unsigned)pitch, (unsigned)height, 0);
        if (rc < 0) {
            vlog("gxm/video: sceGxmTextureInitLinear(%dx%d) FAIL rc=0x%08x",
                 pitch, height, rc);
            cleanup();
            return false;
        }
        sceGxmTextureSetMinFilter(&tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetMagFilter(&tex, SCE_GXM_TEXTURE_FILTER_LINEAR);
        sceGxmTextureSetUAddrMode(&tex, SCE_GXM_TEXTURE_ADDR_CLAMP);
        sceGxmTextureSetVAddrMode(&tex, SCE_GXM_TEXTURE_ADDR_CLAMP);

        img_[i] = nvgxmCreateImageFromHandle(vg, &tex);
        if (img_[i] == 0) {
            vlog("gxm/video: nvgxmCreateImageFromHandle FAIL for slot %d", i + 1);
            cleanup();
            return false;
        }
    }

    vg_     = vg;
    pitch_  = pitch;
    height_ = height;
    vlog("gxm/video: %d x %d, %d slots of %u KB, %s",
         pitch, height, SLOTS, (unsigned)(bytes / 1024),
         csc_name());
    return true;
}

bool GxmVideoRenderer::render(NVGcontext* vg,
                              int frame_w, int frame_h,
                              const uint8_t* y, int y_stride,
                              const uint8_t* uv, int uv_stride,
                              float dx, float dy, float dw, float dh,
                              uint64_t frame_seq)
{
    if (!vg || !y || !uv) return false;
    if (frame_w <= 0 || frame_h <= 0 || y_stride <= 0 || uv_stride <= 0) return false;
    if (dw <= 0.0f || dh <= 0.0f) return false;

    /* The texture is as wide as the STRIDE and as tall as the picture; only
     * `frame_w` x `frame_h` of it is displayable, and the pattern below is what
     * keeps the alignment columns off the screen. */
    if (!ensure(vg, y_stride, frame_h)) return false;

    /* HO-1's rule, kept: the view draws several times per picture, and copying
     * 1.4 MB on each of them is the upload the desktop renderer was measured
     * re-sending 65-74 % of the time for nothing. A new number means new
     * pixels; the same number means draw what is already there. */
    /* === ZERO COPY, WHERE THE LAYOUT ALLOWS IT =========================
     *
     * The decoder's buffers are mapped for the GPU, so the texture can sample
     * them IN PLACE. The condition is not assumed, it is DERIVED: the two
     * planes must be contiguous and exactly `pitch * frame_h` apart, which is
     * the YUV420P2 layout GXM expects. Were the decoder to align its height
     * differently, that gap would differ and the picture would come out
     * skewed -- so we fall back on the copy rather than display something
     * wrong.
     *
     * The decoder rotates four buffers, so the one the GPU reads is not the
     * one the silicon writes: same reason the triple buffer below exists, and
     * that buffer becomes pointless on this path.
     *
     * The measurement that forced this: removing the view's copy dropped
     * video/decode from 18.8 to 3.7 ms, but televerse rose from 7 to 21 -- the
     * remaining copy was reading the decoder's uncached memory. Moving a copy
     * is not removing it. */
    /* `SHADOW_GXM_ZEROCOPY=0` forces the path WITH the copy. Two reasons, and
     * the second is the real one: it is the revert path this repo demands of
     * every fix, and it is the only way to EXERCISE the fallback. Without it,
     * the copying branch would only ever run the day an unexpected resolution
     * triggers it -- which is to say never before the incident. */
    static int g_zero = -1;
    if (g_zero < 0) {
        const char* e = std::getenv("SHADOW_GXM_ZEROCOPY");
        g_zero = e ? std::atoi(e) : 1;
    }
    const bool contiguous = g_zero
                            && (uv == y + (size_t)y_stride * (size_t)frame_h)
                            && (uv_stride == y_stride);
    if (contiguous) {
        if (!ensure(vg, y_stride, frame_h)) return false;
        NVGXMtexture *h = nvgxmImageHandle(vg, img_[0]);
        if (h) {
            sceGxmTextureSetData(&h->tex, (void *)y);
            shown_ = 0;
            uploaded_seq_ = frame_seq;
            if (!said_ok_) {
                said_ok_ = true;
                vlog("gxm/video: zero copie - la texture lit le tampon du decodeur "
                     "(%dx%d, foulee %d)", frame_w, frame_h, y_stride);
            }
            const float ex0 = dw * (float)pitch_  / (float)frame_w;
            const float ey0 = dh * (float)height_ / (float)frame_h;
            NVGpaint p0 = nvgImagePattern(vg, dx, dy, ex0, ey0, 0.0f, img_[0], 1.0f);
            nvgBeginPath(vg);
            nvgRect(vg, dx, dy, dw, dh);
            nvgFillPaint(vg, p0);
            nvgFill(vg);
            return true;
        }
    }

    if (!said_ok_) {
        said_ok_ = true;
        vlog("gxm/video: chemin AVEC COPIE (%dx%d, foulee %d, %s) - 1,4 Mo par image",
             frame_w, frame_h, y_stride,
             g_zero ? "disposition non contigue" : "SHADOW_GXM_ZEROCOPY=0");
    }

    const bool fresh = (frame_seq == kSeqUnknown) || (frame_seq != uploaded_seq_);
    if (fresh) {
        const int      chroma_rows = frame_h / 2;
        const size_t   plane_y     = (size_t)pitch_ * (size_t)frame_h;
        uint8_t* const dst         = mem_[slot_];

        /* Never copy more per row than the SOURCE row holds. `pitch_` is
         * `y_stride` by construction, so the luma path is the whole row and
         * the fast memcpy applies - but nothing guarantees the chroma plane
         * carries the same stride, and reading `pitch_` bytes out of a
         * narrower row would walk off the end of the caller's buffer. The
         * clamp costs nothing and removes the question. */
        const size_t row_y  = (size_t)((y_stride  < pitch_) ? y_stride  : pitch_);
        const size_t row_uv = (size_t)((uv_stride < pitch_) ? uv_stride : pitch_);

        if (y_stride == pitch_) {
            std::memcpy(dst, y, plane_y);
        } else {
            for (int r = 0; r < frame_h; r++)
                std::memcpy(dst + (size_t)r * pitch_, y + (size_t)r * y_stride,
                            row_y);
        }
        if (uv_stride == pitch_) {
            std::memcpy(dst + plane_y, uv, (size_t)pitch_ * chroma_rows);
        } else {
            for (int r = 0; r < chroma_rows; r++)
                std::memcpy(dst + plane_y + (size_t)r * pitch_,
                            uv + (size_t)r * uv_stride, row_uv);
        }

        shown_        = slot_;
        slot_         = (slot_ + 1) % SLOTS;
        uploaded_seq_ = frame_seq;
    }
    if (shown_ < 0) return false;      /* nothing filled yet */

    /* The image occupies the whole texture, so to show only `frame_w x frame_h`
     * of it inside (dx, dy, dw, dh) we stretch the PATTERN past the rectangle
     * in the same proportion, and let the rectangle clip. Cropping by moving
     * texture coordinates is not available through nanovg's paint. */
    const float ex = dw * (float)pitch_  / (float)frame_w;
    const float ey = dh * (float)height_ / (float)frame_h;

    NVGpaint p = nvgImagePattern(vg, dx, dy, ex, ey, 0.0f, img_[shown_], 1.0f);
    nvgBeginPath(vg);
    nvgRect(vg, dx, dy, dw, dh);
    nvgFillPaint(vg, p);
    nvgFill(vg);

    if (!said_ok_) {
        said_ok_ = true;
        vlog("gxm/video: first picture drawn - %dx%d (stride %d) into "
             "%.0fx%.0f at %.0f,%.0f", frame_w, frame_h, y_stride, dw, dh, dx, dy);
    }
    return true;
}

#endif  /* SHADOW_HAS_GXM_VIDEO */
