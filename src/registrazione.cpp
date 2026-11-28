#include <windows.h>

#include "registrazione.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "controlli.h"
#include "formato.h"
#include "ingresso.h"
#include "json.h"
#include "macchina.h"
#include "marker.h"
#include "ripara.h"

namespace fs = std::filesystem;

static std::string nome_file(std::string s) {
    for (char& c : s)
        if (std::string("\\/:*?\"<>|").find(c) != std::string::npos) c = '_';
    return s;
}

static int tc_ora_del_computer(const Cadenza& c) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const int64_t ms = ((t.wHour * 60LL + t.wMinute) * 60 + t.wSecond) * 1000 + t.wMilliseconds;
    return static_cast<int>(c.fotogrammi_in_us(ms * 1000) % c.giorno());
}

Registrazione::Registrazione(std::vector<Ingresso*> ingressi, Orologio& orologio, Marcatore* marcatore)
    : ingressi_(std::move(ingressi)), orologio_(orologio), marcatore_(marcatore) {}

Registrazione::~Registrazione() { ferma(); }

int Registrazione::timecode() const {
    return registra_ ? orologio_.da_ancora(ancora_tc_, ancora_us_, adesso_us()) : -1;
}

int64_t Registrazione::persi() const {
    std::lock_guard<std::mutex> l(m_);
    int64_t n = 0;
    for (const auto& t : tracce_) n += t->conti().persi_coda;
    return n;
}

std::string Registrazione::tracce_json() const {
    std::lock_guard<std::mutex> l(m_);
    std::string r;
    for (size_t k = 0; k < tracce_.size(); ++k) {
        const Conti c = tracce_[k]->conti();
        r += std::string(k ? "," : "") + "{\"file\":" + json_testo(tracce_[k]->nome()) + ",\"porta\":" + json_testo(porte_[k]) +
             ",\"scritti\":" + std::to_string(c.scritti) + ",\"riempiti\":" + std::to_string(c.riempiti) +
             ",\"neri\":" + std::to_string(c.neri) + ",\"persi\":" + std::to_string(c.persi_coda) +
             ",\"coda_max\":" + std::to_string(c.coda_max) + ",\"errore\":" + json_testo(tracce_[k]->errore()) + "}";
    }
    return "[" + r + "]";
}

