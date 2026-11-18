// Multicam Timecode Sync Recorder, forma corta mtsrec (sezioni 9.50 e 10).
//
//   mtsrec                                  la pagina: controllo della macchina e registrazione
//   mtsrec --controllo                      la pagina, rifacendo il controllo della macchina
//   mtsrec --browser                        la pagina nel browser invece che nella finestra del registratore
//   mtsrec --senza-browser                  la pagina senza aprire niente: si apre a mano
//   mtsrec --elenco                         porte DeckLink e codificatore DNxHD, nella console
//   mtsrec --sonda FILE.mov                 timecode, fotogrammi e audio del file in JSON, al posto di ffprobe
//   mtsrec --porte "Duo 2.1 (4)" --secondi 60 --cartella "D:\Recordings\test"
//
// Opzioni della riga di comando, per le prove e per chi registra senza pagina:
//   --porte "a;b;c"   porte da registrare, basta un pezzo del nome
//   --copie N         ogni porta scritta in N file: la prova di carico con un cavo solo
//   --modo            1080p25 (predefinito) o 2160p25
//   --mbit            DNxHD 36, 120 o 185 (predefinito); in 2160p25 DNxHR 728 (HQ), 481 (SQ), 149 (LB)
//   --secondi N       durata; senza, si ferma con Invio
//   --cartella        dove scrivere MOV, CSV e rapporto
//   --audio N         canali audio dall'SDI: 2 (predefinito), 8 o 16. Mai meno di 2, deciso da Andrea il 31 ottobre
//   --senza-wav       niente Broadcast WAV: di base ogni traccia ne scrive uno accanto al MOV, col timecode nel bext
//   --atem IP         registro dei tagli dall'ATEM in rete, sola lettura: atem.jsonl nella cartella
//   --hyperdeck IP    l'HyperDeck registra insieme: parte prima delle tracce e si ferma dopo
//   --controlli       solo i controlli prima del REC, con le misure di disco e processore, e poi esce
//   --tracce N        quante tracce per --controlli senza --porte (predefinito 10, deciso da Andrea)
//   --secondi-disco N quanto dura la scrittura di prova di --controlli (predefinito 60)
//   --forza           registra anche col semaforo rosso
//   --porta-marker N  porta HTTP per i marker da Companion (predefinita 8811, 0 la spegne)
//   --ripara CARTELLA ripara una registrazione interrotta: MOV e WAV tagliati all'ultimo fotogramma intero
//   --anteprima       apre le porte senza registrare: la pagina con le finestrelle, e la misura
//                     dell'immagine per script\mappa_porte.py. Senza --porte tutte quelle che ricevono
//
// Non registra mai in una cartella che ha già dei MOV. Se la cartella ha ancora il segno di una
// registrazione cominciata e non finita, prima la ripara (sezione 9.57).
//
// Durante la registrazione: Invio mette un marker, «rosso applausi» e Invio un marker rosso col testo,
// «stop» e Invio ferma. In rete: GET /marker?testo=…&colore=…&durata=… e GET /stato.
//
// Prima di ogni registrazione i controlli rapidi: sistema e spazio libero. Col rosso non si parte.
#include <windows.h>
#include <comdef.h>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "registro.h"
#include "DeckLinkAPI_h.h"
#include "controlli.h"
#include "esterni.h"
#include "formato.h"
#include "ingresso.h"
#include "interfaccia.h"
#include "macchina.h"
#include "marker.h"
#include "pagina.h"
#include "registrazione.h"
#include "ripara.h"
#include "sonda.h"
#include "traccia.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

namespace fs = std::filesystem;

