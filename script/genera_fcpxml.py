"""Genera un FCPXML 1.10 con dentro la clip multicamera e la timeline dei tagli.

Importato in Resolve produce:
  - una clip multicamera con un angolo per ogni file del multicorder,
    ciascuno al suo sfasamento, e il programma come ultimo angolo
  - una timeline fatta di pezzi della multicamera, ognuno sull'angolo che era
    in onda, con le dissolvenze dove l'ATEM ha fatto un mix

Il formato e' copiato da come Resolve stesso esporta una multicamera:
mc-clip con dentro mc-source angleID. Provato su Resolve 21.1.1.

Si usa come modulo da crea_progetto_resolve.py oppure da solo:
  python genera_fcpxml.py <multicorder.xml> <tagli.csv> <uscita.fcpxml> [--cartella D:\\...]
dove il CSV ha le colonne fotogramma,camera,transizione,durata e camera e' il
numero del file MultiCorderN.
"""
import argparse, csv, sys, uuid
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import quote

sys.path.insert(0, str(Path(__file__).parent))
from cadenza import Cadenza

TB = 25         # la cadenza del multicorder di vMix (crea_progetto_resolve.py); il registratore usa CAD
TC = 3600 * TB  # la multicamera e la timeline partono da 01:00:00:00, come fa Resolve
CAD = Cadenza()  # la cadenza dei file: la imposta chi chiama, con imposta_cadenza (sezione 9.79)


def imposta_cadenza(c):
    global CAD
    CAD = c


def nome_formato(altezza, interlacciato=False):
    """Il nome del formato come lo scrive Final Cut Pro: FFVideoFormat1080p25, 1080i5994, 3840x2160p2997."""
    v = CAD.fps * (2 if interlacciato else 1)
    numero = f"{v:.2f}".replace(".", "").rstrip("0") if CAD.ntsc else str(round(v))
    return f"FFVideoFormat{'3840x2160' if altezza == 2160 else '1080'}{'i' if interlacciato else 'p'}{numero}"
NOMI_TRANSIZIONE = {"mix": "Cross Dissolve"}  # solo Cut e Fade: le altre arrivano gia' come tagli


def t(fotogrammi):
    return CAD.frazione(fotogrammi)


def leggi_multicorder(percorso, cartella=None):
    """Restituisce le camere {numero file: dati} e la durata della timeline del multicorder."""
    r = ET.parse(percorso).getroot()
    camere = {}
    for tr in r.findall("sequence/media/video/track"):
        c = tr.find("clipitem")
        nome = c.findtext("name")
        n = int(nome.split(" ")[0].replace("MultiCorder", ""))
        f = c.find("file")
        p = Path(cartella) / nome if cartella else Path(f.findtext("pathurl").replace("file://localhost/", ""))
        camere[n] = {"n": n, "nome": nome, "percorso": p, "inizio": int(c.findtext("start")),
                     "durata": int(f.findtext("duration")), "angolo": str(uuid.uuid4())}
    return camere, int(r.findtext("sequence/duration"))


