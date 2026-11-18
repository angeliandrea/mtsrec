// I formati video e i codec del registratore, in un posto solo (sezioni 9.74 e 9.79).
//
// Dal 18 novembre 2026 tutte le cadenze dell'ATEM Constellation (Andrea, 9.79): 1080p23,98, 24, 25,
// 29,97, 30, 50, 59,94 e 60; 1080i50, 59,94 e 60; e in UHD, sperimentale, le stesse otto del 1080p.
// Il DNxHD esiste solo fino a 1920×1080; per l'UHD Avid ha fatto il DNxHR, che ha profili e non bitrate.
//
// Il codec si indica col suo nome Avid, che è il flusso nominale in Mbit/s e cambia con la cadenza
// anche se la qualità del fotogramma è la stessa: l'alta è 175 a 23,98 e 24, 185 a 25, 220 a 29,97
// e 30, 365 a 50, 440 a 59,94 e 60. ffmpeg sceglie il tipo di compressione da quel numero.
// Per il DNxHR in UHD si usano 728, 481 e 149 (HQ, SQ, LB), i flussi misurati a 25p il 17 novembre:
// a un'altra cadenza il flusso vero si ricava con mb_al_secondo().
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// La cadenza e il suo timecode. I timecode del registratore sono «fotogrammi dalla mezzanotte»
// contati davvero, uno per fotogramma: col drop-frame l'etichetta salta dei numeri ma il conto no,
// così le differenze fra due timecode restano numeri di fotogrammi anche a 29,97.
struct Cadenza {
    int base = 25;          // fotogrammi del timecode in un secondo: 24, 25, 30, 50, 60
    int num = 25, den = 1;  // la cadenza vera: 30000/1001 per il 29,97
    bool drop = false;      // timecode drop-frame, solo con base 30 o 60 e den 1001

    double fps() const { return static_cast<double>(num) / den; }
    bool ntsc() const { return den == 1001; }
    // Quanti numeri salta l'etichetta a ogni minuto non multiplo di dieci: 2 a 29,97, 4 a 59,94.
    int salto() const { return drop ? base / 15 : 0; }
    int giorno() const;                                    // fotogrammi in 24 ore di timecode
    std::string testo(int f) const;                        // «10:00:01:00», col drop-frame «10:00:01;00»
    // Dall'etichetta ai fotogrammi; -1 se l'etichetta non esiste (col drop-frame 00:01:00;00 non c'è).
    int da_etichetta(int h, int m, int s, int f) const;
    int da_testo(const std::string& t) const;              // «10:00:01:00» o «10:00:01;00»; -1 se non si legge
    // Fotogrammi in un tempo, in microsecondi: per l'orologio, arrotondati.
    int64_t fotogrammi_in_us(int64_t us) const;
    // L'audio a 48 kHz: i campioni del fotogramma k contato dalla partenza. A 29,97 sono 1601 o 1602,
    // in modo che in cinque fotogrammi siano 8008 e l'audio non scivoli mai sul video.
    int campioni(int64_t k) const;
    int campioni_max() const;                              // il più grande dei campioni(k)
    // I campioni dalla mezzanotte al fotogramma f: il timecode di un Broadcast WAV.
    int64_t campioni_da_mezzanotte(int f) const;
    std::string nome() const;                              // «25», «29.97», «59.94»
};

struct Formato {
    const char* nome;            // «1080p25», «1080p29.97», «1080i50», «2160p59.94»
    int larghezza, altezza;
    Cadenza cad;                 // dei fotogrammi: il 1080i50 è a 25 fotogrammi
    bool interlacciato;
};

// nullptr se il nome non è un formato del registratore
const Formato* formato(const std::string& nome);
const std::vector<Formato>& formati();
bool uhd(const std::string& modo);
// Il formato del registratore che corrisponde a un nome dato da Desktop Video al segnale in arrivo
// («1080p25», «1080i59.94», «2160p29.97»), oppure nullptr.
const Formato* formato_del_segnale(const std::string& nome_decklink);

// I codec di un formato, dal più pesante: in HD alta, media, bassa coi nomi Avid della cadenza; in UHD
// DNxHR HQ, SQ, LB. In 1080i la qualità bassa non c'è: il DNxHD 36 esiste solo in progressivo.
std::vector<int> codec_del_formato(const std::string& modo);
bool codec_valido(const std::string& modo, int mbit);
// Il codec equivalente in un altro formato, a pari posizione: 185 a 25p → 365 a 50p, → DNxHR HQ in UHD.
int codec_per(const std::string& modo, int mbit);
// Il codec della stessa qualità alla cadenza di riferimento, 1080p25 o 2160p25: è quello che la prova del
// processore misura, e il lavoro per fotogramma è lo stesso.
int codec_di_prova(const std::string& modo, int mbit);
// «DNxHD 185», «DNxHR HQ»
std::string nome_codec(int mbit);
// «dnxhr_hq» per ffmpeg, nullptr per il DNxHD (che va col bitrate)
const char* profilo_dnxhr(int mbit);
// Il flusso vero del video in MB/s a quella cadenza: per il DNxHD il nome Avid, per il DNxHR in proporzione.
double mb_al_secondo(int mbit, const Cadenza& c);