bool Registrazione::avvia(const Parametri& p, std::string& errore) {
    if (registra_) { errore = "already recording"; return false; }
    p_ = p;
    // Il drop-frame lo dice il segnale (ingresso.cpp): se l'orologio ha la stessa cadenza ne prende la scelta.
    if (const Cadenza c = orologio_.cadenza(); c.num == p_.pc.cad.num && c.den == p_.pc.cad.den) p_.pc.cad.drop = c.drop;
    if (p_.cartella.empty()) { errore = "no folder specified"; return false; }
    if (ingressi_.empty()) { errore = "no input to record"; return false; }
    for (Ingresso* in : ingressi_)
        if (in->canali_audio() != p_.pc.canali_audio) {
            errore = in->nome() + ": the input is open with a different number of audio channels";
            return false;
        }
    std::error_code ec;
    fs::create_directories(fs::u8path(p_.cartella), ec);
    if (ec) { errore = "cannot create the folder: " + ec.message(); return false; }
    // Una cartella già usata no: i MOV con lo stesso nome verrebbero riscritti senza dire niente.
    // Se è rimasta a metà, prima si ripara, così il lavoro della diretta interrotta non si perde.
    if (fs::exists(fs::u8path(p_.cartella) / FILE_IN_CORSO) || fs::exists(fs::u8path(p_.cartella) / FILE_IN_CORSO_VECCHIO)) {
        std::printf("a recording in this folder was interrupted: repairing it\n");
        const int n = ripara_cartella(p_.cartella, p_.pc.cad);
        std::printf(n < 0 ? "⚠ repair failed, the originals are in the \"interrotti\" folder\n"
                          : "tracks repaired: %d, originals in the \"interrotti\" folder\n", n);
    }
    for (const auto& e : fs::directory_iterator(fs::u8path(p_.cartella), ec))
        if (e.path().extension() == ".mov") {
            errore = "the folder already contains MOV files: record to a new folder so they are not overwritten";
            return false;
        }
    // La mappa delle porte (sezione 9.58) va con la registrazione: il progetto Resolve ci ricava quale
    // ingresso dell'ATEM registra ogni traccia, con le sorgenti delle aux che stanno in atem.jsonl.
    {
        const fs::path mappa = fs::path(cartella_utente()) / L"porte.json";
        if (fs::exists(mappa) && fs::copy_file(mappa, fs::u8path(p_.cartella) / "porte.json", fs::copy_options::overwrite_existing, ec))
            std::printf("input map copied to the folder\n");
        else
            std::printf("⚠ no input map (script\\mappa_porte.py): the track sources must be set manually\n");
    }
    std::vector<Voce> voci;
    const Semaforo s = controlli_rapidi(p_.cartella, static_cast<int>(ingressi_.size()) * p_.copie, p_.pc, &voci);
    controlli_.clear();
    for (const auto& v : voci)
        controlli_ += std::string(controlli_.empty() ? "" : ",") + "{\"esito\":\"" +
                      (v.esito == Semaforo::verde ? "verde" : v.esito == Semaforo::giallo ? "giallo" : "rosso") +
                      "\",\"testo\":" + json_testo(v.testo) + "}";
    controlli_ = "[" + controlli_ + "]";
    if (s == Semaforo::rosso && !p_.forza) { errore = "a pre-REC check is red: not recording"; return false; }

    // Le tracce, attaccate agli ingressi che girano già.
    std::unique_lock<std::mutex> l(m_);
    tracce_.clear();
    porte_.clear();
    ritardi_.clear();
    for (size_t k = 0; k < ingressi_.size(); ++k) {
        const std::string base = nome_file(k < p_.nomi.size() && !p_.nomi[k].empty() ? p_.nomi[k] : ingressi_[k]->nome());
        for (int c = 1; c <= p_.copie; ++c) {
            std::string nt = base + (p_.copie > 1 ? " copia " + std::to_string(c) : "");
            // Due sorgenti con lo stesso nome non si sovrascrivono: la seconda prende un numero.
            for (int n = 2;; ++n) {
                bool doppio = false;
                for (const auto& t : tracce_) doppio |= t->nome() == nt;
                if (!doppio) break;
                nt = base + " (" + std::to_string(n) + ")";
            }
            tracce_.push_back(std::make_unique<Traccia>(nt, p_.cartella, p_.pc, p_.pc.cad.base));   // un secondo di coda
            porte_.push_back(ingressi_[k]->nome());
            ritardi_.push_back(ingressi_[k]->ritardo());
            ingressi_[k]->aggiungi(tracce_.back().get());
        }
    }
    l.unlock();
    // Con lo stato dal vivo per la pagina: tally e corsia del programma coi tagli dal REC (beta 12, sezione 9.93).
    if (!p_.atem.empty() && !registro_.avvia(p_.atem, p_.cartella + "\\atem.jsonl", errore, file_atem_vivo(),
                                             p_.cartella + "\\atem-tagli.json", p_.modo)) {
        std::printf("⚠ %s: recording without cuts\n", errore.c_str());
        errore.clear();
    }
    hd_inizio_.clear();
    if (!p_.hyperdeck.empty()) {
        // Prima delle tracce, così la sua clip copre anche il primo fotogramma.
        const std::u8string u = fs::u8path(p_.cartella).filename().u8string();
        std::string e, nome(u.begin(), u.end());
        hd_inizio_ = HyperDeck::comando(p_.hyperdeck, "record: name: " + nome_file(nome), e);
        std::printf("HyperDeck record: %s\n", e.empty() ? hd_inizio_.c_str() : e.c_str());
        if (hd_inizio_.rfind("200", 0) != 0) std::printf("⚠ the HyperDeck is not recording: continuing without it\n");
    }
    // Un secondo per sentire le porte, poi la partenza comune: tutte le tracce cominciano dallo
    // stesso timecode, qualche fotogramma più avanti di adesso.
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (!orologio_.valido()) {
        std::printf("⚠ no input carries timecode: using the computer clock\n");
        orologio_.aggiorna(tc_ora_del_computer(p_.pc.cad), adesso_us());
    }
    inizio_ = (orologio_.stima(adesso_us()) + 5) % orologio_.giorno();
    fine_ = -1;
    orologio_.ancora(ancora_tc_, ancora_us_);
    // Il segno che si toglie solo a fine pulita: se resta, la prossima partenza ripara.
    if (FILE* f = _wfopen((fs::u8path(p_.cartella) / FILE_IN_CORSO).c_str(), L"w")) {
        std::fprintf(f, "Recording started at %s and not yet closed.\n"
                        "If the recorder is not open, the recording was interrupted: mtsrec --ripara \"%s\"\n",
                     tc_testo(inizio_, p_.pc.cad).c_str(), p_.cartella.c_str());
        std::fclose(f);
    }
    std::printf("start at %s, %zu tracks, %s %s, audio %d channels%s\n", tc_testo(inizio_, p_.pc.cad).c_str(),
                tracce_.size(), nome_codec(p_.pc.mbit).c_str(), p_.modo.c_str(), p_.pc.canali_audio, p_.pc.wav ? " plus WAV" : "");
    bool tutte = true;
    for (auto& t : tracce_) {
        std::string e;
        if (!t->avvia(inizio_, e)) { std::printf("%s: %s\n", t->nome().c_str(), e.c_str()); errore += t->nome() + ": " + e + " "; tutte = false; }
    }
    // Finché registra Windows non va in sospensione e non spegne niente per inattività.
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED);
    registra_ = true;
    if (marcatore_)
        // Il timecode dei file, non quello della sorgente: se la sorgente salta (un HyperDeck
        // che ricomincia la clip) i file continuano a contare, e i marker devono stare con loro.
        marcatore_->registra_in(p_.cartella + "\\marker.csv", [this] { return timecode(); });
    if (!tutte) { ferma(); return false; }
    return true;
}

