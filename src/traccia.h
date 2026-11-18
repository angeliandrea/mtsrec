// Una traccia: la coda dei fotogrammi in arrivo, il thread che li codifica, il conto di tutto.
//
// La regola della sincronia: ogni file comincia dallo stesso timecode e avanza di un fotogramma
// alla volta. Se ne manca uno si ripete il precedente, se uno arriva in ritardo si scarta,
// se il segnale cade si scrive nero. Così il fotogramma N di ogni file ha lo stesso timecode
// in tutte le tracce, e la multicamera di Resolve si allinea da sola.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "codificatore.h"
#include "formato.h"

int64_t adesso_us();                                   // orologio di Windows, lo stesso della DeckLink
// 900025 a 25 fps → "10:00:01:00"; a 29,97 drop-frame l'etichetta col punto e virgola (formato.h)
inline std::string tc_testo(int fotogrammi, const Cadenza& c) { return c.testo(fotogrammi); }

// Il timecode più recente letto da una porta con segnale, con l'istante in cui è arrivato.
// Serve alle porte senza timecode: senza segnale, o HDMI che il RP188 non lo porta.
//
// La cadenza la fissano le porte aperte (apri_porte): cambia solo a porte ferme, e col drop-frame
// la corregge il segnale stesso (Ingresso), mai durante una registrazione.
class Orologio {
public:
    explicit Orologio(Cadenza c = Cadenza()) : cad_(c) {}
    // Cambiando cadenza il timecode di prima non vale più: si riparte da zero.
    void imposta(const Cadenza& c);
    void aggiorna(int tc, int64_t hw_us);
    int stima(int64_t hw_us) const;                    // -1 se nessuna porta ha ancora dato un timecode
    bool valido() const;
    // Il punto da cui stima conta adesso: timecode e istante. Fissato alla partenza, dà il timecode
    // dei file anche se poi quello della sorgente salta (sezione 9.56).
    void ancora(int& tc, int64_t& hw_us) const;
    int da_ancora(int tc, int64_t ancora_us, int64_t hw_us) const;
    Cadenza cadenza() const;
    int giorno() const { return cadenza().giorno(); }
private:
    mutable std::mutex m_;
    bool valido_ = false;
    int tc_ = 0;
    int64_t hw_us_ = 0;
    Cadenza cad_;
};

struct InfoFotogramma {
    bool segnale = false;
    int tc = -1;                  // fotogrammi dalla mezzanotte; -1 se sconosciuto
    bool tc_dal_segnale = false;  // letto dal RP188 e non stimato
    int64_t stream_us = 0;
    int64_t hw_us = 0;
    int64_t arrivo_us = 0;
    unsigned coda_scheda = 0;     // fotogrammi in attesa nel driver
    int campioni_audio = 0;       // campioni del pacchetto audio arrivato col fotogramma
};

struct Conti {
    int64_t ricevuti = 0, scritti = 0, riempiti = 0, neri = 0, scartati = 0;
    int64_t persi_coda = 0, salti = 0;
    int64_t ritardo_max_us = 0;
    int coda_max = 0;
};

class Traccia {
public:
    Traccia(std::string nome, std::string cartella, ParametriCodifica p, int posti_in_coda);
    ~Traccia();

    const std::string& nome() const { return nome_; }
    // Dal callback DeckLink: copia il fotogramma e il suo audio e torna subito. Mai lavoro pesante qui.
    // audio: interi a 32 bit intrecciati, info.campioni_audio campioni; nullptr se non c'è.
    void consegna(const uint8_t* uyvy, int byte_per_riga, const int32_t* audio, const InfoFotogramma& info);
    bool avvia(int tc_inizio, std::string& errore);
    // Fine comune: si scrive fino al fotogramma prima di tc_fine, come la partenza è comune.
    void termina_a(int tc_fine) { fine_tc_.store(tc_fine); }
    bool terminata() const { return terminata_.load(); }
    void ferma();
    Conti conti() const;
    std::string errore() const;

private:
    struct Posto { std::vector<uint8_t> dati; std::vector<int32_t> audio; bool con_audio = false; int n_audio = 0;
                   int byte_per_riga = 0; InfoFotogramma info; };
    void lavora();
    bool codifica(const uint8_t* dati, int rb, const int32_t* audio, int n_audio);

    std::string nome_, cartella_;
    ParametriCodifica p_;
    Codificatore cod_;
    std::vector<Posto> posti_;
    std::vector<int> liberi_;
    std::deque<int> pronti_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> attiva_{false};
    bool fine_ = false;
    int giorno_ = 0;
    int atteso_ = 0;              // timecode del prossimo fotogramma da scrivere
    bool iniziato_ = false;       // scritto almeno un fotogramma
    std::atomic<int> fine_tc_{-1};
    std::atomic<bool> terminata_{false};
    std::vector<uint8_t> ultimo_, nero_;
    FILE* csv_ = nullptr;
    Conti conti_;
    std::string errore_;
};
