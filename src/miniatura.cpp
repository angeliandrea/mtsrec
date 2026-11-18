#include "miniatura.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "traccia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
}

void Miniatura::forse_copia(const uint8_t* uyvy, int rb, int larghezza, int altezza, int64_t numero) {
    if (!uyvy || numero % 5 != 0) return;
    if (adesso_us() - guardata_us_.load() > 5'000'000) return;       // nessuno guarda: niente
    std::unique_lock<std::mutex> l(m_, std::try_to_lock);
    if (!l.owns_lock()) return;                                       // la pagina sta leggendo: si salta
    const int px = larghezza / L, py = altezza / A;                   // 6 e 6 in 1080
    for (int r = 0; r < A; ++r) {
        const uint8_t* s = uyvy + static_cast<size_t>(r) * py * rb;
        for (int c = 0; c < L; ++c) y_[r * L + c] = s[2 * (c * px) + 1];
        for (int c = 0; c < L / 2; ++c) {
            const uint8_t* coppia = s + 4 * ((c * 2 * px) / 2);
            u_[r * L / 2 + c] = coppia[0];
            v_[r * L / 2 + c] = coppia[2];
        }
    }
    pronta_ = true;
    numero_ = numero;
}

bool Miniatura::aspetta_se_prima_volta() {
    // Finché nessuno guarda l'ingresso non copia niente: alla prima richiesta si aspetta il primo
    // fotogramma utile, al più 300 ms. Aspetta il filo della pagina, mai la registrazione.
    const bool prima_volta = adesso_us() - guardata_us_.load() > 5'000'000;
    guardata_us_ = adesso_us();
    for (int i = 0; prima_volta && i < 30; ++i) {
        { std::lock_guard<std::mutex> l(m_); if (pronta_) break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return prima_volta;
}

std::string Miniatura::misura() {
    aspetta_se_prima_volta();
    std::lock_guard<std::mutex> l(m_);
    if (!pronta_) return "{\"y\":null}";
    double s = 0, s2 = 0;
    for (uint8_t v : y_) { s += v; s2 += double(v) * v; }
    const double n = static_cast<double>(y_.size()), media = s / n;
    char b[96];
    std::snprintf(b, sizeof b, "{\"y\":%.1f,\"scarto\":%.1f,\"fotogramma\":%lld}", media,
                  std::sqrt(std::max(0.0, s2 / n - media * media)), static_cast<long long>(numero_));
    return b;
}

std::string Miniatura::jpeg() {
    aspetta_se_prima_volta();
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) return {};
    AVFrame* f = av_frame_alloc();
    f->format = AV_PIX_FMT_YUVJ422P;
    f->width = L;
    f->height = A;
    av_frame_get_buffer(f, 0);
    {
        std::lock_guard<std::mutex> l(m_);
        if (!pronta_) { av_frame_free(&f); return {}; }
        // L'SDI è a gamma limitata e il JPEG a gamma piena: si allarga, se no la finestrella esce slavata.
        for (int r = 0; r < A; ++r) {
            for (int c = 0; c < L; ++c)
                f->data[0][r * f->linesize[0] + c] = static_cast<uint8_t>(std::clamp((y_[r * L + c] - 16) * 255 / 219, 0, 255));
            for (int c = 0; c < L / 2; ++c) {
                f->data[1][r * f->linesize[1] + c] = static_cast<uint8_t>(std::clamp(128 + (u_[r * L / 2 + c] - 128) * 255 / 224, 0, 255));
                f->data[2][r * f->linesize[2] + c] = static_cast<uint8_t>(std::clamp(128 + (v_[r * L / 2 + c] - 128) * 255 / 224, 0, 255));
            }
        }
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->width = L;
    ctx->height = A;
    ctx->pix_fmt = AV_PIX_FMT_YUVJ422P;
    ctx->time_base = AVRational{1, 25};
    ctx->flags |= AV_CODEC_FLAG_QSCALE;
    std::string out;
    if (avcodec_open2(ctx, codec, nullptr) == 0) {
        f->quality = FF_QP2LAMBDA * 4;
        AVPacket* p = av_packet_alloc();
        if (avcodec_send_frame(ctx, f) == 0 && avcodec_receive_packet(ctx, p) == 0)
            out.assign(reinterpret_cast<const char*>(p->data), p->size);
        av_packet_free(&p);
    }
    avcodec_free_context(&ctx);
    av_frame_free(&f);
    return out;
}
