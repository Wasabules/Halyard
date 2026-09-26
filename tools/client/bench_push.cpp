/* bench_push.cpp - the cost of StreamView::pushYuvFrame, extracted as a pure
 * module. Reproduces identically the three gestures of stream_view.cpp:398-437:
 *   1. writer_slot.y.assign(data_y, data_y + linesize_y*height)
 *   2. writer_slot.u.resize(uv_stride*uv_h) then the U/V interleaving loop
 *   3. moving the vectors into the queue (std::move)
 * Then, separately, the NV12 path (two assigns, no interleaving).
 */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>
#include <chrono>
using clk = std::chrono::steady_clock;
static double us(clk::time_point a, clk::time_point b) {
    return std::chrono::duration<double, std::micro>(b - a).count();
}

struct YuvFrame { std::vector<uint8_t> y, u, v; int w,h,ys,us_,vs; bool nv12; };

int main(int argc, char** argv) {
    int W = argc>1?atoi(argv[1]):1920, H = argc>2?atoi(argv[2]):1080;
    int N = argc>3?atoi(argv[3]):300;
    int ys = W, us_ = W/2, vs = W/2;
    std::vector<uint8_t> Y((size_t)ys*H), U((size_t)us_*(H/2)), V((size_t)vs*(H/2));
    for (size_t i=0;i<Y.size();i++) Y[i]=(uint8_t)i;
    for (size_t i=0;i<U.size();i++) { U[i]=(uint8_t)(i*3); V[i]=(uint8_t)(i*7); }

    YuvFrame w_slot; std::vector<YuvFrame> q;
    double t_yuv=0, t_nv12=0; volatile uint8_t sink=0;

    /* --- the YUV420P path (SOFTWARE decoding: the Switch's) --- */
    for (int it=0; it<N; it++) {
        auto t0 = clk::now();
        w_slot.y.assign(Y.data(), Y.data()+(size_t)ys*H);
        int uvw = W/2, uvh = H/2; size_t stride = (size_t)uvw*2;
        w_slot.u.resize(stride*uvh);
        uint8_t* dst = w_slot.u.data();
        for (int row=0; row<uvh; row++) {
            const uint8_t* usrc = U.data()+ (size_t)row*us_;
            const uint8_t* vsrc = V.data()+ (size_t)row*vs;
            uint8_t* d = dst + (size_t)row*stride;
            for (int col=0; col<uvw; col++) { d[2*col]=usrc[col]; d[2*col+1]=vsrc[col]; }
        }
        w_slot.v.clear();
        YuvFrame f;
        f.y = std::move(w_slot.y); f.u = std::move(w_slot.u); f.v = std::move(w_slot.v);
        auto t1 = clk::now();
        t_yuv += us(t0,t1);
        sink ^= f.y[0] ^ f.u[0];
        w_slot.y = std::move(f.y); w_slot.u = std::move(f.u);   /* recyclage */
        w_slot.y.clear(); w_slot.u.clear();
    }

    /* --- the NV12 path (HARDWARE decoding): two assignments, nothing else --- */
    std::vector<uint8_t> UV((size_t)W*(H/2));
    for (int it=0; it<N; it++) {
        auto t0 = clk::now();
        w_slot.y.assign(Y.data(), Y.data()+(size_t)ys*H);
        w_slot.u.assign(UV.data(), UV.data()+(size_t)W*(H/2));
        w_slot.v.clear();
        YuvFrame f; f.y=std::move(w_slot.y); f.u=std::move(w_slot.u); f.v=std::move(w_slot.v);
        auto t1 = clk::now();
        t_nv12 += us(t0,t1);
        sink ^= f.y[0];
        w_slot.y=std::move(f.y); w_slot.u=std::move(f.u);
        w_slot.y.clear(); w_slot.u.clear();
    }
    printf("%dx%d, %d iterations  (sink=%u)\n", W,H,N,(unsigned)sink);
    printf("  pushYuvFrame chemin YUV420P (copie Y + entrelacement U/V) : %.3f ms/image\n", t_yuv/N/1000.0);
    printf("  pushYuvFrame chemin NV12    (deux copies)                 : %.3f ms/image\n", t_nv12/N/1000.0);
    printf("  octets recopies : Y=%zu  UV=%zu  total=%.2f Mo/image\n",
           (size_t)ys*H, (size_t)W*(H/2), ((double)ys*H + (double)W*(H/2))/1048576.0);
    return 0;
}
