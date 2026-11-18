"""Le clip degli HyperDeck accanto a una registrazione: cosa c'è, quando, e quale camera è (sezione 9.72).

Decisione di Andrea, 17 novembre 2026: gli HyperDeck non sono tracce del registratore. Se in regia ce ne sono,
chi registra copia le loro clip dalle schede in una cartella e la indica al registratore: il progetto Resolve,
l'FCPXML, l'XML e l'EDL per Premiere le usano come angoli in più, coi tagli dell'ATEM. Gli HyperDeck scrivono
nei file il timecode dell'ATEM che ricevono dall'SDI (verificato il 30 ottobre, sezione 9.48), quindi si
allineano alle tracce DeckLink per timecode, senza tarature.

    clip_hyperdeck.py --registrazione <cartella della registrazione> --cartella <cartella con le clip>

Stampa un JSON per la pagina: la finestra della registrazione, gli ingressi dell'ATEM coi loro nomi e, per
ogni clip, timecode, durata, quanto si sovrappone alla registrazione e la camera proposta. La proposta viene dal
nome: il prefisso che si imposta sull'HyperDeck («Camera 3_0001.mov») o la cartella in cui stanno le sue clip
(«CAM 3\\…»), confrontati coi nomi lunghi e corti degli ingressi dell'ATEM. Chi registra la conferma nella pagina.
"""
import argparse, json, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import tagli_da_atem
from cadenza import Cadenza

ESTENSIONI = {".mov", ".mp4", ".mxf"}
PROGRAMMA = {"program", "programma", "pgm", "pgmout", "programout", "me1", "me1program", "master"}


def normalizza(s):
    return re.sub(r"[^a-z0-9]", "", s.lower())


def prefisso(p):
    """Il nome del deck dal nome del file: «Camera 3_0001» → «Camera 3». Toglie numero di clip e data."""
    s = re.sub(r"[ _-]*\d{4}-\d{2}-\d{2}.*$", "", p.stem)
    return re.sub(r"[ _-]*C?\d{3,5}$", "", s).strip(" _-") or p.stem


def proposta(nomi_da_provare, ingressi):
    """L'ingresso dell'ATEM il cui nome lungo o corto coincide con uno dei nomi, «program» o ""."""
    for nome in nomi_da_provare:
        k = normalizza(nome)
        if not k:
            continue
        if k in PROGRAMMA:
            return "program"
        for n, (lungo, corto) in ingressi.items():
            if k in (normalizza(lungo), normalizza(corto)):
                return str(n)
        # «CAM3» per «Camera 3»: stesse lettere iniziali e stesso numero
        m = re.fullmatch(r"([a-z]+)(\d+)", k)
        if m:
            for n, (lungo, corto) in ingressi.items():
                for x in (normalizza(lungo), normalizza(corto)):
                    mx = re.fullmatch(r"([a-z]+)(\d+)", x)
                    if mx and mx.group(2) == m.group(2) and (mx.group(1).startswith(m.group(1)) or m.group(1).startswith(mx.group(1))):
                        return str(n)
    return ""


def finestra(registrazione):
    """Inizio e durata della registrazione, dalle tracce DeckLink."""
    from progetto_da_registratore import sonda
    tracce = sorted(Path(registrazione).glob("*.mov"))
    if not tracce:
        sys.exit("no tracks in the recording folder")
    tc, durata, _ = sonda(tracce[0])
    return tc, durata


def elenca(registrazione, cartella):
    from progetto_da_registratore import sonda
    import progetto_da_registratore
    registrazione, cartella = Path(registrazione), Path(cartella)
    # la cadenza della registrazione (sezione 9.79): sonda() legge i timecode con quella
    TB = progetto_da_registratore.TB = Cadenza.da_registrazione(registrazione)
    if not cartella.is_dir():
        sys.exit(f"folder not found: {cartella}")
    inizio, durata = finestra(registrazione)
    ingressi = {}
    if (registrazione / "atem.jsonl").exists():
        _, nomi, _ = tagli_da_atem.leggi(registrazione / "atem.jsonl", TB)
        ingressi = {n: v for n, v in nomi.items() if 0 < n < 1000}
    giorno = TB.giorno
    clip = []
    for p in sorted(x for x in cartella.rglob("*") if x.suffix.lower() in ESTENSIONI and x.is_file()):
        if registrazione.resolve() in p.resolve().parents:
            continue                                   # le tracce del registratore non sono clip HyperDeck
        voce = {"file": str(p.relative_to(cartella)), "deck": prefisso(p)}
        try:
            tc, n, audio = sonda(p)
        except Exception:
            voce.update(errore="no timecode or unreadable")
            clip.append(voce)
            continue
        da = (tc - inizio) % giorno
        if da > giorno // 2:
            da -= giorno                               # comincia prima della registrazione
        comune = max(0, min(da + n, durata) - max(da, 0))
        cartella_deck = p.parent.name if p.parent != cartella else ""
        voce.update(tc=tagli_da_atem.tc_testo(tc, TB), fine=tagli_da_atem.tc_testo(tc + n, TB), fotogrammi=n,
                    scarto=da, comune=comune, audio=audio,
                    proposta=proposta([voce["deck"], cartella_deck], ingressi))
        clip.append(voce)
    return {"registrazione": str(registrazione), "cartella": str(cartella),
            "inizio": tagli_da_atem.tc_testo(inizio, TB), "fotogrammi": durata,
            "ingressi": [{"n": n, "nome": v[0], "corto": v[1]} for n, v in sorted(ingressi.items())],
            "clip": clip}


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--registrazione", required=True)
    ap.add_argument("--cartella", required=True)
    a = ap.parse_args()
    try:
        print(json.dumps(elenca(a.registrazione, a.cartella), ensure_ascii=False))
    except SystemExit as e:
        print(json.dumps({"errore": str(e.code)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
