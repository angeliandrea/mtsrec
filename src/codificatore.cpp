#include "codificatore.h"
#include "formato.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
}

static std::string testo_errore(int e) {
    char b[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(e, b, sizeof b);
    return b;
}

Codificatore::~Codificatore() {
    std::string ignora;
    chiudi(ignora);
}

// La traccia audio PCM 24 bit a 48 kHz, uguale nel MOV e nel WAV. Il PCM non passa da un
// codificatore: i pacchetti si fanno a mano in scrivi_audio.
static AVStream* traccia_audio(AVFormatContext* f, int canali) {
    AVStream* s = avformat_new_stream(f, nullptr);
    AVCodecParameters* c = s->codecpar;
    c->codec_type = AVMEDIA_TYPE_AUDIO;
    c->codec_id = AV_CODEC_ID_PCM_S24LE;
    c->sample_rate = 48000;
    av_channel_layout_default(&c->ch_layout, canali);
    c->bits_per_coded_sample = 24;
    c->block_align = canali * 3;
    c->bit_rate = 48000LL * canali * 24;
    s->time_base = AVRational{1, 48000};
    return s;
}

bool Codificatore::apri(const std::string& percorso, const ParametriCodifica& p, int tc_iniziale,
                        std::string& errore) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_DNXHD);
    if (!codec) { errore = "DNxHD encoder not available in ffmpeg"; return false; }
    // «10:00:00:00», col drop-frame «10:00:00;00»: il punto e virgola dice al MOV che è drop-frame
    const std::string tc = p.cad.testo(tc_iniziale);
    cad_ = p.cad;
    tc_iniziale_ = tc_iniziale;
    const bool su_file = !percorso.empty();

    int e = 0;
    if (su_file) {
        e = avformat_alloc_output_context2(&fmt_, nullptr, "mov", (percorso + ".mov").c_str());
        if (e < 0) { errore = "MOV container: " + testo_errore(e); return false; }
    }

    ctx_ = avcodec_alloc_context3(codec);
    ctx_->width = p.larghezza;
    ctx_->height = p.altezza;
    ctx_->pix_fmt = AV_PIX_FMT_YUV422P;
    ctx_->time_base = AVRational{p.cad.den, p.cad.num};
    ctx_->framerate = AVRational{p.cad.num, p.cad.den};
    // DNxHD col bitrate; DNxHR (UHD) col profilo, che fissa da solo la dimensione dei fotogrammi
    const char* profilo = profilo_dnxhr(p.mbit);
    if (profilo) av_opt_set(ctx_->priv_data, "profile", profilo, 0);
    else ctx_->bit_rate = static_cast<int64_t>(p.mbit) * 1000000;
    // In UHD un fotogramma è quattro volte l'HD: con 4 fette per traccia poche tracce lasciavano fermo metà
    // processore (4 tracce DNxHR HQ: 108 fotogrammi al secondo con 4 fette, sezione 9.74)
    ctx_->thread_count = p.altezza > 1080 ? std::max(p.thread, 8) : p.thread;
    ctx_->thread_type = FF_THREAD_SLICE;
    ctx_->color_range = AVCOL_RANGE_MPEG;
    ctx_->colorspace = AVCOL_SPC_BT709;
    ctx_->color_primaries = AVCOL_PRI_BT709;
    ctx_->color_trc = AVCOL_TRC_BT709;
    if (p.interlacciato) {
        ctx_->flags |= AV_CODEC_FLAG_INTERLACED_DCT;
        ctx_->field_order = AV_FIELD_TT;          // l'HD interlacciato comincia dal campo superiore
    }
    if (fmt_ && (fmt_->oformat->flags & AVFMT_GLOBALHEADER))
        ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    e = avcodec_open2(ctx_, codec, nullptr);
    if (e < 0) {
        errore = nome_codec(p.mbit) + " at " + std::to_string(p.larghezza) + "x" + std::to_string(p.altezza) +
                 " not accepted: " + testo_errore(e);
        return false;
    }

    canali_ = su_file ? p.canali_audio : 0;
    coda_audio_.clear();
    if (su_file && !apri_file(percorso, p, tc.c_str(), errore)) return false;

    frame_ = av_frame_alloc();
    frame_->format = ctx_->pix_fmt;
    frame_->width = ctx_->width;
    frame_->height = ctx_->height;
    if (p.interlacciato)
        frame_->flags |= AV_FRAME_FLAG_INTERLACED | AV_FRAME_FLAG_TOP_FIELD_FIRST;
    e = av_frame_get_buffer(frame_, 0);
    if (e < 0) { errore = "frame memory: " + testo_errore(e); return false; }
    pkt_ = av_packet_alloc();
    aperto_ = true;
    return true;
}

