// Il controllo della macchina (sezione 10). Al primo avvio, prima della pagina di registrazione, il
// registratore legge tutto quello che ha intorno — sistema, schede PCIe, DeckLink, dischi, rete,
// impostazioni di Windows, pacchetto — fa una prova veloce di codifica e dice come stanno le cose:
// pronto, gira con delle indicazioni, nessuna garanzia, non gira. Ogni cosa da fare ha il suo rimedio.
//
// Niente è legato a una macchina precisa: si legge quello che c'è, schede di altri produttori comprese.
// Il risultato resta in %USERPROFILE%\mtsrec\macchina.json. Alle aperture dopo si confronta
// in silenzio l'impronta della macchina con quella salvata, e il controllo si rifà solo se è cambiata.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Impostazioni;

class Analisi {
public:
    ~Analisi();
    // Parte in un filo suo: la pagina chiede a che punto è con json().
    void avvia(const Impostazioni& imp);
    bool in_corso() const { return in_corso_; }
    // Passi, risultati e verdetto; a controllo finito è quello salvato in macchina.json.
    std::string json() const;
    // Il verdetto dell'ultimo controllo: "pronto", "indicazioni", "garanzie", "no"; vuoto se mai fatto.
    std::string verdetto() const;

private:
    void lavora(std::string atem, std::string hyperdeck, int porta);
    void passo(int n, const std::string& stato, const std::string& esito = "", const std::string& nota = "");

    mutable std::mutex m_;
    std::atomic<bool> in_corso_{false};
    std::thread filo_;
    std::string passi_[10][3];         // stato, esito, nota
    std::string risultato_;            // il JSON del controllo finito
};

// L'impronta di adesso: processore, memoria, schede video, DeckLink e versione di Desktop Video,
// schede di acquisizione PCIe. Si calcola in un attimo, senza prove.
std::string impronta_macchina();

// macchina.json salvato; vuoto se il controllo non è mai stato fatto.
std::string profilo_salvato();

// true se il controllo va rifatto: mai fatto, o la macchina è cambiata. cosa dice perché.
bool controllo_da_rifare(std::string& cosa);

// La cartella del registratore nel profilo dell'utente, creata se manca.
std::wstring cartella_utente();

// Toglie i collegamenti del vecchio setup di WinISORecorder, se il programma vecchio non c'è più (9.81).
void togli_collegamenti_vecchi();

// La cartella dell'eseguibile, dove stanno python\, script\ e interfaccia\.
std::wstring cartella_programma();
// Quello che il registratore non porta con sé e manca su questa macchina (sezione 9.77): Desktop Video,
// DaVinci Resolve, WebView2. Python, ffmpeg e Visual C++ viaggiano nella cartella del programma.
// {"mancanti":[{"id","nome","perche","essenziale","link"}]}. Un secondo o meno.
std::string prerequisiti_json();
// L'indirizzo ufficiale da cui scaricare un prerequisito, per id; vuoto se l'id non c'è.
std::string link_prerequisito(const std::string& id);

// I dischi di questa macchina in JSON, per il selettore della destinazione e per il controllo.
std::string dischi_json();

// Misura il disco della cartella al massimo e lo annota in dischi.json. MB/s, 0 se non scrive.
double misura_disco(const std::string& cartella);

// La velocità già misurata del disco della cartella, in MB/s; 0 se non è mai stato misurato.
double velocita_misurata(const std::string& cartella);

// ATEM e HyperDeck nelle reti locali di questa macchina, cercati quando l'indirizzo non è scritto:
// l'ATEM col saluto UDP sulla 9910, che non cambia niente nel mixer, l'HyperDeck con la porta TCP 9993.
// Due o tre secondi. Solo reti fino a /22.
struct Apparecchi { std::vector<std::string> atem, hyperdeck; };
Apparecchi cerca_apparecchi();
