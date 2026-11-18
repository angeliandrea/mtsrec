// Controlli prima del REC (sezione 9.50, punti 0-3): sistema, spazio, disco e processore.
// Non cambiano niente sulla macchina: dicono cosa va e cosa no, a semaforo.
#pragma once
#include <string>
#include <vector>

#include "codificatore.h"

enum class Semaforo { verde = 0, giallo = 1, rosso = 2 };

// Una riga dei controlli, per chi la vuole mostrare in una pagina invece che nella console.
struct Voce { Semaforo esito; std::string testo; };

// Byte al secondo che una traccia scrive su disco: video, audio nel MOV e Broadcast WAV.
double byte_al_secondo(const ParametriCodifica& pc);

// Quelli che non dipendono dalla cartella: piano energetico, Windows Update, riavvio in sospeso.
Semaforo controlli_sistema(std::vector<Voce>* voci = nullptr);

// Quelli istantanei, da fare prima di ogni registrazione: sistema e spazio libero.
Semaforo controlli_rapidi(const std::string& cartella, int tracce, const ParametriCodifica& pc,
                          std::vector<Voce>* voci = nullptr);

// Tutti, con le misure: il disco scrive per secondi_disco al ritmo richiesto più il 30%,
// il processore codifica le tracce insieme più in fretta che può.
Semaforo controlli_completi(const std::string& cartella, int tracce, const ParametriCodifica& pc, int secondi_disco);

// Fotogrammi al secondo che il processore codifica in tutto con tracce codifiche insieme.
// ampiezza: la grana dell'immagine di prova, ±livelli (sezione 9.54).
double codifica_al_massimo(int tracce, const ParametriCodifica& pc, double secondi, int ampiezza);

// Quanto scrive il disco della cartella al massimo, in byte al secondo: tracce file insieme,
// per qualche secondo e al più tetto_byte. 0 se nella cartella non si riesce a scrivere.
double disco_al_massimo(const std::string& cartella, int tracce, const ParametriCodifica& pc, double secondi,
                        long long tetto_byte);
