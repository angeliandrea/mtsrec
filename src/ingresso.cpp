#include "ingresso.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>

struct ModoDeckLink { const char* nome; BMDDisplayMode m; };
static const ModoDeckLink T[] = {
    {"1080p23.98", bmdModeHD1080p2398}, {"1080p24", bmdModeHD1080p24},     {"1080p25", bmdModeHD1080p25},
    {"1080p29.97", bmdModeHD1080p2997}, {"1080p30", bmdModeHD1080p30},     {"1080p50", bmdModeHD1080p50},
    {"1080p59.94", bmdModeHD1080p5994}, {"1080p60", bmdModeHD1080p6000},   {"1080i50", bmdModeHD1080i50},
    {"1080i59.94", bmdModeHD1080i5994}, {"1080i60", bmdModeHD1080i6000},   {"2160p23.98", bmdMode4K2160p2398},
    {"2160p24", bmdMode4K2160p24},      {"2160p25", bmdMode4K2160p25},     {"2160p29.97", bmdMode4K2160p2997},
    {"2160p30", bmdMode4K2160p30},      {"2160p50", bmdMode4K2160p50},     {"2160p59.94", bmdMode4K2160p5994},
    {"2160p60", bmdMode4K2160p60},
};

BMDDisplayMode modo_decklink(const std::string& modo) {
    for (const auto& t : T)
        if (modo == t.nome) return t.m;
    return bmdModeHD1080p25;
}

std::vector<Porta> porte_decklink() {
    std::vector<Porta> out;
    IDeckLinkIterator* it = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator,
                                reinterpret_cast<void**>(&it))))
        return out;
    IDeckLink* d = nullptr;
    while (it->Next(&d) == S_OK) {
        BSTR nome = nullptr;
        d->GetDisplayName(&nome);
        std::string n;
        if (nome) {
            const int k = WideCharToMultiByte(CP_UTF8, 0, nome, -1, nullptr, 0, nullptr, nullptr);
            n.assign(k > 0 ? k - 1 : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, nome, -1, n.data(), k, nullptr, nullptr);
            SysFreeString(nome);
        }
        Porta p{d, n, false, ""};
        IDeckLinkProfileAttributes* a = nullptr;
        if (SUCCEEDED(d->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&a)))) {
            LONGLONG v = 0;
            if (SUCCEEDED(a->GetInt(BMDDeckLinkVideoIOSupport, &v))) p.ingresso = (v & bmdDeviceSupportsCapture) != 0;
            if (SUCCEEDED(a->GetInt(BMDDeckLinkVideoInputConnections, &v))) {
                if (v & bmdVideoConnectionSDI) p.connettore = "SDI";
                if (v & bmdVideoConnectionHDMI) p.connettore += p.connettore.empty() ? "HDMI" : " and HDMI";
            }
            a->Release();
        }
        out.push_back(p);
    }
    it->Release();
    // L'SDK non le dà in ordine (sulla Duo 2 la (3) viene prima della (2)): si ordinano per nome,
    // coi numeri contati come numeri, così le righe della pagina seguono le porte.
    auto prima = [](const std::string& a, const std::string& b) {
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
                size_t fi = i, fj = j;
                while (fi < a.size() && std::isdigit(static_cast<unsigned char>(a[fi]))) ++fi;
                while (fj < b.size() && std::isdigit(static_cast<unsigned char>(b[fj]))) ++fj;
                const long x = std::stol(a.substr(i, fi - i)), y = std::stol(b.substr(j, fj - j));
                if (x != y) return x < y;
                i = fi; j = fj;
            } else {
                if (a[i] != b[j]) return a[i] < b[j];
                ++i; ++j;
            }
        }
        return a.size() - i < b.size() - j;
    };
    std::stable_sort(out.begin(), out.end(), [&](const Porta& x, const Porta& y) { return prima(x.nome, y.nome); });
    return out;
}

