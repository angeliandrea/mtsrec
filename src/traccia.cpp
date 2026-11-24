#include "traccia.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>

int64_t adesso_us() {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<int64_t>(c.QuadPart / freq * 1000000 + c.QuadPart % freq * 1000000 / freq);
}

// ------------------------------------------------------------------ orologio
void Orologio::imposta(const Cadenza& c) {
    std::lock_guard<std::mutex> l(m_);
    if (c.num == cad_.num && c.den == cad_.den && c.base == cad_.base && c.drop == cad_.drop) return;
    // Lo stesso numero di fotogrammi è un'altra ora: il timecode si rilegge dal segnale.
    if (c.num != cad_.num || c.den != cad_.den) valido_ = false;
    else if (valido_) tc_ = c.da_testo(cad_.testo(tc_));   // solo drop ↔ non drop: si tiene l'ora
    if (tc_ < 0) { valido_ = false; tc_ = 0; }
    if (!valido_) sdi_ = false;
    cad_ = c;
}

Cadenza Orologio::cadenza() const {
    std::lock_guard<std::mutex> l(m_);
    return cad_;
}

void Orologio::aggiorna(int tc, int64_t hw_us, bool sdi) {
    std::lock_guard<std::mutex> l(m_);
    if (!sdi && sdi_ && valido_) return;               // c'è già una porta SDI: comanda lei
    if (sdi) sdi_ = true;
    valido_ = true;
    tc_ = tc;
    hw_us_ = hw_us;
}

int Orologio::stima(int64_t hw_us) const {
    std::lock_guard<std::mutex> l(m_);
    if (!valido_) return -1;
    const int64_t passi = cad_.fotogrammi_in_us(hw_us - hw_us_);
    const int64_t g = cad_.giorno();
    return static_cast<int>(((tc_ + passi) % g + g) % g);
}

void Orologio::ancora(int& tc, int64_t& hw_us) const {
    std::lock_guard<std::mutex> l(m_);
    tc = tc_;
    hw_us = hw_us_;
}

int Orologio::da_ancora(int tc, int64_t ancora_us, int64_t hw_us) const {
    const Cadenza c = cadenza();
    const int64_t passi = c.fotogrammi_in_us(hw_us - ancora_us);
    const int64_t g = c.giorno();
    return static_cast<int>(((tc + passi) % g + g) % g);
}

bool Orologio::valido() const {
    std::lock_guard<std::mutex> l(m_);
    return valido_;
}

bool Orologio::da_sdi() const {
    std::lock_guard<std::mutex> l(m_);
    return valido_ && sdi_;
}

// ------------------------------------------------------------------ traccia
Traccia::Traccia(std::string nome, std::string cartella, ParametriCodifica p, int posti_in_coda)
    : nome_(std::move(nome)), cartella_(std::move(cartella)), p_(p) {
    const size_t byte = static_cast<size_t>(p_.larghezza) * p_.altezza * 2;
    posti_.resize(posti_in_coda);
    const size_t campioni = static_cast<size_t>(p_.campioni_max()) * p_.canali_audio;
    for (int i = 0; i < posti_in_coda; ++i) {
        posti_[i].dati.resize(byte);
        posti_[i].audio.resize(campioni);
        liberi_.push_back(i);
    }
    nero_.resize(byte);
    for (size_t i = 0; i < byte; i += 2) { nero_[i] = 0x80; nero_[i + 1] = 0x10; }  // U/V 128, Y 16
    giorno_ = p_.cad.giorno();
}

Traccia::~Traccia() { ferma(); }

void Traccia::consegna(const uint8_t* uyvy, int rb, const int32_t* audio, const InfoFotogramma& info) {
    if (!attiva_.load(std::memory_order_acquire)) return;
    int i;
    {
        std::lock_guard<std::mutex> l(m_);
        ++conti_.ricevuti;
        conti_.ritardo_max_us = std::max(conti_.ritardo_max_us, info.arrivo_us - info.hw_us);
        if (liberi_.empty()) { ++conti_.persi_coda; return; }   // il codificatore non sta al passo
        i = liberi_.back();
        liberi_.pop_back();
    }
    Posto& p = posti_[i];
    if (uyvy) std::memcpy(p.dati.data(), uyvy, std::min(p.dati.size(), static_cast<size_t>(rb) * p_.altezza));
    p.byte_per_riga = uyvy ? rb : p_.larghezza * 2;
    // Audio: si copia quello che è arrivato, al massimo campioni_max() campioni. Quanti ne vanno nel file
    // per ogni fotogramma lo decide il codificatore, così non può scivolare rispetto al video (Cadenza::campioni).
    p.con_audio = audio && !p.audio.empty();
    p.n_audio = 0;
    if (p.con_audio) {
        const size_t n = std::min(p.audio.size(), static_cast<size_t>(info.campioni_audio) * p_.canali_audio);
        std::memcpy(p.audio.data(), audio, n * sizeof(int32_t));
        p.n_audio = static_cast<int>(n / p_.canali_audio);
    }
    p.info = info;
    if (!uyvy) p.info.segnale = false;                        // niente immagine: si scrive nero
    {
        std::lock_guard<std::mutex> l(m_);
        pronti_.push_back(i);
        conti_.coda_max = std::max(conti_.coda_max, static_cast<int>(pronti_.size()));
    }
    cv_.notify_one();
}

