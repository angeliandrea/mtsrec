#pragma once
// Il registro della sessione su file (sezione 9.77). Il registratore scrive avvisi ed errori sulla sua
// console, che aperto col doppio clic è nascosta: chiusa la console i messaggi si perdevano, e il rapporto di
// prova di un tester a cui il programma si è chiuso non aveva una riga sul perché.
//
// Da avvia_registro() in poi stdout e stderr passano da una pipe: un filo li manda dove andavano prima
// (console, oppure la pipe di uno script come mappa_porte.py, senza ritardi) e insieme in
// %USERPROFILE%\mtsrec\logs\mtsrec <data ora> <pid>.log, con l'ora a ogni riga.
// Si tengono gli ultimi dieci. rapporto_prova.py ne mette gli ultimi due nel rapporto, ripuliti.
void avvia_registro();
