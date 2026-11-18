"""Dal registro dell'ATEM (atem.jsonl di registro_atem.py) la lista dei tagli del M/E 1, in timecode.

La regola è quella misurata nella sezione 9.48:
  - taglio: arriva PrgI; l'ultimo fotogramma SDI arrivato in quel momento ha il timecode X,
    la nuova immagine compare su X+2
  - dissolvenza: stesso calcolo sul primo TrPs, la durata è data dai fotogrammi rimasti + 1;
    il PrgI che arriva alla fine del mix non è un taglio
X si prende dagli arrivi dei fotogrammi scritti nel CSV di una traccia del registratore, che usa
lo stesso orologio di Windows del registro. ⚠ 31 ottobre: l'arrivo è quello SUL CAVO, hw_us, uguale per
tutte le schede perché è il ritmo del segnale dell'ATEM, e non quello al programma, hw_us + ritardo_us,
che dipende dal driver di ogni scheda: la Duo 2.2 consegna 2 ms dopo la Duo 2.1 e, col PrgI che arriva
2,0-2,3 ms dopo l'inizio del fotogramma sul cavo, bastava a spostare tutti i tagli di uno. Il confronto
si fa a metà fotogramma, così il messaggio ha una ventina di millisecondi di margine da tutte e due le
parti. Provato su dieci registrazioni con stacchi barre-nero: 96 controlli esatti su 96, qualunque
scheda faccia da riferimento. Senza CSV si prende dal Time che l'ATEM mette nello
stesso pacchetto del taglio, meno uno.
⚠ Sezione 9.52: quel Time porta già il fotogramma successivo, X+1, mentre i Time regolari, ogni
10 fotogrammi, portano quello che sta arrivando. Contare dall'ultimo Time ricevuto dava quindi un
fotogramma di troppo su tutti i tagli.

Dallo stato iniziale prende anche i nomi degli ingressi (InPr) e lo stile di transizione (TrSS).

    py -3.13 tagli_da_atem.py <atem.jsonl> [--da 22:10:00:00 --a 22:11:00:00]
"""
import argparse, bisect, csv, json, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from cadenza import Cadenza, righe_csv_traccia

FPS = 25                     # le funzioni accettano anche una Cadenza (sezione 9.79)
STILI = {0: "mix", 1: "dip", 2: "wipe", 3: "DVE", 4: "sting"}


def tc_da_testo(s, fps=FPS):
    return Cadenza.di(fps).da_testo(s)


def tc_testo(n, fps=FPS):
    return Cadenza.di(fps).testo(n)


def arrivi_da_csv(percorso, fps=FPS):
    """Dal CSV di una traccia: (istante in µs, timecode) dei fotogrammi col RP188. L'istante è l'inizio
    del fotogramma sul cavo meno mezzo fotogramma: il PrgI arriva ~2 ms dopo l'inizio di X e cade così
    a metà fra due confronti invece che sul bordo.

    Si tengono tutti i fotogrammi col segnale a partire dal primo RP188 letto, non solo quelli col RP188:
    a 50, 59.94 e 60p l'ATEM lo manda su un fotogramma su due (sonda del 17 novembre 2026, sezione 9.80)
    e col solo RP188 un taglio arrivato dopo il fotogramma «stimato» veniva datato uno prima. Il
    timecode degli altri è quello scritto nel file e l'istante viene dalla scheda. Senza nessun RP188
    la traccia non dà arrivi: il suo timecode sarebbe quello del computer e non quello dell'ATEM."""
    # Negli interlacciati l'ATEM lavora a campi e il PrgI arriva ~2 ms dopo l'inizio di uno dei due campi,
    # anche del secondo, a 0,55 fotogrammi: lo stacco cade lo stesso all'inizio del fotogramma dopo X+1.
    # Il margine è allora mezzo campo, un quarto di fotogramma (prova del 17 novembre 2026, sezione 9.80)
    c = Cadenza.di(fps)
    mezzo = 1e6 / c.fps / (4 if c.interlacciato else 2)
    out = []
    agganciato = False
    for r in righe_csv_traccia(percorso):
        agganciato = agganciato or (r["tc"] and r["tc_dal_segnale"] == "1")
        if agganciato and r["tc"] and r["segnale"] == "1":
            out.append((int(r["hw_us"]) - mezzo, tc_da_testo(r["tc"], fps)))
    return sorted(set(out))


