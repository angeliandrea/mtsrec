#include "ripara.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <thread>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
}

namespace fs = std::filesystem;

namespace {

std::string u8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// Quello che serve sapere di un file: i pacchetti video e quanti campioni interi di audio.
struct Esame {
    bool aperto = false;
    std::vector<int> video;           // dimensione di ogni fotogramma
    int64_t campioni = 0;             // campioni audio interi
    int blocco = 0;                   // byte per campione, tutti i canali
    bool ha_audio = false;
    std::string tc;
};

Esame esamina(const fs::path& p) {
    Esame e;
    AVFormatContext* f = nullptr;
    if (avformat_open_input(&f, u8(p).c_str(), nullptr, nullptr) < 0) return e;
    if (avformat_find_stream_info(f, nullptr) < 0) { avformat_close_input(&f); return e; }
    e.aperto = true;
    const int v = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int a = av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (v >= 0)
        if (const AVDictionaryEntry* t = av_dict_get(f->streams[v]->metadata, "timecode", nullptr, 0)) e.tc = t->value;
    if (e.tc.empty())
        if (const AVDictionaryEntry* t = av_dict_get(f->metadata, "timecode", nullptr, 0)) e.tc = t->value;
    if (a >= 0) {
        const AVCodecParameters* c = f->streams[a]->codecpar;
        e.ha_audio = true;
        e.blocco = c->ch_layout.nb_channels * std::max(c->bits_per_coded_sample, 8) / 8;
    }
    // Un pacchetto audio è troncato se ha meno byte di quelli che servono per la durata che dichiara:
    // non se è solo più corto degli altri (sezione 9.53, la terza trappola).
    std::vector<std::pair<int, int64_t>> audio;     // byte, campioni dichiarati
    AVPacket* k = av_packet_alloc();
    while (av_read_frame(f, k) >= 0) {
        if (k->stream_index == v) {
            e.video.push_back(k->size);
        } else if (k->stream_index == a) {
            const int64_t dichiarati = k->duration > 0
                ? av_rescale_q(k->duration, f->streams[a]->time_base, AVRational{1, 48000})
                : k->size / std::max(e.blocco, 1);
            audio.emplace_back(k->size, dichiarati);
        }
        av_packet_unref(k);
    }
    av_packet_free(&k);
    while (!audio.empty() && audio.back().first < audio.back().second * e.blocco) audio.pop_back();
    for (const auto& [byte, n] : audio) e.campioni += std::min<int64_t>(n, byte / std::max(e.blocco, 1));
    avformat_close_input(&f);
    return e;
}

// Il DNxHD ha fotogrammi tutti della stessa dimensione: in fondo, uno più corto è scritto a metà.
int fotogrammi_interi(const std::vector<int>& dim) {
    if (dim.empty()) return 0;
    std::map<int, int> conta;
    for (int d : dim) ++conta[d];
    const int normale = std::max_element(conta.begin(), conta.end(),
                                         [](auto& x, auto& y) { return x.second < y.second; })->first;
    int n = static_cast<int>(dim.size());
    while (n > 0 && dim[n - 1] < normale) --n;
    return n;
}

bool segnaposto(const fs::path& wav) {
    unsigned char b[8] = {};
    FILE* f = _wfopen(wav.c_str(), L"rb");
    if (!f) return false;
    const size_t n = std::fread(b, 1, 8, f);
    std::fclose(f);
    return n == 8 && b[4] == 0xFF && b[5] == 0xFF && b[6] == 0xFF && b[7] == 0xFF;
}

// Un MOV rimasto a frammenti: la registrazione è stata interrotta prima che hybrid_fragmented lo
// riscrivesse come normale. Si guardano solo i blocchi di primo livello, saltando da uno all'altro.
// Premiere Pro non collega i MOV a frammenti (prova di Andrea del 7 novembre) e Resolve non ne
// decodifica l'audio PCM: anche un file intero va riscritto.
bool frammentato(const fs::path& mov) {
    FILE* f = _wfopen(mov.c_str(), L"rb");
    if (!f) return false;
    bool si = false;
    int64_t pos = 0;
    for (int giri = 0; giri < 100000; ++giri) {
        unsigned char h[16];
        if (_fseeki64(f, pos, SEEK_SET) != 0 || std::fread(h, 1, 8, f) != 8) break;
        uint64_t n = (uint64_t(h[0]) << 24) | (uint64_t(h[1]) << 16) | (uint64_t(h[2]) << 8) | h[3];
        if (n == 1) {
            if (std::fread(h + 8, 1, 8, f) != 8) break;
            n = 0;
            for (int i = 8; i < 16; ++i) n = (n << 8) | h[i];
        }
        if (h[4] == 'm' && h[5] == 'o' && h[6] == 'o' && h[7] == 'f') { si = true; break; }
        if (n < 8) break;                                  // 0 = fino in fondo, oppure blocco rotto
        pos += static_cast<int64_t>(n);
    }
    std::fclose(f);
    return si;
}

fs::path da_parte(const fs::path& f) {
    const fs::path cartella = f.parent_path() / "interrotti";
    fs::create_directories(cartella);
    fs::path dest = cartella / f.filename();
    for (int i = 2; fs::exists(dest); ++i)
        dest = cartella / (f.stem().wstring() + L" (" + std::to_wstring(i) + L")" + f.extension().wstring());
    fs::rename(f, dest);
    return dest;
}

// Copia i primi «fotogrammi» fotogrammi video e i primi «campioni» campioni audio, tagliando il PCM
// al campione. Per il WAV fotogrammi è 0 e conta solo l'audio. Il timecode e il bext passano tali e quali.
bool copia(const fs::path& da, const fs::path& a, const char* formato, int fotogrammi, int64_t campioni,
           std::string& errore) {
    AVFormatContext* in = nullptr;
    AVFormatContext* out = nullptr;
    if (avformat_open_input(&in, u8(da).c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(in, nullptr) < 0) {
        errore = "cannot open " + u8(da);
        avformat_close_input(&in);
        return false;
    }
    avformat_alloc_output_context2(&out, nullptr, formato, u8(a).c_str());
    std::vector<int> mappa(in->nb_streams, -1);
    std::vector<int> blocco(in->nb_streams, 0);
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        const AVStream* s = in->streams[i];
        const AVMediaType t = s->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO && t != AVMEDIA_TYPE_AUDIO) continue;   // la traccia timecode la rifà il muxer
        AVStream* o = avformat_new_stream(out, nullptr);
        avcodec_parameters_copy(o->codecpar, s->codecpar);
        o->codecpar->codec_tag = 0;
        o->time_base = s->time_base;
        o->avg_frame_rate = s->avg_frame_rate;
        av_dict_copy(&o->metadata, s->metadata, 0);
        mappa[i] = o->index;
        if (t == AVMEDIA_TYPE_AUDIO)
            blocco[i] = s->codecpar->ch_layout.nb_channels * std::max(s->codecpar->bits_per_coded_sample, 8) / 8;
    }
    av_dict_copy(&out->metadata, in->metadata, 0);
    AVDictionary* opz = nullptr;
    if (std::string(formato) == "wav") {
        av_dict_set(&opz, "write_bext", "1", 0);
        av_dict_set(&opz, "rf64", "auto", 0);
    }
    bool ok = avio_open(&out->pb, u8(a).c_str(), AVIO_FLAG_WRITE) >= 0 && avformat_write_header(out, &opz) >= 0;
    av_dict_free(&opz);
    if (!ok) errore = "cannot write " + u8(a);
    std::vector<int64_t> fatti(in->nb_streams, 0);
    AVPacket* k = av_packet_alloc();
    while (ok && av_read_frame(in, k) >= 0) {
        const int i = k->stream_index;
        bool tieni = mappa[i] >= 0;
        if (tieni && blocco[i] == 0) {                       // video: i fotogrammi interi
            tieni = fatti[i] < fotogrammi;
            if (tieni) ++fatti[i];
        } else if (tieni) {                                   // audio: al campione esatto
            const int64_t qui = k->size / blocco[i];
            const int64_t resta = campioni - fatti[i];
            tieni = resta > 0;
            if (tieni && qui > resta) {
                k->size = static_cast<int>(resta * blocco[i]);
                k->duration = av_rescale_q(resta, AVRational{1, 48000}, in->streams[i]->time_base);
            }
            if (tieni) fatti[i] += std::min(qui, resta);
        }
        if (tieni) {
            av_packet_rescale_ts(k, in->streams[i]->time_base, out->streams[mappa[i]]->time_base);
            k->stream_index = mappa[i];
            k->pos = -1;
            if (av_interleaved_write_frame(out, k) < 0) { errore = "error writing " + u8(a); ok = false; }
        }
        av_packet_unref(k);
    }
    av_packet_free(&k);
    if (ok && av_write_trailer(out) < 0) { errore = "error closing " + u8(a); ok = false; }
    avio_closep(&out->pb);
    avformat_free_context(out);
    avformat_close_input(&in);
    return ok;
}

