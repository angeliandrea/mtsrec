// mtsrec --sonda FILE.mov: timecode di partenza, numero di fotogrammi e audio di un MOV, in JSON
// su una riga. Prende il posto di ffprobe.exe, che non è firmato e che Smart App Control di Windows 11
// bloccherebbe (sezione 9.81): lo usa script\progetto_da_registratore.py per costruire il progetto.
//
//   {"timecode":"10:00:00:00","fotogrammi":1500,"audio":true}
//
// I fotogrammi si leggono dall'indice del file, in un attimo. Si contano i pacchetti solo se l'indice non
// lo dice, come in un MOV a frammenti rimasto aperto: con dieci tracce da un'ora contarli sempre faceva
// partire il progetto dopo mezz'ora (15 novembre 2026).
#include <windows.h>
#include <cstdio>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include "sonda.h"

static std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string json_testo(const std::string& s) {
    std::string r = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { r += '\\'; r += c; }
        else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); r += b; }
        else r += c;
    }
    return r + "\"";
}

int sonda(const std::wstring& percorso) {
    av_log_set_level(AV_LOG_ERROR);
    const std::string p = utf8(percorso);   // ffmpeg su Windows apre i percorsi in UTF-8
    AVFormatContext* fc = nullptr;
    if (int e = avformat_open_input(&fc, p.c_str(), nullptr, nullptr); e < 0) {
        char b[256]; av_strerror(e, b, sizeof b);
        std::fprintf(stderr, "cannot open %s: %s\n", p.c_str(), b);
        return 1;
    }
    int video = -1;
    bool audio = false;
    std::string tc;
    for (unsigned i = 0; i < fc->nb_streams; ++i) {
        const AVStream* st = fc->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && video < 0) video = (int)i;
        if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) audio = true;
        // Il timecode il demuxer MOV lo mette sulla traccia video, dalla traccia tmcd; come ffprobe si
        // prende il primo che c'è, video o tmcd.
        if (tc.empty())
            if (const AVDictionaryEntry* t = av_dict_get(st->metadata, "timecode", nullptr, 0)) tc = t->value;
    }
    if (video < 0) { std::fprintf(stderr, "no video stream in %s\n", p.c_str()); avformat_close_input(&fc); return 1; }
    if (tc.empty()) { std::fprintf(stderr, "no timecode in %s\n", p.c_str()); avformat_close_input(&fc); return 1; }
    int64_t fotogrammi = fc->streams[video]->nb_frames;
    if (fotogrammi <= 0) {
        fotogrammi = 0;
        AVPacket* pk = av_packet_alloc();
        while (av_read_frame(fc, pk) >= 0) {
            if (pk->stream_index == video) ++fotogrammi;
            av_packet_unref(pk);
        }
        av_packet_free(&pk);
    }
    avformat_close_input(&fc);
    std::printf("{\"timecode\":%s,\"fotogrammi\":%lld,\"audio\":%s}\n", json_testo(tc).c_str(),
                (long long)fotogrammi, audio ? "true" : "false");
    return 0;
}