def leggi(percorso, fps=FPS, arrivi=None):
    """Restituisce (tagli, nomi, programma_a).

    tagli: lista di dict {tc, tipo «cut» o «mix», durata, sorgente, nota} in ordine di tempo.
    nomi: {numero ingresso ATEM: (nome lungo, nome corto)}.
    programma_a(tc): la sorgente in onda sul M/E 1 a quel timecode.
    arrivi: quelli di arrivi_da_csv; senza, si usa solo la rete.
    """
    righe = [json.loads(r) for r in open(percorso, encoding="utf-8")]
    comandi = [r for r in righe if "cmd" in r]
    cad = Cadenza.di(fps)
    periodo = 1e9 / cad.fps

    # il Time di ogni pacchetto, e quelli regolari per i pacchetti che non ne hanno
    nel_pacchetto, regolari = {}, []
    for r in comandi:
        if r["cmd"] == "Time":
            b = bytes.fromhex(r["hex"])
            # Il Time dell'ATEM: ore, minuti, secondi, fotogrammi; col drop-frame l'etichetta (sezione 9.79)
            x = cad.da_etichetta(b[0], b[1], b[2], b[3])
            if x is not None:
                nel_pacchetto[r["perf_ns"]] = x
    eventi = {r["perf_ns"] for r in comandi if r["cmd"] in ("PrgI", "PrvI", "TrPs", "TlIn", "TlSr")}
    regolari = sorted((t, v) for t, v in nel_pacchetto.items() if t not in eventi)
    if not regolari:
        sys.exit("no Time command in the log: the ATEM did not send timecode")
    istanti = [t for t, _ in regolari]
    istanti_arrivo = [a for a, _ in arrivi or []]

    def in_arrivo(t):
        """Il timecode X dell'ultimo fotogramma arrivato all'istante t, e da quale fonte."""
        rete = nel_pacchetto[t] - 1 if t in nel_pacchetto else None
        if rete is None:
            i = max(bisect.bisect_right(istanti, t) - 1, 0)
            rete = regolari[i][1] + int((t - regolari[i][0]) // periodo)
        i = bisect.bisect_right(istanti_arrivo, t / 1000) - 1
        if i < 0 or t / 1000 - istanti_arrivo[i] > 2 * periodo / 1000:
            return rete, ""                              # nessun fotogramma vicino: vale la rete
        x = arrivi[i][1]
        return x, "" if x == rete else f"⚠ network and frames disagree: {rete - x:+d}"

    nomi, tagli = {}, []
    programma = anteprima = None
    iniziale = None                                      # in onda prima del primo taglio
    stile, in_mix, pronto = 0, False, False
    for r in comandi:
        b = bytes.fromhex(r["hex"])
        c = r["cmd"]
        if c == "InPr" and len(b) >= 26:
            nomi[struct.unpack(">H", b[0:2])[0]] = (b[2:22].split(b"\0")[0].decode("utf-8", "replace"),
                                                     b[22:26].split(b"\0")[0].decode("utf-8", "replace"))
        elif c == "TrSS" and b[0] == 0:
            stile = b[1]
        elif c == "PrvI" and b[0] == 0:
            anteprima = struct.unpack(">H", b[2:4])[0]
        elif c == "InCm":
            pronto = True
            iniziale = programma
        elif c == "PrgI" and b[0] == 0:
            s = struct.unpack(">H", b[2:4])[0]
            if pronto and s != programma and not in_mix:
                x, nota = in_arrivo(r["perf_ns"])
                tagli.append({"tc": x + 2, "tipo": "cut", "durata": 0, "sorgente": s,
                              "nota": nota, "perf_ns": r["perf_ns"]})
            elif pronto and in_mix and s != programma:
                tagli[-1]["nota"] += f" ⚠ mix should end on {programma}, ends on {s}"
            in_mix = False                               # PrgI arriva a mix finito
            programma = s
        elif c == "TrPs" and b[0] == 0 and pronto:
            if b[1] and not in_mix:
                in_mix = True
                x, nota = in_arrivo(r["perf_ns"])
                if stile != 0:
                    nota = f"{STILI.get(stile, stile)} transition, edited as mix " + nota
                tagli.append({"tc": x + 2, "tipo": "mix", "durata": b[2] + 1,
                              "sorgente": anteprima, "perf_ns": r["perf_ns"], "nota": nota.strip()})
                programma = anteprima                    # da qui la sorgente in onda è quella nuova
            elif not b[1]:
                in_mix = False

    def programma_a(tc):
        s = iniziale
        for t in tagli:
            if t["tc"] <= tc:
                s = t["sorgente"]
        return s

    return tagli, nomi, programma_a


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("registro")
    ap.add_argument("--da")
    ap.add_argument("--a")
    ap.add_argument("--csv", help="CSV of a recorder track, for the frame arrival times")
    a = ap.parse_args()
    tagli, nomi, programma_a = leggi(a.registro, arrivi=arrivi_da_csv(a.csv) if a.csv else None)
    da = tc_da_testo(a.da) if a.da else None
    fino = tc_da_testo(a.a) if a.a else None
    nome = lambda s: nomi.get(s, (str(s), ""))[0]
    if da is not None:
        print("on air at start:", nome(programma_a(da)))
    for t in tagli:
        if (da is None or t["tc"] >= da) and (fino is None or t["tc"] < fino):
            print(tc_testo(t["tc"]), t["tipo"], t["durata"] or "", "→", nome(t["sorgente"]), t["nota"])


if __name__ == "__main__":
    main()
