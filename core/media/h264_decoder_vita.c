/* h264_decoder_vita.c - SceAvcdec. See the header for WHY.
 *
 * Compiled on every platform and empty off the Vita, like `audio_out_vita.c`.
 */
#include "h264_decoder_vita.h"

#if defined(__vita__) || defined(__psp2__)

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <psp2/videodec.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/gxm.h>

#include "../services/journal.h"
#define vlog(...) JOURNAL_INFO_(JOURNAL_CAT_VIDEO, __VA_ARGS__)

/* A bare 0x80620802 costs a trip to the SDK headers every time. The names are
 * the difference between "it refused" and "it refused THIS", and the first
 * console run needed exactly that. */
static const char *videodec_err(int rc)
{
    switch ((unsigned)rc) {
        case 0x80620801u: return "INVALID_TYPE";
        case 0x80620802u: return "INVALID_PARAM";
        case 0x80620803u: return "OUT_OF_MEMORY";
        case 0x80620804u: return "INVALID_STATE";
        case 0x80620805u: return "UNSUPPORT_IMAGE_SIZE";
        case 0x80620806u: return "INVALID_COLOR_FORMAT";
        case 0x80620807u: return "NOT_PHY_CONTINUOUS_MEMORY";
        case 0x80620808u: return "ALREADY_USED";
        case 0x80620809u: return "INVALID_POINTER";
        case 0x8062080Au: return "ES_BUFFER_FULL";
        case 0x8062080Bu: return "INITIALIZE";
        case 0x8062080Cu: return "NOT_INITIALIZE";
        case 0x8062080Du: return "INVALID_STREAM";
        case 0x8062080Eu: return "INVALID_ARGUMENT_SIZE";
        default:          return "?";
    }
}

/* The decoder writes its pictures into memory WE own, and it must be
 * physically contiguous: the video block is a separate engine, not the CPU.
 * PHYCONT_NC_RW is the type every Vita decoder sample uses - non-cached,
 * because the CPU only ever reads what the hardware wrote and a stale cache
 * line would show the previous picture. */
#define ALIGN_UP(v, a) (((v) + (a) - 1u) & ~((a) - 1u))

struct h264_vita {
    SceAvcdecCtrl ctrl;
    SceUID        frame_uid;      /* the decoder's own working memory */
    /* === SEVERAL PICTURE BUFFERS, AND WHY ============================
     *
     * There was only ONE, so the picture had to be copied before the next
     * decode: the view made a 1.4 MB memcpy of it (VI2) and the GXM renderer a
     * second one. Measured on console: sceAvcdecDecode costs 3.8 ms, while
     * `video/decode` -- which spans the callback -- costs 18.8. The 15 ms
     * between them is the copy, and `video/televerse` adds 7 more for the
     * other.
     *
     * With four buffers in rotation, the picture handed over stays valid well
     * past its display: the presentation queue holds two at most, plus the one
     * being drawn. The decoder therefore comes back to a buffer four pictures
     * later, long after that one was displayed. Same reasoning as the audio
     * double buffer and the GXM triple buffer -- except here it is the
     * CONSUMER being protected, not the hardware. */
#define VITA_PIC_SLOTS 4
    SceUID        pic_uid[VITA_PIC_SLOTS];
    void         *pic_slot[VITA_PIC_SLOTS];
    int           pic_cur;
    void         *pic_base;      /* the current picture's buffer */
    int           pic_gpu;       /* the buffers are mapped for the GPU */
    uint32_t      pic_size;
    int           width, height;
    int           lib_open;
};

