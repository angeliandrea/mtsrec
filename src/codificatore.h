// Codifica di una traccia: fotogrammi UYVY dalla DeckLink → DNxHD → file MOV col timecode.
// L'audio dell'SDI va nello stesso MOV in PCM 24 bit e, a richiesta, anche in un Broadcast WAV
// a parte col timecode di partenza nell'intestazione bext (richiesta di Andrea, sezione 9.52).
//
// Il MOV è scritto a frammenti di un secondo: se la macchina si spegne durante la registrazione
// il file resta leggibile fino all'ultimo frammento chiuso (sezione 9.50, punto 7).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "formato.h"

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVPacket;
struct AVStream;

struct ParametriCodifica {
    int larghezza = 1920;
    int altezza = 1080;
    Cadenza cad;                  // dei fotogrammi, anche per l'interlacciato (1080i50 = 25), col drop-frame del segnale
    bool interlacciato = false;
    int mbit = 185;               // DNxHD 185, 120, 36 in HD; DNxHR HQ 728, SQ 481, LB 149 in UHD (formato.h)
    int thread = 4;               // fette del fotogramma codificate in parallelo
    int canali_audio = 0;         // 0 senza audio, oppure 2, 8, 16 dall'SDI
    bool wav = false;             // l'audio anche in un Broadcast WAV accanto al MOV
    // Il pacchetto audio più grande di un fotogramma: a 29,97 i fotogrammi hanno 1601 o 1602 campioni.
    int campioni_max() const { return cad.campioni_max(); }
};

class Codificatore {
public:
    Codificatore() = default;
    ~Codificatore();
    Codificatore(const Codificatore&) = delete;
    Codificatore& operator=(const Codificatore&) = delete;

    // percorso senza estensione: si scrivono <percorso>.mov e, con p.wav, <percorso>.wav.
    // percorso vuoto: si codifica e basta, senza file. Serve alla misura del processore.
    // tc_iniziale in fotogrammi dalla mezzanotte: la traccia timecode del MOV e il bext del WAV.
    bool apri(const std::string& percorso, const ParametriCodifica& p, int tc_iniziale, std::string& errore);
    // Un fotogramma UYVY 4:2:2 8 bit, come lo dà la DeckLink in bmdFormat8BitYUV, e il suo audio:
    // n_audio campioni interi a 32 bit intrecciati, oppure nullptr per il silenzio. Nel file ogni fotogramma
    // ha i campioni che gli spettano (Cadenza::campioni): quelli arrivati passano da una piccola coda, così a
    // 29,97 un pacchetto da 1602 su un posto da 1601 non perde né inventa niente.
    bool scrivi(const uint8_t* uyvy, int byte_per_riga, const int32_t* audio, int n_audio, std::string& errore);
    bool chiudi(std::string& errore);
    int64_t fotogrammi() const { return pts_; }

private:
    bool svuota(bool fine, std::string& errore);
    bool scrivi_audio(const int32_t* audio, int n_audio, std::string& errore);
    bool apri_file(const std::string& percorso, const ParametriCodifica& p, const char* tc, std::string& errore);

    AVFormatContext* fmt_ = nullptr;
    AVFormatContext* wav_ = nullptr;
    AVStream* st_audio_ = nullptr;
    AVPacket* pkt_audio_ = nullptr;
    int canali_ = 0;
    Cadenza cad_;
    int tc_iniziale_ = 0;
    std::vector<int32_t> coda_audio_;   // campioni arrivati e non ancora scritti, intrecciati
    int64_t pts_audio_ = 0;
    AVCodecContext* ctx_ = nullptr;
    AVStream* st_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    int64_t pts_ = 0;
    bool aperto_ = false;
};
