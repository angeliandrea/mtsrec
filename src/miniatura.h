// La finestrella di un ingresso (sezione 9.50, decisione di Andrea): 320×180 a 5 fotogrammi al secondo,
// sempre sacrificabile. Costa qualcosa solo se qualcuno la guarda: l'ingresso copia un fotogramma su
// cinque soltanto se negli ultimi 5 secondi è stata chiesta, e non aspetta mai il lucchetto.
#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class Miniatura {
public:
    static constexpr int L = 320, A = 180;
    // Dal callback della DeckLink: un fotogramma UYVY qualunque, rimpicciolito saltando i pixel.
    void forse_copia(const uint8_t* uyvy, int byte_per_riga, int larghezza, int altezza, int64_t numero);
    // Per la pagina: l'ultima immagine in JPEG, vuota se non ce n'è ancora una.
    std::string jpeg();
    // Luminanza media e scarto, in codici SDI, dell'ultima finestrella: per riconoscere nero e barre
    // nella mappa delle porte (sezione 9.58). {"y":null} se non arriva niente.
    std::string misura();

private:
    bool aspetta_se_prima_volta();
    int64_t numero_ = -1;             // il fotogramma da cui viene la finestrella
    std::mutex m_;
    std::vector<uint8_t> y_ = std::vector<uint8_t>(L * A), u_ = std::vector<uint8_t>(L / 2 * A),
                         v_ = std::vector<uint8_t>(L / 2 * A);
    bool pronta_ = false;
    std::atomic<int64_t> guardata_us_{0};
};