bool Traccia::avvia(int tc_inizio, std::string& errore) {
    const std::string base = cartella_ + "\\" + nome_;
    if (!cod_.apri(base, p_, tc_inizio, errore)) return false;
    csv_ = std::fopen((base + ".csv").c_str(), "w");
    if (csv_) std::fputs("tc;segnale;tc_dal_segnale;stream_us;hw_us;ritardo_us;coda_scheda;audio;esito\n", csv_);
    atteso_ = tc_inizio;
    fine_ = false;
    thread_ = std::thread(&Traccia::lavora, this);
    attiva_.store(true, std::memory_order_release);
    return true;
}

void Traccia::ferma() {
    if (!thread_.joinable()) return;
    attiva_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> l(m_);
        fine_ = true;
    }
    cv_.notify_one();
    thread_.join();
    std::string e;
    if (!cod_.chiudi(e)) {
        std::lock_guard<std::mutex> l(m_);
        errore_ = e;
    }
    if (csv_) { std::fclose(csv_); csv_ = nullptr; }
}

Conti Traccia::conti() const {
    std::lock_guard<std::mutex> l(m_);
    return conti_;
}

std::string Traccia::errore() const {
    std::lock_guard<std::mutex> l(m_);
    return errore_;
}

bool Traccia::codifica(const uint8_t* dati, int rb, const int32_t* audio, int n_audio) {
    std::string e;
    if (cod_.scrivi(dati, rb, audio, n_audio, e)) return true;
    std::lock_guard<std::mutex> l(m_);
    if (errore_.empty()) errore_ = e;
    return false;
}

void Traccia::lavora() {
    for (;;) {
        int i;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [&] { return fine_ || !pronti_.empty(); });
            if (pronti_.empty()) return;              // fine e coda vuota
            i = pronti_.front();
            pronti_.pop_front();
        }
        Posto& p = posti_[i];
        const InfoFotogramma& f = p.info;
        const char* esito = "scritto";
        int riempiti = 0;
        bool scrivi = true;

        if (f.tc < 0) {
            esito = "senza_tc";                       // nessun orologio ancora: non si sa dove metterlo
            scrivi = false;
        } else {
            // distanza dal fotogramma atteso, sul giro di 24 ore
            const int d = ((f.tc - atteso_) % giorno_ + giorno_) % giorno_;
            if (d == 0) {
            } else if (d < 10 * p_.cad.base) {
                riempiti = d;                         // mancano d fotogrammi: si ripete l'ultimo
            } else if (d > giorno_ - 10 * p_.cad.base) {
                // prima della partenza comune è normale; dopo vuol dire arrivato in ritardo o doppio
                esito = iniziato_ ? "scartato" : "prima_della_partenza";
                scrivi = false;
            } else {
                esito = "salto";                      // il timecode è saltato: si ricomincia da lì
                std::lock_guard<std::mutex> l(m_);
                ++conti_.salti;
                atteso_ = f.tc;
            }
        }

        // Fine comune: niente oltre il fotogramma prima di fine_tc_, riempiti compresi.
        const int fine = fine_tc_.load();
        if (fine >= 0 && iniziato_ && f.tc >= 0) {
            const int resto = ((fine - atteso_) % giorno_ + giorno_) % giorno_;   // fotogrammi ancora da scrivere
            if (resto == 0 || resto > giorno_ / 2) {
                esito = "dopo_la_fine";
                scrivi = false;
                riempiti = 0;
                terminata_.store(true);
            } else {
                riempiti = std::min(riempiti, resto);
                if (riempiti == resto) {               // il buco arriva fino alla fine
                    esito = "dopo_la_fine";
                    scrivi = false;
                    terminata_.store(true);
                }
            }
        }

        bool ok = true;
        // Un fotogramma ripetuto ha l'audio in silenzio: ripetere un pezzo di audio si sentirebbe.
        for (int k = 0; k < riempiti && ok; ++k)
            ok = codifica(ultimo_.empty() ? nero_.data() : ultimo_.data(), p_.larghezza * 2, nullptr, 0);
        if (scrivi && ok) {
            const uint8_t* dati = f.segnale ? p.dati.data() : nero_.data();
            const int rb = f.segnale ? p.byte_per_riga : p_.larghezza * 2;
            ok = codifica(dati, rb, f.segnale && p.con_audio ? p.audio.data() : nullptr, p.n_audio);
            if (f.segnale) {
                ultimo_.resize(static_cast<size_t>(p_.larghezza) * 2 * p_.altezza);
                for (int y = 0; y < p_.altezza; ++y)
                    std::memcpy(ultimo_.data() + static_cast<size_t>(y) * p_.larghezza * 2,
                                dati + static_cast<size_t>(y) * rb, static_cast<size_t>(p_.larghezza) * 2);
            }
            atteso_ = (f.tc + 1) % giorno_;
            iniziato_ = true;
        }
        {
            std::lock_guard<std::mutex> l(m_);
            if (scrivi) ++conti_.scritti;
            if (scrivi && !f.segnale) ++conti_.neri;
            if (std::strcmp(esito, "scartato") == 0) ++conti_.scartati;
            conti_.riempiti += riempiti;
            liberi_.push_back(i);
        }
        if (csv_)
            std::fprintf(csv_, "\"%s\";%d;%d;%lld;%lld;%lld;%u;%d;%s%s\n",
                         f.tc >= 0 ? tc_testo(f.tc, p_.cad).c_str() : "", f.segnale ? 1 : 0,
                         f.tc_dal_segnale ? 1 : 0, f.stream_us, f.hw_us, f.arrivo_us - f.hw_us,
                         f.coda_scheda, f.campioni_audio, esito, riempiti ? (" +" + std::to_string(riempiti)).c_str() : "");
    }
}
