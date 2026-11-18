// Una registrazione, da REC a STOP. La usano la riga di comando e la pagina.
//
// Gli ingressi sono già aperti e girano: qui si attaccano le tracce, si avviano il registro
// dell'ATEM e l'HyperDeck, si fissa la partenza comune, e alla fine la fine comune, il rapporto
// e quello che serve al progetto Resolve. Le porte restano aperte: la pagina continua a guardarle.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "codificatore.h"
#include "esterni.h"
#include "traccia.h"

class Ingresso;
class Marcatore;

class Registrazione {
public:
    struct Parametri {
        std::string cartella;              // la cartella della registrazione: si crea, e non deve avere MOV
        ParametriCodifica pc;
        std::string modo = "1080p25";
        int copie = 1;                     // tracce per ingresso, per le prove di carico
        std::string atem, hyperdeck;
        std::vector<std::string> nomi;     // il nome del file di ogni ingresso; vuoto = il nome della porta
        bool forza = false;                // registra anche col semaforo rosso dei controlli
    };

    Registrazione(std::vector<Ingresso*> ingressi, Orologio& orologio, Marcatore* marcatore);
    ~Registrazione();

    // Parte: dura un secondo e mezzo, il tempo di sentire le porte. false con l'errore se non parte.
    bool avvia(const Parametri& p, std::string& errore);
    // Fine comune, file chiusi, rapporto e registrazione.json scritti.
    void ferma();

    bool registra() const { return registra_; }
    const std::string& cartella() const { return p_.cartella; }
    int inizio() const { return inizio_; }
    int fine() const { return fine_; }
    int timecode() const;                  // il timecode dei file adesso
    int64_t persi() const;
    // Le tracce per /stato: nome del file, porta, fotogrammi scritti, riempiti, persi.
    std::string tracce_json() const;
    // Le righe dei controlli prima del REC, per la pagina.
    const std::string& controlli_json() const { return controlli_; }
    const std::string& hyperdeck_risposta() const { return hd_inizio_; }

private:
    std::vector<Ingresso*> ingressi_;
    Orologio& orologio_;
    Marcatore* marcatore_;
    Parametri p_;
    mutable std::mutex m_;                 // tracce_ e porte_: la pagina le legge mentre REC le crea
    std::vector<std::unique_ptr<Traccia>> tracce_;
    std::vector<std::string> porte_;       // la porta di ogni traccia
    std::vector<int> ritardi_;             // il ritardo tolto al timecode di ogni traccia, in fotogrammi
    RegistroAtem registro_;
    std::string hd_inizio_, controlli_;
    std::atomic<bool> registra_{false};
    int inizio_ = -1, fine_ = -1;
    int ancora_tc_ = 0;                    // il punto da cui conta il timecode dei file, fissato alla partenza
    int64_t ancora_us_ = 0;
};