/* === WHERE THE DECODER'S MEMORY COMES FROM, AND WHY IT IS ASKED FOR TWICE ==
 *
 * Measured on console 2026-09-13, and it corrected the diagnosis: the descending
 * ladder in `h264_decoder.c` showed `sceVideodecInitLibrary` SUCCEEDING at every
 * geometry below 1080p, and then THIS function failing - 12.75 MB, 10.5, 6.5,
 * and finally 3.25 MB, all refused. A 3.25 MB request failing exactly as hard as
 * a 12.75 MB one is not fragmentation and not a decoder limit: it says the pool
 * itself is empty or the request is malformed. The video problem was never in
 * the video code.
 *
 * Two candidates, and the point of the code below is that it does not choose
 * between them - it makes the console answer.
 *
 * FIRST, THE GRAIN. This used to round to 256 KB, on a comment asserting that
 * "Sce allocations are granted in 256 KB units for this type" - an assertion
 * nothing had checked, and every one of the five refused sizes was a multiple
 * of 256 KB and NOT of 1 MiB. PHYCONT blocks are granted in 1 MiB units, so
 * that is the grain now. Note what the old comment cost: it read like a
 * verified fact and was a guess, which is why the sizes in the log looked
 * reasonable while being unaskable.
 *
 * SECOND, THE BUDGET. A Vita app declares its memory budget in `param.sfo`, and
 * ours passes `ATTRIBUTE2=12` ("max heap size", inherited from Borealis' own
 * demo CMake, not chosen here). Those modes trade the pools against each other,
 * so the maximum heap may well be paid for out of PHYCONT - leaving a budget of
 * zero, which would refuse 3.25 MB exactly as it refuses 12.75.
 * `sceKernelGetFreeMemorySize` settles that in one line rather than in another
 * round trip, so it is logged once before the first request.
 *
 * And rather than stop at PHYCONT, we try a SECOND physically contiguous pool:
 * CDRAM, the 128 MB the GPU sees, which the video engine can also read. It is a
 * real candidate, not a guess stacked on a guess - and the log says which pool
 * served the block, so the answer survives the session. */
static int free_mem_reported = 0;

/* MEM-1 2026-09-14 - THE MEMORY CURVE, NOT A SINGLE READING.
 *
 * `report_free_memory` answers "what was free when the decoder started", which
 * is the question that was being asked when it was written. It cannot answer
 * the one that matters now: a session died at 104 s with an exception unwinding
 * out of an allocation, and a single reading at second 53 says nothing about
 * whether memory was draining.
 *
 * One line every 10 s of stream. A figure that holds is an exoneration; one
 * that slides names a leak and says how fast. */