Ingresso::Ingresso(IDeckLink* scheda, std::string nome, Orologio& orologio)
    : scheda_(scheda), nome_(std::move(nome)), orologio_(orologio) {
    scheda_->AddRef();
    IDeckLinkProfileAttributes* a = nullptr;
    if (SUCCEEDED(scheda_->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&a)))) {
        LONGLONG v = 0;
        if (SUCCEEDED(a->GetInt(BMDDeckLinkVideoInputConnections, &v)))
            solo_hdmi_ = (v & bmdVideoConnectionHDMI) && !(v & bmdVideoConnectionSDI);
        a->Release();
    }
}

Ingresso::~Ingresso() {
    ferma();
    if (inp_) inp_->Release();
    scheda_->Release();
}

HRESULT Ingresso::QueryInterface(REFIID iid, void** p) {
    if (iid == IID_IUnknown || iid == IID_IDeckLinkInputCallback) {
        *p = static_cast<IDeckLinkInputCallback*>(this);
        AddRef();
        return S_OK;
    }
    *p = nullptr;
    return E_NOINTERFACE;
}

void Ingresso::aggiungi(Traccia* t) {
    std::lock_guard<std::mutex> l(m_tracce_);
    if (tracce_.empty()) {
        std::lock_guard<std::mutex> ls(m_storia_);
        storia_.clear();
    }
    tracce_.push_back(t);
    registra_.store(true);
}

void Ingresso::togli_tracce() {
    std::lock_guard<std::mutex> l(m_tracce_);
    tracce_.clear();
    registra_.store(false);
}

std::vector<float> Ingresso::storia(size_t da) const {
    std::lock_guard<std::mutex> l(m_storia_);
    if (da >= storia_.size()) return {};
    return std::vector<float>(storia_.begin() + static_cast<std::ptrdiff_t>(da), storia_.end());
}

float Ingresso::picco(int canale) {
    if (canale < 0 || canale > 1) return 0;
    return static_cast<float>(picco_[canale].exchange(0)) / 2147483647.0f;
}

std::string Ingresso::formato_arrivo() const {
    std::lock_guard<std::mutex> l(m_formato_);
    if (!formato_.empty()) return formato_;
    if (aperto_ && segnale_.load())
        for (const auto& t : T)
            if (t.m == modo_) return t.nome;
    return {};
}

bool Ingresso::apri(BMDDisplayMode modo, int canali_audio, std::string& errore) {
    if (!inp_ && FAILED(scheda_->QueryInterface(IID_IDeckLinkInput, reinterpret_cast<void**>(&inp_)))) {
        errore = nome_ + ": not an input";
        return false;
    }
    {
        // Riaperta in un altro formato: quello visto prima non vale più finché la scheda non lo ridice.
        std::lock_guard<std::mutex> l(m_formato_);
        formato_.clear();
        modo_ = modo;
    }
    // Il riconoscimento del formato solo per sapere cosa arriva: la porta resta nel formato scelto,
    // e un segnale diverso arriva come «senza segnale», come prima.
    BMDVideoInputFlags flag = bmdVideoInputFlagDefault;
    IDeckLinkProfileAttributes* a = nullptr;
    if (SUCCEEDED(scheda_->QueryInterface(IID_IDeckLinkProfileAttributes, reinterpret_cast<void**>(&a)))) {
        BOOL si = FALSE;
        if (SUCCEEDED(a->GetFlag(BMDDeckLinkSupportsInputFormatDetection, &si)) && si) flag |= bmdVideoInputEnableFormatDetection;
        a->Release();
    }
    // Prima si chiede se la porta il formato lo riceve: una 3G-SDI rifiuta il 2160p25, e «input in use?»
    // mandava a cercare un altro programma che non c'era (sezione 9.74).
    BMDDisplayMode effettivo;
    BOOL supporta = TRUE;
    if (SUCCEEDED(inp_->DoesSupportVideoMode(bmdVideoConnectionUnspecified, modo, bmdFormat8BitYUV, bmdNoVideoInputConversion,
                                             bmdSupportedVideoModeDefault, &effettivo, &supporta)) && !supporta) {
        const Cadenza c = orologio_.cadenza();
        const bool u = (modo >> 16) == 0x346B;                // i modi 4K cominciano con «4k»
        errore = nome_ + (u ? ": does not capture UHD 2160p" + c.nome() + " (a 12G-SDI or HDMI 2.0 input is needed)"
                            : ": does not capture 1080 at " + c.nome() + " fps");
        return false;
    }
    HRESULT hr = inp_->EnableVideoInput(modo, bmdFormat8BitYUV, flag);
    if (FAILED(hr)) {
        char b[96];
        std::snprintf(b, sizeof b, ": EnableVideoInput refused (0x%08lX), input in use?",
                      static_cast<unsigned long>(hr));
        errore = nome_ + b;
        return false;
    }
    aperto_ = true;
    if (canali_audio > 0) {
        // 48 kHz interi a 32 bit: l'SDI porta 24 bit, la DeckLink li mette nei bit alti.
        hr = inp_->EnableAudioInput(bmdAudioSampleRate48kHz, bmdAudioSampleType32bitInteger, canali_audio);
        if (FAILED(hr)) {
            errore = nome_ + ": " + std::to_string(canali_audio) + "-channel audio refused";
            return false;
        }
        audio_ = true;
        canali_ = canali_audio;
    }
    inp_->SetCallback(this);
    return true;
}

