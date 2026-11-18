#include "formato.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// ------------------------------------------------------------------ cadenza
int Cadenza::giorno() const {
    // 24 ore di etichette meno quelle che il drop-frame salta: 2 o 4 a ogni minuto che non è multiplo di dieci
    return base * 86400 - salto() * (1440 - 144);
}

std::string Cadenza::testo(int f) const {
    const int g = giorno();
    f = ((f % g) + g) % g;
    if (drop) {
        // Dal conto all'etichetta: si rimettono i numeri saltati (SMPTE 12M).
        const int D = salto(), al_minuto = base * 60 - D, ai_dieci = base * 600 - D * 9;
        const int d = f / ai_dieci, m = f % ai_dieci;
        f += D * 9 * d + (m > D ? D * ((m - D) / al_minuto) : 0);
    }
    char b[16];
    std::snprintf(b, sizeof b, "%02d:%02d:%02d%c%02d", f / base / 3600, f / base / 60 % 60, f / base % 60,
                  drop ? ';' : ':', f % base);
    return b;
}

int Cadenza::da_etichetta(int h, int m, int s, int f) const {
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59 || f < 0 || f >= base) return -1;
    const int D = salto();
    if (D && s == 0 && m % 10 != 0 && f < D) return -1;
    const int minuti = h * 60 + m;
    return ((h * 60 + m) * 60 + s) * base + f - D * (minuti - minuti / 10);
}

int Cadenza::da_testo(const std::string& t) const {
    int h, m, s, f;
    char a, b, c;
    if (std::sscanf(t.c_str(), "%d%c%d%c%d%c%d", &h, &a, &m, &b, &s, &c, &f) != 7) return -1;
    return da_etichetta(h, m, s, f);
}

int64_t Cadenza::fotogrammi_in_us(int64_t us) const {
    return std::llround(static_cast<double>(us) * num / (1e6 * den));
}

int Cadenza::campioni(int64_t k) const {
    // 48000 × den / num campioni per fotogramma, distribuiti in modo che la somma sia sempre esatta
    const int64_t q = 48000LL * den;
    return static_cast<int>((k + 1) * q / num - k * q / num);
}

int Cadenza::campioni_max() const {
    const int64_t q = 48000LL * den;
    return static_cast<int>((q + num - 1) / num);
}

int64_t Cadenza::campioni_da_mezzanotte(int f) const {
    return static_cast<int64_t>(f) * 48000LL * den / num;
}

std::string Cadenza::nome() const {
    if (den == 1) return std::to_string(num);
    char b[16];
    std::snprintf(b, sizeof b, "%.2f", fps());
    return b;
}

// ------------------------------------------------------------------ formati
static constexpr Cadenza C2398{24, 24000, 1001, false}, C24{24, 24, 1, false}, C25{25, 25, 1, false},
                         C2997{30, 30000, 1001, true}, C30{30, 30, 1, false}, C50{50, 50, 1, false},
                         C5994{60, 60000, 1001, true}, C60{60, 60, 1, false};

const std::vector<Formato>& formati() {
    // Tutte le cadenze dell'ATEM Constellation (manuale, pagina 31), decise da Andrea il 18 novembre 2026
    // (sezione 9.79). Il 29,97 e il 59,94 partono drop-frame, ma comanda il segnale: se il timecode RP188
    // arriva non drop-frame il registratore lo segue (ingresso.cpp).
    static const std::vector<Formato> f = {
        {"1080p23.98", 1920, 1080, C2398, false}, {"1080p24", 1920, 1080, C24, false},
        {"1080p25", 1920, 1080, C25, false},      {"1080p29.97", 1920, 1080, C2997, false},
        {"1080p30", 1920, 1080, C30, false},      {"1080p50", 1920, 1080, C50, false},
        {"1080p59.94", 1920, 1080, C5994, false}, {"1080p60", 1920, 1080, C60, false},
        {"1080i50", 1920, 1080, C25, true},       {"1080i59.94", 1920, 1080, C2997, true},
        {"1080i60", 1920, 1080, C30, true},
        {"2160p23.98", 3840, 2160, C2398, false}, {"2160p24", 3840, 2160, C24, false},
        {"2160p25", 3840, 2160, C25, false},      {"2160p29.97", 3840, 2160, C2997, false},
        {"2160p30", 3840, 2160, C30, false},      {"2160p50", 3840, 2160, C50, false},
        {"2160p59.94", 3840, 2160, C5994, false}, {"2160p60", 3840, 2160, C60, false},
    };
    return f;
}

