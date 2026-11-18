// Il registratore comandato dalla pagina (sezione 10): si apre con un doppio clic, senza argomenti.
//
// Al primo avvio, o se la macchina è cambiata, la pagina è il controllo della macchina; poi quella
// di registrazione, con una riga per ogni porta d'ingresso che la macchina ha davvero. Le porte
// restano aperte in anteprima per finestrelle, timecode e livelli; REC attacca le tracce, STOP le stacca.
// Dalla console restano i marker con Invio e «esci» per chiudere, come prima.
#pragma once

// Dove si apre la pagina: nella finestra del registratore (WebView2, sezione 9.61), nel browser
// predefinito, o da nessuna parte e la si apre a mano.
enum class Apertura { finestra, browser, niente };

// controllo: rifà il controllo della macchina anche se non è cambiata.
int interfaccia(bool controllo, Apertura apertura);
