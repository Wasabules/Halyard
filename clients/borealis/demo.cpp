/* demo.cpp - see demo.hpp. */

#include "demo.hpp"

#include <borealis.hpp>

#include "activity/shadow_app.hpp"
#include "activity/stream_view.hpp"
#include "ui/i18n.hpp"

#include <borealis/core/assets.hpp>   /* BRLS_RESOURCES */

extern "C" {
#include "../../core/common/stats.h"
#include "../../core/services/journal.h"
#include <libavutil/pixfmt.h>
/* nanovg compiles stb_image into Borealis (nanovg.c), with extern linkage:
 * the decoder is already in the binary, only its declaration is missing. */
unsigned char *stbi_load(const char *filename, int *x, int *y, int *comp, int req_comp);
void stbi_image_free(void *retval_from_stbi_load);
}

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>

namespace demo {

static bool env_on(const char *name)
{
    const char *e = std::getenv(name);
    return e && std::atoi(e) != 0;
}

/* Process-wide on purpose: the demo is a way of RUNNING the app, not session
 * state, and it cannot change while the app runs. */
bool enabled()     { static const bool on = env_on("SHADOW_DEMO"); return on; }
bool holdPairing() { return enabled() && env_on("SHADOW_DEMO_PAIRING"); }

const char *datacenter()  { return "Europe West"; }
const char *pairingCode() { return "HLYD-2026"; }
const char *pairingUrl()  { return "https://example.com/device"; }

void account(std::string &plan, std::string &drive)
{
    plan  = ui::tr("vm/plan", "Demo") + " - " + ui::tr("vm/plan_since", "01/09/2026");
    drive = ui::tr("vm/drive_on");
}

void fillVms()
{
    ShadowApp &app = ShadowApp::instance();
    app.vms.clear();
    const char *names[][2] = {
        {"demo-vm-1", "Gaming PC"},
        {"demo-vm-2", "Workstation"},
        {"demo-vm-3", "Render node"},
    };
    for (const auto &n : names) {
        ShadowVm v;
        v.id = n[0];
        v.name = n[1];
        v.alias = n[1];
        app.vms.push_back(v);
    }
}

/* The picture: the app's own background, converted once to I420. */
struct Picture {
    int w = 0, h = 0;
    std::vector<uint8_t> y, u, v;
};

static bool load_picture(Picture &p)
{
    /* SHADOW_DEMO_PICTURE names another image (any size, JPEG or PNG): the
     * app's background makes a dim "game". */
    const char *env = std::getenv("SHADOW_DEMO_PICTURE");
    const std::string path = (env && *env) ? std::string(env)
                                           : std::string(BRLS_RESOURCES) + "img/background.jpg";
    int w = 0, h = 0, comp = 0;
    unsigned char *rgb = stbi_load(path.c_str(), &w, &h, &comp, 3);
    if (!rgb || w < 2 || h < 2) {
        if (rgb) stbi_image_free(rgb);
        return false;
    }
    /* Larger than 1080p: box-average down by an integer factor, so the view
     * uploads a stream-sized picture 60 times a second, not a poster. */
    const int f = (w + 1919) / 1920;
    if (f > 1) {
        const int nw = w / f, nh = h / f;
        unsigned char *small = (unsigned char *)std::malloc((size_t)nw * nh * 3);
        if (small) {
            for (int yy = 0; yy < nh; yy++)
                for (int xx = 0; xx < nw; xx++)
                    for (int c = 0; c < 3; c++) {
                        unsigned sum = 0;
                        for (int dy = 0; dy < f; dy++)
                            for (int dx = 0; dx < f; dx++)
                                sum += rgb[((size_t)(yy * f + dy) * w + (xx * f + dx)) * 3 + c];
                        small[((size_t)yy * nw + xx) * 3 + c] = (unsigned char)(sum / (f * f));
                    }
            stbi_image_free(rgb);
            rgb = small;
            w = nw;
            h = nh;
        }
    }
    w &= ~1;
    h &= ~1;
    p.w = w;
    p.h = h;
    p.y.resize((size_t)w * h);
    p.u.resize((size_t)(w / 2) * (h / 2));
    p.v.resize((size_t)(w / 2) * (h / 2));
    /* BT.709, limited range: what the renderer expects from a real stream
     * (COL1 - the stream is limited-range BT.709). */
    auto px = [&](int x, int y, float &r, float &g, float &b) {
        const unsigned char *s = rgb + ((size_t)y * (size_t)(w) + x) * 3;
        r = s[0] / 255.0f; g = s[1] / 255.0f; b = s[2] / 255.0f;
    };
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            float r, g, b;
            px(xx, yy, r, g, b);
            const float Y = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            p.y[(size_t)yy * w + xx] = (uint8_t)(16.0f + 219.0f * Y + 0.5f);
        }
    for (int yy = 0; yy < h; yy += 2)
        for (int xx = 0; xx < w; xx += 2) {
            float r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 2; dy++)
                for (int dx = 0; dx < 2; dx++) {
                    float rr, gg, bb;
                    px(xx + dx, yy + dy, rr, gg, bb);
                    r += rr; g += gg; b += bb;
                }
            r /= 4; g /= 4; b /= 4;
            const float Y  = 0.2126f * r + 0.7152f * g + 0.0722f * b;
            const float Cb = (b - Y) / 1.8556f;
            const float Cr = (r - Y) / 1.5748f;
            const size_t i = (size_t)(yy / 2) * (w / 2) + xx / 2;
            p.u[i] = (uint8_t)(128.0f + 224.0f * Cb + 0.5f);
            p.v[i] = (uint8_t)(128.0f + 224.0f * Cr + 0.5f);
        }
    std::free(rgb);   /* stb's default allocator is malloc, like the downscale's */
    return true;
}

