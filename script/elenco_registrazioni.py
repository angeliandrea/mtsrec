"""Le registrazioni nella cartella di destinazione, per la pagina Review del registratore (beta 12, sezione 9.93).

    py -3.13 elenco_registrazioni.py --cartella "H:\\Recordings"

Stampa un JSON su una riga: {"cartella": ..., "registrazioni": [...]}, la più recente prima.
Una registrazione è una sottocartella con registrazione.json, quella che il registratore scrive allo STOP;
una cartella con «RECORDING IN PROGRESS.txt», o con dei MOV col loro CSV e senza registrazione.json, è una
registrazione interrotta.
SOLA LETTURA: registrazione.json, rapporto.txt, marker.csv, atem-tagli.json o tagli.csv, progetto.json,
la prima riga di atem.jsonl e le grandezze dei file. Non apre i MOV.
"""
import argparse, json, os, re, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # il Python del pacchetto non guarda la cartella dello script
from cadenza import Cadenza

IN_CORSO = ("RECORDING IN PROGRESS.txt", "REGISTRAZIONE IN CORSO.txt")
RIGA = re.compile(r"^(.*?): received (\d+) written (\d+) filled (\d+) black (\d+) discarded (\d+) dropped in queue (\d+)")
CODA = re.compile(r"max queue (\d+)")


def righe_csv(p):
    try:
        return max(0, sum(1 for r in p.open(encoding="utf-8", errors="replace") if r.strip()) - 1)
    except OSError:
        return None


def dal_csv(d):
    """Per una registrazione senza registrazione.json, cioè interrotta: timecode del primo e dell'ultimo
    fotogramma scritto e durata, dal CSV dei fotogrammi del primo MOV. Legge l'inizio e gli ultimi 8 KB."""
    csv = sorted(p for p in d.glob("*.csv") if (d / (p.stem + ".mov")).exists())
    if not csv:
        return None
    scritto = lambda r: r.rstrip().endswith(";scritto") and r.count(";") >= 8
    primo = None
    with csv[0].open(encoding="utf-8", errors="replace") as f:
        for k, r in enumerate(f):
            if scritto(r):
                primo = r.split(";")
                break
            if k > 5000:
                break
    if not primo:
        return None
    with csv[0].open("rb") as f:
        f.seek(0, 2)
        f.seek(max(0, f.tell() - 8192))
        coda = f.read().decode("utf-8", "replace").splitlines()
    ultimo = next((r.split(";") for r in reversed(coda) if scritto(r)), None)
    if not ultimo:
        return None
    try:
        durata = (int(ultimo[3]) - int(primo[3])) / 1e6
    except ValueError:
        durata = None
    return primo[0].strip('"'), ultimo[0].strip('"'), durata


