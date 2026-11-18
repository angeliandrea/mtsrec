// I marker inseriti durante la registrazione (sezione 9.23): dalla tastiera o in rete, da Companion.
// Ognuno prende il timecode del momento in cui arriva e si scrive subito in marker.csv, così resta
// anche se la registrazione si interrompe. Il progetto Resolve li legge da lì (sezione 9.55).
#pragma once
#include "formato.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Marker {
    int tc = -1;                  // fotogrammi dalla mezzanotte
    std::string colore = "Blue";  // il nome che usa Resolve
    int durata = 1;               // fotogrammi
    std::string fonte, testo;
};

class Marcatore {
public:
    // adesso: il timecode di questo istante, in fotogrammi dalla mezzanotte.
    // Percorso vuoto: si serve solo la pagina, e /marker risponde che non si registra.
    // cadenza: quella dell'orologio, che col drop-frame la prende dal segnale.
    Marcatore(const std::string& percorso_csv, std::function<Cadenza()> cadenza, std::function<int()> adesso);
    ~Marcatore();
    Marcatore(const Marcatore&) = delete;
    Marcatore& operator=(const Marcatore&) = delete;

    // Da qui in poi i marker vanno in questo file, col timecode che dice adesso; il conto riparte.
    // La pagina resta accesa fra una registrazione e l'altra, il file cambia ogni volta.
    void registra_in(const std::string& percorso_csv, std::function<int()> adesso);
    void smetti();                    // fine registrazione: /marker torna a rispondere 409
    bool attivo() const;
    std::string elenco_json() const;  // i marker di questa registrazione, per la pagina

    Marker aggiungi(std::string testo, const std::string& colore, int durata, const std::string& fonte);
    // Una riga scritta alla tastiera: se la prima parola è un colore, «rosso applausi», vale come colore.
    Marker da_tastiera(const std::string& riga);

    // Server HTTP, una richiesta per connessione:
    //   GET /marker?testo=…&colore=…&durata=…   aggiunge un marker, risponde col suo timecode
    //   GET /stato                              lo stato della registrazione, in JSON
    //   tutto il resto lo chiede a pagine(): la pagina con le finestrelle e le immagini
    bool ascolta(int porta, std::string& errore);
    void ferma();
    int quanti() const { return quanti_; }
    std::function<std::string()> stato;
    // percorso e query → corpo e tipo; false se la pagina non c'è
    std::function<bool(const std::string&, const std::string&, std::string&, std::string&)> pagine;

private:
    void servi();
    std::string rispondi(const std::string& richiesta, int& codice, std::string& tipo);

    std::string percorso_;
    std::function<Cadenza()> cadenza_;
    std::function<int()> adesso_;
    std::vector<Marker> elenco_;
    mutable std::mutex m_;
    std::atomic<int> quanti_{0};
    std::atomic<bool> basta_{false};
    unsigned long long ascolto_ = ~0ull;      // SOCKET, senza tirare dentro winsock2.h
    std::thread filo_;
};

// «rosso», «Red», «ROSSO» → «Red»; vuoto se non è un colore di Resolve.
std::string colore_resolve(const std::string& nome);

// Il valore di un parametro di una query string, già decodificato; vuoto se non c'è.
std::string parametro_query(const std::string& query, const std::string& nome);
