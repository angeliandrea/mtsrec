// Gli apparecchi della regia che il registratore comanda o ascolta in rete (sezione 9.50, punti 4 e 9).
#pragma once
#include <string>

// Il registro dei tagli: script\registro_atem.py in un processo a parte, sola lettura dell'ATEM.
// Si ferma chiudendogli lo standard input, così finisce di scrivere il file da sé.
// Dalla beta 12 (sezione 9.93) scrive anche lo stato dal vivo per la pagina: vivo è atem-vivo.json
// (programma, anteprima, mix, aux), tagli i tagli dall'aggancio. Con uscita vuota non scrive il registro
// grezzo: è l'ascolto da fermo, che esce da solo se l'ATEM tace per 5 secondi.
class RegistroAtem {
public:
    ~RegistroAtem();
    bool avvia(const std::string& ip, const std::string& uscita, std::string& errore,
               const std::string& vivo = "", const std::string& tagli = "", const std::string& modo = "");
    void ferma();                         // aspetta al massimo 3 s, poi lo chiude
    bool attivo() const { return processo_ != nullptr; }
    bool finito() const;                  // il processo è uscito da solo
private:
    void* processo_ = nullptr;
    void* stdin_ = nullptr;
};

// atem-vivo.json nella cartella dell'utente, in UTF-8 per la riga di comando e in largo per leggerlo.
std::string file_atem_vivo();
std::wstring file_atem_vivo_largo();

// Gli script del pacchetto col Python del pacchetto (python\ e script\ accanto al registratore);
// se mancano, quelli di questo computer con py -3.13.
namespace Python {
// La riga di comando completa per lo script, con gli argomenti già scritti come vanno.
std::wstring riga(const std::string& script, const std::string& argomenti);
// Lo lancia, aspetta al massimo attesa_ms e restituisce quello che ha stampato.
std::string esegui(const std::string& script, const std::string& argomenti, int attesa_ms, std::string& errore);
// Lo lancia in una finestra sua e non aspetta: per il progetto Resolve a fine registrazione.
// Con processo, restituisce lì il processo da tenere d'occhio, da chiudere con CloseHandle.
bool lancia(const std::string& script, const std::string& argomenti, std::string& errore, void** processo = nullptr);
}

// L'HyperDeck sul protocollo di testo, porta 9993: record all'inizio, stop alla fine.
// Registra sulla scheda e col formato che ha già: il registratore non cambia la sua configurazione.
class HyperDeck {
public:
    // Manda un comando e restituisce la prima riga della risposta, per esempio «200 ok».
    static std::string comando(const std::string& ip, const std::string& testo, std::string& errore);
};