bool Ingresso::avvia(std::string& errore) {
    if (FAILED(inp_->StartStreams())) { errore = nome_ + ": StartStreams refused"; return false; }
    avviato_ = true;
    return true;
}

void Ingresso::ferma() {
    if (!inp_) return;
    if (avviato_) { inp_->StopStreams(); avviato_ = false; }
    if (aperto_) {
        inp_->SetCallback(nullptr);
        if (audio_) { inp_->DisableAudioInput(); audio_ = false; canali_ = 0; }
        inp_->DisableVideoInput();
        aperto_ = false;
    }
}

HRESULT Ingresso::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents, IDeckLinkDisplayMode* modo,
                                          BMDDetectedVideoInputFormatFlags) {
    // Il formato è fisso per tutta la registrazione: un segnale diverso arriva come «senza segnale».
    // Qui si annota soltanto cosa arriva, per dirlo nella pagina.
    if (modo) {
        BSTR n = nullptr;
        if (SUCCEEDED(modo->GetName(&n)) && n) {
            const int k = WideCharToMultiByte(CP_UTF8, 0, n, -1, nullptr, 0, nullptr, nullptr);
            std::string s(k > 0 ? k - 1 : 0, '\0');
            WideCharToMultiByte(CP_UTF8, 0, n, -1, s.data(), k, nullptr, nullptr);
            SysFreeString(n);
            std::lock_guard<std::mutex> l(m_formato_);
            formato_ = s;
        }
    }
    return S_OK;
}

