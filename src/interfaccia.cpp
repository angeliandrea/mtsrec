#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include "interfaccia.h"
#include "registro.h"
#include "ripara.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "carico.h"
#include "esterni.h"
#include "finestra.h"
#include "formato.h"
#include "impostazioni.h"
#include "ingresso.h"
#include "json.h"
#include "macchina.h"
#include "marker.h"
#include "registrazione.h"
#include "traccia.h"

namespace fs = std::filesystem;

namespace {

const char* FASI[] = {"ferma", "partenza", "registra", "fine"};
enum Fase { ferma = 0, partenza = 1, registra = 2, fine = 3 };

std::string da_largo(const std::wstring& s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string r(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, r.data(), n, nullptr, nullptr);
    return r;
}

std::string testo_di(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

double dbfs(float picco) { return picco > 1e-6f ? 20.0 * std::log10(picco) : -120.0; }

// L'aux su cui è cablata una porta, da porte.json (mappa_porte.py, sezione 9.58); 0 se non c'è.
int aux_della_porta(const std::string& mappa, const std::string& porta) {
    const size_t p = mappa.find("\"" + porta + "\"");
    return p == std::string::npos ? 0 : std::atoi(json_valore(mappa.substr(p), "aux").c_str());
}

// La sorgente su un'aux e il suo nome, da quello che ha risposto nomi_atem.py.
void sorgente_dell_aux(const std::string& nomi, int aux, int& sorgente, std::string& nome) {
    sorgente = 0;
    nome.clear();
    const size_t a = nomi.find("\"aux\""), b = nomi.find("\"nomi\"");
    if (a == std::string::npos || aux <= 0) return;
    const std::string sez = nomi.substr(a, b == std::string::npos ? std::string::npos : b - a);
    const size_t p = sez.find("\"" + std::to_string(aux) + "\":");
    if (p == std::string::npos) return;
    const std::string r = sez.substr(p + std::to_string(aux).size() + 3);
    sorgente = std::atoi(json_valore(r, "sorgente").c_str());
    nome = json_valore(r, "nome");
}

struct App {
    Impostazioni imp;
    Analisi analisi;
    bool analisi_avviata = false, accettato = false, da_controllare = false;
    std::string perche;
    Carico carico;
    Orologio orologio;
    std::vector<Porta> porte;                         // tutte, da rilasciare alla fine
    std::vector<const Porta*> porte_ingresso;         // una riga della pagina per ognuna
    std::vector<std::unique_ptr<Ingresso>> ingressi;  // uno per porta d'ingresso
    std::vector<std::string> errori_porte;            // vuoto se la porta è aperta
    std::string modo_aperto;
    int audio_aperto = -1;
    std::unique_ptr<Marcatore> marcatore;
    std::unique_ptr<Registrazione> reg;
    std::atomic<int> fase{ferma};
    std::thread lavoro;                               // REC e STOP, che durano qualche secondo
    std::mutex m;                                     // i testi qui sotto, scritti dai fili di lavoro
    std::string errore_rec, progetto, nomi_atem, nomi_errore, nomi_ora;
    int progetto_stato = 0;                           // sotto m: 0 niente, 1 in costruzione, 2 pronto, 3 non creato
    void* progetto_proc = nullptr;                    // sotto m: la finestra dello script del progetto
    std::string progetto_cartella;                    // sotto m: la registrazione di cui si costruisce il progetto
    std::atomic<int64_t> t_rec_us{0};
    std::atomic<bool> nomi_in_corso{false}, misura_in_corso{false}, esci{false}, disco_rileggi{false};
    std::thread filo_nomi, filo_misura, filo_cerca, filo_ritardi, filo_mappa, filo_ripara;
    std::atomic<bool> mappa_in_corso{false};
    std::string mappa_esito = "null";                  // sotto m; il JSON dell'ultima mappa delle porte (9.78)
    std::atomic<bool> ritardi_in_corso{false}, ritardi_nuovi{false};
    std::string ritardi_esito = "null";                // sotto m; il JSON dell'ultima misura dei ritardi
    std::vector<std::pair<std::string, int>> ritardi_trovati;   // sotto m; porta e fotogrammi da salvare
    std::atomic<bool> cerca_in_corso{false}, cerca_nuovi{false};
    Apparecchi trovati;                               // sotto m; l'ultima ricerca in rete
    // Il formato non si sceglie: lo dice l'ATEM, o in mancanza il segnale (Andrea, 17 novembre 2026, 9.79).
    std::thread filo_formato;
    std::atomic<bool> formato_in_corso{false};
    int64_t formato_letto_us = 0;                     // sotto m: l'ultima risposta dell'ATEM
    int64_t formato_chiesto_us = 0;                   // solo il filo del server
    std::string formato_atem, formato_atem_errore;    // sotto m; formato_atem vuoto se l'ATEM non è in un formato registrabile
    int formato_atem_numero = -1;                     // sotto m; il numero del Video Mode, -1 se non letto
    std::string formato_fonte = "settings";           // solo il filo del server: «ATEM», «signal» o «settings»
    double misura_mb_s = -1, disco_mb_s = 0;
    std::string misura_cartella, disco_di;            // disco_di: la cartella per cui vale disco_mb_s
};

// Le porte d'ingresso aperte in anteprima col formato e l'audio delle impostazioni. Si riaprono
// quando cambiano, mai mentre si registra.
void apri_porte(App& a) {
    const BMDDisplayMode bmd = modo_decklink(a.imp.modo);
    // L'orologio prende la cadenza del formato; il drop-frame già letto dal segnale si tiene (ingresso.cpp).
    if (const Formato* f = formato(a.imp.modo)) {
        Cadenza c = f->cad;
        if (const Cadenza o = a.orologio.cadenza(); o.num == c.num && o.den == c.den) c.drop = o.drop;
        a.orologio.imposta(c);
    }
    if (a.ingressi.empty())
        for (const Porta* p : a.porte_ingresso) {
            a.ingressi.push_back(std::make_unique<Ingresso>(p->scheda, p->nome, a.orologio));
            a.errori_porte.emplace_back();
        }
    for (size_t k = 0; k < a.ingressi.size(); ++k) {
        a.ingressi[k]->imposta_ritardo(a.imp.ritardo(a.ingressi[k]->nome()));
        a.ingressi[k]->ferma();
        std::string e;
        if (!a.ingressi[k]->apri(bmd, a.imp.audio, e) || !a.ingressi[k]->avvia(e)) {
            a.ingressi[k]->ferma();
            a.errori_porte[k] = e.empty() ? "cannot be opened" : e;
            std::printf("⚠ %s\n", a.errori_porte[k].c_str());
        } else {
            a.errori_porte[k].clear();
        }
    }
    a.modo_aperto = a.imp.modo;
    a.audio_aperto = a.imp.audio;
    std::printf("%zu inputs open in preview, %s, audio %d channels\n", a.ingressi.size(), a.imp.modo.c_str(), a.imp.audio);
}

void aggiorna_disco(App& a) {
    if (a.disco_rileggi.exchange(false)) a.disco_di = "?";
    if (a.disco_di == a.imp.cartella) return;
    a.disco_di = a.imp.cartella;
    a.disco_mb_s = velocita_misurata(a.imp.cartella);
}

void leggi_nomi(App& a);
void usa_trovati(App& a);
void usa_ritardi(App& a);

// Il progetto Resolve in una finestra sua (progetto_da_registratore.py), allo STOP o col tasto PROGETTO.
// Se Resolve è chiuso lo script lo apre. L'esito lo scrive in progetto.json nella cartella: qui si
// legge quel file e si guarda se la finestra si è chiusa senza scriverlo, così la pagina non resta su
// «in costruzione» (16 novembre 2026). Tutte e due con a.m preso.
static void avvia_progetto(App& a, const std::string& cartella) {
    if (a.progetto_proc) { CloseHandle(a.progetto_proc); a.progetto_proc = nullptr; }
    std::error_code ec;
    fs::remove(fs::u8path(cartella) / "progetto.json", ec);
    a.progetto_cartella = cartella;
    std::string e;
    void* h = nullptr;
    if (Python::lancia("progetto_da_registratore.py", "\"" + cartella + "\" --atem --dal-registratore", e, &h)) {
        a.progetto_proc = h;
        a.progetto_stato = 1;
        a.progetto = "Resolve project being built in the window that opened; Resolve is launched if it is closed";
    } else {
        a.progetto_stato = 3;
        a.progetto = "Resolve project not built: " + e;
    }
}

static void guarda_progetto(App& a) {
    if (a.progetto_stato != 1) return;
    const bool finito = !a.progetto_proc || WaitForSingleObject(a.progetto_proc, 0) == WAIT_OBJECT_0;
    const std::string j = leggi_file((fs::u8path(a.progetto_cartella) / "progetto.json").wstring());
    const std::string ok = json_valore(j, "ok");
    if (!ok.empty()) {
        a.progetto_stato = ok == "true" ? 2 : 3;
        a.progetto = json_valore(j, "testo");
    } else if (finito) {
        a.progetto_stato = 3;
        a.progetto = "Resolve project not built: the window closed without reporting the result";
    } else return;
    if (finito && a.progetto_proc) { CloseHandle(a.progetto_proc); a.progetto_proc = nullptr; }
}

// Per i parametri di un indirizzo mailto: tutto quello che non è lettera o cifra in %XX, gli a capo come %0D%0A.
[[maybe_unused]] static std::string url_codifica(const std::string& s) {
    static const char* H = "0123456789ABCDEF";
    std::string r;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') r += static_cast<char>(c);
        else if (c == '\n') r += "%0D%0A";
        else { r += '%'; r += H[c >> 4]; r += H[c & 15]; }
    }
    return r;
}

// L'ATEM letto in un filo suo ogni 5 secondi, fuori registrazione: stato_atem.py, sola lettura.
static void leggi_formato_atem(App& a) {
    if (a.formato_in_corso || a.imp.atem.empty() || a.fase != ferma) return;
    if (adesso_us() - a.formato_chiesto_us < 5'000'000) return;
    a.formato_chiesto_us = adesso_us();
    if (a.filo_formato.joinable()) a.filo_formato.join();
    a.formato_in_corso = true;
    a.filo_formato = std::thread([&a, ip = a.imp.atem] {
        std::string e;
        const std::string r = Python::esegui("stato_atem.py", "--ip " + ip, 15000, e);
        std::lock_guard<std::mutex> l(a.m);
        if (e.empty() && r.find("\"ok\": true") != std::string::npos) {
            a.formato_atem = json_valore(r, "formato");
            if (a.formato_atem == "null") a.formato_atem.clear();
            const std::string n = json_valore(r, "video_mode");
            a.formato_atem_numero = n.empty() || n == "null" ? -1 : std::atoi(n.c_str());
            a.formato_atem_errore.clear();
            a.formato_letto_us = adesso_us();
        } else {
            a.formato_atem_errore = e.empty() ? json_valore(r, "errore") : e;
            if (a.formato_atem_errore.empty()) a.formato_atem_errore = "no answer";
        }
        a.formato_in_corso = false;
    });
}

// Il formato da registrare: quello dell'ATEM se ha risposto da poco, altrimenti quello che arriva a tutte
// le porte con segnale, se è lo stesso. Se cambia, le porte si riaprono, mai durante la registrazione.
// Niente conversioni né ingrandimenti: il registratore registra quello che arriva (Andrea, 17 novembre 2026).
static void usa_formato(App& a) {
    if (a.fase != ferma || !a.accettato) return;
    leggi_formato_atem(a);
    std::string voluto, fonte = "settings";
    {
        std::lock_guard<std::mutex> l(a.m);
        if (!a.imp.atem.empty() && a.formato_letto_us && adesso_us() - a.formato_letto_us < 15'000'000) {
            fonte = "ATEM";
            voluto = a.formato_atem;                   // vuoto: l'ATEM è in 720p o 8K, la pagina lo dice
        }
    }
    if (fonte != "ATEM") {
        std::string visto;
        bool concordi = true;
        for (auto& in : a.ingressi) {
            const Formato* f = formato_del_segnale(in->formato_arrivo());
            if (!f) continue;
            if (visto.empty()) visto = f->nome;
            else if (visto != f->nome) concordi = false;
        }
        if (!visto.empty() && concordi) { fonte = "signal"; voluto = visto; }
    }
    a.formato_fonte = fonte;
    if (voluto.empty() || voluto == a.imp.modo) return;
    std::printf("format from the %s: %s (was %s)\n", fonte.c_str(), voluto.c_str(), a.imp.modo.c_str());
    a.imp.modo = voluto;
    a.imp.mbit = codec_per(voluto, a.imp.mbit);
    a.imp.salva();
    apri_porte(a);
}

std::string stato_json(App& a) {
    usa_trovati(a);
    usa_ritardi(a);
    usa_formato(a);
    const int fase = a.fase;
    Registrazione* reg = a.reg.get();
    const bool rec = reg && reg->registra();
    std::string r = "{\"fase\":\"" + std::string(FASI[fase]) + "\",\"registra\":" + json_si(rec) +
                    ",\"versione\":" + json_testo(WINISO_VERSIONE);
    const int tc = rec ? reg->timecode() : a.orologio.valido() ? a.orologio.stima(adesso_us()) : -1;
    r += ",\"timecode\":\"" + (tc >= 0 ? tc_testo(tc, a.orologio.cadenza()) : std::string("--:--:--:--")) + "\"";
    r += ",\"tc_dal_segnale\":" + std::string(json_si(a.orologio.valido()));
    if (reg) {
        r += ",\"inizio\":\"" + (reg->inizio() >= 0 ? tc_testo(reg->inizio(), a.orologio.cadenza()) : std::string("--")) + "\"";
        r += ",\"cartella\":" + json_testo(reg->cartella());
        r += ",\"tracce\":" + reg->tracce_json() + ",\"persi\":" + std::to_string(reg->persi());
        r += ",\"controlli\":" + (reg->controlli_json().empty() ? std::string("[]") : reg->controlli_json());
        r += ",\"hyperdeck_record\":" + json_testo(reg->hyperdeck_risposta());
    }
    if (rec) r += ",\"secondi\":" + json_num((adesso_us() - a.t_rec_us) / 1e6, 1);
    r += ",\"marker\":" + std::to_string(a.marcatore->quanti()) + ",\"elenco_marker\":" + a.marcatore->elenco_json();
    {
        std::lock_guard<std::mutex> l(a.m);
        guarda_progetto(a);
        r += ",\"errore\":" + json_testo(a.errore_rec) + ",\"progetto\":" + json_testo(a.progetto);
        r += ",\"progetto_stato\":" + std::to_string(a.progetto_stato);
        r += ",\"nomi_letti\":" + json_testo(a.nomi_ora) + ",\"nomi_errore\":" + json_testo(a.nomi_errore);
        r += ",\"formato\":{\"modo\":" + json_testo(a.imp.modo) + ",\"fonte\":" + json_testo(a.formato_fonte) +
             ",\"drop\":" + json_si(a.orologio.cadenza().drop) + ",\"atem_numero\":" + std::to_string(a.formato_atem_numero) +
             ",\"atem_registrabile\":" + json_si(a.formato_atem_numero < 0 || !a.formato_atem.empty()) +
             ",\"atem_errore\":" + json_testo(a.formato_atem_errore) + "}";
    }
    r += ",\"nomi_in_corso\":" + std::string(json_si(a.nomi_in_corso));
    {
        // Quante aux ha l'ATEM: una voce con "sorgente" per aux in quello che ha risposto nomi_atem.py,
        // 0 se non si è ancora letto. È uno dei limiti alle tracce che la pagina propone (17 novembre).
        std::lock_guard<std::mutex> l(a.m);
        int aux = 0;
        for (size_t p = a.nomi_atem.find("\"sorgente\""); p != std::string::npos; p = a.nomi_atem.find("\"sorgente\"", p + 1)) ++aux;
        r += ",\"aux_atem\":" + std::to_string(aux);
    }
    {
        std::lock_guard<std::mutex> l(a.m);
        auto elenco = [](const std::vector<std::string>& v) {
            std::string s;
            for (const auto& x : v) s += (s.empty() ? "" : ",") + json_testo(x);
            return "[" + s + "]";
        };
        r += ",\"ritardi\":{\"in_corso\":" + std::string(json_si(a.ritardi_in_corso)) + ",\"esito\":" + a.ritardi_esito + "}";
        r += ",\"mappa\":{\"in_corso\":" + std::string(json_si(a.mappa_in_corso)) + ",\"esito\":" + a.mappa_esito + "}";
        r += ",\"cerca\":{\"in_corso\":" + std::string(json_si(a.cerca_in_corso)) + ",\"atem\":" + elenco(a.trovati.atem) +
             ",\"hyperdeck\":" + elenco(a.trovati.hyperdeck) + "}";
    }
    // Le righe: una per porta d'ingresso della macchina, aperta o no.
    const std::string mappa = leggi_file(cartella_utente() + L"\\porte.json");
    std::string nomi;
    {
        std::lock_guard<std::mutex> l(a.m);
        nomi = a.nomi_atem;
    }
    r += ",\"ingressi\":[";
    for (size_t k = 0; k < a.ingressi.size(); ++k) {
        Ingresso& in = *a.ingressi[k];
        const bool aperta = a.errori_porte[k].empty();
        const int aux = aux_della_porta(mappa, in.nome());
        int sorgente = 0;
        std::string nome_sorgente;
        sorgente_dell_aux(nomi, aux, sorgente, nome_sorgente);
        double l1 = -120, l2 = -120;
        if (aperta && in.canali_audio() >= 2) { l1 = dbfs(in.picco(0)); l2 = dbfs(in.picco(1)); }
        r += std::string(k ? "," : "") + "{\"n\":" + std::to_string(k) + ",\"porta\":" + json_testo(in.nome()) +
             ",\"connettore\":" + json_testo(a.porte_ingresso[k]->connettore) + ",\"aperta\":" + json_si(aperta) +
             ",\"errore\":" + json_testo(a.errori_porte[k]) + ",\"segnale\":" + json_si(aperta && in.segnale()) +
             ",\"tc\":\"" + (aperta && in.tc() >= 0 ? tc_testo(in.tc(), a.orologio.cadenza()) : std::string("--")) + "\"" +
             ",\"tc_dal_segnale\":" + json_si(aperta && in.tc_dal_segnale()) + ",\"formato\":" + json_testo(in.formato_arrivo()) + ",\"formato_registratore\":" +
             json_testo([&] { const Formato* f = formato_del_segnale(in.formato_arrivo()); return f ? std::string(f->nome) : std::string(); }()) +
             ",\"db\":[" + json_num(l1, 1) + "," + json_num(l2, 1) + "],\"aux\":" + std::to_string(aux) +
             ",\"sorgente\":" + std::to_string(sorgente) + ",\"nome_sorgente\":" + json_testo(nome_sorgente) +
             ",\"ritardo\":" + std::to_string(in.ritardo()) + "}";
    }
    r += "],\"carico\":" + a.carico.json();
    aggiorna_disco(a);
    ULARGE_INTEGER libero{}, totale{};
    const bool c_e = !a.imp.cartella.empty() &&
                     GetDiskFreeSpaceExW(fs::u8path(a.imp.cartella).c_str(), &libero, &totale, nullptr);
    r += ",\"disco\":{\"esiste\":" + std::string(json_si(c_e)) + ",\"libero_gb\":" + json_num(libero.QuadPart / 1e9, 1) +
         ",\"totale_gb\":" + json_num(totale.QuadPart / 1e9, 1) + ",\"mb_s\":" + json_num(a.disco_mb_s) + "}";
    r += ",\"misura\":{\"in_corso\":" + std::string(json_si(a.misura_in_corso)) + ",\"mb_s\":" + json_num(a.misura_mb_s) +
         ",\"cartella\":" + json_testo(a.misura_cartella) + "}";
    r += ",\"impostazioni\":" + a.imp.json();
    return r + "}";
}

std::string cartelle_json(const std::string& percorso) {
    std::error_code ec;
    const fs::path p = fs::u8path(percorso);
    if (percorso.empty() || !fs::is_directory(p, ec))
        return "{\"percorso\":" + json_testo(percorso) + ",\"errore\":\"folder not found\",\"cartelle\":[]}";
    std::vector<std::string> v;
    for (const auto& e : fs::directory_iterator(p, fs::directory_options::skip_permission_denied, ec)) {
        std::error_code e2;
        if (!e.is_directory(e2)) continue;
        const DWORD at = GetFileAttributesW(e.path().c_str());
        if (at != INVALID_FILE_ATTRIBUTES && (at & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        v.push_back(testo_di(e.path().filename()));
        if (v.size() >= 1000) break;
    }
    std::sort(v.begin(), v.end());
    std::string r = "{\"percorso\":" + json_testo(testo_di(p)) + ",\"su\":" +
                    json_testo(p.has_relative_path() && p.parent_path() != p ? testo_di(p.parent_path()) : "") + ",\"cartelle\":[";
    for (size_t k = 0; k < v.size(); ++k) r += std::string(k ? "," : "") + json_testo(v[k]);
    return r + "]}";
}

std::string pagina_file(const wchar_t* nome) {
    std::string s = leggi_file(cartella_programma() + L"\\interfaccia\\" + nome);
    if (s.empty()) {
        // Compilando: la pagina dalla cartella dei sorgenti.
        const std::string d = WINISO_PAGINE_DIR;
        s = leggi_file(std::wstring(d.begin(), d.end()) + L"/" + nome);
    }
    return s.empty() ? "<!doctype html><meta charset=utf-8><p>The page is missing from the recorder's interfaccia folder: "
                       "copy the whole folder again.</p>"
                     : s;
}

void leggi_nomi(App& a) {
    if (a.nomi_in_corso || a.imp.atem.empty()) return;
    if (a.filo_nomi.joinable()) a.filo_nomi.join();
    a.nomi_in_corso = true;
    a.filo_nomi = std::thread([&a, ip = a.imp.atem] {
        std::string e;
        const std::string r = Python::esegui("nomi_atem.py", "--atem " + ip, 15000, e);
        char ora[16];
        const std::time_t t = std::time(nullptr);
        std::tm loc{};
        localtime_s(&loc, &t);
        std::strftime(ora, sizeof ora, "%H:%M:%S", &loc);
        std::lock_guard<std::mutex> l(a.m);
        const std::string err = e.empty() ? json_valore(r, "errore") : e;
        if (err.empty() && r.find("\"aux\"") != std::string::npos) {
            a.nomi_atem = r;
            a.nomi_ora = ora;
            a.nomi_errore.clear();
        } else {
            a.nomi_errore = err.empty() ? "unrecognised response" : err;
        }
        a.nomi_in_corso = false;
    });
}

// ATEM e HyperDeck cercati in rete, in un filo suo. Quello che trova lo mette nelle impostazioni il
// filo del server, al primo /stato, perché le impostazioni le tocca solo lui.
void cerca(App& a) {
    if (a.cerca_in_corso) return;
    if (a.filo_cerca.joinable()) a.filo_cerca.join();
    a.cerca_in_corso = true;
    a.filo_cerca = std::thread([&a] {
        const Apparecchi r = cerca_apparecchi();
        std::string t;
        for (const auto& x : r.atem) t += " ATEM " + x;
        for (const auto& x : r.hyperdeck) t += " HyperDeck " + x;
        std::printf("network search:%s\n", t.empty() ? " no ATEM or HyperDeck found" : t.c_str());
        {
            std::lock_guard<std::mutex> l(a.m);
            a.trovati = r;
        }
        a.cerca_nuovi = true;
        a.cerca_in_corso = false;
    });
}

void usa_trovati(App& a) {
    if (!a.cerca_nuovi.exchange(false) || a.fase != ferma) return;
    Apparecchi r;
    {
        std::lock_guard<std::mutex> l(a.m);
        r = a.trovati;
    }
    bool cambiato = false;
    if (a.imp.atem.empty() && !r.atem.empty()) { a.imp.atem = r.atem[0]; cambiato = true; }
    if (a.imp.hyperdeck.empty() && !r.hyperdeck.empty()) { a.imp.hyperdeck = r.hyperdeck[0]; cambiato = true; }
    if (!cambiato) return;
    a.imp.salva();
    std::printf("addresses found on the network: ATEM %s, HyperDeck %s\n", a.imp.atem.c_str(), a.imp.hyperdeck.c_str());
    leggi_nomi(a);
}

// ============================================================ misura dei ritardi (sezione 9.60)
// Uno stacco visto da una porta: il timecode del primo fotogramma col nuovo stato e se è salito o sceso.
struct Stacco { int tc; bool su; };

std::vector<Stacco> stacchi_visti(const std::vector<Ingresso::Luce>& v) {
    std::vector<Stacco> r;
    if (v.size() < 50) return r;
    float lo = v[0].y, hi = v[0].y;
    for (const auto& x : v) { lo = std::min(lo, x.y); hi = std::max(hi, x.y); }
    if (hi - lo < 20) return r;                        // l'immagine non è cambiata: la porta non vede il programma
    const float mezzo = (lo + hi) / 2;
    bool stato = v[0].y > mezzo;
    size_t ultimo = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        const bool s = v[i].y > mezzo;
        if (s == stato) continue;
        stato = s;
        if (r.empty() || i - ultimo > 3) r.push_back({v[i].tc, s});   // un fotogramma a metà non è uno stacco
        ultimo = i;
    }
    return r;
}

// Quanti fotogrammi dopo il riferimento la porta vede gli stessi stacchi. Il riferimento porta il
// timecode dell'ATEM nel segnale: i suoi stacchi cadono sul timecode giusto (sezione 9.59).
std::string confronta(const std::vector<Stacco>& rif, const std::vector<Stacco>& porta, int giorno, int& ritardo) {
    ritardo = -1;
    if (porta.empty()) return "no cuts seen: the input receives neither the program nor the clean feed";
    std::vector<int> d;
    for (const auto& s : porta) {
        int meglio = 1 << 30;
        for (const auto& x : rif) {
            if (x.su != s.su) continue;
            int k = ((s.tc - x.tc) % giorno + giorno) % giorno;
            if (k > giorno / 2) k -= giorno;
            if (std::abs(k) < std::abs(meglio)) meglio = k;
        }
        if (std::abs(meglio) <= 12) d.push_back(meglio);
    }
    if (d.size() < 4) return "only " + std::to_string(d.size()) + " cuts in common with the reference";
    std::vector<int> o = d;
    std::sort(o.begin(), o.end());
    const int m = o[o.size() / 2];
    const auto uguali = std::count(d.begin(), d.end(), m);
    if (uguali * 4 < static_cast<long long>(d.size()) * 3)
        return "inconsistent cuts: " + std::to_string(uguali) + " of " + std::to_string(d.size()) + " at " + std::to_string(m);
    if (m < 0) return "the input is " + std::to_string(-m) + " frames ahead of the reference: the reference is not valid";
    if (m > 60) return "more than 60 frames";
    ritardo = m;
    return std::to_string(uguali) + " of " + std::to_string(d.size()) + " cuts at " + std::to_string(m) + " frames";
}

// Fermare la mappa o la misura dei ritardi (24 novembre 2026, sezione 9.87). Il registratore crea un file e
// lo script lo vede fra un passo e l'altro: rimette quello che ha toccato nell'ATEM, verifica ed esce.
// Così non si uccide mai uno script a metà, con un'aux rimasta sul nero. Lo usano il tasto Stop della
// pagina e la chiusura della finestra, che chiede prima se fermare.
static std::wstring file_ferma(const wchar_t* nome) { return cartella_utente() + L"\\" + nome; }
static const wchar_t* FERMA_MAPPA = L"stop input map";
static const wchar_t* FERMA_RITARDI = L"stop delay measurement";

static void togli_ferma(const wchar_t* nome) { DeleteFileW(file_ferma(nome).c_str()); }

static std::string argomento_ferma(const wchar_t* nome) {
    togli_ferma(nome);
    return " --ferma \"" + da_largo(file_ferma(nome)) + "\"";
}

static void crea_ferma(const wchar_t* nome) {
    HANDLE h = CreateFileW(file_ferma(nome).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
}

// Chiede di fermare quello che scrive nell'ATEM, se c'è. Vero se qualcosa era in corso.
static bool ferma_scritture_atem(App& a) {
    bool in_corso = false;
    if (a.mappa_in_corso) { crea_ferma(FERMA_MAPPA); in_corso = true; }
    if (a.ritardi_in_corso) { crea_ferma(FERMA_RITARDI); in_corso = true; }
    if (in_corso) std::printf("stop requested: the ATEM is being put back as it was\n");
    return in_corso;
}

// Prima di scrivere nell'ATEM: se la lettura di ogni 5 secondi dice che non risponde, inutile partire e far
// aspettare (24 novembre 2026: con l'ATEM spento la mappa restava su «mapping…» 20 secondi).
static std::string atem_muto(App& a) {
    std::lock_guard<std::mutex> l(a.m);
    if (a.formato_atem_errore.empty()) return {};
    return "the ATEM at " + a.imp.atem + " does not answer: check that it is on and on the network, then try again";
}

// /misura-ritardi?porte=0,3: stacchi barre e nero sul programma, e per ogni porta il ritardo che ne esce.
std::string misura_ritardi(App& a, const std::string& query, int& codice) {
    if (a.fase != ferma) { codice = 409; return "{\"errore\":\"delays cannot be measured while recording\"}"; }
    if (a.ritardi_in_corso) { codice = 409; return "{\"errore\":\"a measurement is already running\"}"; }
    if (a.mappa_in_corso) { codice = 409; return "{\"errore\":\"the input map is being made: wait until it restores the ATEM\"}"; }
    if (a.imp.atem.empty()) { codice = 409; return "{\"errore\":\"set the ATEM address in Settings: the measurement performs the cuts\"}"; }
    if (const std::string m = atem_muto(a); !m.empty()) { codice = 409; return "{\"errore\":" + json_testo(m) + "}"; }
    std::vector<size_t> scelte;
    const std::string s = parametro_query(query, "porte");
    for (size_t p = 0; p < s.size();) {
        const size_t f = std::min(s.find(',', p), s.size());
        const size_t n = static_cast<size_t>(std::atoi(s.substr(p, f - p).c_str()));
        if (f > p && n < a.ingressi.size() && a.errori_porte[n].empty()) scelte.push_back(n);
        p = f + 1;
    }
    if (scelte.empty()) { codice = 409; return "{\"errore\":\"no open input to measure\"}"; }
    const std::string mappa = leggi_file(cartella_utente() + L"\\porte.json");
    auto scelta = [&](size_t k) { return std::find(scelte.begin(), scelte.end(), k) != scelte.end(); };
    // Il riferimento: una porta col timecode dell'ATEM nel segnale, meglio se non si misura e ha un'aux.
    int rif = -1, voto = -1;
    for (size_t k = 0; k < a.ingressi.size(); ++k) {
        Ingresso& in = *a.ingressi[k];
        if (!a.errori_porte[k].empty() || !in.segnale() || !in.tc_dal_segnale()) continue;
        const int v = (scelta(k) ? 0 : 2) + (aux_della_porta(mappa, in.nome()) > 0 ? 1 : 0);
        if (v > voto) { voto = v; rif = static_cast<int>(k); }
    }
    if (rif < 0) { codice = 409; return "{\"errore\":\"an SDI input receiving the ATEM timecode is needed as reference\"}"; }
    std::vector<int> aux;
    auto metti = [&](size_t k) {
        const int x = aux_della_porta(mappa, a.ingressi[k]->nome());
        if (x > 0 && std::find(aux.begin(), aux.end(), x) == aux.end()) aux.push_back(x);
    };
    for (size_t k : scelte) metti(k);
    metti(static_cast<size_t>(rif));
    std::string lista;
    for (int x : aux) lista += (lista.empty() ? "" : ",") + std::to_string(x);
    if (a.filo_ritardi.joinable()) a.filo_ritardi.join();
    a.ritardi_in_corso = true;
    const std::string ferma_arg = argomento_ferma(FERMA_RITARDI);
    a.filo_ritardi = std::thread([&a, scelte, rif, lista, ferma_arg, ip = a.imp.atem] {
        std::vector<Ingresso*> coinvolti{a.ingressi[rif].get()};
        for (size_t k : scelte) coinvolti.push_back(a.ingressi[k].get());
        for (Ingresso* in : coinvolti) in->misura_luce(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        std::string e;
        const std::string r = Python::esegui("ritardi_porte.py", "--atem " + ip + (lista.empty() ? "" : " --aux " + lista) + ferma_arg, 60000, e);
        togli_ferma(FERMA_RITARDI);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));   // l'ultimo stacco arriva anche alle porte in ritardo
        for (Ingresso* in : coinvolti) in->misura_luce(false);
        const int giorno = a.orologio.giorno();
        const std::vector<Stacco> sr = stacchi_visti(a.ingressi[rif]->luci());
        const bool rimesso = json_valore(r, "rimesso") == "true";
        std::string err = e.empty() ? json_valore(r, "errore") : e;
        if (err.empty() && sr.size() < 4) err = "the reference did not see the cuts";
        std::string esito = "{\"riferimento\":" + json_testo(a.ingressi[rif]->nome()) + ",\"aux\":" + json_testo(lista) +
                            ",\"stacchi_riferimento\":" + std::to_string(sr.size()) + ",\"errore\":" + json_testo(err) +
                            ",\"atem_rimesso\":" + json_si(rimesso);
        std::vector<std::pair<std::string, int>> trovati;
        esito += ",\"porte\":[";
        for (size_t i = 0; i < scelte.size(); ++i) {
            Ingresso& in = *a.ingressi[scelte[i]];
            int n = -1;
            std::string nota;
            if (static_cast<int>(scelte[i]) == rif) { n = 0; nota = "reference input: the timecode comes with the signal"; }
            else if (err.empty()) nota = confronta(sr, stacchi_visti(in.luci()), giorno, n);
            else nota = "not measured";
            if (n >= 0) trovati.emplace_back(in.nome(), n);
            esito += std::string(i ? "," : "") + "{\"n\":" + std::to_string(scelte[i]) + ",\"porta\":" + json_testo(in.nome()) +
                     ",\"ritardo\":" + std::to_string(n) + ",\"nota\":" + json_testo(nota) + "}";
            std::printf("delay %s: %s\n", in.nome().c_str(), nota.c_str());
        }
        esito += "]}";
        if (!rimesso) std::printf("⚠ delay measurement: the ATEM was not confirmed as restored: %s\n", r.c_str());
        std::lock_guard<std::mutex> l(a.m);
        a.ritardi_esito = esito;
        a.ritardi_trovati = trovati;
        a.ritardi_nuovi = true;
        a.ritardi_in_corso = false;
    });
    return "{\"ok\":true,\"riferimento\":" + json_testo(a.ingressi[rif]->nome()) + "}";
}

// /mappa-ingressi: il tasto «Map inputs…» (sezione 9.78). mappa_porte.py porta ogni aux per un attimo sul nero
// e sulle barre e guarda quale porta aperta qui cambia, leggendo /misura da questo registratore. Scrive
// porte.json, che /stato rilegge a ogni richiesta: il menu delle porte si aggiorna da solo.
// ⚠ Scrive nell'ATEM: solo fuori diretta, e la pagina lo fa confermare.
std::string mappa_ingressi(App& a, const std::string& query, int& codice) {
    if (a.fase != ferma) { codice = 409; return "{\"errore\":\"the input map cannot be made while recording\"}"; }
    if (a.mappa_in_corso) { codice = 409; return "{\"errore\":\"the input map is already being made\"}"; }
    if (a.ritardi_in_corso) { codice = 409; return "{\"errore\":\"delay measurement in progress: wait until it restores the ATEM\"}"; }
    if (a.imp.atem.empty()) { codice = 409; return "{\"errore\":\"set the ATEM address first: the input map switches its aux outputs\"}"; }
    if (const std::string m = atem_muto(a); !m.empty()) { codice = 409; return "{\"errore\":" + json_testo(m) + "}"; }
    std::string aux = parametro_query(query, "aux");
    for (char c : aux)
        if (!(c >= '0' && c <= '9') && c != '-') { codice = 400; return "{\"errore\":\"aux range: a number or two joined by a dash, for example 9-12\"}"; }
    if (a.filo_mappa.joinable()) a.filo_mappa.join();
    a.mappa_in_corso = true;
    {
        std::lock_guard<std::mutex> l(a.m);
        a.mappa_esito = "null";
    }
    const std::string ferma_arg = argomento_ferma(FERMA_MAPPA);
    a.filo_mappa = std::thread([&a, aux, ferma_arg, ip = a.imp.atem] {
        std::printf("input map: started, ATEM %s%s\n", ip.c_str(), aux.empty() ? "" : (", aux " + aux).c_str());
        std::string e;
        const std::string r = Python::esegui("mappa_porte.py", "--nel-registratore --atem " + ip + (aux.empty() ? "" : " --aux " + aux) + ferma_arg, 300000, e);
        togli_ferma(FERMA_MAPPA);
        std::printf("%s", r.c_str());               // nel registro della sessione, riga per riga
        // L'esito è l'ultima riga che comincia con «{».
        std::string esito;
        for (size_t fine = r.size(); fine > 0;) {
            size_t inizio = r.rfind('\n', fine - 1);
            inizio = inizio == std::string::npos ? 0 : inizio + 1;
            std::string riga = r.substr(inizio, fine - inizio);
            while (!riga.empty() && (riga.back() == '\r' || riga.back() == '\n')) riga.pop_back();
            if (!riga.empty() && riga[0] == '{') { esito = riga; break; }
            fine = inizio > 0 ? inizio - 1 : 0;
        }
        if (esito.empty()) esito = "{\"ok\":false,\"errore\":" + json_testo(e.empty() ? "the input map script gave no result" : e) + "}";
        std::lock_guard<std::mutex> l(a.m);
        a.mappa_esito = esito;
        a.mappa_in_corso = false;
    });
    return "{\"ok\":true}";
}

// Le impostazioni le tocca solo il filo del server: i ritardi misurati si salvano qui.
void usa_ritardi(App& a) {
    if (!a.ritardi_nuovi.exchange(false)) return;
    std::vector<std::pair<std::string, int>> v;
    {
        std::lock_guard<std::mutex> l(a.m);
        v = a.ritardi_trovati;
    }
    for (const auto& [p, n] : v) a.imp.metti_ritardo(p, n);
    a.imp.salva();
    for (auto& in : a.ingressi) in->imposta_ritardo(a.imp.ritardo(in->nome()));
}

std::string nome_registrazione() {
    char b[64];
    const std::time_t t = std::time(nullptr);
    std::tm loc{};
    localtime_s(&loc, &t);
    std::strftime(b, sizeof b, "Recording %Y-%m-%d %H-%M", &loc);
    return b;
}

// /rec: la partenza in un filo suo, la pagina segue la fase in /stato.
std::string avvia_rec(App& a, const std::string& query, int& codice) {
    if (a.fase != ferma) { codice = 409; return "{\"errore\":\"already recording\"}"; }
    if (a.ritardi_in_corso) { codice = 409; return "{\"errore\":\"delay measurement in progress: the ATEM program is changing\"}"; }
    if (a.mappa_in_corso) { codice = 409; return "{\"errore\":\"the input map is being made: the ATEM aux outputs are changing\"}"; }
    if (a.lavoro.joinable()) a.lavoro.join();
    std::error_code ec;
    if (a.imp.cartella.empty() || !fs::is_directory(fs::u8path(a.imp.cartella), ec)) {
        codice = 409;
        return "{\"errore\":\"choose the destination\"}";
    }
    if (a.imp.nome.empty()) a.imp.nome = nome_registrazione();
    a.imp.salva();
    // Le righe da registrare: quelle armate nella pagina, fra le porte che si sono aperte.
    std::vector<bool> armata(a.ingressi.size(), true);
    std::vector<std::string> nomi_salvati(a.ingressi.size());
    if (const std::string s = parametro_query(query, "armate"); query.find("armate=") != std::string::npos) {
        armata.assign(a.ingressi.size(), false);
        for (size_t p = 0; p < s.size();) {
            const size_t f = std::min(s.find(',', p), s.size());
            const size_t n = static_cast<size_t>(std::atoi(s.substr(p, f - p).c_str()));
            if (f > p && n < armata.size()) armata[n] = true;
            p = f + 1;
        }
    } else if (!a.imp.tracce.empty()) {
        // Un REC chiamato senza «armate», cioè non dalla pagina: valgono le tracce salvate, mai tutte
        // le porte aperte (prova del 17 novembre, registrate 12 porte invece di 9).
        armata.assign(a.ingressi.size(), false);
        const std::string& t = a.imp.tracce;
        for (size_t p = 0; p < t.size();) {
            const size_t f = std::min(t.find('|', p), t.size());
            const std::string voce = t.substr(p, f - p);
            p = f + 1;
            if (voce.empty() || voce[0] == '-') continue;
            const size_t u = voce.find('=');
            const std::string porta = voce.substr(0, u);
            for (size_t k = 0; k < a.ingressi.size(); ++k)
                if (a.ingressi[k]->nome() == porta) {
                    armata[k] = true;
                    if (u != std::string::npos) nomi_salvati[k] = voce.substr(u + 1);
                }
        }
    }
    std::vector<Ingresso*> v;
    Registrazione::Parametri p;
    for (size_t k = 0; k < a.ingressi.size(); ++k) {
        if (!armata[k] || !a.errori_porte[k].empty()) continue;
        v.push_back(a.ingressi[k].get());
        std::string nome = parametro_query(query, "nome" + std::to_string(k));
        if (nome.empty()) nome = nomi_salvati[k];
        p.nomi.push_back(nome);
    }
    if (v.empty()) { codice = 409; return "{\"errore\":\"no track to record\"}"; }
    a.carico.azzera_statistica();
    p.cartella = testo_di(fs::u8path(a.imp.cartella) / fs::u8path(a.imp.nome));
    p.modo = a.imp.modo;
    if (const Formato* f = formato(a.imp.modo)) {
        p.pc.larghezza = f->larghezza;
        p.pc.altezza = f->altezza;
        p.pc.cad = f->cad;
        p.pc.interlacciato = f->interlacciato;
    }
    p.pc.mbit = a.imp.mbit;
    p.pc.canali_audio = a.imp.audio;
    p.pc.wav = true;               // Andrea, 30 ottobre: il Broadcast WAV va sempre scritto
    p.atem = a.imp.atem;
    p.hyperdeck = a.imp.hyperdeck;
    p.forza = parametro_query(query, "forza") == "1";
    // Il nome si consuma: la prossima registrazione ne vuole uno nuovo.
    a.imp.nome.clear();
    a.imp.salva();
    a.reg = std::make_unique<Registrazione>(v, a.orologio, a.marcatore.get());
    {
        std::lock_guard<std::mutex> l(a.m);
        a.errore_rec.clear();
        a.progetto.clear();
        a.progetto_stato = 0;
    }
    a.fase = partenza;
    a.lavoro = std::thread([&a, p] {
        std::string e;
        a.t_rec_us = adesso_us();
        if (a.reg->avvia(p, e)) {
            a.t_rec_us = adesso_us();
            a.fase = registra;
        } else {
            std::lock_guard<std::mutex> l(a.m);
            a.errore_rec = e;
            a.fase = ferma;
            std::printf("⚠ recording did not start: %s\n", e.c_str());
        }
    });
    return "{\"ok\":true,\"cartella\":" + json_testo(p.cartella) + "}";
}

std::string ferma_rec(App& a, int& codice) {
    if (a.fase != registra) { codice = 409; return "{\"errore\":\"not recording\"}"; }
    if (a.lavoro.joinable()) a.lavoro.join();
    a.fase = fine;
    a.lavoro = std::thread([&a] {
        a.reg->ferma();
        const std::string c = a.reg->cartella();
        scrivi_file((fs::u8path(c) / "carico.json").wstring(), a.carico.statistica_json() + "\n");
        std::error_code ec;
        std::lock_guard<std::mutex> l(a.m);
        a.progetto_stato = 0;
        if (!a.imp.resolve) a.progetto = "Resolve project not requested: use the Project button";
        else if (!fs::exists(fs::u8path(c) / "atem.jsonl", ec))
            a.progetto = "no ATEM log: the Resolve project cannot be built";
        else avvia_progetto(a, c);
        a.fase = ferma;
    });
    return "{\"ok\":true}";
}

bool rispondi(App& a, const std::string& percorso, const std::string& query, std::string& corpo, std::string& tipo, int& codice) {
    const std::string html = "text/html; charset=utf-8";
    if (percorso == "/") {
        corpo = pagina_file(a.da_controllare && !a.accettato ? L"primo-avvio.html" : L"registratore.html");
        tipo = html;
        return true;
    }
    if (percorso == "/registratore") { corpo = pagina_file(L"registratore.html"); tipo = html; return true; }
    if (percorso == "/primo-avvio") { corpo = pagina_file(L"primo-avvio.html"); tipo = html; return true; }
    if (percorso == "/analisi") {
        std::string j = a.analisi_avviata ? a.analisi.json() : profilo_salvato();
        if (j.empty()) j = "{\"in_corso\":false,\"mai\":true}";
        corpo = "{\"perche\":" + json_testo(a.perche) + ",\"da_controllare\":" + json_si(a.da_controllare) +
                ",\"accettato\":" + json_si(a.accettato) + "," + j.substr(j.find('{') + 1);
        return true;
    }
    if (percorso == "/analisi/avvia") {
        if (a.fase != ferma) { codice = 409; corpo = "{\"errore\":\"the system check cannot run while recording\"}"; return true; }
        a.analisi.avvia(a.imp);
        a.analisi_avviata = true;
        corpo = "{\"ok\":true}";
        return true;
    }
    if (percorso == "/apri") {          // dal controllo alla pagina di registrazione
        a.accettato = true;
        if (a.ingressi.empty() || a.modo_aperto.empty()) apri_porte(a);
        corpo = "{\"ok\":true}";
        return true;
    }
    if (percorso == "/impostazioni") {
        std::string errore;
        if (!query.empty()) {
            if (a.fase != ferma) { codice = 409; corpo = "{\"errore\":\"settings cannot be changed while recording\"}"; return true; }
            const std::string atem = a.imp.atem;
            a.imp.da_query(query, errore);
            a.imp.salva();
            if (a.imp.modo != a.modo_aperto || a.imp.audio != a.audio_aperto) apri_porte(a);
            for (auto& in : a.ingressi) in->imposta_ritardo(a.imp.ritardo(in->nome()));
            if (a.imp.atem != atem) {
                std::lock_guard<std::mutex> l(a.m);
                a.nomi_atem.clear();
                a.nomi_ora.clear();
            }
        }
        corpo = "{\"errore\":" + json_testo(errore) + ",\"impostazioni\":" + a.imp.json() + "}";
        return true;
    }
    // I setup con un nome: /setup elenca, ?salva=, ?carica=, ?cancella= (17 novembre). Mai durante la registrazione.
    if (percorso == "/setup") {
        std::string errore;
        const bool cambia = query.find("salva=") != std::string::npos || query.find("carica=") != std::string::npos ||
                            query.find("cancella=") != std::string::npos;
        if (cambia && a.fase != ferma) { codice = 409; corpo = "{\"errore\":\"setups cannot be changed while recording\"}"; return true; }
        if (query.find("salva=") != std::string::npos) errore = setup_salva(parametro_query(query, "salva"), a.imp);
        else if (query.find("cancella=") != std::string::npos) errore = setup_cancella(parametro_query(query, "cancella"), a.imp);
        else if (query.find("carica=") != std::string::npos) {
            const std::string atem = a.imp.atem;
            errore = setup_carica(parametro_query(query, "carica"), a.imp);
            if (errore.empty()) {
                if (a.imp.modo != a.modo_aperto || a.imp.audio != a.audio_aperto) apri_porte(a);
                for (auto& in : a.ingressi) in->imposta_ritardo(a.imp.ritardo(in->nome()));
                if (a.imp.atem != atem) {
                    std::lock_guard<std::mutex> l(a.m);
                    a.nomi_atem.clear();
                    a.nomi_ora.clear();
                }
            }
        }
        corpo = "{\"errore\":" + json_testo(errore) + ",\"elenco\":" + setup_elenco_json() + ",\"impostazioni\":" + a.imp.json() + "}";
        return true;
    }
    if (percorso == "/dischi") { corpo = dischi_json(); return true; }
    if (percorso == "/cartelle") { corpo = cartelle_json(parametro_query(query, "percorso")); return true; }
    if (percorso == "/crea") {
        std::error_code ec;
        const std::string c = parametro_query(query, "percorso");
        fs::create_directories(fs::u8path(c), ec);
        corpo = ec ? "{\"errore\":" + json_testo("cannot create the folder: " + ec.message()) + "}" : "{\"ok\":true}";
        return true;
    }
    if (percorso == "/misura-disco") {
        const std::string c = parametro_query(query, "cartella");
        if (!c.empty() && !a.misura_in_corso) {
            if (a.fase != ferma) { codice = 409; corpo = "{\"errore\":\"the drive cannot be measured while recording\"}"; return true; }
            if (a.filo_misura.joinable()) a.filo_misura.join();
            a.misura_in_corso = true;
            a.misura_cartella = c;
            a.misura_mb_s = -1;
            a.filo_misura = std::thread([&a, c] {
                const double v = misura_disco(c);
                a.misura_mb_s = v;
                a.disco_rileggi = true;        // la velocità del disco scelto si rilegge
                a.misura_in_corso = false;
            });
        }
        corpo = "{\"in_corso\":" + std::string(json_si(a.misura_in_corso)) + ",\"mb_s\":" + json_num(a.misura_mb_s) +
                ",\"cartella\":" + json_testo(a.misura_cartella) + "}";
        return true;
    }
    if (percorso == "/livelli") {
        // La storia dei livelli di ogni porta dal REC: dBFS di picco, uno ogni PASSO_STORIA fotogrammi.
        // ?da=N salta i primi N valori.
        size_t da = 0;
        try { da = static_cast<size_t>(std::stoul(parametro_query(query, "da"))); } catch (...) {}
        std::string j = "{\"passo_s\":" + json_num(Ingresso::PASSO_STORIA / 25.0, 2) + ",\"ingressi\":[";
        for (size_t k = 0; k < a.ingressi.size(); ++k) {
            const std::vector<float> s = a.ingressi[k]->storia(da);
            j += std::string(k ? "," : "") + "{\"n\":" + std::to_string(k) + ",\"db\":[";
            for (size_t q = 0; q < s.size(); ++q) j += (q ? "," : "") + json_num(dbfs(s[q]), 1);
            j += "]}";
        }
        corpo = j + "]}";
        tipo = "application/json; charset=utf-8";
        return true;
    }
    if (percorso == "/nomi") {
        if (parametro_query(query, "leggi") == "1") leggi_nomi(a);
        std::lock_guard<std::mutex> l(a.m);
        corpo = "{\"in_corso\":" + std::string(json_si(a.nomi_in_corso)) + ",\"ora\":" + json_testo(a.nomi_ora) +
                ",\"errore\":" + json_testo(a.nomi_errore) + ",\"atem\":" + (a.nomi_atem.empty() ? "{}" : a.nomi_atem) + "}";
        return true;
    }
    if (percorso == "/cerca") {
        if (a.fase != ferma) { codice = 409; corpo = "{\"errore\":\"network search is not available while recording\"}"; return true; }
        cerca(a);
        corpo = "{\"ok\":true}";
        return true;
    }
    if (percorso == "/misura-ritardi") { corpo = misura_ritardi(a, query, codice); return true; }
    if (percorso == "/mappa-ingressi") { corpo = mappa_ingressi(a, query, codice); return true; }
    if (percorso == "/ferma-atem") {    // il tasto Stop di Map e Measure: lo script rimette l'ATEM ed esce
        corpo = ferma_scritture_atem(a) ? "{\"ok\":true}" : "{\"ok\":true,\"niente\":true}";
        return true;
    }
    if (percorso == "/rec") { corpo = avvia_rec(a, query, codice); return true; }
    if (percorso == "/stop") { corpo = ferma_rec(a, codice); return true; }
    if (percorso == "/progetto") {      // il tasto PROGETTO: rifà il progetto dell'ultima registrazione
        std::error_code ec;
        if (a.fase != ferma) { codice = 409; corpo = "{\"errore\":\"the project can be built only when recording is stopped\"}"; return true; }
        if (!a.reg) { codice = 409; corpo = "{\"errore\":\"no recording to build it from\"}"; return true; }
        const std::string c = a.reg->cartella();
        if (!fs::exists(fs::u8path(c) / "atem.jsonl", ec)) {
            codice = 409; corpo = "{\"errore\":\"no ATEM log: the Resolve project cannot be built\"}"; return true;
        }
        std::lock_guard<std::mutex> l(a.m);
        guarda_progetto(a);
        if (a.progetto_stato == 1) { codice = 409; corpo = "{\"errore\":\"the project is already being built\"}"; return true; }
        avvia_progetto(a, c);
        corpo = "{\"ok\":true}";
        return true;
    }
    // Le clip degli HyperDeck copiate dalle schede in una cartella (sezione 9.72, Andrea 17 novembre): elenco con
    // la camera proposta, poi il progetto con le clip confermate. Una registrazione qualunque, non solo l'ultima.
    if (percorso == "/hyperdeck/elenco" || percorso == "/hyperdeck/usa") {
        std::error_code ec;
        const std::string reg = parametro_query(query, "registrazione"), clip = parametro_query(query, "cartella");
        if (reg.empty() || !fs::exists(fs::u8path(reg) / "atem.jsonl", ec)) {
            codice = 409; corpo = "{\"errore\":\"the recording folder has no ATEM log\"}"; return true;
        }
        if (clip.empty() || !fs::is_directory(fs::u8path(clip), ec)) {
            codice = 409; corpo = "{\"errore\":\"HyperDeck clip folder not found\"}"; return true;
        }
        if (reg.find('"') != std::string::npos || clip.find('"') != std::string::npos) {
            codice = 409; corpo = "{\"errore\":\"invalid folder name\"}"; return true;
        }
        if (percorso == "/hyperdeck/elenco") {
            std::string e;
            const std::string r = Python::esegui("clip_hyperdeck.py", "--registrazione \"" + reg + "\" --cartella \"" + clip + "\"", 120000, e);
            const size_t k = r.find('{');
            if (!e.empty() || k == std::string::npos) {
                codice = 500; corpo = "{\"errore\":" + json_testo(e.empty() ? "no answer from clip_hyperdeck.py" : e) + "}"; return true;
            }
            corpo = r.substr(k);
            return true;
        }
        if (a.fase != ferma) { codice = 409; corpo = "{\"errore\":\"the project can be built only when recording is stopped\"}"; return true; }
        const std::string abb = parametro_query(query, "abbinamenti");
        for (unsigned char c : abb)      // le barre rovesciate sì: le clip stanno anche in sottocartelle, «Deck 2\clip.mov»
            if (c < 0x20 || c == '"') { codice = 409; corpo = "{\"errore\":\"invalid assignments\"}"; return true; }
        std::lock_guard<std::mutex> l(a.m);
        guarda_progetto(a);
        if (a.progetto_stato == 1) { codice = 409; corpo = "{\"errore\":\"the project is already being built\"}"; return true; }
        const std::wstring f = (fs::u8path(reg) / "hyperdeck.json").wstring();
        if (abb.empty()) fs::remove(fs::path(f), ec);
        else if (!scrivi_file(f, "{\"cartella\": " + json_testo(clip) + ", \"abbinamenti\": " + json_testo(abb) + "}\n")) {
            codice = 500; corpo = "{\"errore\":\"could not write hyperdeck.json in the recording folder\"}"; return true;
        }
        avvia_progetto(a, reg);
        corpo = "{\"ok\":true}";
        return true;
    }
    // L'avviso della versione di prova: si accetta una volta per versione, in un file suo e non nelle
    // impostazioni, perché non deve viaggiare coi setup.
    // Quello che manca da fuori (sezione 9.77): la pagina lo propone, il download si apre nel browser predefinito
    // sulla pagina ufficiale. Mai un indirizzo arrivato dalla richiesta: solo gli id della lista in macchina.cpp.
    if (percorso == "/prerequisiti") {
        const std::wstring f = cartella_utente() + L"\\prerequisites skipped.txt";
        if (const std::string id = parametro_query(query, "apri"); !id.empty()) {
            const std::string l = link_prerequisito(id);
            if (l.empty()) { codice = 404; corpo = "{\"errore\":\"unknown item\"}"; return true; }
            ShellExecuteA(nullptr, "open", l.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (const std::string id = parametro_query(query, "salta"); !id.empty() && !link_prerequisito(id).empty())
            scrivi_file(f, leggi_file(f) + id + "\n");
        std::string s = leggi_file(f), saltati;
        for (size_t p = 0; p < s.size();) {
            const size_t e = std::min(s.find('\n', p), s.size());
            std::string r = s.substr(p, e - p);
            while (!r.empty() && r.back() == '\r') r.pop_back();
            if (!r.empty()) saltati += std::string(saltati.empty() ? "" : ",") + json_testo(r);
            p = e + 1;
        }
        corpo = prerequisiti_json();
        corpo.insert(corpo.size() - 1, ",\"saltati\":[" + saltati + "]");
        return true;
    }
    // Il manuale d'uso in PDF, accanto al programma (sezione 9.77): lo apre il lettore PDF della macchina.
    if (percorso == "/manuale") {
        const std::wstring m = cartella_programma() + L"\\Multicam Timecode Sync Recorder User Manual.pdf";
        if (GetFileAttributesW(m.c_str()) == INVALID_FILE_ATTRIBUTES) {
            codice = 404;
            corpo = "{\"errore\":\"the manual is missing from the program folder: reinstall the recorder\"}";
            return true;
        }
        const bool ok = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", m.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
        corpo = ok ? "{\"ok\":true}" : "{\"errore\":\"no program on this computer opens PDF files\"}";
        return true;
    }
    if (percorso == "/beta") {
        const std::wstring f = cartella_utente() + L"\\beta accepted.txt";
        if (parametro_query(query, "accetta") == "1") scrivi_file(f, std::string(WINISO_VERSIONE) + "\n");
        std::string v = leggi_file(f);
        while (!v.empty() && (v.back() == '\n' || v.back() == '\r')) v.pop_back();
        corpo = "{\"versione\":" + json_testo(WINISO_VERSIONE) + ",\"accettata\":" + json_si(v == WINISO_VERSIONE) + "}";
        return true;
    }
    // Il rapporto di prova per i beta tester (sezioni 9.68, 9.73 e 9.84): l'anteprima mostra il testo, «crea» scrive
    // lo zip sul Desktop e apre la cartella col file selezionato. Dalla 0.9 beta 8 non apre più la mail: il tester
    // lo manda come vuole, per mail o dalla pagina Filemail, che si apre solo col tasto («/rapporto/portale»).
    // Il programma non manda niente da solo.
    if (percorso == "/rapporto/portale") {
        ShellExecuteA(nullptr, "open", WINISO_PORTALE_RAPPORTI, nullptr, nullptr, SW_SHOWNORMAL);
        corpo = "{\"ok\":true}";
        return true;
    }
    if (percorso == "/rapporto/anteprima" || percorso == "/rapporto/crea") {
        const std::string nota = parametro_query(query, "nota");
        const std::wstring fn = cartella_utente() + L"\\nota rapporto.txt";
        std::error_code ec;
        if (nota.empty()) fs::remove(fs::path(fn), ec);
        else scrivi_file(fn, nota.substr(0, 8000));
        std::string ultima;
        {
            std::lock_guard<std::mutex> l(a.m);
            if (a.reg) ultima = a.reg->cartella();
        }
        if (ultima.find('"') != std::string::npos) ultima.clear();
        std::string arg = "--versione \"" + std::string(WINISO_VERSIONE) + "\"";
        if (!ultima.empty()) arg += " --registrazione \"" + ultima + "\"";
        // Il Desktop vero dell'utente, anche se spostato in OneDrive; se Windows non lo dà, la cartella di prima.
        std::string uscita = testo_di(fs::path(cartella_utente()) / "test reports");
        PWSTR desktop = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) && desktop) uscita = testo_di(fs::path(desktop));
        CoTaskMemFree(desktop);
        if (uscita.find('"') != std::string::npos) uscita = testo_di(fs::path(cartella_utente()) / "test reports");
        if (percorso == "/rapporto/crea") arg += " --uscita \"" + uscita + "\"";
        std::string e;
        const std::string r = Python::esegui("rapporto_prova.py", arg, 180000, e);
        const size_t k = r.find('{');
        if (!e.empty() || k == std::string::npos) {
            codice = 500; corpo = "{\"errore\":" + json_testo(e.empty() ? "no answer from rapporto_prova.py" : e) + "}"; return true;
        }
        corpo = r.substr(k);
        while (!corpo.empty() && corpo.back() != '}') corpo.pop_back();   // Python chiude con \r\n
        if (percorso == "/rapporto/crea") {
            const std::string file = json_valore(corpo, "file"), nome = json_valore(corpo, "nome");
            if (!file.empty()) {
                const std::wstring sel = L"/select,\"" + fs::u8path(file).wstring() + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
            }
            const std::string oggetto = "Multicam Timecode Sync Recorder test report " + std::string(WINISO_VERSIONE);
            corpo.insert(corpo.size() - 1, ",\"indirizzo\":" + json_testo(WINISO_EMAIL_RAPPORTI) +
                                           ",\"portale\":" + json_testo(WINISO_PORTALE_RAPPORTI) + ",\"oggetto\":" + json_testo(oggetto));
        }
        return true;
    }
    if (percorso == "/esci") {
        if (a.fase != ferma) { codice = 409; corpo = "{\"errore\":\"stop the recording first\"}"; return true; }
        if (a.mappa_in_corso || a.ritardi_in_corso) { codice = 409; corpo = "{\"errore\":\"wait until the ATEM is restored\"}"; return true; }
        a.esci = true;
        corpo = "{\"ok\":true}";
        return true;
    }
    const size_t n = static_cast<size_t>(std::atoi(parametro_query(query, "n").c_str()));
    if (percorso == "/miniatura" && n < a.ingressi.size() && a.errori_porte[n].empty()) {
        corpo = a.ingressi[n]->miniatura().jpeg();
        tipo = "image/jpeg";
        return !corpo.empty();
    }
    if (percorso == "/misura" && n < a.ingressi.size() && a.errori_porte[n].empty()) {
        corpo = a.ingressi[n]->miniatura().misura();
        return true;
    }
    return false;
}

}   // namespace

int interfaccia(bool controllo, Apertura apertura) {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { std::printf("COM initialisation failed\n"); return 1; }
    auto a = std::make_unique<App>();
    a->imp.carica();
    a->da_controllare = controllo_da_rifare(a->perche) || controllo;
    if (controllo && a->perche.empty()) a->perche = "check requested";
    a->porte = porte_decklink();
    for (const Porta& p : a->porte)
        if (p.ingresso) a->porte_ingresso.push_back(&p);
    a->marcatore = std::make_unique<Marcatore>("", [o = &a->orologio] { return o->cadenza(); }, [] { return -1; });
    a->marcatore->stato = [&] { return stato_json(*a); };
    // Il server serve una richiesta alla volta: le pagine e i comandi rispondono subito, quello che
    // dura (REC, STOP, misure, nomi dall'ATEM) va in un filo suo e la pagina ne segue lo stato.
    a->marcatore->pagine = [&](const std::string& percorso, const std::string& query, std::string& corpo, std::string& tipo) {
        int codice = 200;
        const bool ok = rispondi(*a, percorso, query, corpo, tipo, codice);
        if (codice != 200) corpo = corpo.empty() ? "{\"errore\":\"no\"}" : corpo;
        return ok;
    };
    if (a->da_controllare) {
        std::printf("system check: %s\n", a->perche.c_str());
        a->analisi.avvia(a->imp);
        a->analisi_avviata = true;
    } else {
        a->accettato = true;
        apri_porte(*a);
    }
    // Indirizzi non scritti: si cercano subito, così chi apre il registratore trova già l'ATEM.
    if (a->imp.atem.empty() || a->imp.hyperdeck.empty()) cerca(*a);
    else leggi_nomi(*a);
    std::string e;
    if (!a->marcatore->ascolta(a->imp.porta, e)) {
        std::printf("⚠ %s: is another recorder already open?\n", e.c_str());
        // Col doppio clic la console si chiude subito: il motivo va detto in una finestra.
        if (apertura == Apertura::finestra)
            MessageBoxW(nullptr, L"The recorder cannot start: port 8811 is already in use.\n\nAnother Multicam Timecode Sync Recorder is probably already open: "
                                 L"close it, or use that one.", L"Multicam Timecode Sync Recorder", MB_OK | MB_ICONWARNING);
        a->marcatore.reset();
        a.reset();
        CoUninitialize();
        return 1;
    }
    // Le registrazioni rimaste a metà nella destinazione si riparano da sole, in un filo suo (vedi ripara.h).
    // Solo qui, dopo la porta: un secondo registratore aperto per sbaglio esce sopra senza toccare niente.
    a->filo_ripara = std::thread([d = a->imp.cartella, p = a.get()] {
        const int n = ripara_interrotte(d, &p->esci);
        if (n > 0) std::printf("interrupted recordings repaired at start: %d\n", n);
    });
    const std::string url = "http://localhost:" + std::to_string(a->imp.porta) + "/";
    std::printf("mtsrec: the page is %s\n"
                "while recording, Enter adds a marker, optionally preceded by text (\"red applause\"); "
                "type quit and Enter to close\n", url.c_str());
    Finestra finestra;
    if (apertura == Apertura::browser) ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (apertura == Apertura::finestra) {
        finestra.apri(url, cartella_utente() + L"\\finestra", [&a]() -> Finestra::Blocco {
            if (a->fase != ferma) return {"Recording in progress: press STOP before closing the recorder.", false};
            // Mappa e ritardi si possono fermare: la finestra chiede, e se sì lo script rimette l'ATEM ed esce
            // prima che il registratore si chiuda (24 novembre 2026: prima si poteva solo aspettare).
            if (a->ritardi_in_corso) return {"Delay measurement in progress: it is switching the ATEM program and aux outputs.", true};
            if (a->mappa_in_corso) return {"Input map in progress: it is switching the ATEM aux outputs.", true};
            return {};
        }, [&a] { ferma_scritture_atem(*a); });
        // Aperto col doppio clic la console è solo sua e si nasconde; da un terminale resta.
        DWORD processi[2];
        if (GetConsoleProcessList(processi, 2) == 1) ShowWindow(GetConsoleWindow(), SW_HIDE);
    }

    // La tastiera: marker durante la registrazione, «esci» per chiudere. Il filo resta fermo su
    // ReadFile anche dopo la fine, quindi tiene solo lo stato condiviso. Mai std::cin: vedi registro.h.
    struct Tastiera { std::atomic<bool> esci{false}, finito{false}; std::mutex m; std::vector<std::string> righe; };
    auto tast = std::make_shared<Tastiera>();
    std::thread([tast] {
        std::string s;
        while (!tast->finito && riga_da_tastiera(s)) {
            while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
            if (s == "esci" || s == "quit" || s == "exit" || s == "q") { tast->esci = true; continue; }
            std::lock_guard<std::mutex> l(tast->m);
            tast->righe.push_back(s);
        }
    }).detach();

    while (!a->esci) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (finestra.chiusa()) a->esci = true;            // chiusa da chi la usa: lei ha già chiesto se si può
        if (tast->esci) {
            tast->esci = false;
            if (a->fase == ferma) a->esci = true;
            else std::printf("recording in progress: press STOP on the page first\n");
        }
        std::vector<std::string> righe;
        {
            std::lock_guard<std::mutex> l(tast->m);
            righe.swap(tast->righe);
        }
        for (const std::string& r : righe) {
            if (a->marcatore->attivo()) a->marcatore->da_tastiera(r);
            else std::printf("not recording: no marker added\n");
        }
    }
    tast->finito = true;
    std::printf("closing\n");
    finestra.chiudi();
    if (a->lavoro.joinable()) a->lavoro.join();
    if (a->filo_nomi.joinable()) a->filo_nomi.join();
    if (a->filo_formato.joinable()) a->filo_formato.join();
    if (a->filo_misura.joinable()) a->filo_misura.join();
    if (a->filo_cerca.joinable()) a->filo_cerca.join();
    if (a->filo_ritardi.joinable()) a->filo_ritardi.join();
    if (a->filo_mappa.joinable()) a->filo_mappa.join();
    if (a->filo_ripara.joinable()) a->filo_ripara.join();
    a->marcatore->ferma();
    a->reg.reset();
    for (auto& in : a->ingressi) in->ferma();
    a->ingressi.clear();
    for (auto& p : a->porte) p.scheda->Release();
    a.reset();
    CoUninitialize();
    return 0;
}
