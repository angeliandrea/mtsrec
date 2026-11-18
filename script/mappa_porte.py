"""La mappa porta DeckLink → aux dell'ATEM (sezione 9.58, punto 5 della 9.50).

Il cavo fra un'aux e una porta DeckLink non lo sa nessun apparecchio: si scopre. Il registratore
apre tutte le porte in anteprima, senza scrivere niente, e per ogni aux questo script la porta per un
attimo sul nero e poi sulle barre: la porta che passa dal nero alle barre è quella dell'aux. Subito
dopo l'aux torna com'era. Alla fine si rilegge l'ATEM e si verifica che ogni aux sia come prima.

    py -3 mappa_porte.py [--atem 192.168.10.240] [--uscita porte.json] [--aux 9-12] [--prova]
    py -3 mappa_porte.py --atem <ip> --nel-registratore

Con --nel-registratore lo lancia il tasto «Map inputs…» del registratore (sezione 9.78): usa le porte che
il registratore ha già aperte invece di aprirne uno in anteprima, e in fondo stampa un JSON su una riga
con l'esito, che la pagina mostra.

⚠ SCRIVE NELL'ATEM: solo in configurazione, MAI durante una diretta. Ogni aux resta cambiata circa
un secondo. Prima di scrivere lo stato delle aux va su disco accanto alla mappa, così se lo script
cade si sa com'erano. --prova legge l'ATEM e apre le porte senza cambiare niente.

La mappa va in %USERPROFILE%\\mtsrec\\porte.json: il registratore la copia in ogni cartella
di registrazione, e progetto_da_registratore.py ci ricava quale ingresso dell'ATEM registra ogni
traccia, insieme alle sorgenti delle aux che l'ATEM manda all'aggancio e che stanno in atem.jsonl.
"""
import argparse, datetime, json, os, socket, struct, subprocess, sys, threading, time, urllib.request
from pathlib import Path

PORTA_ATEM = 9910
NERO, BARRE = 0, 1000
# Nel pacchetto il registratore sta nella cartella sopra gli script; qui, in quella di compilazione.
ESEGUIBILE = next((p for p in (Path(__file__).resolve().parent.parent / "mtsrec.exe",
                               Path(os.path.expandvars(r"%USERPROFILE%\WinISORecorder-build\mtsrec.exe")))
                   if p.exists()), Path("mtsrec.exe"))
CARTELLA = Path(os.path.expandvars(r"%USERPROFILE%\mtsrec"))
if not CARTELLA.exists() and CARTELLA.with_name("WinISORecorder").exists():   # fino alla 0.9 beta 5
    CARTELLA = CARTELLA.with_name("WinISORecorder")
PAGINA = "http://127.0.0.1:8811"


def pacchetto(flag, sessione, ack=0, id_locale=0, dati=b""):
    return struct.pack(">HHHHHH", (flag << 11) | (12 + len(dati)), sessione, ack, 0, 0, id_locale) + dati


def comando(nome, dati):
    return struct.pack(">HH", 8 + len(dati), 0) + nome.encode("ascii") + dati


class Atem:
    """Una sessione che resta aperta: conferma tutto quello che arriva, tiene aggiornate aux e nomi
    e manda i comandi uno alla volta aspettando la conferma. Formato dei comandi di atem-connection."""

    def __init__(self, ip):
        self.ip, self.aux, self.nomi = ip, {}, {}
        self.sessione, self.pronto, self.confermati, self.id = None, threading.Event(), set(), 0
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(0.2)
        self.basta = False
        self.s.sendto(pacchetto(0x02, 0x53AB, dati=bytes([1, 0, 0, 0, 0, 0, 0, 0])), (ip, PORTA_ATEM))
        threading.Thread(target=self._ascolta, daemon=True).start()
        if not self.pronto.wait(20):
            raise RuntimeError(f"ATEM {ip} did not send its initial state")

    def _ascolta(self):
        while not self.basta:
            try:
                d, _ = self.s.recvfrom(65535)
            except (socket.timeout, OSError):
                continue
            flag, sess = d[0] >> 3, struct.unpack(">H", d[2:4])[0]
            if flag & 0x02:                                   # risposta all'aggancio
                self.s.sendto(pacchetto(0x10, sess, 0), (self.ip, PORTA_ATEM))
                continue
            self.sessione = sess
            if flag & 0x01:
                self.s.sendto(pacchetto(0x10, sess, struct.unpack(">H", d[10:12])[0]), (self.ip, PORTA_ATEM))
            if flag & 0x10:
                self.confermati.add(struct.unpack(">H", d[4:6])[0])
            lunghezza, i = struct.unpack(">H", d[0:2])[0] & 0x07FF, 12
            while i + 8 <= lunghezza:
                n = struct.unpack(">H", d[i:i + 2])[0]
                if n < 8:
                    break
                nome, b = d[i + 4:i + 8].decode("ascii", "replace"), d[i + 8:i + n]
                if nome == "AuxS":
                    self.aux[b[0] + 1] = struct.unpack(">H", b[2:4])[0]
                elif nome == "InPr":
                    self.nomi[struct.unpack(">H", b[0:2])[0]] = b[2:22].split(b"\0")[0].decode("utf-8", "replace")
                elif nome == "InCm":
                    self.pronto.set()
                i += n

    def aux_su(self, aux, sorgente, attesa=1.0):
        """Porta l'aux sulla sorgente e aspetta che l'ATEM confermi il pacchetto e lo stato nuovo."""
        self.id += 1
        dati = comando("CAuS", struct.pack(">BBH", 1, aux - 1, sorgente))
        for _ in range(5):
            self.s.sendto(pacchetto(0x01, self.sessione, 0, self.id, dati), (self.ip, PORTA_ATEM))
            t = time.time()
            while time.time() - t < attesa:
                if self.id in self.confermati and self.aux.get(aux) == sorgente:
                    return True
                time.sleep(0.01)
        return False

    def chiudi(self):
        self.basta = True
        self.s.close()