std::string time_reference(const fs::path& wav) {
    AVFormatContext* f = nullptr;
    std::string r;
    if (avformat_open_input(&f, u8(wav).c_str(), nullptr, nullptr) >= 0) {
        if (const AVDictionaryEntry* t = av_dict_get(f->metadata, "time_reference", nullptr, 0)) r = t->value;
        avformat_close_input(&f);
    }
    return r;
}

// Una traccia: il MOV e il WAV accanto. Restituisce 1 se ha riparato, 0 se era intera, -1 se ha fallito.
int ripara(const fs::path& mov, const Cadenza& cad) {
    // A 29,97 i fotogrammi non hanno tutti gli stessi campioni: si contano come li ha scritti il codificatore.
    auto campioni_di = [&cad](int64_t fotogrammi) { return fotogrammi * 48000LL * cad.den / cad.num; };
    auto fotogrammi_di = [&cad](int64_t campioni) { return campioni * cad.num / (48000LL * cad.den); };
    const std::string nome = u8(mov.filename());
    const Esame m = esamina(mov);
    if (!m.aperto) { std::printf("%s: unreadable, left as it is\n", nome.c_str()); return -1; }
    const int interi = fotogrammi_interi(m.video);
    const fs::path wav = fs::path(mov).replace_extension(".wav");
    const bool c_e_wav = fs::exists(wav);
    const Esame w = c_e_wav ? esamina(wav) : Esame{};
    int tieni = interi;
    if (m.ha_audio) tieni = std::min<int>(tieni, static_cast<int>(fotogrammi_di(m.campioni)));
    if (c_e_wav && w.aperto) tieni = std::min<int>(tieni, static_cast<int>(fotogrammi_di(w.campioni)));
    const bool a_frammenti = frammentato(mov);
    const bool fare_mov = a_frammenti || tieni < static_cast<int>(m.video.size()) ||
                          (m.ha_audio && m.campioni != campioni_di(tieni));
    const bool fare_wav = c_e_wav && (w.campioni != campioni_di(tieni) || segnaposto(wav));
    if (!fare_mov && !fare_wav) {
        std::printf("%s: intact, %zu frames\n", nome.c_str(), m.video.size());
        return 0;
    }
    if (tieni == 0) { std::printf("%s: no complete frame, left as it is\n", nome.c_str()); return -1; }
    std::string errore;
    const std::string tr_prima = c_e_wav ? time_reference(wav) : "";
    if (fare_mov) {
        const fs::path vecchio = da_parte(mov);
        if (!copia(vecchio, mov, "mov", tieni, campioni_di(tieni), errore)) {
            std::printf("%s: %s\n", nome.c_str(), errore.c_str());
            return -1;
        }
        std::printf("%s: MOV from %zu video frames and %lld audio frames to %d%s\n", nome.c_str(), m.video.size(),
                    static_cast<long long>(fotogrammi_di(m.campioni)), tieni, a_frammenti ? ", fragmented MOV rewritten as standard MOV" : "");
    }
    if (fare_wav) {
        const fs::path vecchio = da_parte(wav);
        if (!copia(vecchio, wav, "wav", 0, campioni_di(tieni), errore)) {
            std::printf("%s: %s\n", nome.c_str(), errore.c_str());
            return -1;
        }
        std::printf("%s: WAV from %lld audio frames to %d, header rewritten\n", nome.c_str(),
                    static_cast<long long>(fotogrammi_di(w.campioni)), tieni);
    }
    // Verifica: fotogrammi, campioni, timecode e bext come devono essere.
    const Esame m2 = esamina(mov);
    std::string problemi;
    if (static_cast<int>(m2.video.size()) != tieni || fotogrammi_interi(m2.video) != tieni)
        problemi += " MOV with " + std::to_string(m2.video.size()) + " frames;";
    if (m2.tc != m.tc) problemi += " MOV timecode " + m2.tc + " instead of " + m.tc + ";";
    if (m.ha_audio && m2.campioni != campioni_di(tieni)) problemi += " MOV audio " + std::to_string(m2.campioni) + ";";
    if (c_e_wav) {
        const Esame w2 = esamina(wav);
        if (w2.campioni != campioni_di(tieni)) problemi += " WAV with " + std::to_string(w2.campioni) + " samples;";
        if (time_reference(wav) != tr_prima) problemi += " time_reference lost;";
        if (segnaposto(wav)) problemi += " WAV header still a placeholder;";
    }
    std::printf("%s: %s\n", nome.c_str(), problemi.empty() ? "verified" : ("⚠ VERIFICATION FAILED:" + problemi).c_str());
    return problemi.empty() ? 1 : -1;
}

}  // namespace