HRESULT Ingresso::VideoInputFrameArrived(IDeckLinkVideoInputFrame* f, IDeckLinkAudioInputPacket* pacchetto) {
    if (!f) return S_OK;
    InfoFotogramma info;
    info.arrivo_us = adesso_us();
    info.segnale = (f->GetFlags() & bmdFrameHasNoInputSource) == 0;
    segnale_.store(info.segnale);
    ++fotogrammi_;

    BMDTimeValue t = 0, d = 0;
    f->GetStreamTime(&t, &d, 1000000);
    info.stream_us = t;
    f->GetHardwareReferenceTimestamp(1000000, &t, &d);
    info.hw_us = t;
    inp_->GetAvailableVideoFrameCount(&info.coda_scheda);

    if (info.segnale) {
        // Sopra i 30 fotogrammi l'RP188 classico (VITC e LTC) conta a coppie: il numero va da 0 a 24 o 29
        // e il «field mark» dice se è il primo o il secondo della coppia (SMPTE 12-1). Solo il timecode ad
        // alta cadenza (SMPTE 12-3) conta fino a 49 o 59: se c'è si prende quello, senza ambiguità.
        Cadenza c = orologio_.cadenza();
        // Diagnosi (sezione 9.79): con WINISO_SONDA_TC=n si stampano per i primi n fotogrammi tutti i tipi di RP188
        // che la scheda vede, coi contrassegni. Serve a capire cosa manda un mixer a una cadenza nuova.
        static const int sonda = [] { const char* v = std::getenv("WINISO_SONDA_TC"); return v ? std::atoi(v) : 0; }();
        if (sonda > 0 && sondati_ < sonda) {
            ++sondati_;
            std::string r = nome_ + " #" + std::to_string(sondati_);
            const struct { const char* n; BMDTimecodeFormat f; } tipi[] = {
                {"HFR", bmdTimecodeRP188HighFrameRate}, {"VITC1", bmdTimecodeRP188VITC1},
                {"VITC2", bmdTimecodeRP188VITC2}, {"LTC", bmdTimecodeRP188LTC}, {"Any", bmdTimecodeRP188Any}};
            for (const auto& tp : tipi) {
                IDeckLinkTimecode* x = nullptr;
                if (f->GetTimecode(tp.f, &x) == S_OK && x) {
                    unsigned char h = 0, m = 0, s = 0, fr = 0;
                    x->GetComponents(&h, &m, &s, &fr);
                    char b[64];
                    std::snprintf(b, sizeof b, " %s=%02d:%02d:%02d:%02d/f%lu", tp.n, h, m, s, fr, static_cast<unsigned long>(x->GetFlags()));
                    r += b;
                    x->Release();
                } else {
                    r += std::string(" ") + tp.n + "=-";
                }
            }
            std::printf("TC probe %s\n", r.c_str());
        }
        IDeckLinkTimecode* tc = nullptr;
        bool hfr = c.base > 30 && f->GetTimecode(bmdTimecodeRP188HighFrameRate, &tc) == S_OK && tc;
        if (!hfr && f->GetTimecode(bmdTimecodeRP188Any, &tc) != S_OK) tc = nullptr;
        // Il timecode HDMI dei Micro Converter SDI to HDMI 3G porta il numero del fotogramma prima
        // (24 novembre 2026, 23 stacchi su 23 a −1). Se una porta SDI tiene l'orologio, la porta HDMI
        // prende il timecode da lì, come quando l'HDMI non ne porta: provato esatto al fotogramma.
        if (tc && solo_hdmi_ && orologio_.da_sdi()) { tc->Release(); tc = nullptr; }
        if (tc) {
            unsigned char h = 0, m = 0, s = 0, fr = 0;
            tc->GetComponents(&h, &m, &s, &fr);
            const BMDTimecodeFlags fl = tc->GetFlags();
            tc->Release();
            // Il drop-frame lo dice il segnale: a 29,97 e 59,94 il registratore segue quello che arriva.
            const bool drop = (fl & bmdTimecodeIsDropFrame) != 0;
            if (c.ntsc() && c.base % 30 == 0 && drop != c.drop) {
                c.drop = drop;
                orologio_.imposta(c);
            }
            int f_tc = fr;
            if (c.base > 30 && !hfr && fr < c.base / 2) f_tc = fr * 2 + ((fl & bmdTimecodeFieldMark) ? 1 : 0);
            info.tc = c.da_etichetta(h, m, s, f_tc);
            if (info.tc >= 0) {
                info.tc_dal_segnale = true;
                if (!ritardo_) orologio_.aggiorna(info.tc, info.hw_us, !solo_hdmi_);
            }
        }
    }
    if (info.tc < 0) info.tc = orologio_.stima(info.hw_us);
    const int tc_grezzo = info.tc;
    // L'immagine arrivata adesso è quella che il mixer aveva qualche fotogramma fa (sezione 9.60).
    if (const int r = ritardo_.load(); r > 0 && info.tc >= 0) {
        const int g = orologio_.giorno();
        info.tc = (info.tc - r + g) % g;
    }
    tc_.store(info.tc);
    tc_segnale_.store(info.tc_dal_segnale);

    // L'audio arriva nello stesso callback del fotogramma: resta valido finché non si torna.
    const int32_t* audio = nullptr;
    if (pacchetto) {
        void* a = nullptr;
        if (SUCCEEDED(pacchetto->GetBytes(&a)) && a) {
            audio = static_cast<const int32_t*>(a);
            info.campioni_audio = static_cast<int>(pacchetto->GetSampleFrameCount());
            // Il picco dei primi due canali per i livelli della pagina: 1920 campioni, un attimo.
            for (int c = 0; c < 2 && c < canali_; ++c) {
                int32_t p = 0;
                for (int s = 0; s < info.campioni_audio; ++s) {
                    const int32_t x = audio[s * canali_ + c];
                    p = std::max(p, x == INT32_MIN ? INT32_MAX : (x < 0 ? -x : x));
                }
                for (int32_t v = picco_[c].load(); p > v && !picco_[c].compare_exchange_weak(v, p);) {}
                finestra_max_ = std::max(finestra_max_, p);
            }
        }
    }
    // La storia dei livelli, solo mentre si registra: un valore ogni PASSO_STORIA fotogrammi.
    if (registra_.load()) {
        if (++finestra_n_ >= PASSO_STORIA) {
            std::lock_guard<std::mutex> l(m_storia_);
            storia_.push_back(static_cast<float>(finestra_max_) / 2147483647.0f);
            finestra_max_ = 0;
            finestra_n_ = 0;
        }
    } else {
        finestra_max_ = 0;
        finestra_n_ = 0;
    }

    const uint8_t* dati = nullptr;
    IDeckLinkVideoBuffer* buf = nullptr;
    if (info.segnale && SUCCEEDED(f->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buf)))) {
        void* p = nullptr;
        if (SUCCEEDED(buf->StartAccess(bmdBufferAccessRead)) && SUCCEEDED(buf->GetBytes(&p)))
            dati = static_cast<const uint8_t*>(p);
        const int rb = static_cast<int>(f->GetRowBytes());
        {
            std::lock_guard<std::mutex> l(m_tracce_);
            for (Traccia* tr : tracce_) tr->consegna(dati, rb, audio, info);
        }
        if (misura_.load() && dati && tc_grezzo >= 0) {
            // 32 × 18 punti: basta a distinguere barre e nero.
            const int w = static_cast<int>(f->GetWidth()), h = static_cast<int>(f->GetHeight());
            int64_t s = 0;
            for (int r = 0; r < 18; ++r) {
                const uint8_t* riga = dati + static_cast<size_t>((r * 2 + 1) * h / 36) * rb;
                for (int c = 0; c < 32; ++c) s += riga[2 * ((c * 2 + 1) * w / 64) + 1];
            }
            std::lock_guard<std::mutex> l(m_luci_);
            if (luci_.size() < 3000) luci_.push_back({tc_grezzo, static_cast<float>(s) / (32 * 18)});
        }
        miniatura_.forse_copia(dati, rb, static_cast<int>(f->GetWidth()), static_cast<int>(f->GetHeight()), fotogrammi_);
        if (dati) buf->EndAccess(bmdBufferAccessRead);
        buf->Release();
    } else {
        std::lock_guard<std::mutex> l(m_tracce_);
        for (Traccia* tr : tracce_) tr->consegna(nullptr, 0, audio, info);
    }
    return S_OK;
}

void Ingresso::misura_luce(bool accesa) {
    if (accesa) {
        std::lock_guard<std::mutex> l(m_luci_);
        luci_.clear();
    }
    misura_.store(accesa);
}

std::vector<Ingresso::Luce> Ingresso::luci() const {
    std::lock_guard<std::mutex> l(m_luci_);
    return luci_;
}