def leggi(percorso):
    with urllib.request.urlopen(PAGINA + percorso, timeout=3) as r:
        return json.loads(r.read().decode("utf-8"))


# Gli indici delle porte per /misura: in anteprima tutte in fila, nel registratore il loro «n».
INDICI = []


def misure(n):
    return [leggi(f"/misura?n={INDICI[k] if k < len(INDICI) else k}") for k in range(n)]


def aspetta_nuove(n, prima, fotogrammi=10, limite=2.0):
    """Misure prese almeno «fotogrammi» dopo quelle di prima, su tutte le porte che ricevono."""
    t = time.time()
    while True:
        m = misure(n)
        if all(a.get("y") is None or b.get("y") is None or b["fotogramma"] >= a["fotogramma"] + fotogrammi
               for a, b in zip(prima, m)) or time.time() - t > limite:
            return m
        time.sleep(0.05)


def nero(m):
    return m.get("y") is not None and m["scarto"] < 3 and m["y"] < 40


def barre(m):
    return m.get("y") is not None and m["scarto"] > 20


def intervallo(testo, tutte):
    if not testo:
        return tutte
    a, _, b = testo.partition("-")
    return [n for n in tutte if int(a) <= n <= int(b or a)]


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--atem", default="192.168.10.240")
    ap.add_argument("--uscita", default=str(CARTELLA / "porte.json"))
    ap.add_argument("--aux", help="for example 9-12; default all")
    ap.add_argument("--prova", action="store_true", help="read and measure without changing anything")
    ap.add_argument("--nel-registratore", action="store_true",
                    help="launched by the recorder's Map inputs button: use the inputs it already has open")
    a = ap.parse_args()
    if not a.nel_registratore:
        return mappa(a)
    # Per il registratore: qualunque cosa succeda, l'ultima riga è un JSON con l'esito.
    try:
        esito = mappa(a) or {}
    except SystemExit as e:
        esito = {"ok": False, "errore": str(e.code)}
    except Exception as e:
        esito = {"ok": False, "errore": f"{type(e).__name__}: {e}"}
    print(json.dumps(esito, ensure_ascii=False), flush=True)