void Registrazione::ferma() {
    if (!registra_) return;
    if (marcatore_) {
        marcatore_->smetti();
        if (marcatore_->quanti()) std::printf("marker: %d in marker.csv\n", marcatore_->quanti());
    }
    // Fine comune, qualche fotogramma più avanti di adesso: si aspetta che tutte ci arrivino.
    fine_ = (orologio_.stima(adesso_us()) + 5) % orologio_.giorno();
    for (auto& t : tracce_) t->termina_a(fine_);
    const auto limite = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    auto tutte = [&] { for (auto& t : tracce_) if (!t->terminata()) return false; return true; };
    while (!tutte() && std::chrono::steady_clock::now() < limite) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::printf("end at %s%s\n", tc_testo(fine_, p_.pc.cad).c_str(), tutte() ? "" : " ⚠ not all tracks reached it");
    // Prima si staccano dalle porte, che continuano a girare per la pagina, poi si chiudono i file.
    for (Ingresso* in : ingressi_) in->togli_tracce();
    for (auto& t : tracce_) t->ferma();
    SetThreadExecutionState(ES_CONTINUOUS);
    registra_ = false;
    // I file sono chiusi: la registrazione è intera.
    std::error_code ec;
    fs::remove(fs::u8path(p_.cartella) / FILE_IN_CORSO, ec);
    fs::remove(fs::u8path(p_.cartella) / FILE_IN_CORSO_VECCHIO, ec);
    if (!p_.hyperdeck.empty() && hd_inizio_.rfind("200", 0) == 0) {
        std::string e;
        const std::string r = HyperDeck::comando(p_.hyperdeck, "stop", e);
        std::printf("HyperDeck stop: %s\n", e.empty() ? r.c_str() : e.c_str());
    }
    registro_.ferma();

    // Quello che serve per costruire il progetto Resolve: partenza e fine comuni, tracce con la loro
    // porta, apparecchi. I file possono avere il nome della sorgente e non quello della porta.
    std::string j = "{\n  \"programma\": \"mtsrec\",\n  \"fps\": " + json_num(p_.pc.cad.fps(), 3) +
                    ",\n  \"cadenza\": {\"base\": " + std::to_string(p_.pc.cad.base) + ", \"num\": " +
                    std::to_string(p_.pc.cad.num) + ", \"den\": " + std::to_string(p_.pc.cad.den) + ", \"drop\": " +
                    json_si(p_.pc.cad.drop) + "}" + ",\n  \"interlacciato\": " + json_si(p_.pc.interlacciato) +
                    ",\n  \"modo\": " + json_testo(p_.modo) + ",\n  \"canali_audio\": " + std::to_string(p_.pc.canali_audio) +
                    ",\n  \"wav\": " + json_si(p_.pc.wav) + ",\n  \"mbit\": " + std::to_string(p_.pc.mbit) +
                    ",\n  \"inizio\": " + json_testo(tc_testo(inizio_, p_.pc.cad)) +
                    ",\n  \"fine\": " + json_testo(fine_ >= 0 ? tc_testo(fine_, p_.pc.cad) : "") +
                    ",\n  \"atem\": " + json_testo(p_.atem) + ",\n  \"hyperdeck\": " + json_testo(p_.hyperdeck) +
                    ",\n  \"hyperdeck_record\": " + json_testo(hd_inizio_) + ",\n  \"tracce\": [";
    for (size_t k = 0; k < tracce_.size(); ++k)
        j += std::string(k ? "," : "") + "\n    {\"file\": " + json_testo(tracce_[k]->nome()) + ", \"porta\": " + json_testo(porte_[k]) +
             ", \"ritardo\": " + std::to_string(ritardi_[k]) + "}";
    j += "\n  ]\n}\n";
    scrivi_file((fs::u8path(p_.cartella) / "registrazione.json").wstring(), j);

    // Rapporto: per ogni traccia quanto scritto, quanto riempito, quanto perso.
    FILE* r = _wfopen((fs::u8path(p_.cartella) / "rapporto.txt").c_str(), L"w");
    for (auto& t : tracce_) {
        Conti c = t->conti();
        char riga[512];
        std::snprintf(riga, sizeof riga,
            "%s: received %lld written %lld filled %lld black %lld discarded %lld dropped in queue %lld "
            "timecode jumps %lld | max queue %d | max delay %.1f ms%s%s\n",
            t->nome().c_str(), c.ricevuti, c.scritti, c.riempiti, c.neri, c.scartati, c.persi_coda,
            c.salti, c.coda_max, c.ritardo_max_us / 1000.0,
            t->errore().empty() ? "" : " | ERROR ", t->errore().c_str());
        std::printf("%s", riga);
        if (r) std::fputs(riga, r);
    }
    if (r) std::fclose(r);
}
