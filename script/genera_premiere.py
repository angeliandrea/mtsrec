"""Il montaggio della diretta per Premiere Pro e per gli altri programmi: Final Cut Pro 7 XML ed EDL.

L'FCPXML di genera_fcpxml.py è il formato di Final Cut Pro X: Resolve lo legge, Premiere no.
Premiere importa invece il Final Cut Pro 7 XML (xmeml), formato pubblico documentato da Apple,
e l'EDL CMX 3600. Da qui escono tutti e due, con gli stessi tagli e le stesse dissolvenze.

- XML: una sequenza coi pezzi delle camere sulla traccia V1 e le dissolvenze come Cross Dissolve,
  i file già collegati col loro percorso, i marker della registrazione come marker di sequenza.
  Niente multicamera: in Premiere la si crea dai file col timecode (Crea sequenza sorgente multicamera).
- EDL: una traccia sola, tagli e dissolvenze. I file non sono collegati: Premiere crea clip offline
  da ricollegare coi nomi dei file, che stanno nei commenti. Il timecode dei file è quello dell'ATEM,
  quindi ogni pezzo cade al suo posto.
  ⚠ All'importazione Premiere propone NTSC e 23,976: va scelto il predefinito con la cadenza della
  registrazione, a 25 PAL e 25 fps, altrimenti la sequenza parte da un timecode sbagliato (provato il
  7 novembre). Il titolo dell'EDL dice quale (sezione 9.79). Per ricollegare:
  selezionate tutte le clip offline, Collega oggetto multimediale, Individua, un file: gli altri seguono.
- L'audio segue il video, come nella timeline di Resolve: ogni pezzo porta l'audio stereo del suo file
  (PCM 24 bit a 48 kHz, due canali), su A1 e A2 nell'XML, un canale per traccia come in Final Cut Pro 7, e come AA/V nell'EDL. Tutte le uscite dell'ATEM
  portano lo stesso audio del programma, quindi passando da un file all'altro l'audio continua.
  Aggiunto il 15 novembre 2026: prima c'era solo il video.

Si usa come modulo da progetto_da_registratore.py, con gli stessi dati di scrivi_fcpxml:
camere {n: {"nome", "percorso", "inizio", "durata", "tc"}} e tagli [(fotogramma, n, tipo, durata, nota)],
coi fotogrammi contati dall'inizio della registrazione e le dissolvenze date dal centro.
"""
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import quote

sys.path.insert(0, str(Path(__file__).parent))
from cadenza import Cadenza

CAD = Cadenza()  # la cadenza dei file: la imposta chi chiama, con imposta_cadenza (sezione 9.79)


def imposta_cadenza(c):
    global CAD
    CAD = c


def tc_testo(fr):
    return CAD.testo(fr)


def segmenti(camere, tagli):
    """I pezzi della timeline: camera, inizio e fine in fotogrammi dall'inizio della registrazione,
    e la dissolvenza in entrata. Le durate delle dissolvenze sono ridotte come in genera_fcpxml:
    una dissolvenza non può superare i due pezzi che unisce."""
    tagli = sorted(tagli)
    fine_tutto = max(c["inizio"] + c["durata"] for c in camere.values())
    out = []
    for i, (fr, cam, tipo, dur, nota) in enumerate(tagli):
        c = camere[cam]
        fine = min(tagli[i + 1][0] if i + 1 < len(tagli) else fine_tutto, c["inizio"] + c["durata"])
        if fine <= fr:
            continue
        mix = 0
        if out and tipo == "mix" and dur > 0 and out[-1]["b"] == fr:
            mix = min(dur, 2 * (fr - out[-1]["a"] - 1), 2 * (fine - fr - 1))
            mix = mix if mix > 1 else 0
        out.append({"cam": cam, "a": fr, "b": fine, "mix": mix, "nota": nota})
    return out


def _indice(c, fr):
    """Il fotogramma del file che corrisponde a un punto della timeline."""
    return fr - c["inizio"]


def _rate(padre):
    r = ET.SubElement(padre, "rate")
    # Final Cut Pro 7: la base intera e NTSC vero per le cadenze /1001 (30 e TRUE è 29,97)
    ET.SubElement(r, "timebase").text = str(CAD.base)
    ET.SubElement(r, "ntsc").text = "TRUE" if CAD.ntsc else "FALSE"


def _timecode(padre, fr):
    t = ET.SubElement(padre, "timecode")
    _rate(t)
    ET.SubElement(t, "string").text = tc_testo(fr)
    ET.SubElement(t, "frame").text = str(fr % CAD.giorno)
    ET.SubElement(t, "displayformat").text = "DF" if CAD.drop else "NDF"


