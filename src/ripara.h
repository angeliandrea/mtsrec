// La riparazione dopo un'interruzione (sezione 9.57), il prototipo era script\ripara_interrotti.py.
//
// Quando la macchina si spegne il MOV a frammenti resta leggibile, ma l'ultimo fotogramma può essere
// scritto a metà e l'ultimo frammento può avere meno audio che video; il WAV, scritto senza frammenti,
// ha più audio e le lunghezze dell'intestazione segnaposto. Si tiene per MOV e WAV la parte in cui
// video e audio sono interi tutti e due, senza ricodificare niente: i pacchetti si copiano, il PCM si
// taglia al campione. Gli originali non si cancellano, vanno in «interrotti».
#pragma once
#include <atomic>
#include <string>

// Il segno che una registrazione è cominciata e non è finita: lo scrive la partenza, lo toglie la fine.
inline const char* const FILE_IN_CORSO = "RECORDING IN PROGRESS.txt";
// Il nome di prima della traduzione in inglese (17 novembre 2026): una cartella interrotta allora si ripara lo stesso.
inline const char* const FILE_IN_CORSO_VECCHIO = "REGISTRAZIONE IN CORSO.txt";

// Ripara tutti i MOV di una cartella, coi WAV accanto. Restituisce quanti ne ha riparati, -1 se ha fallito.
#include "formato.h"
int ripara_cartella(const std::string& cartella, const Cadenza& cad);

// All'apertura del registratore (18 novembre 2026): le registrazioni rimaste a metà nella cartella di
// destinazione, un livello sotto, si riparano da sole. Prima si riparavano solo registrando di nuovo nella
// stessa cartella, cosa che dalla pagina non succede mai perché ogni REC ha un nome nuovo: manuale e guida
// promettevano la riparazione all'apertura, e la prova da capo l'ha trovata mancante. La cadenza si legge dal
// MOV; una cartella con un MOV scritto negli ultimi 30 secondi potrebbe essere ancora in corso, e si riprova
// ogni 5 secondi per al più 90: chi riapre subito dopo una caduta la trova riparata lo stesso (prova T8,
// 18 novembre 2026). Se esci diventa vero si smette di aspettare. Restituisce quante cartelle ha riparato.
int ripara_interrotte(const std::string& destinazione, const std::atomic<bool>* esci = nullptr);