int ripara_cartella(const std::string& cartella, const Cadenza& cad) {
    std::vector<fs::path> mov;
    for (const auto& e : fs::directory_iterator(fs::u8path(cartella)))
        if (e.is_regular_file() && e.path().extension() == ".mov") mov.push_back(e.path());
    std::sort(mov.begin(), mov.end());
    // ffmpeg protesta per ogni pacchetto troncato: è proprio quello che si sta riparando, e il resoconto lo diamo noi.
    const int livello = av_log_get_level();
    av_log_set_level(AV_LOG_FATAL);
    int riparati = 0;
    bool fallito = false;
    for (const auto& p : mov) {
        const int r = ripara(p, cad);
        if (r > 0) ++riparati;
        if (r < 0) fallito = true;
    }
    av_log_set_level(livello);
    if (!fallito) {
        std::error_code ec;
        fs::remove(fs::u8path(cartella) / FILE_IN_CORSO, ec);
        fs::remove(fs::u8path(cartella) / FILE_IN_CORSO_VECCHIO, ec);
    }
    return fallito ? -1 : riparati;
}

// Una cartella interrotta: 1 riparata, 0 lasciata (niente MOV, cadenza illeggibile, riparazione fallita),
// -1 se un MOV è stato scritto negli ultimi 30 secondi. Il messaggio «ancora in scrittura» lo dà il chiamante.
static int ripara_una(const fs::path& d) {
    std::error_code ec;
    if (!fs::exists(d / FILE_IN_CORSO, ec) && !fs::exists(d / FILE_IN_CORSO_VECCHIO, ec)) return 0;
    fs::path primo;
    bool recente = false;
    const auto ora = fs::file_time_type::clock::now();
    for (const auto& e : fs::directory_iterator(d, ec)) {
        if (!e.is_regular_file(ec) || e.path().extension() != ".mov") continue;
        if (primo.empty() || e.path() < primo) primo = e.path();
        if (ora - fs::last_write_time(e.path(), ec) < std::chrono::seconds(30)) recente = true;
    }
    if (recente) return -1;
    if (primo.empty()) return 0;
    const std::string nome = u8(d.filename());
    Cadenza cad;
    AVFormatContext* f = nullptr;
    bool letta = false;
    if (avformat_open_input(&f, u8(primo).c_str(), nullptr, nullptr) >= 0) {
        if (avformat_find_stream_info(f, nullptr) >= 0) {
            const int v = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
            if (v >= 0) {
                const AVRational r = f->streams[v]->r_frame_rate;
                if (r.num > 0 && r.den > 0) {
                    cad.num = r.num;
                    cad.den = r.den;
                    cad.base = static_cast<int>((r.num + r.den - 1) / r.den);   // 30000/1001 → 30
                    letta = true;
                }
            }
        }
        avformat_close_input(&f);
    }
    if (!letta) { std::printf("interrupted recording \"%s\": frame rate unreadable, left as it is\n", nome.c_str()); return 0; }
    std::printf("interrupted recording \"%s\": repairing it\n", nome.c_str());
    const int n = ripara_cartella(u8(d), cad);
    std::printf(n < 0 ? "⚠ \"%s\": repair failed, the originals are in the \"interrotti\" folder\n"
                      : "\"%s\": tracks repaired: %d, originals in the \"interrotti\" folder\n", nome.c_str(), n);
    return n >= 0 ? 1 : 0;
}