def _caratteristiche(padre, larghezza, altezza, campi):
    s = ET.SubElement(padre, "samplecharacteristics")
    _rate(s)
    ET.SubElement(s, "width").text = str(larghezza)
    ET.SubElement(s, "height").text = str(altezza)
    ET.SubElement(s, "anamorphic").text = "FALSE"
    ET.SubElement(s, "pixelaspectratio").text = "square"
    ET.SubElement(s, "fielddominance").text = campi


def _audio_file(padre, canali=2):
    # Come lo scrive Premiere Pro 2026 esportando un nostro MOV (15 novembre): una voce <audio> per
    # canale, mono, con l'etichetta del canale. Con una voce sola a due canali Premiere importava la
    # sequenza senza audio.
    etichette = {1: "left", 2: "right"}
    for n in range(1, canali + 1):
        a = ET.SubElement(padre, "audio")
        s = ET.SubElement(a, "samplecharacteristics")
        ET.SubElement(s, "depth").text = "24"
        ET.SubElement(s, "samplerate").text = "48000"
        ET.SubElement(a, "channelcount").text = "1"
        ET.SubElement(a, "layout").text = "stereo"
        ac = ET.SubElement(a, "audiochannel")
        ET.SubElement(ac, "sourcechannel").text = str(n)
        ET.SubElement(ac, "channellabel").text = etichette.get(n, f"ch{n}")


def _url(percorso):
    # file://localhost/H:/cartella/file.mov, con codificati solo spazi e caratteri strani. Provato da Andrea
    # in Premiere Pro 2026 il 7 novembre: coi due punti come %3a e le parentesi come %28 %29 i file
    # restavano offline, così si collegano da soli.
    return "file://localhost/" + quote(Path(percorso).as_posix(), safe="/:()")