static std::string minuscolo(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Uso del processore di tutta la macchina dall'ultima chiamata, in percentuale.
static double processore() {
    static ULONGLONG ri = 0, rk = 0, ru = 0;
    FILETIME i, k, u;
    GetSystemTimes(&i, &k, &u);
    auto v = [](FILETIME f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    const ULONGLONG di = v(i) - ri, dk = v(k) - rk, du = v(u) - ru;   // il kernel comprende l'inattività
    ri = v(i); rk = v(k); ru = v(u);
    return dk + du ? 100.0 * static_cast<double>(dk + du - di) / static_cast<double>(dk + du) : 0.0;
}

// Gli ingressi per /stato: nome, segnale, timecode della sorgente, quante tracce scrive.
static std::string ingressi_json(const std::vector<std::unique_ptr<Ingresso>>& ingressi, const Cadenza& cad, int tracce) {
    std::string r;
    for (size_t k = 0; k < ingressi.size(); ++k)
        r += std::string(k ? "," : "") + "{\"nome\":\"" + ingressi[k]->nome() + "\",\"segnale\":" +
             (ingressi[k]->segnale() ? "true" : "false") + ",\"tc\":\"" +
             (ingressi[k]->tc() >= 0 ? tc_testo(ingressi[k]->tc(), cad) : "--") + "\",\"tracce\":" +
             std::to_string(tracce) + "}";
    return "[" + r + "]";
}

// La pagina con le finestrelle, le finestrelle (/miniatura?n=<ingresso>) e la misura dell'immagine
// (/misura?n=<ingresso>) che usa la mappa delle porte.
static bool pagina_ingressi(std::vector<std::unique_ptr<Ingresso>>& ingressi, const std::string& percorso,
                            const std::string& query, std::string& corpo, std::string& tipo) {
    if (percorso == "/") {
        corpo = pagina_registratore();
        tipo = "text/html; charset=utf-8";
        return true;
    }
    const size_t n = static_cast<size_t>(std::atoi(query.c_str() + (query.rfind("n=", 0) == 0 ? 2 : 0)));
    if (n >= ingressi.size()) return false;
    if (percorso == "/miniatura") {
        corpo = ingressi[n]->miniatura().jpeg();
        tipo = "image/jpeg";
        return !corpo.empty();
    }
    if (percorso == "/misura") {
        corpo = ingressi[n]->miniatura().misura();
        return true;
    }
    return false;
}

// --prova-cadenze: i conti del timecode di tutti i formati (sezione 9.79). Ogni fotogramma delle 24 ore
// va in etichetta e torna, le etichette che il drop-frame salta non si leggono, i valori noti SMPTE 12M
// tornano, e l'audio a 48 kHz non scivola mai. Restituisce 0 se va tutto bene.
static int prova_cadenze() {
    int errori = 0;
    auto atteso = [&errori](bool ok, const std::string& cosa) {
        if (!ok) { ++errori; std::printf("  FAILED: %s\n", cosa.c_str()); }
    };
    for (const Formato& f : formati()) {
        const Cadenza& c = f.cad;
        const int g = c.giorno();
        int sbagliati = 0;
        for (int k = 0; k < g; ++k) {
            const std::string t = c.testo(k);
            if (c.da_testo(t) != k) { if (++sbagliati <= 3) std::printf("  %s: %d -> %s -> %d\n", f.nome, k, t.c_str(), c.da_testo(t)); }
        }
        atteso(sbagliati == 0, std::string(f.nome) + ": round trip, " + std::to_string(sbagliati) + " wrong");
        atteso(c.testo(g) == c.testo(0), std::string(f.nome) + ": midnight wraps");
        // in 1001 secondi l'audio è un numero intero di campioni e i fotogrammi tornano
        int64_t somma = 0;
        const int64_t n = static_cast<int64_t>(c.num) * 1001 / c.den;
        for (int64_t k = 0; k < n; ++k) somma += c.campioni(k);
        atteso(somma == 48000LL * 1001, std::string(f.nome) + ": audio in 1001 s = " + std::to_string(somma));
        int mx = 0;
        for (int64_t k = 0; k < 5000; ++k) mx = std::max(mx, c.campioni(k));
        atteso(mx == c.campioni_max(), std::string(f.nome) + ": campioni_max");
        atteso(c.campioni_da_mezzanotte(1000) == 1000LL * 48000 * c.den / c.num, std::string(f.nome) + ": bext");
        std::printf("  %-11s base %d, %d/%d, %s, day %d frames, %d-%d samples per frame, %s\n", f.nome, c.base, c.num, c.den,
                    c.drop ? "drop-frame" : "non-drop", g, c.campioni(1) < c.campioni_max() ? c.campioni(1) : c.campioni_max(),
                    c.campioni_max(), c.testo(36000 * c.base - 1).c_str());
    }
    // Valori noti del drop-frame (SMPTE 12M): a 29,97 il minuto 1 comincia da ;02, il 10 da ;00.
    const Cadenza df30 = formato("1080p29.97")->cad, df60 = formato("1080p59.94")->cad;
    atteso(df30.giorno() == 2589408, "29.97 DF day = 2589408");
    atteso(df60.giorno() == 5178816, "59.94 DF day = 5178816");
    atteso(df30.testo(1799) == "00:00:59;29", "29.97 DF 1799 = 00:00:59;29, is " + df30.testo(1799));
    atteso(df30.testo(1800) == "00:01:00;02", "29.97 DF 1800 = 00:01:00;02, is " + df30.testo(1800));
    atteso(df30.testo(17982) == "00:10:00;00", "29.97 DF 17982 = 00:10:00;00, is " + df30.testo(17982));
    atteso(df30.testo(107892) == "01:00:00;00", "29.97 DF 107892 = 01:00:00;00, is " + df30.testo(107892));
    atteso(df60.testo(3600) == "00:01:00;04", "59.94 DF 3600 = 00:01:00;04, is " + df60.testo(3600));
    atteso(df60.testo(215784) == "01:00:00;00", "59.94 DF 215784 = 01:00:00;00, is " + df60.testo(215784));
    atteso(df30.da_testo("00:01:00;00") == -1 && df30.da_testo("00:01:00;01") == -1, "29.97 DF skipped labels refused");
    atteso(df30.da_testo("00:10:00;00") == 17982, "29.97 DF 00:10:00;00 is a real label");
    atteso(df60.da_testo("00:02:00;03") == -1, "59.94 DF 00:02:00;03 refused");
    // un'ora di timecode drop-frame è un'ora vera a meno di 3,6 ms
    atteso(std::abs(107892 / df30.fps() - 3600.0) < 0.004, "29.97 DF hour = real hour");
    // 29,97 audio: cinque fotogrammi, 8008 campioni, mai di più né di meno
    atteso(df30.campioni(0) + df30.campioni(1) + df30.campioni(2) + df30.campioni(3) + df30.campioni(4) == 8008, "29.97 audio 8008");
    // codec per formato
    atteso(codec_del_formato("1080p50") == std::vector<int>{365, 240, 75}, "1080p50 codecs");
    atteso(codec_del_formato("1080i59.94") == std::vector<int>{220, 145}, "1080i59.94 codecs");
    atteso(codec_per("1080p50", 185) == 365 && codec_per("1080p25", 440) == 185 && codec_per("1080i50", 36) == 120, "codec_per");
    atteso(codec_di_prova("1080p59.94", 290) == 120 && codec_di_prova("2160p50", 149) == 149, "codec_di_prova");
    atteso(formato_del_segnale("1080p5994") && std::string(formato_del_segnale("1080p5994")->nome) == "1080p59.94", "signal 1080p5994");
    atteso(formato_del_segnale("1080i59.94") && std::string(formato_del_segnale("1080i59.94")->nome) == "1080i59.94", "signal 1080i59.94");
    atteso(formato_del_segnale("2160p25") && std::string(formato_del_segnale("2160p25")->nome) == "2160p25", "signal 2160p25");
    atteso(formato_del_segnale("1080p60") && std::string(formato_del_segnale("1080p60")->nome) == "1080p60", "signal 1080p60");
    atteso(!formato_del_segnale("720p50"), "signal 720p50 not recorded");
    std::printf(errori ? "%d checks FAILED\n" : "all checks passed\n", errori);
    return errori ? 1 : 0;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Senza buffer: quando l'uscita va in una pipe (mappa_porte.py, ritardi_porte.py) le righe devono
    // arrivare subito, non alla chiusura. Trovato il 15 novembre 2026: la mappa aspettava 20 s e cadeva.
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // --sonda prima del registro: la chiama lo script del progetto una volta per file, e non è una sessione (9.81).
    // Il percorso si prende in UTF-16 dalla riga di comando, perché argv perde i caratteri fuori dalla code page.
    if (argc == 3 && std::string(argv[1]) == "--sonda") {
        int n = 0;
        LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
        const int r = (w && n == 3) ? sonda(w[2]) : 2;
        LocalFree(w);
        return r;
    }
    avvia_registro();   // stdout e stderr anche su file, per il rapporto di prova (sezione 9.77)
    togli_collegamenti_vecchi();
    std::vector<std::string> nomi;
    int copie = 1, mbit = 185, secondi = 0, audio = 2, tracce_prova = 10, secondi_disco = 60, porta_marker = 8811;   // Andrea, 30 ottobre: sempre 10 tracce
    bool wav = true;              // Andrea, 30 ottobre: il Broadcast WAV va sempre scritto
    bool solo_controlli = false, forza = false, anteprima = false, elenco = false;
    std::string modo = "1080p25", cartella, atem, hyperdeck, da_riparare;
    // Senza argomenti, o solo con --controllo, --browser e --senza-browser, il registratore si comanda dalla pagina (sezione 10).
    {
        bool solo_pagina = true, controllo = false;
        Apertura apertura = Apertura::finestra;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            if (a == "--controllo") controllo = true;
            else if (a == "--browser") apertura = Apertura::browser;
            else if (a == "--senza-browser") apertura = Apertura::niente;
            else solo_pagina = false;
        }
        if (solo_pagina) return interfaccia(controllo, apertura);
    }
    if (argc == 2 && std::string(argv[1]) == "--prova-cadenze") return prova_cadenze();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--porte") {
            std::string s = v;
            for (size_t k; (k = s.find(';')) != std::string::npos; s.erase(0, k + 1)) nomi.push_back(s.substr(0, k));
            if (!s.empty()) nomi.push_back(s);
            ++i;
        } else if (a == "--copie") { copie = std::atoi(v); ++i; }
        else if (a == "--mbit") { mbit = std::atoi(v); ++i; }
        else if (a == "--secondi") { secondi = std::atoi(v); ++i; }
        else if (a == "--modo") { modo = v; ++i; }
        else if (a == "--cartella") { cartella = v; ++i; }
        else if (a == "--atem") { atem = v; ++i; }
        else if (a == "--audio") { audio = std::atoi(v); ++i; }
        else if (a == "--senza-wav") { wav = false; }
        else if (a == "--hyperdeck") { hyperdeck = v; ++i; }
        else if (a == "--controlli") { solo_controlli = true; }
        else if (a == "--tracce") { tracce_prova = std::atoi(v); ++i; }
        else if (a == "--secondi-disco") { secondi_disco = std::atoi(v); ++i; }
        else if (a == "--forza") { forza = true; }
        else if (a == "--porta-marker") { porta_marker = std::atoi(v); ++i; }
        else if (a == "--ripara") { da_riparare = v; ++i; }
        else if (a == "--anteprima") { anteprima = true; }
        else if (a == "--elenco") { elenco = true; }
        else { std::printf("unknown option: %s\n", a.c_str()); return 2; }
    }
    const Formato* fm = formato(modo);
    if (!fm) { std::printf("unsupported mode: %s\n", modo.c_str()); return 2; }
    if (!codec_valido(modo, mbit)) {
        const int prima = mbit;
        mbit = codec_per(modo, mbit);
        std::printf("%s: %d is not a codec of this format, using %s\n", modo.c_str(), prima, nome_codec(mbit).c_str());
    }
    if (audio != 2 && audio != 8 && audio != 16) { std::printf("--audio: 2, 8 or 16, never less than 2\n"); return 2; }

    ParametriCodifica pc;
    pc.larghezza = fm->larghezza;
    pc.altezza = fm->altezza;
    pc.cad = fm->cad;
    pc.interlacciato = fm->interlacciato;
    pc.mbit = mbit;
    pc.canali_audio = audio;
    pc.wav = wav;

    if (!da_riparare.empty()) {
        if (!fs::is_directory(fs::u8path(da_riparare))) { std::printf("folder not found: %s\n", da_riparare.c_str()); return 2; }
        const int n = ripara_cartella(da_riparare, pc.cad);
        std::printf(n < 0 ? "repair failed: the originals are in the \"interrotti\" folder\n"
                          : "tracks repaired: %d, originals in the \"interrotti\" folder\n", n);
        return n < 0 ? 1 : 0;
    }

    if (solo_controlli) {
        if (cartella.empty()) { std::printf("--cartella missing: the checks measure its drive\n"); return 2; }
        const int n = nomi.empty() ? tracce_prova : static_cast<int>(nomi.size()) * copie;
        return static_cast<int>(controlli_completi(cartella, n, pc, secondi_disco));
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) { std::printf("COM initialisation failed\n"); return 1; }
    std::vector<Porta> porte = porte_decklink();
    const BMDDisplayMode bmd = modo_decklink(modo);

    // Anteprima (sezione 9.58): le porte aperte e nessun file. Serve a guardare cosa arriva prima del
    // REC e alla mappa delle porte. Una porta occupata da un altro programma si salta e si dice.
    if (anteprima) {
        Orologio orologio(pc.cad);
        std::vector<std::unique_ptr<Ingresso>> ingressi;
        for (const auto& p : porte) {
            bool scelta = p.ingresso && nomi.empty();
            for (const std::string& n : nomi)
                if (p.ingresso && minuscolo(p.nome).find(minuscolo(n)) != std::string::npos) scelta = true;
            if (!scelta) continue;
            auto in = std::make_unique<Ingresso>(p.scheda, p.nome, orologio);
            std::string e;
            if (!in->apri(bmd, 0, e) || !in->avvia(e)) { std::printf("skipped %s\n", e.c_str()); continue; }
            std::printf("  %zu  %s\n", ingressi.size(), p.nome.c_str());
            ingressi.push_back(std::move(in));
        }
        auto stop = std::make_shared<std::atomic<bool>>(false);
        {
            Marcatore pagina("", [&orologio] { return orologio.cadenza(); }, [&orologio] { return orologio.stima(adesso_us()); });
            pagina.stato = [&] {
                return "{\"registra\":false,\"inizio\":\"--\",\"timecode\":\"" +
                       (orologio.valido() ? tc_testo(orologio.stima(adesso_us()), orologio.cadenza()) : std::string("--")) +
                       "\",\"tracce\":0,\"persi\":0,\"marker\":0,\"ingressi\":" + ingressi_json(ingressi, orologio.cadenza(), 0) + "}";
            };
            pagina.pagine = [&](const std::string& percorso, const std::string& query, std::string& corpo,
                                std::string& tipo) { return pagina_ingressi(ingressi, percorso, query, corpo, tipo); };
            std::string e;
            if (!pagina.ascolta(porta_marker > 0 ? porta_marker : 8811, e)) std::printf("⚠ %s\n", e.c_str());
            else std::printf("preview of %zu inputs, nothing recorded: http://localhost:%d/ - type stop and Enter to close\n",
                             ingressi.size(), porta_marker > 0 ? porta_marker : 8811);
            std::thread([stop, secondi] {
                std::string s;
                while (std::getline(std::cin, s)) {
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
                    if (s == "stop" || s == "fine" || s == "q") { *stop = true; return; }
                }
                if (secondi <= 0) *stop = true;
            }).detach();
            const auto t0 = std::chrono::steady_clock::now();
            while (!*stop && (secondi <= 0 || std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secondi)))
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            pagina.ferma();
        }
        for (auto& in : ingressi) in->ferma();
        ingressi.clear();
        for (auto& p : porte) p.scheda->Release();
        CoUninitialize();
        return 0;
    }

    if (elenco || nomi.empty()) {
        std::printf("DeckLink ports: %zu\n", porte.size());
        for (size_t i = 0; i < porte.size(); ++i)
            std::printf("  %2zu  %-30s %s %s\n", i + 1, porte[i].nome.c_str(), porte[i].ingresso ? "input" : "output only",
                        porte[i].connettore.c_str());
        std::printf("ffmpeg %s: DNxHD encoder %s\n", av_version_info(),
                    avcodec_find_encoder(AV_CODEC_ID_DNXHD) ? "present" : "MISSING");
        std::printf("ffmpeg built with: %s\n", avcodec_configuration());
        for (auto& p : porte) p.scheda->Release();
        CoUninitialize();
        return 0;
    }

    if (cartella.empty()) { std::printf("--cartella missing\n"); return 2; }
    Orologio orologio(pc.cad);

    // Le porte si aprono e girano; poi la registrazione ci attacca le tracce, come dalla pagina.
    std::vector<std::unique_ptr<Ingresso>> ingressi;
    int esito = 0;
    for (const std::string& n : nomi) {
        const Porta* scelta = nullptr;
        for (const auto& p : porte)
            if (p.ingresso && minuscolo(p.nome).find(minuscolo(n)) != std::string::npos) { scelta = &p; break; }
        if (!scelta) { std::printf("input not found: %s\n", n.c_str()); esito = 1; break; }
        auto in = std::make_unique<Ingresso>(scelta->scheda, scelta->nome, orologio);
        std::string e;
        if (!in->apri(bmd, audio, e) || !in->avvia(e)) { std::printf("%s\n", e.c_str()); esito = 1; break; }
        ingressi.push_back(std::move(in));
    }

    if (esito == 0) {
        // Marker dalla tastiera e dalla rete (sezione 9.55). Lo stato è condiviso col filo della
        // tastiera, che resta fermo su getline anche dopo la fine e quindi non può tenere riferimenti.
        struct Comandi {
            std::atomic<bool> stop{false}, attivo{true};
            std::shared_ptr<Marcatore> marcatore;
        };
        auto comandi = std::make_shared<Comandi>();
        comandi->marcatore = std::make_shared<Marcatore>("", [&orologio] { return orologio.cadenza(); }, [] { return -1; });
        std::vector<Ingresso*> v;
        for (auto& in : ingressi) v.push_back(in.get());
        Registrazione reg(v, orologio, comandi->marcatore.get());
        Registrazione::Parametri par;
        par.cartella = cartella;
        par.pc = pc;
        par.modo = modo;
        par.copie = copie;
        par.atem = atem;
        par.hyperdeck = hyperdeck;
        par.forza = forza;
        std::string e;
        if (!reg.avvia(par, e)) {
            std::printf("%s\n", e.c_str());
            esito = 1;
        } else {
            comandi->marcatore->stato = [&] {
                return "{\"registra\":true,\"inizio\":\"" + tc_testo(reg.inizio(), orologio.cadenza()) + "\",\"timecode\":\"" +
                       tc_testo(reg.timecode(), orologio.cadenza()) + "\",\"tracce\":" + std::to_string(ingressi.size() * copie) +
                       ",\"persi\":" + std::to_string(reg.persi()) + ",\"marker\":" +
                       std::to_string(comandi->marcatore->quanti()) + ",\"ingressi\":" +
                       ingressi_json(ingressi, orologio.cadenza(), copie) + "}";
            };
            comandi->marcatore->pagine = [&](const std::string& percorso, const std::string& query, std::string& corpo,
                                             std::string& tipo) {
                return pagina_ingressi(ingressi, percorso, query, corpo, tipo);
            };
            if (porta_marker > 0) {
                if (comandi->marcatore->ascolta(porta_marker, e))
                    std::printf("thumbnail page: http://localhost:%d/; network markers: "
                                "http://<this computer>:%d/marker?testo=…&colore=…\n", porta_marker, porta_marker);
                else
                    std::printf("⚠ %s: markers from the keyboard only\n", e.c_str());
            }
            std::printf("recording: Enter adds a marker, optionally preceded by text"
                        " (\"red applause\"); type stop and Enter to stop\n");
            std::thread([comandi, secondi] {
                std::string s;
                while (std::getline(std::cin, s)) {
                    if (!comandi->attivo) return;
                    while (!s.empty() && (s.back() == ' ' || s.back() == '\r')) s.pop_back();
                    if (s == "stop" || s == "fine" || s == "q") { comandi->stop = true; return; }
                    comandi->marcatore->da_tastiera(s);
                }
                // Senza tastiera e senza durata non si saprebbe quando fermarsi.
                if (secondi <= 0) comandi->stop = true;
            }).detach();
            const auto t0 = std::chrono::steady_clock::now();
            processore();
            for (int s = 1; !comandi->stop && (secondi <= 0 || s <= secondi); ++s) {
                std::this_thread::sleep_until(t0 + std::chrono::seconds(s));
                std::printf("%4d s | CPU %3.0f%%", s, processore());
                for (auto& in : ingressi) std::printf(" | %s %s", in->nome().c_str(), in->segnale() ? "signal" : "NO SIGNAL");
                std::printf(" | dropped %lld\n", reg.persi());
            }
            comandi->attivo = false;
            comandi->marcatore->ferma();
            reg.ferma();
        }
    }

    for (auto& in : ingressi) in->ferma();
    ingressi.clear();
    for (auto& p : porte) p.scheda->Release();
    CoUninitialize();
    return esito;
}
