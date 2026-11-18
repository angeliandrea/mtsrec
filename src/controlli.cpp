#include <windows.h>
#include <powrprof.h>

#include "controlli.h"
#include "formato.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

static const char* nome(Semaforo s) {
    return s == Semaforo::verde ? "GREEN " : s == Semaforo::giallo ? "YELLOW" : "RED   ";
}

// Dove finiscono le righe anche per la pagina, oltre che nella console: lo fissano le funzioni pubbliche.
static thread_local std::vector<Voce>* raccolta = nullptr;

static void riga(Semaforo& peggiore, Semaforo s, const std::string& testo) {
    std::printf("  [%s] %s\n", nome(s), testo.c_str());
    if (raccolta) raccolta->push_back({s, testo});
    peggiore = std::max(peggiore, s);
}

static std::string utf8(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string r(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, r.data(), n, nullptr, nullptr);
    return r;
}

static std::string minuscolo(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool chiave_esiste(const wchar_t* chiave) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, chiave, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

static std::wstring valore_testo(const wchar_t* chiave, const wchar_t* nome_valore) {
    wchar_t buf[256];
    DWORD n = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, chiave, nome_valore, RRF_RT_REG_SZ, nullptr, buf, &n) != ERROR_SUCCESS)
        return {};
    return buf;
}

double byte_al_secondo(const ParametriCodifica& pc) {
    // Il contenitore aggiunge poco: il 2% copre intestazioni e frammenti (misurato sulle prove 03-09).
    const double video = mb_al_secondo(pc.mbit, pc.cad) * 1e6 * 1.02;
    const double audio = pc.canali_audio * 3.0 * 48000 * (pc.wav ? 2 : 1);
    return video + audio;
}

