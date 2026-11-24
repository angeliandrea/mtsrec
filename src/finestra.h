// La finestra del registratore (sezione 9.61): la pagina dentro WebView2, il motore di Edge che
// Windows 10 e 11 hanno già, al posto del browser. La pagina resta la stessa e resta anche la porta
// 8811: Companion e gli altri computer continuano a usarla.
//
// Gira in un filo suo con la sua coda di messaggi. Se WebView2 non c'è si apre il browser.
#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>

class Finestra {
public:
    // Perché la finestra non si chiude adesso. perche vuoto: si chiude. fermabile: quello che la tiene aperta
    // si può fermare (mappa, misura dei ritardi), e la finestra chiede se fermarlo e chiudere (9.87).
    struct Blocco { std::string perche; bool fermabile = false; };
    ~Finestra();
    // perche_no: chiamata quando si chiude la finestra. ferma: chiamata se chi chiude risponde sì.
    // cartella_dati: dove WebView2 tiene i suoi file, fuori dalla cartella del programma.
    void apri(const std::string& url, const std::wstring& cartella_dati, std::function<Blocco()> perche_no,
              std::function<void()> ferma = {});
    // La finestra è stata chiusa da chi la usa, o non si è potuta aprire.
    bool chiusa() const { return chiusa_.load(); }
    // Dal programma, a registratore chiuso: la chiude senza chiedere.
    void chiudi();

private:
    void lavora(std::string url, std::wstring cartella_dati);
    std::thread filo_;
    std::atomic<bool> chiusa_{false};
    std::atomic<void*> hwnd_{nullptr};
    std::function<Blocco()> perche_no_;
    std::function<void()> ferma_;
};