void shadow_vita_report_memory(const char *tag)
{
    static uint32_t next_ms = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const uint32_t now = (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
    if (now < next_ms) return;
    next_ms = now + 10000u;

    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (sceKernelGetFreeMemorySize(&info) < 0) return;
    vlog("[MEM1] %s : user %u KB, cdram %u KB, PHYCONT %u KB", tag,
         (unsigned)(info.size_user / 1024), (unsigned)(info.size_cdram / 1024),
         (unsigned)(info.size_phycont / 1024));
}

static void report_free_memory(void)
{
    if (free_mem_reported) return;
    free_mem_reported = 1;
    SceKernelFreeMemorySizeInfo info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    const int rc = sceKernelGetFreeMemorySize(&info);
    if (rc < 0) {
        vlog("h264/vita: sceKernelGetFreeMemorySize FAIL rc=0x%08x", rc);
        return;
    }
    /* phycont is the one that matters here; the other two are printed because a
     * zero next to two healthy figures is what tells you it is a BUDGET and not
     * a machine out of memory. */
    vlog("h264/vita: free memory - user %u KB, cdram %u KB, PHYCONT %u KB",
         (unsigned)(info.size_user / 1024), (unsigned)(info.size_cdram / 1024),
         (unsigned)(info.size_phycont / 1024));
}

/* The physically contiguous pools, in the order we want them, each with the
 * grain its type is granted in. */
static const struct {
    const char           *name;
    SceKernelMemBlockType type;
    uint32_t              grain;
} PHYS_POOLS[] = {
    { "PHYCONT", SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_PHYCONT_NC_RW, 1024u * 1024u },
    { "CDRAM",   SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW,            256u * 1024u },
};

static void *alloc_phycont(const char *name, uint32_t size, SceUID *uid)
{
    report_free_memory();
    *uid = -1;

    for (size_t i = 0; i < sizeof PHYS_POOLS / sizeof PHYS_POOLS[0]; i++) {
        const uint32_t sz = ALIGN_UP(size, PHYS_POOLS[i].grain);
        const SceUID id = sceKernelAllocMemBlock(name, PHYS_POOLS[i].type,
                                                 sz, NULL);
        if (id < 0) {
            vlog("h264/vita: %s refused %u KB (asked %u) rc=0x%08x",
                 PHYS_POOLS[i].name, (unsigned)(sz / 1024),
                 (unsigned)size, (unsigned)id);
            continue;
        }
        void *base = NULL;
        const int rc = sceKernelGetMemBlockBase(id, &base);
        if (rc < 0 || !base) {
            vlog("h264/vita: %s block %u KB has no base rc=0x%08x",
                 PHYS_POOLS[i].name, (unsigned)(sz / 1024), rc);
            sceKernelFreeMemBlock(id);
            continue;
        }
        vlog("h264/vita: %u KB from %s", (unsigned)(sz / 1024),
             PHYS_POOLS[i].name);
        *uid = id;
        return base;
    }
    return NULL;
}

h264_vita *h264_vita_open(int width, int height, int ref_frames)
{
    if (width <= 0 || height <= 0) return NULL;
    if (ref_frames < 1) ref_frames = 1;

    h264_vita *d = (h264_vita *)calloc(1, sizeof *d);
    if (!d) return NULL;
    d->frame_uid = -1;
    for (int i = 0; i < VITA_PIC_SLOTS; i++) { d->pic_uid[i] = -1; d->pic_slot[i] = NULL; }
    d->pic_cur = 0;
    d->width = width; d->height = height;

    SceVideodecQueryInitInfoHwAvcdec init;
    memset(&init, 0, sizeof init);
    init.size           = sizeof init;
    init.horizontal     = (uint32_t)width;
    init.vertical       = (uint32_t)height;
    init.numOfRefFrames = (uint32_t)ref_frames;
    init.numOfStreams   = 1;              /* the SDK requires exactly 1 */

    int rc = sceVideodecInitLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC, &init);
    if (rc < 0) {
        vlog("h264/vita: sceVideodecInitLibrary(%dx%d, %d refs) FAIL rc=0x%08x %s",
             width, height, ref_frames, rc, videodec_err(rc));
        free(d); return NULL;
    }
    d->lib_open = 1;

    /* How much working memory this geometry needs. Asked rather than guessed:
     * it depends on the reference count and on the silicon's own tiling. */
    SceAvcdecQueryDecoderInfo q;
    memset(&q, 0, sizeof q);
    q.horizontal = (uint32_t)width;
    q.vertical   = (uint32_t)height;
    q.numOfRefFrames = (uint32_t)ref_frames;

    SceAvcdecDecoderInfo info;
    memset(&info, 0, sizeof info);
    rc = sceAvcdecQueryDecoderMemSize(SCE_VIDEODEC_TYPE_HW_AVCDEC, &q, &info);
    if (rc < 0) {
        vlog("h264/vita: QueryDecoderMemSize FAIL rc=0x%08x %s", rc, videodec_err(rc));
        h264_vita_close(d); return NULL;
    }

    d->ctrl.frameBuf.size = info.frameMemSize;
    d->ctrl.frameBuf.pBuf = alloc_phycont("shadow_avcdec_work",
                                          info.frameMemSize, &d->frame_uid);
    if (!d->ctrl.frameBuf.pBuf) {
        vlog("h264/vita: %u B of decoder memory REFUSED", info.frameMemSize);
        h264_vita_close(d); return NULL;
    }

    rc = sceAvcdecCreateDecoder(SCE_VIDEODEC_TYPE_HW_AVCDEC, &d->ctrl, &q);
    if (rc < 0) {
        vlog("h264/vita: CreateDecoder FAIL rc=0x%08x %s", rc, videodec_err(rc));
        h264_vita_close(d); return NULL;
    }

    /* One output picture, NV12. The pitch the hardware wants is the width
     * rounded up; we reserve for the padded geometry so a stream that is not a
     * multiple of 16 still fits. */
    const uint32_t pw = ALIGN_UP((uint32_t)width, 16u);
    const uint32_t ph = ALIGN_UP((uint32_t)height, 16u);
    d->pic_size = pw * ph * 3u / 2u;                /* Y + interleaved UV */
    for (int i = 0; i < VITA_PIC_SLOTS; i++) {
        d->pic_slot[i] = alloc_phycont("shadow_avcdec_pic", d->pic_size, &d->pic_uid[i]);
        if (!d->pic_slot[i]) break;
        /* === MAPPED FOR THE GPU =======================================
         *
         * The GXM renderer can then SAMPLE this buffer directly instead of
         * copying 1.4 MB of it into a texture of its own. Measured: removing
         * the VIEW's copy dropped `video/decode` from 18.8 to 3.7 ms, but
         * `video/televerse` rose from 7 to 21 -- the remaining copy was now
         * reading this UNCACHED memory. Moving a copy is not removing it; both
         * have to go.
         *
         * Failure here is silent, deliberately: with no mapping the renderer
         * falls back on its copy, which works. A slow picture beats no
         * picture. */
        const int mrc = sceGxmMapMemory(d->pic_slot[i], ALIGN_UP(d->pic_size, 4096u),
                                        SCE_GXM_MEMORY_ATTRIB_READ);
        if (mrc < 0 && i == 0)
            vlog("h264/vita: sceGxmMapMemory FAIL rc=0x%08x - le rendu copiera", mrc);
        else if (i == 0)
            d->pic_gpu = 1;
    }
    d->pic_base = d->pic_slot[0];
    if (!d->pic_slot[VITA_PIC_SLOTS - 1]) {
        vlog("h264/vita: %u B x %d of picture memory REFUSED",
             d->pic_size, VITA_PIC_SLOTS);
        h264_vita_close(d); return NULL;
    }

    vlog("h264/vita: hardware decoder ready - %dx%d, %d ref frame(s), "
         "%u B working + %u B picture", width, height, ref_frames,
         info.frameMemSize, d->pic_size);
    return d;
}