// ------------------------------------------------------------------ sistema e spazio
Semaforo controlli_sistema(std::vector<Voce>* voci) {
    std::vector<Voce>* prima = raccolta;
    if (voci) raccolta = voci;
    Semaforo s = Semaforo::verde;

    // Piano energetico: col «Bilanciato» il processore scende di frequenza fra un fotogramma e l'altro.
    GUID* attivo = nullptr;
    if (PowerGetActiveScheme(nullptr, &attivo) == ERROR_SUCCESS) {
        wchar_t buf[128];
        DWORD n = sizeof buf;
        std::string nome_piano = "?";
        if (PowerReadFriendlyName(nullptr, attivo, nullptr, nullptr, reinterpret_cast<UCHAR*>(buf), &n) == ERROR_SUCCESS)
            nome_piano = utf8(buf);
        static const GUID prestazioni_elevate = {0x8c5e7fda, 0xe8bf, 0x4a96, {0x9a, 0x85, 0xa6, 0xe2, 0x3a, 0x8c, 0x63, 0x5c}};
        static const GUID prestazioni_massime = {0xe9a42b02, 0xd5df, 0x448d, {0xaa, 0x03, 0xf1, 0x47, 0x49, 0xeb, 0x61, 0x00}};
        const bool buono = IsEqualGUID(*attivo, prestazioni_elevate) || IsEqualGUID(*attivo, prestazioni_massime) ||
                           minuscolo(nome_piano).find("prestazioni") != std::string::npos ||
                           minuscolo(nome_piano).find("performance") != std::string::npos;
        riga(s, buono ? Semaforo::verde : Semaforo::giallo,
             "power plan: " + nome_piano + (buono ? "" : " — High performance recommended"));
        LocalFree(attivo);
    }

    // Windows Update: in pausa non scarica e non riavvia durante la diretta.
    const std::wstring pausa = valore_testo(L"SOFTWARE\\Microsoft\\WindowsUpdate\\UX\\Settings", L"PauseUpdatesExpiryTime");
    char adesso[32];
    const std::time_t t = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &t);
    std::strftime(adesso, sizeof adesso, "%Y-%m-%dT%H:%M:%SZ", &utc);
    const std::string fino = utf8(pausa);
    if (!fino.empty() && fino > adesso)
        riga(s, Semaforo::verde, "Windows Update paused until " + fino.substr(0, 10));
    else
        riga(s, Semaforo::giallo, "Windows Update active: it may download while recording — "
                                  "Settings › Windows Update › Pause updates");
    if (chiave_esiste(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired") ||
        chiave_esiste(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending"))
        riga(s, Semaforo::giallo, "a restart is pending for updates: better do it first");
    raccolta = prima;
    return s;
}

Semaforo controlli_rapidi(const std::string& cartella, int tracce, const ParametriCodifica& pc, std::vector<Voce>* voci) {
    std::printf("Pre-REC checks: %d tracks in %s\n", tracce, cartella.c_str());
    Semaforo s = controlli_sistema(voci);
    std::vector<Voce>* prima = raccolta;
    if (voci) raccolta = voci;

    // Cartelle sincronizzate: dentro, i file si caricano mentre si scrivono; sullo stesso disco, lo contendono.
    const std::u8string assoluta = fs::absolute(fs::u8path(cartella)).u8string();
    const std::string c = minuscolo(std::string(assoluta.begin(), assoluta.end()));
    const char unita = c.empty() ? 0 : c[0];
    struct Sync { const char* nome; std::string percorso; };
    std::vector<Sync> sync;
    if (const char* od = std::getenv("OneDrive")) sync.push_back({"OneDrive", minuscolo(od)});
    for (const char* d : {"c", "d", "e", "f", "g", "h"})
        if (fs::exists(std::string(d) + ":\\SynologyDrive")) sync.push_back({"Synology Drive", std::string(d) + ":\\synologydrive"});
    bool sync_ok = true;
    for (const auto& x : sync) {
        if (c.rfind(x.percorso, 0) == 0) {
            riga(s, Semaforo::rosso, std::string("the folder is inside ") + x.nome + ": choose one outside it");
            sync_ok = false;
        } else if (!x.percorso.empty() && x.percorso[0] == unita) {
            riga(s, Semaforo::giallo, std::string("the ") + x.nome +
                                      " folder is on the same drive: if it syncs while recording it competes for the drive");
            sync_ok = false;
        }
    }
    if (sync_ok) riga(s, Semaforo::verde, "no synced folder on the recording drive");

    // Antivirus: le esclusioni di Defender le legge solo l'amministratore.
    HKEY h = nullptr;
    const LONG e = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Defender\\Exclusions\\Paths", 0,
                                 KEY_READ, &h);
    if (e == ERROR_SUCCESS) {
        bool esclusa = false;
        wchar_t nome_v[512];
        for (DWORD i = 0;; ++i) {
            DWORD n = 512;
            if (RegEnumValueW(h, i, nome_v, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            const std::string p = minuscolo(utf8(nome_v));
            if (!p.empty() && c.rfind(p, 0) == 0) esclusa = true;
        }
        RegCloseKey(h);
        riga(s, esclusa ? Semaforo::verde : Semaforo::giallo,
             esclusa ? "folder excluded from antivirus scanning" : "folder not excluded from antivirus scanning: Defender reads every file written");
    } else {
        std::printf("  [  --  ] antivirus exclusion: readable only by an administrator, check it manually\n");
    }
    std::printf("  [  --  ] sleep: blocked by the program while recording\n");

    // Spazio: quante ore ci stanno con queste tracce.
    ULARGE_INTEGER libero{};
    fs::create_directories(fs::u8path(cartella));
    if (GetDiskFreeSpaceExW(fs::u8path(cartella).c_str(), &libero, nullptr, nullptr)) {
        const double serve = byte_al_secondo(pc) * tracce;
        const double ore = libero.QuadPart * 0.95 / serve / 3600;     // il 5% resta libero: un disco pieno rallenta
        char testo[200];
        std::snprintf(testo, sizeof testo, "free space %.0f GB: %d tracks at %.0f MB/s fit for %d h %02d min",
                      libero.QuadPart / 1e9, tracce, serve / 1e6, static_cast<int>(ore),
                      static_cast<int>(ore * 60) % 60);
        riga(s, ore < 0.5 ? Semaforo::rosso : ore < 2 ? Semaforo::giallo : Semaforo::verde, testo);
    }
    raccolta = prima;
    return s;
}

// ------------------------------------------------------------------ disco
// Ogni traccia scrive il suo file, senza la cache di Windows, così si misura il disco e non la memoria.
// I file si cancellano da soli alla chiusura.
struct ProvaDisco { double lag_max_s = 0; double latenza_max_ms = 0; long long byte = 0; double secondi = 0; };

static ProvaDisco scrivi_disco(const fs::path& cartella, int tracce, size_t blocco, double scritture_al_secondo,
                               double secondi, long long tetto_byte) {
    ProvaDisco r;
    std::atomic<long long> totale{0};
    std::vector<double> lag(tracce, 0), latenza(tracce, 0);
    std::vector<std::thread> fili;
    void* buf = VirtualAlloc(nullptr, blocco, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    for (size_t i = 0; i < blocco; ++i) static_cast<uint8_t*>(buf)[i] = static_cast<uint8_t>(i * 2654435761u >> 24);
    const auto t0 = Clock::now();
    const auto fine = t0 + std::chrono::duration<double>(secondi);
    for (int k = 0; k < tracce; ++k) {
        fili.emplace_back([&, k] {
            const fs::path f = cartella / ("disk test " + std::to_string(k + 1) + ".bin");
            HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                   FILE_FLAG_NO_BUFFERING | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            if (h == INVALID_HANDLE_VALUE) { lag[k] = 1e9; return; }
            for (long long n = 0;; ++n) {
                const auto previsto = t0 + std::chrono::duration<double>(scritture_al_secondo > 0 ? n / scritture_al_secondo : 0);
                if (previsto >= fine || totale >= tetto_byte) break;
                auto ora = Clock::now();
                if (ora < previsto) std::this_thread::sleep_until(previsto);
                else lag[k] = std::max(lag[k], std::chrono::duration<double>(ora - previsto).count());
                const auto a = Clock::now();
                DWORD scritti = 0;
                if (!WriteFile(h, buf, static_cast<DWORD>(blocco), &scritti, nullptr)) { lag[k] = 1e9; break; }
                latenza[k] = std::max(latenza[k], std::chrono::duration<double, std::milli>(Clock::now() - a).count());
                totale += scritti;
            }
            CloseHandle(h);
        });
    }
    for (auto& f : fili) f.join();
    r.secondi = std::chrono::duration<double>(Clock::now() - t0).count();
    VirtualFree(buf, 0, MEM_RELEASE);
    r.byte = totale;
    r.lag_max_s = *std::max_element(lag.begin(), lag.end());
    r.latenza_max_ms = *std::max_element(latenza.begin(), latenza.end());
    return r;
}

double disco_al_massimo(const std::string& cartella, int tracce, const ParametriCodifica& pc, double secondi,
                        long long tetto_byte) {
    size_t blocco = static_cast<size_t>(byte_al_secondo(pc) / pc.cad.fps());
    blocco = (blocco + 4095) / 4096 * 4096;
    const fs::path dir = fs::u8path(cartella) / "disk test";
    std::error_code ec;
    fs::create_directories(dir, ec);
    ProvaDisco m = scrivi_disco(dir, tracce, blocco, 0, secondi, tetto_byte);
    fs::remove(dir, ec);
    return m.lag_max_s >= 1e8 || m.secondi <= 0 ? 0 : m.byte / m.secondi;
}

// ------------------------------------------------------------------ processore
double codifica_al_massimo(int tracce, const ParametriCodifica& pc, double secondi, int ampiezza) {
    // Un'immagine che somiglia a una ripresa: barre in alto, sfumature sotto e grana da sensore,
    // ampiezza ±N livelli. La velocità del DNxHD a bitrate fisso dipende molto dalla grana:
    // sulla stessa immagine ffmpeg va a 532 fotogrammi al secondo senza e a 101 con ±8.
    // Il rumore puro non somiglia a niente di vero e dà il caso peggiore (sezione 9.54).
    std::vector<uint8_t> uyvy(static_cast<size_t>(pc.larghezza) * pc.altezza * 2);
    uint32_t x = 12345;
    auto grana = [&] { x = x * 1664525 + 1013904223; return static_cast<int>((x >> 16) % (2 * ampiezza + 1)) - ampiezza; };
    static const uint8_t barre[8][3] = {{180, 128, 128}, {168, 44, 136}, {145, 147, 44}, {133, 63, 52},
                                        {63, 193, 204}, {51, 109, 212}, {28, 212, 120}, {16, 128, 128}};
    for (int r = 0; r < pc.altezza; ++r)
        for (int c = 0; c < pc.larghezza / 2; ++c) {
            uint8_t* p = uyvy.data() + (static_cast<size_t>(r) * pc.larghezza + 2 * c) * 2;
            int y, u, v;
            if (r < pc.altezza / 3) {
                const auto& b = barre[c * 16 / pc.larghezza];
                y = b[0]; u = b[1]; v = b[2];
            } else {
                y = 30 + (c * 2 + r) * 190 / (pc.larghezza + pc.altezza);
                u = 90 + c * 76 / (pc.larghezza / 2);
                v = 166 - r * 76 / pc.altezza;
            }
            auto tieni = [](int a) { return static_cast<uint8_t>(std::clamp(a, 16, 235)); };
            p[0] = tieni(u); p[1] = tieni(y + grana()); p[2] = tieni(v); p[3] = tieni(y + grana());
        }
    std::atomic<long long> fatti{0};
    std::atomic<bool> basta{false};
    std::vector<std::thread> fili;
    for (int k = 0; k < tracce; ++k)
        fili.emplace_back([&] {
            Codificatore c;
            std::string e;
            if (!c.apri("", pc, 0, e)) { std::printf("  encoder: %s\n", e.c_str()); return; }
            while (!basta && c.scrivi(uyvy.data(), pc.larghezza * 2, nullptr, 0, e)) ++fatti;
            c.chiudi(e);
        });
    const auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(secondi));
    basta = true;
    for (auto& f : fili) f.join();
    return fatti / std::chrono::duration<double>(Clock::now() - t0).count();
}

Semaforo controlli_completi(const std::string& cartella, int tracce, const ParametriCodifica& pc, int secondi_disco) {
    Semaforo s = controlli_rapidi(cartella, tracce, pc);

    // Disco: il ritmo vero delle tracce più il 30%, per secondi_disco. La coda del registratore
    // tiene un secondo: se il disco resta indietro di più, si perdono fotogrammi.
    const double serve = byte_al_secondo(pc) * tracce;
    size_t blocco = static_cast<size_t>(byte_al_secondo(pc) / pc.cad.fps());
    blocco = (blocco + 4095) / 4096 * 4096;
    const fs::path dir = fs::u8path(cartella) / "disk test";
    fs::create_directories(dir);
    std::printf("  ...  drive: %d files at %.0f MB/s in total, i.e. the required rate plus 30%%, for %d s\n", tracce,
                serve * 1.3 / 1e6, secondi_disco);
    ProvaDisco d = scrivi_disco(dir, tracce, blocco, pc.cad.fps() * 1.3, secondi_disco, LLONG_MAX);
    char testo[260];
    if (d.lag_max_s >= 1e8) {
        riga(s, Semaforo::rosso, "drive: writing to the folder fails");
    } else {
        std::snprintf(testo, sizeof testo, "drive: %.1f GB in %.0f s, maximum lag %.0f ms, slowest write %.0f ms",
                      d.byte / 1e9, d.secondi, d.lag_max_s * 1000, d.latenza_max_ms);
        riga(s, d.lag_max_s >= 1.0 ? Semaforo::rosso : d.lag_max_s >= 0.4 ? Semaforo::giallo : Semaforo::verde, testo);
        // Poi quanto regge al massimo, per sapere il margine: pochi secondi e al più 20 GB.
        ProvaDisco m = scrivi_disco(dir, tracce, blocco, 0, 5, 20'000'000'000LL);
        std::snprintf(testo, sizeof testo, "drive at full speed: %.0f MB/s, %.1f times the required rate",
                      m.byte / m.secondi / 1e6, m.byte / m.secondi / serve);
        riga(s, m.byte / m.secondi >= serve * 1.3 ? Semaforo::verde : Semaforo::giallo, testo);
        std::printf("  [  --  ] the short test does not reach beyond the NVMe fast cache: over hours of recording "
                    "the drive may slow down\n");
    }
    std::error_code ec;
    fs::remove(dir, ec);

    // Processore: tutte le tracce insieme, più in fretta che si può, due volte. Con l'immagine
    // pulita decide il semaforo; con la grana di una camera a guadagno alto, in una sala buia,
    // al più avvisa: è il caso peggiore e non quello di ogni giorno.
    for (const auto& [ampiezza, come] : {std::pair{2, "clean image"}, std::pair{8, "camera with high gain"}}) {
        std::printf("  ...  CPU: %d simultaneous %s encodes at %dx%d for 10 s, %s\n", tracce, nome_codec(pc.mbit).c_str(),
                    pc.larghezza, pc.altezza, come);
        const double fps = codifica_al_massimo(tracce, pc, 10, ampiezza);
        const double volte = fps / (tracce * pc.cad.fps());
        std::snprintf(testo, sizeof testo, "CPU, %s: %.0f frames per second in total, %.2f times real time%s",
                      come, fps, volte, volte < 1.0 ? " — too slow: it would drop frames"
                                        : volte < 1.3 ? " — below the 30% margin" : "");
        Semaforo v = volte >= 1.3 ? Semaforo::verde : volte >= 1.0 ? Semaforo::giallo : Semaforo::rosso;
        if (ampiezza > 2 && v == Semaforo::rosso) v = Semaforo::giallo;
        riga(s, v, testo);
    }

    std::printf("Result: %s\n", nome(s));
    return s;
}