bool Codificatore::apri_file(const std::string& percorso, const ParametriCodifica& p, const char* tc,
                             std::string& errore) {
    st_ = avformat_new_stream(fmt_, nullptr);
    avcodec_parameters_from_context(st_->codecpar, ctx_);
    st_->time_base = ctx_->time_base;
    st_->avg_frame_rate = ctx_->framerate;
    av_dict_set(&fmt_->metadata, "timecode", tc, 0);
    if (canali_ > 0) st_audio_ = traccia_audio(fmt_, canali_);

    int e = avio_open(&fmt_->pb, (percorso + ".mov").c_str(), AVIO_FLAG_WRITE);
    if (e < 0) { errore = "file " + percorso + ".mov: " + testo_errore(e); return false; }

    // Frammenti da almeno un secondo: ogni fotogramma DNxHD è un keyframe, e senza il minimo
    // il MOV avrebbe un frammento per fotogramma. delay_moov e non empty_moov: l'intestazione
    // si scrive dopo il primo fotogramma, da cui il MOV copia i dati DNxHD che servono ai
    // programmi Avid e a Resolve. Con empty_moov ffmpeg avvisa «vos_data too small».
    // hybrid_fragmented: alla chiusura ffmpeg riscrive il MOV come normale, senza copiarlo.
    // Resolve non decodifica l'audio PCM di un MOV a frammenti e trascinandolo si chiude
    // (31 ottobre), e Premiere non lo collega (7 novembre). Un file interrotto resta a frammenti:
    // la riparazione automatica lo riscrive come MOV normale (ripara.cpp, frammentato()).
    AVDictionary* opz = nullptr;
    av_dict_set(&opz, "movflags", "+hybrid_fragmented+frag_keyframe+delay_moov+default_base_moof", 0);
    av_dict_set(&opz, "min_frag_duration", "1000000", 0);
    e = avformat_write_header(fmt_, &opz);
    av_dict_free(&opz);
    if (e < 0) { errore = "MOV header: " + testo_errore(e); return false; }

    if (canali_ > 0 && p.wav) {
        // Broadcast WAV: nel bext il punto di partenza in campioni dalla mezzanotte, che Resolve
        // e i programmi audio leggono come timecode. rf64 se supera i 4 GB, a 16 canali dopo 23 minuti.
        e = avformat_alloc_output_context2(&wav_, nullptr, "wav", (percorso + ".wav").c_str());
        if (e < 0) { errore = "WAV container: " + testo_errore(e); return false; }
        traccia_audio(wav_, canali_);
        // Il tempo vero dalla mezzanotte: a 29,97 un fotogramma dura 1001/30000 di secondo.
        const long long campioni_da_mezzanotte = p.cad.campioni_da_mezzanotte(tc_iniziale_);
        av_dict_set(&wav_->metadata, "time_reference", std::to_string(campioni_da_mezzanotte).c_str(), 0);
        av_dict_set(&wav_->metadata, "originator", "mtsrec", 0);
        av_dict_set(&wav_->metadata, "description", (std::string("timecode ") + tc).c_str(), 0);
        e = avio_open(&wav_->pb, (percorso + ".wav").c_str(), AVIO_FLAG_WRITE);
        if (e < 0) { errore = "file " + percorso + ".wav: " + testo_errore(e); return false; }
        av_dict_set(&opz, "write_bext", "1", 0);
        av_dict_set(&opz, "rf64", "auto", 0);
        e = avformat_write_header(wav_, &opz);
        av_dict_free(&opz);
        if (e < 0) { errore = "WAV header: " + testo_errore(e); return false; }
    }
    if (canali_ > 0) pkt_audio_ = av_packet_alloc();
    return true;
}

bool Codificatore::scrivi(const uint8_t* uyvy, int byte_per_riga, const int32_t* audio, int n_audio, std::string& errore) {
    int e = av_frame_make_writable(frame_);
    if (e < 0) { errore = "frame not writable: " + testo_errore(e); return false; }

    // UYVY intrecciato → tre piani Y, U, V. Nessun cambio di risoluzione né di colore:
    // basta separare i byte, ed è molto più rapido di swscale.
    const int w = ctx_->width;
    for (int y = 0; y < ctx_->height; ++y) {
        const uint8_t* s = uyvy + static_cast<size_t>(y) * byte_per_riga;
        uint8_t* Y = frame_->data[0] + static_cast<size_t>(y) * frame_->linesize[0];
        uint8_t* U = frame_->data[1] + static_cast<size_t>(y) * frame_->linesize[1];
        uint8_t* V = frame_->data[2] + static_cast<size_t>(y) * frame_->linesize[2];
        for (int x = 0; x < w / 2; ++x) {
            U[x] = s[4 * x];
            Y[2 * x] = s[4 * x + 1];
            V[x] = s[4 * x + 2];
            Y[2 * x + 1] = s[4 * x + 3];
        }
    }
    const int64_t k = pts_;                         // il fotogramma, contato dalla partenza del file
    frame_->pts = pts_++;
    e = avcodec_send_frame(ctx_, frame_);
    if (e < 0) { errore = "encoding: " + testo_errore(e); return false; }
    if (!svuota(false, errore)) return false;
    if (canali_ == 0) return true;
    // I campioni che spettano a questo fotogramma: a 25p sempre 1920, a 29,97 1601 o 1602. Quelli arrivati
    // vanno in coda e se ne prendono esattamente tanti; se mancano si completa col silenzio. Un fotogramma
    // ripetuto o nero (audio nullptr) ha il silenzio, ma prima esce quello che era rimasto in coda.
    const int serve = cad_.campioni(k);
    if (audio && n_audio > 0) coda_audio_.insert(coda_audio_.end(), audio, audio + static_cast<size_t>(n_audio) * canali_);
    // La coda non deve crescere: la DeckLink dà in media gli stessi campioni, al massimo uno di scarto.
    const size_t tetto = static_cast<size_t>(cad_.campioni_max()) * 2 * canali_;
    if (coda_audio_.size() > tetto) coda_audio_.erase(coda_audio_.begin(), coda_audio_.end() - static_cast<std::ptrdiff_t>(tetto));
    return scrivi_audio(coda_audio_.data(), serve, errore);
}