def scrivi_xml_premiere(camere, tagli, uscita, nome, tc_inizio, marker=(), larghezza=1920, altezza=1080,
                        interlacciato=False):
    """La sequenza in Final Cut Pro 7 XML. marker: (fotogramma, colore, nome, nota, durata)."""
    segs = segmenti(camere, tagli)
    campi = "upper" if interlacciato else "none"
    fine = segs[-1]["b"] if segs else 0

    root = ET.Element("xmeml", version="4")
    seq = ET.SubElement(root, "sequence", id="sequence-1")
    ET.SubElement(seq, "name").text = nome
    ET.SubElement(seq, "duration").text = str(fine)
    _rate(seq)
    _timecode(seq, tc_inizio)
    ET.SubElement(seq, "in").text = "-1"
    ET.SubElement(seq, "out").text = "-1"
    media = ET.SubElement(seq, "media")
    video = ET.SubElement(media, "video")
    fmt = ET.SubElement(video, "format")
    _caratteristiche(fmt, larghezza, altezza, campi)
    traccia = ET.SubElement(video, "track")
    con_audio = any(camere[s["cam"]].get("audio_file") for s in segs)
    if con_audio:
        audio = ET.SubElement(media, "audio")
        ET.SubElement(audio, "numOutputChannels").text = "2"
        af = ET.SubElement(audio, "format")
        s_ = ET.SubElement(af, "samplecharacteristics")
        ET.SubElement(s_, "depth").text = "24"
        ET.SubElement(s_, "samplerate").text = "48000"
        # Uscite e tracce come nell'XML che Premiere Pro 2026 esporta da un nostro MOV: due tracce, una
        # per canale, ognuna sulla sua uscita. Con una traccia sola A1 restava vuota (15 novembre).
        uscite = ET.SubElement(audio, "outputs")
        for n in (1, 2):
            g = ET.SubElement(uscite, "group")
            ET.SubElement(g, "index").text = str(n)
            ET.SubElement(g, "numchannels").text = "1"
            ET.SubElement(g, "downmix").text = "0"
            ET.SubElement(ET.SubElement(g, "channel"), "index").text = str(n)
        tracce_a = [ET.SubElement(audio, "track", currentExplodedTrackIndex="0", totalExplodedTrackCount="1",
                                  premiereTrackType="Stereo") for _ in (1, 2)]

    definiti = set()
    dissolvenze = 0
    for i, s in enumerate(segs, 1):
        c = camere[s["cam"]]
        dopo = segs[i]["mix"] if i < len(segs) else 0
        if s["mix"]:
            # la dissolvenza sta a cavallo del taglio, centrata: prima del pezzo che entra
            t = ET.SubElement(traccia, "transitionitem")
            ET.SubElement(t, "start").text = str(s["a"] - s["mix"] // 2)
            ET.SubElement(t, "end").text = str(s["a"] - s["mix"] // 2 + s["mix"])
            ET.SubElement(t, "alignment").text = "center"
            _rate(t)
            e = ET.SubElement(t, "effect")
            ET.SubElement(e, "name").text = "Cross Dissolve"
            ET.SubElement(e, "effectid").text = "Cross Dissolve"
            ET.SubElement(e, "effectcategory").text = "Dissolve"
            ET.SubElement(e, "effecttype").text = "transition"
            ET.SubElement(e, "mediatype").text = "video"
            dissolvenze += 1
        ci = ET.SubElement(traccia, "clipitem", id=f"clipitem-{i}")
        ET.SubElement(ci, "name").text = c["nome"]
        ET.SubElement(ci, "enabled").text = "TRUE"
        ET.SubElement(ci, "duration").text = str(c["durata"])
        _rate(ci)
        # Come Final Cut Pro 7 e Premiere: -1 dove il pezzo comincia o finisce dentro una dissolvenza;
        # in e out sono sempre ai punti di taglio, e le maniglie le prende dal file.
        ET.SubElement(ci, "start").text = "-1" if s["mix"] else str(s["a"])
        ET.SubElement(ci, "end").text = "-1" if dopo else str(s["b"])
        ET.SubElement(ci, "in").text = str(_indice(c, s["a"]))
        ET.SubElement(ci, "out").text = str(_indice(c, s["b"]))
        fid = f"file-{s['cam']}"
        f = ET.SubElement(ci, "file", id=fid)
        if fid not in definiti:
            definiti.add(fid)
            ET.SubElement(f, "name").text = c["nome"]
            ET.SubElement(f, "pathurl").text = _url(c["percorso"])
            _rate(f)
            ET.SubElement(f, "duration").text = str(c["durata"])
            _timecode(f, c.get("tc", 0))
            mf = ET.SubElement(f, "media")
            fm = ET.SubElement(mf, "video")
            _caratteristiche(fm, larghezza, altezza, campi)
            if c.get("audio_file"):
                _audio_file(mf)
        if con_audio and c.get("audio_file"):
            # l'audio del pezzo, collegato al video: taglio netto anche dove il video dissolve.
            # Canale 1 su A1 e canale 2 su A2.
            pezzi_a = []
            for canale, traccia_a in enumerate(tracce_a, 1):
                ca = ET.SubElement(traccia_a, "clipitem", id=f"clipitem-a{canale}-{i}", premiereChannelType="mono")
                ET.SubElement(ca, "name").text = c["nome"]
                ET.SubElement(ca, "enabled").text = "TRUE"
                ET.SubElement(ca, "duration").text = str(c["durata"])
                _rate(ca)
                ET.SubElement(ca, "start").text = str(s["a"])
                ET.SubElement(ca, "end").text = str(s["b"])
                ET.SubElement(ca, "in").text = str(_indice(c, s["a"]))
                ET.SubElement(ca, "out").text = str(_indice(c, s["b"]))
                ET.SubElement(ca, "file", id=fid)
                st = ET.SubElement(ca, "sourcetrack")
                ET.SubElement(st, "mediatype").text = "audio"
                ET.SubElement(st, "trackindex").text = str(canale)
                pezzi_a.append(ca)
            for padre in (ci, *pezzi_a):
                for ref, tipo, ti in ((f"clipitem-{i}", "video", 1), (f"clipitem-a1-{i}", "audio", 1),
                                      (f"clipitem-a2-{i}", "audio", 2)):
                    l = ET.SubElement(padre, "link")
                    ET.SubElement(l, "linkclipref").text = ref
                    ET.SubElement(l, "mediatype").text = tipo
                    ET.SubElement(l, "trackindex").text = str(ti)
                    ET.SubElement(l, "clipindex").text = str(i)
                    if tipo == "audio":
                        ET.SubElement(l, "groupindex").text = "1"

    if con_audio:
        for canale, traccia_a in enumerate(tracce_a, 1):
            ET.SubElement(traccia_a, "enabled").text = "TRUE"
            ET.SubElement(traccia_a, "locked").text = "FALSE"
            ET.SubElement(traccia_a, "outputchannelindex").text = str(canale)

    for fr, colore, testo, nota, dur in marker:
        m = ET.SubElement(seq, "marker")
        ET.SubElement(m, "name").text = testo
        ET.SubElement(m, "comment").text = nota
        ET.SubElement(m, "in").text = str(fr)
        ET.SubElement(m, "out").text = str(fr + dur) if dur > 1 else "-1"
    for s in segs:
        if s["nota"]:
            m = ET.SubElement(seq, "marker")
            ET.SubElement(m, "name").text = "Note"
            ET.SubElement(m, "comment").text = s["nota"]
            ET.SubElement(m, "in").text = str(s["a"])
            ET.SubElement(m, "out").text = "-1"

    ET.indent(root)
    with open(uscita, "wb") as f:
        f.write(b'<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE xmeml>\n')
        f.write(ET.tostring(root, encoding="utf-8"))
    return {"pezzi": len(segs), "dissolvenze": dissolvenze, "file": len(definiti), "audio": con_audio}


def scrivi_edl(camere, tagli, uscita, nome, tc_inizio):
    """L'EDL CMX 3600: una traccia video con l'audio che la segue (AA/V), tagli e dissolvenze, queste solo sul video con
    l'audio in un evento a parte, il timecode di registrazione uguale
    a quello dell'ATEM. Le bobine si chiamano CAM01, CAM02…, il nome del file sta nei commenti."""
    segs = segmenti(camere, tagli)
    bobine = {n: f"CAM{i:02d}" for i, n in enumerate(sorted({s["cam"] for s in segs}), 1)}
    src = lambda c, fr: tc_testo(c.get("tc", 0) + _indice(c, fr))
    rec = lambda fr: tc_testo(tc_inizio + fr)
    titolo = "".join(ch for ch in nome if ch.isascii())[:70]
    righe = [f"TITLE: {titolo}", "FCM: DROP FRAME" if CAD.drop else "FCM: NON-DROP FRAME", ""]
    # In un EDL la dissolvenza comincia a metà durata prima del taglio: lì finisce il pezzo che esce.
    inizio_di = lambda s: s["a"] - s["mix"] // 2
    ch = lambda c: "AA/V " if c.get("audio_file") else "V    "   # AA/V: due canali audio più il video
    ev = 0
    for i, s in enumerate(segs):
        c = camere[s["cam"]]
        fine = inizio_di(segs[i + 1]) if i + 1 < len(segs) and segs[i + 1]["mix"] else s["b"]
        ev += 1
        if s["mix"]:
            p = segs[i - 1]
            cp = camere[p["cam"]]
            a = inizio_di(s)
            # La dissolvenza sta solo sul video e l'audio entra come taglio in un evento a parte, come scrive Premiere
            # nei suoi EDL: una dissolvenza AA/V Premiere Pro 2026 la disegna ma avvisa «EDL import error at event N -
            # skipped» (15 novembre). Il risultato in timeline è lo stesso, audio con un taglio netto a inizio dissolvenza.
            righe.append(f"{ev:03d}  {bobine[p['cam']]:<8} V     C        "
                         f"{src(cp, a)} {src(cp, a)} {rec(a)} {rec(a)}")
            righe.append(f"{ev:03d}  {bobine[s['cam']]:<8} V     D    {s['mix']:03d} "
                         f"{src(c, a)} {src(c, fine)} {rec(a)} {rec(fine)}")
            righe.append(f"* FROM CLIP NAME: {cp['nome']}")
            righe.append(f"* TO CLIP NAME: {c['nome']}")
            if c.get("audio_file"):
                righe.append(f"* SOURCE FILE: {c['nome']}")
                righe.append("")
                ev += 1
                righe.append(f"{ev:03d}  {bobine[s['cam']]:<8} AA    C        "
                             f"{src(c, a)} {src(c, fine)} {rec(a)} {rec(fine)}")
                righe.append(f"* FROM CLIP NAME: {c['nome']}")
        else:
            righe.append(f"{ev:03d}  {bobine[s['cam']]:<8} {ch(c)} C        "
                         f"{src(c, s['a'])} {src(c, fine)} {rec(s['a'])} {rec(fine)}")
            righe.append(f"* FROM CLIP NAME: {c['nome']}")
        righe.append(f"* SOURCE FILE: {c['nome']}")
        righe.append("")
    # newline="" perché su Windows write_text trasformerebbe ogni \n in \r\n e le righe finirebbero in \r\r\n (15 novembre).
    Path(uscita).write_text("\r\n".join(righe) + "\r\n", encoding="ascii", errors="replace", newline="")
    return {"eventi": ev, "dissolvenze": sum(1 for s in segs if s["mix"]), "bobine": bobine}