int ripara_interrotte(const std::string& destinazione, const std::atomic<bool>* esci) {
    std::error_code ec;
    const fs::path dest = fs::u8path(destinazione);
    if (destinazione.empty() || !fs::is_directory(dest, ec)) return 0;
    int fatte = 0;
    std::vector<fs::path> attese;
    for (const auto& d : fs::directory_iterator(dest, ec)) {
        if (!d.is_directory(ec)) continue;
        const int r = ripara_una(d.path());
        if (r > 0) ++fatte;
        if (r < 0) {
            std::printf("interrupted recording \"%s\": written less than 30 s ago, repair in a moment\n", u8(d.path().filename()).c_str());
            attese.push_back(d.path());
        }
    }
    // Riaperto subito dopo la caduta: i MOV sono «recenti» solo perché la caduta è di pochi secondi fa.
    // Si riprova ogni 5 secondi per al più 90, e si smette appena si chiude il registratore.
    const auto inizio = std::chrono::steady_clock::now();
    while (!attese.empty() && std::chrono::steady_clock::now() - inizio < std::chrono::seconds(90)) {
        for (int i = 0; i < 20; ++i) {
            if (esci && esci->load()) return fatte;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        std::vector<fs::path> ancora;
        for (const auto& d : attese) {
            const int r = ripara_una(d);
            if (r > 0) ++fatte;
            if (r < 0) ancora.push_back(d);
        }
        attese.swap(ancora);
    }
    for (const auto& d : attese)
        std::printf("interrupted recording \"%s\": still being written, left as it is\n", u8(d.filename()).c_str());
    return fatte;
}