def una(d):
    reg = d / "registrazione.json"
    # interrotta: il segno della registrazione in corso è rimasto, o ci sono MOV del registratore senza
    # registrazione.json (la chiusura pulita lo scrive sempre allo STOP; la riparazione toglie il segno).
    # Del registratore vuol dire col CSV dei fotogrammi accanto: una cartella di clip copiate non lo ha.
    interrotta = any((d / n).exists() for n in IN_CORSO)
    if not reg.exists() and not interrotta:
        nomi = {n.lower() for n in os.listdir(d)}
        interrotta = any(n.endswith(".mov") and n[:-4] + ".csv" in nomi for n in nomi)
    if not reg.exists() and not interrotta:
        return None
    # riparata: il segno non c'è più e gli originali sono in «interrotti» (ripara.h); se no aspetta l'apertura
    riparata = interrotta and not any((d / n).exists() for n in IN_CORSO) and (d / "interrotti").is_dir()
    voce = {"nome": d.name, "percorso": str(d), "interrotta": interrotta, "riparata": riparata}
    j = {}
    if reg.exists():
        try:
            j = json.loads(reg.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            j = {}
    cad = Cadenza.da_registrazione(d)
    voce.update({"modo": j.get("modo", ""), "inizio": j.get("inizio", ""), "fine": j.get("fine", ""),
                 "tracce": len(j.get("tracce", [])), "hyperdeck": bool(j.get("hyperdeck"))})
    durata = None
    try:
        if voce["inizio"] and voce["fine"]:
            durata = (cad.da_testo(voce["fine"]) - cad.da_testo(voce["inizio"])) % cad.giorno
    except Exception:
        durata = None
    voce["durata_s"] = None if durata is None else round(durata / cad.fps, 2)
    if not reg.exists():
        try:
            c = dal_csv(d)
        except OSError:
            c = None
        if c:
            voce["inizio"], voce["fine"] = c[0], c[1]
            voce["durata_s"] = None if c[2] is None else round(c[2], 2)
    # grandezza e prima creazione dei file
    peso, mov, creato, pesi = 0, 0, None, {}
    try:
        for e in os.scandir(d):
            if e.is_file():
                st = e.stat()
                peso += st.st_size
                if e.name.lower().endswith(".mov"):
                    mov += 1
                    pesi[e.name[:-4]] = st.st_size
                    creato = st.st_ctime if creato is None else min(creato, st.st_ctime)
    except OSError:
        pass
    voce.update({"byte": peso, "mov": mov})
    # l'ora di partenza: la prima riga del registro dell'ATEM, se c'è, altrimenti il primo MOV creato
    inizio = None
    try:
        with (d / "atem.jsonl").open(encoding="utf-8") as f:
            inizio = json.loads(f.readline()).get("inizio_epoch")
    except (OSError, ValueError, AttributeError):
        pass
    inizio = inizio or creato or (reg.stat().st_mtime if reg.exists() else d.stat().st_mtime)
    voce["avvio"] = time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(inizio))
    voce["_ordine"] = inizio
    # fotogrammi persi come nella pagina: «dropped in queue»; e quelli neri, senza segnale
    persi = neri = None
    conti = {}
    try:
        for r in (d / "rapporto.txt").open(encoding="utf-8", errors="replace"):
            m = RIGA.match(r)
            if m:
                persi = (persi or 0) + int(m.group(7))
                neri = (neri or 0) + int(m.group(5))
                c = CODA.search(r)
                conti[m.group(1)] = {"scritti": int(m.group(3)), "riempiti": int(m.group(4)), "neri": int(m.group(5)),
                                     "persi": int(m.group(7)), "coda_max": int(c.group(1)) if c else None}
    except OSError:
        pass
    voce.update({"persi": persi, "neri": neri, "marker": righe_csv(d / "marker.csv")})
    # le tracce una per una, per la tabella di Review: nell'ordine di registrazione.json, o dei file se manca
    nomi = [t.get("file", "") for t in j.get("tracce", [])] or sorted(pesi)
    porte = {t.get("file", ""): t.get("porta", "") for t in j.get("tracce", [])}
    voce["dettaglio"] = [dict({"file": n + ".mov", "byte": pesi.get(n)},
                              **(conti.get(n) or conti.get(porte.get(n, "")) or {})) for n in nomi if n]
    # i tagli dell'ATEM: quelli dal vivo (beta 12), o quelli del progetto
    tagli = dissolvenze = None
    try:
        t = json.loads((d / "atem-tagli.json").read_text(encoding="utf-8")).get("tagli", [])
        tagli = sum(1 for x in t if x.get("tipo") == "cut")
        dissolvenze = sum(1 for x in t if x.get("tipo") == "mix")
    except (OSError, ValueError):
        try:
            tipi = [r.split(";")[1] for r in (d / "tagli.csv").open(encoding="utf-8", errors="replace") if r.count(";") >= 2][1:]
            tagli, dissolvenze = tipi.count("cut"), tipi.count("mix")
        except OSError:
            pass
    voce.update({"tagli": tagli, "dissolvenze": dissolvenze, "registro_atem": (d / "atem.jsonl").exists()})
    # il progetto Resolve
    progetto, testo = "none", ""
    try:
        p = json.loads((d / "progetto.json").read_text(encoding="utf-8"))
        progetto, testo = ("built" if p.get("ok") else "failed"), p.get("testo", "")
    except (OSError, ValueError):
        if any(e.suffix.lower() == ".drp" for e in d.iterdir() if e.is_file()):
            progetto = "built"
    voce.update({"progetto": progetto, "progetto_testo": testo,
                 "premiere": (d / "Edit - ATEM cuts - Premiere.xml").exists()})
    return voce


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--cartella", required=True)
    ap.add_argument("--quante", type=int, default=300)
    a = ap.parse_args()
    radice = Path(a.cartella)
    if not radice.is_dir():
        print(json.dumps({"cartella": a.cartella, "errore": "folder not found", "registrazioni": []}, ensure_ascii=False))
        return 1
    v = []
    try:
        figli = [e for e in os.scandir(radice) if e.is_dir()]
    except OSError as e:
        print(json.dumps({"cartella": a.cartella, "errore": str(e), "registrazioni": []}, ensure_ascii=False))
        return 1
    for e in figli:
        try:
            x = una(Path(e.path))
        except OSError:
            x = None
        if x:
            v.append(x)
    v.sort(key=lambda x: x["_ordine"], reverse=True)
    for x in v:
        del x["_ordine"]
    print(json.dumps({"cartella": str(radice), "registrazioni": v[:a.quante], "tutte": len(v)}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
