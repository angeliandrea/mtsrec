// Un ingresso DeckLink: riceve i fotogrammi dal driver, legge timecode e orari, li passa alle tracce.
//
// Il callback gira su un thread del driver e deve tornare in fretta: se tarda, sotto sforzo il
// driver può consegnare l'immagine del fotogramma dopo col timecode giusto (sezione 9.47).
// Qui si legge, si copia e si esce.
//
// Le tracce si attaccano e si staccano anche a porta aperta: la pagina tiene le porte aperte per le
// finestrelle e i livelli, e il REC aggiunge le tracce senza chiudere niente.
#pragma once
#include <windows.h>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "DeckLinkAPI_h.h"
#include "miniatura.h"
#include "traccia.h"

// Una porta DeckLink di questa macchina, come la dà l'SDK: niente è scritto per una scheda precisa.
struct Porta {
    IDeckLink* scheda;
    std::string nome;          // «DeckLink Duo (3)»: il nome che Desktop Video dà alla porta
    bool ingresso;             // può acquisire
    std::string connettore;    // «SDI», «HDMI», «SDI e HDMI»
};
std::vector<Porta> porte_decklink();   // le schede vanno rilasciate con Release()
// Il modo DeckLink di un formato del registratore (formato.h): tutti i 19, dal 1080p23.98 al 2160p60.
BMDDisplayMode modo_decklink(const std::string& modo);

class Ingresso : public IDeckLinkInputCallback {
public:
    Ingresso(IDeckLink* scheda, std::string nome, Orologio& orologio);
    virtual ~Ingresso();

    const std::string& nome() const { return nome_; }
    void aggiungi(Traccia* t);
    void togli_tracce();
    bool apri(BMDDisplayMode modo, int canali_audio, std::string& errore);
    bool avvia(std::string& errore);
    void ferma();
    int64_t fotogrammi() const { return fotogrammi_.load(); }
    bool segnale() const { return segnale_.load(); }
    int tc() const { return tc_.load(); }                 // l'ultimo timecode, -1 se mai
    bool tc_dal_segnale() const { return tc_segnale_.load(); }
    Miniatura& miniatura() { return miniatura_; }
    int canali_audio() const { return canali_; }
    // Il picco dei canali 1 e 2 dall'ultima lettura, da 0 a 1; la lettura lo azzera.
    float picco(int canale);
    // Quanti fotogrammi arriva tardi il segnale di questa porta: si tolgono dal suo timecode, così
    // in Resolve l'angolo sta col resto. Una porta in ritardo non fa da orologio alle altre.
    void imposta_ritardo(int fotogrammi) { ritardo_.store(fotogrammi); }
    int ritardo() const { return ritardo_.load(); }
    // La misura del ritardo: finché è accesa annota la luminanza media di ogni fotogramma col suo
    // timecode prima della correzione. Pochi punti dell'immagine, nel callback non costa niente.
    struct Luce { int tc; float y; };
    void misura_luce(bool accesa);
    std::vector<Luce> luci() const;
    // Il formato che arriva sulla porta, se la scheda lo sa riconoscere; vuoto se non lo sa.
    // Il formato che arriva, col nome di Desktop Video: quello visto dal riconoscimento del formato, oppure,
    // se il segnale c'è e il riconoscimento non ha detto niente, quello con cui la porta è aperta.
    std::string formato_arrivo() const;
    // La storia dei livelli della registrazione: un picco ogni PASSO_STORIA fotogrammi, da 0 a 1, il
    // più alto dei canali 1 e 2. Comincia al REC e resta fino al REC dopo, così una pagina aperta a
    // metà registrazione o dopo lo STOP disegna la forma d'onda dall'inizio (15 novembre 2026).
    static constexpr int PASSO_STORIA = 12;
    std::vector<float> storia(size_t da) const;

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** p) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++rif_; }
    ULONG STDMETHODCALLTYPE Release() override { return --rif_; }   // la vita dell'oggetto la decide main

    // IDeckLinkInputCallback
    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents eventi,
        IDeckLinkDisplayMode* modo, BMDDetectedVideoInputFormatFlags segnali) override;
    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* f,
        IDeckLinkAudioInputPacket* audio) override;

private:
    IDeckLink* scheda_;
    IDeckLinkInput* inp_ = nullptr;
    std::string nome_;
    Orologio& orologio_;
    std::mutex m_tracce_;
    std::vector<Traccia*> tracce_;
    std::atomic<ULONG> rif_{1};
    std::atomic<int64_t> fotogrammi_{0};
    std::atomic<bool> segnale_{false}, tc_segnale_{false};
    std::atomic<int> tc_{-1}, ritardo_{0};
    std::atomic<bool> misura_{false};
    mutable std::mutex m_luci_;
    std::vector<Luce> luci_;
    std::atomic<int32_t> picco_[2] = {0, 0};
    mutable std::mutex m_storia_;
    std::vector<float> storia_;
    std::atomic<bool> registra_{false};
    int32_t finestra_max_ = 0;     // solo dal callback
    int finestra_n_ = 0;
    int canali_ = 0;
    mutable std::mutex m_formato_;
    std::string formato_;
    int sondati_ = 0;                                  // fotogrammi già stampati dalla sonda WINISO_SONDA_TC
    bool solo_hdmi_ = false;                           // la porta ha solo l'ingresso HDMI (sezione 9.86)
    BMDDisplayMode modo_ = bmdModeUnknown;
    Miniatura miniatura_;
    bool aperto_ = false, avviato_ = false, audio_ = false;
};