const Formato* formato(const std::string& nome) {
    for (const Formato& f : formati())
        if (nome == f.nome) return &f;
    return nullptr;
}

bool uhd(const std::string& modo) {
    const Formato* f = formato(modo);
    return f && f->altezza == 2160;
}

const Formato* formato_del_segnale(const std::string& n) {
    if (const Formato* f = formato(n)) return f;
    // Desktop Video scrive «1080p6000» o «2160p60» secondo la versione: si confronta la sola parte numerica
    for (const Formato& f : formati()) {
        const std::string a = f.nome;
        if (n.size() > 5 && a.compare(0, 5, n, 0, 5) == 0) {
            const double x = std::atof(n.c_str() + 5), y = std::atof(a.c_str() + 5);
            if (std::fabs(x - y) < 0.02 || std::fabs(x / 100 - y) < 0.02) return &f;
        }
    }
    return nullptr;
}

// I nomi Avid delle tre qualità DNxHD a ogni base del timecode: la qualità è la stessa, il nome no.
static std::vector<int> dnxhd_della_cadenza(const Cadenza& c, bool interlacciato) {
    std::vector<int> v;
    switch (c.base) {
        case 24: v = {175, 115, 36}; break;
        case 25: v = {185, 120, 36}; break;
        case 30: v = {220, 145, 45}; break;
        case 50: v = {365, 240, 75}; break;
        default: v = {440, 290, 90}; break;                  // 60
    }
    if (interlacciato) v.pop_back();                          // in 1080i il DNxHD 36 non esiste
    return v;
}

static const std::vector<int> DNXHR = {728, 481, 149};

std::vector<int> codec_del_formato(const std::string& modo) {
    const Formato* f = formato(modo);
    if (!f) return dnxhd_della_cadenza(C25, false);
    return f->altezza == 2160 ? DNXHR : dnxhd_della_cadenza(f->cad, f->interlacciato);
}

bool codec_valido(const std::string& modo, int mbit) {
    const std::vector<int> c = codec_del_formato(modo);
    return std::find(c.begin(), c.end(), mbit) != c.end();
}

// La posizione di un codec nella sua lista: 0 alta, 1 media, 2 bassa. -1 se non è un codec conosciuto.
static int posizione(int mbit) {
    for (const auto& l : {std::vector<int>{175, 115, 36}, std::vector<int>{185, 120, 36}, std::vector<int>{220, 145, 45},
                          std::vector<int>{365, 240, 75}, std::vector<int>{440, 290, 90}, DNXHR}) {
        const auto i = std::find(l.begin(), l.end(), mbit);
        if (i != l.end()) return static_cast<int>(i - l.begin());
    }
    return -1;
}

int codec_per(const std::string& modo, int mbit) {
    if (codec_valido(modo, mbit)) return mbit;
    const std::vector<int> a = codec_del_formato(modo);
    const int k = posizione(mbit);
    return k >= 0 ? a[std::min<size_t>(k, a.size() - 1)] : a[0];
}

int codec_di_prova(const std::string& modo, int mbit) {
    return codec_per(uhd(modo) ? "2160p25" : "1080p25", mbit);
}

std::string nome_codec(int mbit) {
    switch (mbit) {
        case 728: return "DNxHR HQ";
        case 481: return "DNxHR SQ";
        case 149: return "DNxHR LB";
        default: return "DNxHD " + std::to_string(mbit);
    }
}

const char* profilo_dnxhr(int mbit) {
    switch (mbit) {
        case 728: return "dnxhr_hq";
        case 481: return "dnxhr_sq";
        case 149: return "dnxhr_lb";
        default: return nullptr;
    }
}

double mb_al_secondo(int mbit, const Cadenza& c) {
    // Il DNxHR ha la dimensione del fotogramma fissa: 728 è il flusso a 25p, misurato.
    const double m = profilo_dnxhr(mbit) ? mbit * c.fps() / 25.0 : mbit;
    return m / 8.0;
}