bool Codificatore::scrivi_audio(const int32_t* audio, int campioni, std::string& errore) {
    // La DeckLink dà interi a 32 bit con i 24 bit dell'SDI in alto: si tengono i tre byte alti.
    const int n = campioni * canali_;
    const int dalla_coda = std::min(n, static_cast<int>(coda_audio_.size()));
    int e = av_new_packet(pkt_audio_, n * 3);
    if (e < 0) { errore = "audio memory: " + testo_errore(e); return false; }
    uint8_t* d = pkt_audio_->data;
    for (int i = 0; i < dalla_coda; ++i) {
        const uint32_t v = static_cast<uint32_t>(audio[i]);
        d[3 * i] = static_cast<uint8_t>(v >> 8);
        d[3 * i + 1] = static_cast<uint8_t>(v >> 16);
        d[3 * i + 2] = static_cast<uint8_t>(v >> 24);
    }
    std::memset(d + static_cast<size_t>(dalla_coda) * 3, 0, static_cast<size_t>(n - dalla_coda) * 3);
    coda_audio_.erase(coda_audio_.begin(), coda_audio_.begin() + dalla_coda);
    pkt_audio_->pts = pkt_audio_->dts = pts_audio_;
    pkt_audio_->duration = campioni;
    pts_audio_ += campioni;
    if (wav_) {
        AVPacket* copia = av_packet_clone(pkt_audio_);
        copia->stream_index = 0;
        e = av_write_frame(wav_, copia);
        av_packet_free(&copia);
        if (e < 0) { av_packet_unref(pkt_audio_); errore = "WAV write: " + testo_errore(e); return false; }
    }
    pkt_audio_->stream_index = st_audio_->index;
    av_packet_rescale_ts(pkt_audio_, AVRational{1, 48000}, st_audio_->time_base);
    e = av_interleaved_write_frame(fmt_, pkt_audio_);
    if (e < 0) { errore = "audio write: " + testo_errore(e); return false; }
    return true;
}

bool Codificatore::svuota(bool fine, std::string& errore) {
    if (fine) avcodec_send_frame(ctx_, nullptr);
    for (;;) {
        int e = avcodec_receive_packet(ctx_, pkt_);
        if (e == AVERROR(EAGAIN) || e == AVERROR_EOF) return true;
        if (e < 0) { errore = "encoding: " + testo_errore(e); return false; }
        if (!fmt_) { av_packet_unref(pkt_); continue; }           // solo codifica: si butta
        av_packet_rescale_ts(pkt_, ctx_->time_base, st_->time_base);
        pkt_->stream_index = st_->index;
        e = av_interleaved_write_frame(fmt_, pkt_);   // con l'audio i due flussi vanno intrecciati
        av_packet_unref(pkt_);
        if (e < 0) { errore = "write: " + testo_errore(e); return false; }
    }
}

bool Codificatore::chiudi(std::string& errore) {
    bool ok = true;
    if (aperto_) {
        ok = svuota(true, errore);
        int e = fmt_ ? av_write_trailer(fmt_) : 0;
        if (e < 0 && ok) { errore = "MOV close: " + testo_errore(e); ok = false; }
        if (wav_) {
            e = av_write_trailer(wav_);
            if (e < 0 && ok) { errore = "WAV close: " + testo_errore(e); ok = false; }
        }
        aperto_ = false;
    }
    if (fmt_ && fmt_->pb) avio_closep(&fmt_->pb);
    if (wav_ && wav_->pb) avio_closep(&wav_->pb);
    if (wav_) { avformat_free_context(wav_); wav_ = nullptr; }
    av_packet_free(&pkt_audio_);
    av_packet_free(&pkt_);
    av_frame_free(&frame_);
    avcodec_free_context(&ctx_);
    if (fmt_) { avformat_free_context(fmt_); fmt_ = nullptr; }
    return ok;
}
