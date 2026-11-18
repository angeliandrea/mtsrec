// Le impostazioni della pagina di registrazione, salvate in %USERPROFILE%\mtsrec\impostazioni.json
// così alla prossima apertura si ritrovano. La destinazione si sceglie col selettore: niente è legato
// a un disco di una macchina precisa.
#pragma once
#include <string>

struct Impostazioni {
    std::string cartella;              // dove si registra: disco e cartella, senza il nome della registrazione
    std::string nome;                  // il nome della registrazione, cioè la cartella che si crea dentro
    std::string modo = "1080p25";      // uno dei 19 formati di formato.h; non si sceglie: lo dice l'ATEM o il segnale (9.79)
    int mbit = 185;                    // DNxHD 185, 120, 36 in HD; DNxHR 728, 481, 149 in UHD
    int audio = 2;                     // canali dall'SDI: 2, 8 o 16, mai meno di 2
    bool resolve = true;               // a fine registrazione si crea il progetto Resolve
    std::string atem, hyperdeck;       // indirizzi; vuoti se non ci sono
    int porta = 8811;
    // Il ritardo di ogni porta in fotogrammi, «nome della porta=N|…»: quanto arriva tardi il segnale
    // che passa da un apparecchio prima del registratore, come l'HDMI dell'HyperDeck (sezione 9.60).
    std::string ritardi;
    // Le tracce scelte con «Add track», nell'ordine della pagina: «porta|porta=nome|-porta|…».
    // «=nome» è il nome del file scritto a mano, «-» davanti una traccia esclusa col tasto R (17 novembre).
    std::string tracce;
    std::string setup;                 // il setup caricato o salvato per ultimo, vuoto se nessuno
    // Quale ingresso dell'ATEM registra ogni HyperDeck, per nome del deck: «deck=ingresso|deck=program|…».
    // Lo ricorda la pagina dopo la conferma delle clip HyperDeck (sezione 9.72) e viaggia coi setup.
    std::string abbinamenti_hd;
    int ritardo(const std::string& porta) const;
    void metti_ritardo(const std::string& porta, int fotogrammi);

    void carica();
    bool salva() const;
    std::string json() const;
    // Cambia quello che la query dice: cartella=…&nome=…&modo=…&mbit=…&audio=…&resolve=…&atem=…&hyperdeck=…
    // e ritardo=<porta>&fotogrammi=N.
    // Quello che non va bene resta com'era e finisce in errore.
    void da_query(const std::string& query, std::string& errore);
    void da_json(const std::string& j);  // come carica(), da un testo: serve anche ai setup
};

// I setup di registrazione con un nome (Andrea, 17 novembre: «salvare una configurazione dopo il setup, così
// ciascuno può farsi diversi setup di registrazione»): le impostazioni intere, tracce comprese, in
// %USERPROFILE%\mtsrec\setup\<nome>.json. Il nome della registrazione e la porta della pagina
// non viaggiano coi setup. Ognuna restituisce un errore in inglese, vuoto se è andata.
std::string setup_elenco_json();
std::string setup_salva(const std::string& nome, Impostazioni& imp);
std::string setup_carica(const std::string& nome, Impostazioni& imp);
std::string setup_cancella(const std::string& nome, Impostazioni& imp);