int h264_vita_decode(h264_vita *d, const uint8_t *au, size_t len, int64_t pts,
                     h264_vita_picture *out)
{
    if (!d || !au || !len || !out) return -1;

    /* The access unit is handed over BY POINTER: the hardware reads it where it
     * is. `vid_reasm` owns that buffer and keeps it alive across this call,
     * which is the whole reason this decoder needs no copy. */
    SceAvcdecAu a;
    memset(&a, 0, sizeof a);
    a.pts.upper = (uint32_t)((uint64_t)pts >> 32);
    a.pts.lower = (uint32_t)pts;
    a.dts       = a.pts;          /* no B-frames in this stream: DTS == PTS */
    a.es.pBuf   = (void *)(uintptr_t)au;
    a.es.size   = (uint32_t)len;

    const uint32_t pw = ALIGN_UP((uint32_t)d->width, 16u);

    SceAvcdecPicture pic;
    memset(&pic, 0, sizeof pic);
    pic.size                 = sizeof pic;
    pic.frame.pixelType      = SCE_AVCDEC_PIXELFORMAT_YUV420_PACKED_RASTER;
    pic.frame.framePitch     = pw;
    pic.frame.frameWidth     = pw;
    pic.frame.frameHeight    = ALIGN_UP((uint32_t)d->height, 16u);
    /* Rotate BEFORE decoding: that is what keeps the previous picture intact
     * for as long as the consumer has not let go of it. */
    d->pic_cur  = (d->pic_cur + 1) % VITA_PIC_SLOTS;
    d->pic_base = d->pic_slot[d->pic_cur];
    pic.frame.pPicture[0]    = d->pic_base;
    pic.frame.pPicture[1]    = (uint8_t *)d->pic_base + (size_t)pw * pic.frame.frameHeight;

    SceAvcdecPicture *plist[1] = { &pic };
    SceAvcdecArrayPicture arr;
    memset(&arr, 0, sizeof arr);
    arr.numOfElm   = 1;
    arr.pPicture   = plist;

    /* === WHAT THE SILICON COSTS, ON ITS OWN ===========================
     *
     * `video/decode` measures the whole of `h264_decoder_feed_annexb`,
     * callback included -- so, on this console, the hardware call PLUS the
     * 1.4 MB copy the view makes to recycle its buffer (VI2). It reads 19.8 ms
     * here against 7.8 on Switch, and nothing in that figure says which of the
     * two is expensive.
     *
     * This one brackets `sceAvcdecDecode` alone. The difference between the
     * two IS the price of the copy, and it decides what comes next: speeding
     * up silicon that already returns in 5 ms would change nothing. */
    static int64_t sum_us = 0;
    static uint32_t n_dec = 0;
    struct timespec t_a, t_b;
    const int timed = (clock_gettime(CLOCK_MONOTONIC, &t_a) == 0);

    const int rc = sceAvcdecDecode(&d->ctrl, &a, &arr);

    if (timed && clock_gettime(CLOCK_MONOTONIC, &t_b) == 0) {
        sum_us += (int64_t)(t_b.tv_sec - t_a.tv_sec) * 1000000
                + (t_b.tv_nsec - t_a.tv_nsec) / 1000;
        if (++n_dec % 300 == 0) {
            shadow_vita_report_memory("flux");
            vlog("h264/vita: sceAvcdecDecode seul = %.1f ms en moyenne (%u images)",
                 (double)sum_us / (double)n_dec / 1000.0, n_dec);
        }
    }
    if (rc < 0) return -1;
    if (arr.numOfOutput == 0) return 0;        /* consumed, nothing out yet */

    /* The DISPLAYABLE size is the coded size minus the SPS crop. Reporting the
     * padded geometry instead would show the encoder's alignment rows, which is
     * the green-band defect every port of this kind hits once. */
    out->y      = (const uint8_t *)pic.frame.pPicture[0];
    out->uv     = (const uint8_t *)pic.frame.pPicture[1];
    out->pitch  = (int)pic.frame.framePitch;
    out->width  = (int)(pic.frame.horizontalSize
                        - pic.frame.frameCropLeftOffset - pic.frame.frameCropRightOffset);
    out->height = (int)(pic.frame.verticalSize
                        - pic.frame.frameCropTopOffset - pic.frame.frameCropBottomOffset);
    if (out->width  <= 0) out->width  = d->width;
    if (out->height <= 0) out->height = d->height;
    out->pts = pts;
    return 1;
}

void h264_vita_close(h264_vita *d)
{
    if (!d) return;
    if (d->ctrl.handle) sceAvcdecDeleteDecoder(&d->ctrl);
    for (int i = 0; i < VITA_PIC_SLOTS; i++)
        if (d->pic_uid[i] >= 0) sceKernelFreeMemBlock(d->pic_uid[i]);
    if (d->frame_uid >= 0) sceKernelFreeMemBlock(d->frame_uid);
    if (d->lib_open) sceVideodecTermLibrary(SCE_VIDEODEC_TYPE_HW_AVCDEC);
    free(d);
}

#else
/* Not a Vita: empty on purpose, and a typedef rather than a `static const`
 * for the reason spelled out in `audio_out_vita.c`. */
typedef int h264_decoder_vita_vita_only;
#endif /* __vita__ */