def scrivi_fcpxml(camere, tagli, uscita, nome="Edit from ATEM cuts",
                  multicamera="Multicam", programma=1, nomi_angoli=None, tc_inizio=None,
                  larghezza=1920, altezza=1080, interlacciato=False):
    """tagli: lista di (fotogramma, numero file, transizione, durata, nota).

    Il fotogramma e' il punto in cui il pezzo comincia sulla timeline del multicorder.
    Per una dissolvenza e' il centro: Resolve la stende per meta' prima e meta' dopo.
    tc_inizio: timecode della timeline e della multicamera, in fotogrammi; senza, 01:00:00:00.
    mtsrec passa quello dell'ATEM, così la timeline segna l'ora della diretta.
    """
    TC = CAD.da_testo("01:00:00:00") if tc_inizio is None else tc_inizio
    fmt_tc = "DF" if CAD.drop else "NDF"
    nomi_angoli = nomi_angoli or {}
    fine = max(c["inizio"] + c["durata"] for c in camere.values())
    tagli = sorted(tagli)

    root = ET.Element("fcpxml", version="1.10")
    res = ET.SubElement(root, "resources")
    # 1080 o UHD, dal formato della registrazione (sezione 9.74)
    # e dalla cadenza: 1001/30000s il fotogramma a 29,97 (sezione 9.79)
    formato = {"fieldOrder": "upper first"} if interlacciato else {}
    ET.SubElement(res, "format", id="r0", name=nome_formato(altezza, interlacciato),
                  frameDuration=CAD.frazione(1), width=str(larghezza), height=str(altezza), **formato)

    # prima le camere, poi il programma: l'ordine degli angoli e' quello della multicamera
    # c["tc"] e' il timecode del primo fotogramma del file: zero per il multicorder di vMix,
    # quello dell'ATEM per i file di mtsrec. c["audio"] falso per i file senza audio.
    ordine = [n for n in sorted(camere) if n != programma] + ([programma] if programma in camere else [])
    for i, n in enumerate(ordine, 1):
        c = camere[n]
        c["asset"] = f"a{i}"
        audio = {"hasAudio": "1", "audioSources": "1", "audioChannels": "2"} if c.get("audio", True) else {"hasAudio": "0"}
        asset = ET.SubElement(res, "asset", id=c["asset"], name=c["nome"], start=t(c.get("tc", 0)),
                              duration=t(c["durata"]), hasVideo="1", format="r0", **audio)
        ET.SubElement(asset, "media-rep", kind="original-media",
                      src="file://localhost/" + quote(c["percorso"].as_posix(), safe="/:"))

    media = ET.SubElement(res, "media", id="m1", name=multicamera)
    mc = ET.SubElement(media, "multicam", format="r0", tcStart=t(TC), tcFormat=fmt_tc)
    for n in ordine:
        c = camere[n]
        nome_angolo = nomi_angoli.get(n) or ("PROGRAM" if n == programma else f"MultiCorder{n}")
        ang = ET.SubElement(mc, "mc-angle", name=nome_angolo, angleID=c["angolo"])
        if c["inizio"] > 0:
            ET.SubElement(ang, "gap", name="Gap", offset="0s", start=t(TC), duration=t(c["inizio"]))
        # inizio negativo: la clip comincia prima della multicamera, come quella dell'HyperDeck
        # che parte un attimo prima delle tracce; se ne usa solo la parte da lì in avanti
        salta = max(0, -c["inizio"])
        ET.SubElement(ang, "asset-clip", ref=c["asset"], name=c["nome"], format="r0", tcFormat=fmt_tc,
                      offset=t(max(0, c["inizio"])), start=t(c.get("tc", 0) + salta), duration=t(c["durata"] - salta))

    lib = ET.SubElement(root, "library")
    ev = ET.SubElement(lib, "event", name=nome)
    pr = ET.SubElement(ev, "project", name=nome)
    seq = ET.SubElement(pr, "sequence", format="r0", tcStart=t(TC), tcFormat=fmt_tc, duration=t(fine))
    spine = ET.SubElement(seq, "spine")

    if tagli and tagli[0][0] > 0:
        ET.SubElement(spine, "gap", name="Gap", offset=t(TC), start=t(TC), duration=t(tagli[0][0]))
    dissolvenze, note = 0, []
    for i, (fr, cam, tipo, dur, nota) in enumerate(tagli):
        fine_pezzo = tagli[i + 1][0] if i + 1 < len(tagli) else fine
        if i > 0 and tipo in NOMI_TRANSIZIONE and dur > 0:
            # la dissolvenza sta a cavallo del taglio: non puo' superare i due pezzi che unisce
            prima = fr - tagli[i - 1][0]
            dopo = fine_pezzo - fr
            dur = min(dur, 2 * (prima - 1), 2 * (dopo - 1))
        if i > 0 and tipo in NOMI_TRANSIZIONE and dur > 1:
            ET.SubElement(spine, "transition", name=NOMI_TRANSIZIONE[tipo],
                          offset=t(TC + fr - dur // 2), duration=t(dur))
            dissolvenze += 1
        clip = ET.SubElement(spine, "mc-clip", ref="m1", name=multicamera,
                             offset=t(TC + fr), start=t(TC + fr), duration=t(fine_pezzo - fr))
        ET.SubElement(clip, "mc-source", angleID=camere[cam]["angolo"], srcEnable="all")
        if nota:
            # Resolve ignora i marker dentro una mc-clip: li mette crea_progetto_resolve.py dopo
            note.append((fr, nota))

    ET.indent(root)
    with open(uscita, "wb") as f:
        f.write(b'<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE fcpxml>\n')
        f.write(ET.tostring(root, encoding="utf-8"))
    return {"pezzi": len(tagli), "dissolvenze": dissolvenze, "angoli": len(ordine), "fotogrammi": fine,
            "note": note}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("multicorder")
    ap.add_argument("tagli")
    ap.add_argument("uscita")
    ap.add_argument("--cartella")
    ap.add_argument("--programma", type=int, default=1)
    ap.add_argument("--nome", default="Edit from ATEM cuts")
    ap.add_argument("--multicamera", default="Multicam")
    a = ap.parse_args()

    camere, _ = leggi_multicorder(a.multicorder, a.cartella)
    tagli = []
    with open(a.tagli, encoding="utf-8") as f:
        for riga in csv.DictReader(f):
            tagli.append((int(riga["fotogramma"]), int(riga["camera"]),
                          riga["transizione"].strip().lower(), int(riga.get("durata") or 0), ""))
    r = scrivi_fcpxml(camere, tagli, a.uscita, a.nome, a.multicamera, a.programma)
    print(f"{r['pezzi']} clips, {r['dissolvenze']} dissolves, {r['angoli']} angles -> {a.uscita}")


if __name__ == "__main__":
    main()