def mappa(a):
    if a.nel_registratore:
        try:
            stato = leggi("/stato")
        except OSError:
            sys.exit("the recorder does not answer on port 8811")
        if stato.get("registra"):
            sys.exit("the recorder is recording: the input map can only be made while stopped")
    else:
        try:
            if leggi("/stato").get("registra"):
                sys.exit("the recorder is recording: the input map can only be made while stopped")
            sys.exit("a recorder is already open on port 8811: quit it before making the input map")
        except OSError:
            pass

    atem = Atem(a.atem)
    prima = dict(atem.aux)
    nome = lambda s: atem.nomi.get(s, "Black" if s == NERO else str(s))
    da_provare = intervallo(a.aux, sorted(prima))
    print(f"ATEM {a.atem}: {len(prima)} aux, testing {da_provare[0]}-{da_provare[-1]}")
    uscita = Path(a.uscita)
    uscita.parent.mkdir(parents=True, exist_ok=True)
    salvato = uscita.with_name(f"aux before input map {datetime.datetime.now():%Y-%m-%d %H%M%S}.json")
    salvato.write_text(json.dumps({"atem": a.atem, "aux": prima}, indent=2), encoding="utf-8")

    rec, righe = None, []
    if a.nel_registratore:
        # Le porte del registratore aperto: solo quelle aperte, ognuna col suo indice per /misura.
        aperte = [i for i in leggi("/stato")["ingressi"] if i.get("aperta", True)]
        INDICI[:] = [i["n"] for i in aperte]
        porte = [i["porta"] for i in aperte]
        if not porte:
            atem.chiudi()
            sys.exit("no DeckLink input is open")
    else:
        rec = subprocess.Popen([str(ESEGUIBILE), "--anteprima"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, encoding="utf-8", bufsize=1)
        threading.Thread(target=lambda: [righe.append(r.rstrip()) for r in rec.stdout], daemon=True).start()
        t = time.time()
        while not any("anteprima di" in r or "http://localhost:" in r for r in righe):
            if rec.poll() is not None or time.time() - t > 20:
                atem.chiudi()
                sys.exit("the recorder did not open in preview mode:\n" + "\n".join(righe))
            time.sleep(0.05)
        time.sleep(1.0)
        porte = [i["nome"] for i in leggi("/stato")["ingressi"]]
    saltate = [r for r in righe if r.lower().startswith(("saltata", "skipped"))]
    m = aspetta_nuove(len(porte), misure(len(porte)), 5)
    col_segnale = [k for k, x in enumerate(m) if x.get("y") is not None]
    print(f"{len(porte)} inputs open, {len(col_segnale)} with signal:", ", ".join(porte[k] for k in col_segnale) or "none")
    for r in saltate:
        print("  ⚠", r)

    trovate, cambiate = {}, []
    try:
        for aux in da_provare:
            if a.prova:
                break
            cambiate.append(aux)
            ok = atem.aux_su(aux, NERO)
            m0 = aspetta_nuove(len(porte), misure(len(porte)))
            ok = atem.aux_su(aux, BARRE) and ok
            m1 = aspetta_nuove(len(porte), m0)
            ok = atem.aux_su(aux, prima[aux]) and ok
            qui = [porte[k] for k in col_segnale if nero(m0[k]) and barre(m1[k])]
            for p in qui:
                trovate.setdefault(p, []).append(aux)
            print(f"AUX {aux:2} ({nome(prima[aux])}): {', '.join(qui) or '—'}{'' if ok else '  ⚠ no confirmation from the ATEM'}")
    finally:
        # Comunque sia andata: ogni aux toccata torna com'era, e si verifica rileggendo.
        for aux in cambiate:
            if atem.aux.get(aux) != prima[aux]:
                atem.aux_su(aux, prima[aux])
        if rec:
            rec.stdin.write("stop\n")
            rec.stdin.flush()
            rec.wait(timeout=20)
    time.sleep(0.5)
    diverse = {x: (prima[x], atem.aux.get(x)) for x in prima if atem.aux.get(x) != prima[x]}
    atem.chiudi()
    verifica = Atem(a.atem)                                    # una sessione nuova: lo stato vero
    diverse.update({x: (prima[x], verifica.aux.get(x)) for x in prima if verifica.aux.get(x) != prima[x]})
    verifica.chiudi()
    print("aux restored, verified by reading the ATEM again" if not diverse else f"⚠ AUX DIFFERENT FROM BEFORE: {diverse}")
    rimesso = not diverse
    if a.prova:
        return {"ok": True, "prova": True, "rimesso": rimesso, "aperte": len(porte), "col_segnale": len(col_segnale)}

    doppie = {p: x for p, x in trovate.items() if len(x) > 1}
    porte_mappa = {p: {"aux": x[0]} for p, x in trovate.items() if len(x) == 1}
    # Un ingresso che adesso non ha segnale, o la cui aux non era fra quelle da provare (--aux), tiene l'aux
    # della mappa di prima, se nessun
    # altro ingresso l'ha presa (17 novembre 2026 sera, sezione 9.78: un convertitore spento non deve
    # cancellare la riga del programma).
    mantenute = {}
    try:
        vecchia = json.loads(uscita.read_text(encoding="utf-8")).get("porte", {}) if uscita.exists() else {}
    except (OSError, ValueError):
        vecchia = {}
    prese = {v["aux"] for v in porte_mappa.values()}
    for k, p in enumerate(porte):
        x = (vecchia.get(p) or {}).get("aux")
        if (k not in col_segnale or x not in da_provare) and p not in porte_mappa and x and x not in prese and x in prima:
            porte_mappa[p] = {"aux": x, "dalla_mappa_di_prima": True}
            mantenute[p] = x
            prese.add(x)
    mappa = {
        "creata": datetime.datetime.now().isoformat(timespec="seconds"),
        "atem": a.atem,
        "porte": porte_mappa,
        "aux_allora": {str(x): {"sorgente": prima[x], "nome": nome(prima[x])} for x in sorted(prima)},
        "col_segnale_senza_aux": [porte[k] for k in col_segnale if porte[k] not in trovate],
        "senza_segnale": [p for k, p in enumerate(porte) if k not in col_segnale],
        "ambigue": doppie,
    }
    uscita.write_text(json.dumps(mappa, indent=2, ensure_ascii=False), encoding="utf-8")
    print(f"input map: {len(mappa['porte'])} inputs with their aux → {uscita}")
    for p, v in mappa["porte"].items():
        print(f"  {p} ← AUX {v['aux']} ← {nome(prima[v['aux']])}" + ("  (no signal now: kept from the previous map)" if v.get("dalla_mappa_di_prima") else ""))
    if doppie:
        print("  ⚠ inputs that responded to more than one aux, check them:", doppie)
    return {"ok": True, "rimesso": rimesso, "aux_provate": len(cambiate), "aperte": len(porte),
            "col_segnale": len(col_segnale), "mappate": len(mappa["porte"]), "mantenute": mantenute,
            "porte": {p: v["aux"] for p, v in mappa["porte"].items()},
            "senza_aux": mappa["col_segnale_senza_aux"], "senza_segnale": mappa["senza_segnale"], "ambigue": doppie}


if __name__ == "__main__":
    main()