void runStream(const std::function<bool()> &abandon)
{
    Picture pic;
    if (!load_picture(pic)) {
        JOURNAL_WARN_(JOURNAL_CAT_UI, "[DEMO-1] no picture for the demo stream");
    } else {
        JOURNAL_INFO_(JOURNAL_CAT_UI, "[DEMO-1] demo stream %dx%d", pic.w, pic.h);
    }

    session_stats_reset();
    session_stats_t st = {};
    st.h264_width  = pic.w;
    st.h264_height = pic.h;
    st.kernel_drops_valid = true;

    using clock = std::chrono::steady_clock;
    const auto start = clock::now();
    auto next_frame = start;
    auto next_stats = start;
    int64_t pts = 0;
    /* Plausible and steady: 60 frames, ~25 Mb/s of video in ~1200-byte
     * chunks, Opus at 100 packets a second, an 18 ms round trip. */
    const double video_bps = 25e6;
    uint64_t frames = 0;

    while (!abandon()) {
        const auto now = clock::now();
        if (now >= next_frame && pic.w > 0) {
            stream_view_push_yuv(pic.w, pic.h,
                                 pic.y.data(), pic.w,
                                 pic.u.data(), pic.w / 2,
                                 pic.v.data(), pic.w / 2,
                                 AV_PIX_FMT_YUV420P, pts, nullptr);
            pts += 16667;
            frames++;
            next_frame += std::chrono::microseconds(16667);
        }
        if (now >= next_stats) {
            const double t = std::chrono::duration<double>(now - start).count();
            st.session_seconds = (int)t;
            st.rtp_video_bytes = (uint64_t)(video_bps / 8.0 * t);
            st.rtp_video_packets = (uint32_t)(st.rtp_video_bytes / 1200);
            st.chunks_expected = st.rtp_video_packets;
            st.rtp_audio_packets = (uint32_t)(100.0 * t);
            st.rtp_audio_bytes = (uint64_t)st.rtp_audio_packets * 160;
            st.opus_packets = st.opus_decoded = st.opus_pushed = st.rtp_audio_packets;
            st.h264_frames_decoded = (uint32_t)frames;
            st.ctrl_rtt_us = 18000 + (uint32_t)((frames * 37) % 1500);
            st.ctrl_rtt_avg_us = 18200;
            st.ctrl_rtt_p90_us = 19400;
            st.ctrl_rtt_jitter_us = 600;
            session_stats_publish(&st);
            next_stats = now + std::chrono::milliseconds(250);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    session_stats_reset();
}

}  // namespace demo
