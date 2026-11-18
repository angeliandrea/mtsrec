"""Il rapporto di prova per i beta tester (sezione 9.68, deciso da Andrea il 17 novembre 2026).

Uno zip di testo, pochi KB, che il tester vede prima di mandarlo e allega a mano a una mail. Dentro:
versione, Windows, processore, schede video, DeckLink, dischi e velocità misurate, il controllo della
macchina, e per le ultime registrazioni durata, fotogrammi scritti e persi per traccia, timecode dal
segnale, carico del processore, tagli, marker e progetto Resolve. Dal 17 novembre sera (sezione 9.77)
anche le versioni di Resolve e WebView2, la mappa delle porte e le ultime righe dei registri della sessione
(cartella logs), che prima mancavano: un registratore chiuso da solo non lasciava una riga sul perché. Mai dentro: video, indirizzi IP,
nomi del computer o dell'utente, percorsi, nomi delle registrazioni o delle sorgenti (che possono essere
nomi di persone). Un identificativo casuale della macchina distingue i rapporti.

    rapporto_prova.py --versione V [--registrazione CARTELLA] [--uscita FILE.zip]

Senza --uscita stampa in JSON l'anteprima, cioè il testo che finirà nello zip: {"testo": …, "file": […]}.
Con --uscita scrive lo zip e stampa {"file": percorso, "nome": nome del file}.
Le note del tester, se ci sono, stanno in «nota rapporto.txt» nella cartella dei dati del registratore.
"""
import argparse, csv, datetime, io, json, os, re, sys, uuid, zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from cadenza import Cadenza, righe_csv_traccia

DATI = Path(os.environ.get("USERPROFILE", ".")) / "mtsrec"
if not DATI.exists() and DATI.with_name("WinISORecorder").exists():   # fino alla 0.9 beta 5
    DATI = DATI.with_name("WinISORecorder")
MAX_REGISTRAZIONI = 5
# I valori interni del controllo della macchina restano in italiano (sezione 9.69): nel rapporto si traducono.
PAROLE = {"interno": "internal", "rete": "network", "rimovibile": "removable", "verde": "ok", "giallo": "warning",
          "rosso": "problem", "pronto": "ready", "indicazioni": "ready, with suggestions", "garanzie": "not guaranteed",
          "no": "not suitable", "consigliato": "recommended", "necessario": "required"}


# Il codec col suo flusso nominale: DNxHD in HD, DNxHR in UHD (sezione 9.74, formato.h).
CODEC = {728: "DNxHR HQ", 481: "DNxHR SQ", 149: "DNxHR LB"}


def codec(m):
    try:
        return CODEC.get(int(m), f"DNxHD {m}")
    except (TypeError, ValueError):
        return f"DNxHD {m}"


def en(v):
    return PAROLE.get(v, v) if isinstance(v, str) else v


# ------------------------------------------------------------------ cosa si toglie
class Pulitore:
    """Toglie da un testo indirizzi, nomi del computer e dell'utente, percorsi e nomi scelti dal tester."""

    def __init__(self):
        self.coppie = []
        for var, segno in (("USERNAME", "<user>"), ("COMPUTERNAME", "<computer>"), ("USERDOMAIN", "<domain>")):
            v = os.environ.get(var, "")
            if len(v) >= 3:
                self.coppie.append((v, segno))
        home = os.environ.get("USERPROFILE", "")
        if home:
            self.coppie.append((home, "<home>"))

    def aggiungi(self, testo, segno):
        if testo and len(testo) >= 2:
            self.coppie.append((testo, segno))

    def __call__(self, s):
        if not isinstance(s, str):
            return s
        # i più lunghi prima: un percorso contiene il nome dell'utente
        for v, segno in sorted(self.coppie, key=lambda c: -len(c[0])):
            s = re.sub(re.escape(v), segno, s, flags=re.I)
        s = re.sub(r"\b\d{1,3}(?:\.\d{1,3}){3}(?:/\d{1,2})?\b", "<IP>", s)
        s = re.sub(r"\b[0-9a-f]{2}(?:[:-][0-9a-f]{2}){5}\b", "<MAC>", s, flags=re.I)
        # un percorso va fino all'ultima barra della riga anche attraverso gli spazi, poi fino al primo spazio
        s = re.sub(r"[A-Za-z]:\\(?:[^\\\r\n\"';|]*\\)*[^\s\"';|]*", "<path>", s)
        s = re.sub(r"\\\\(?:[^\\\r\n\"';|]*\\)*[^\s\"';|]+", "<path>", s)
        return s


def pulisci_json(o, p):
    if isinstance(o, dict):
        return {k: pulisci_json(v, p) for k, v in o.items()}
    if isinstance(o, list):
        return [pulisci_json(v, p) for v in o]
    return p(o)


# ------------------------------------------------------------------ letture
def leggi_json(f):
    try:
        return json.loads(Path(f).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


def leggi_testo(f):
    try:
        return Path(f).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def identificativo():
    """Casuale, scelto una volta per macchina: distingue i rapporti senza dire di chi sono."""
    f = DATI / "report id.txt"
    v = leggi_testo(f).strip()
    if not re.fullmatch(r"[0-9a-f]{8}", v):
        v = uuid.uuid4().hex[:8]
        try:
            DATI.mkdir(parents=True, exist_ok=True)
            f.write_text(v + "\n", encoding="utf-8")
        except OSError:
            pass
    return v


def versione_file(percorso):
    """La versione scritta dentro un .exe di Windows, «21.2.0 build 13»; vuoto se non c'è."""
    try:
        import ctypes
        v = ctypes.windll.version
        n = v.GetFileVersionInfoSizeW(str(percorso), None)
        if not n:
            return ""
        buf = ctypes.create_string_buffer(n)
        if not v.GetFileVersionInfoW(str(percorso), 0, n, buf):
            return ""
        p, l = ctypes.c_void_p(), ctypes.c_uint()
        if not v.VerQueryValueW(buf, "\\", ctypes.byref(p), ctypes.byref(l)):
            return ""
        ms, ls = ctypes.cast(p, ctypes.POINTER(ctypes.c_uint32))[2:4]
        # «build» e non un quarto punto: quattro numeri col punto il pulitore li prende per un indirizzo IP
        return f"{ms >> 16}.{ms & 0xFFFF}.{ls >> 16} build {ls & 0xFFFF}"
    except Exception:
        return ""


def versione_resolve():
    pf = os.environ.get("ProgramFiles", r"C:\Program Files")
    return versione_file(Path(pf) / "Blackmagic Design" / "DaVinci Resolve" / "Resolve.exe")


def versione_webview2():
    import winreg
    chiave = r"Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}"
    for radice, base in ((winreg.HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\"), (winreg.HKEY_CURRENT_USER, "Software\\")):
        try:
            with winreg.OpenKey(radice, base + chiave) as k:
                v = winreg.QueryValueEx(k, "pv")[0]
                if v and v != "0.0.0.0":
                    return v
        except OSError:
            continue
    return ""


def tc_fotogrammi(tc, cad):
    try:
        return Cadenza.di(cad).da_testo(tc)
    except (ValueError, TypeError, AttributeError):
        return None


def durata_testo(fotogrammi, cad):
    s = int(round(Cadenza.di(cad).secondi(fotogrammi)))
    return f"{s // 3600}:{s // 60 % 60:02d}:{s % 60:02d}"


def modello_atem(cartella):
    """Il nome del modello dal pacchetto _pin del registro dell'ATEM: è un dato del prodotto, non della rete."""
    try:
        with open(cartella / "atem.jsonl", encoding="utf-8") as f:
            for k, riga in enumerate(f):
                if k > 400:
                    break
                if '"_pin"' in riga:
                    b = bytes.fromhex(json.loads(riga)["hex"])
                    return b[:44].split(b"\0")[0].decode("ascii", "replace").strip()
    except (OSError, ValueError, KeyError):
        pass
    return ""


def conta_tagli(cartella):
    tagli = dissolvenze = 0
    try:
        with open(cartella / "tagli.csv", encoding="utf-8") as f:
            for r in csv.reader(f, delimiter=";"):
                if len(r) > 4:   # il tipo è la quarta colonna da destra, anche se un timecode
                    t = r[-4]    # drop-frame senza virgolette ha spezzato la prima (9.80)
                    tagli += t == "cut"
                    dissolvenze += t not in ("cut", "start", "type", "inizio", "tipo", "")
    except OSError:
        return None
    return tagli, dissolvenze


def conta_righe(f):
    try:
        with open(f, encoding="utf-8") as x:
            return max(0, sum(1 for r in x if r.strip()) - 1)
    except OSError:
        return None


def timecode_dal_segnale(f):
    """Su quanti fotogrammi scritti il timecode è arrivato dall'SDI, e su quanti c'era il segnale."""
    scritti = dal_segnale = segnale = 0
    try:
        for r in righe_csv_traccia(f):
            if r.get("esito") != "scritto":
                continue
            scritti += 1
            dal_segnale += r.get("tc_dal_segnale") == "1"
            segnale += r.get("segnale") == "1"
    except (OSError, csv.Error):
        return None
    return scritti, dal_segnale, segnale


# ------------------------------------------------------------------ le parti del rapporto
def parte_macchina(p):
    m = leggi_json(DATI / "macchina.json") or {}
    righe = ["MACHINE"]
    if not m:
        return ["MACHINE", "  System check never run on this machine."], {}
    si = m.get("sistema", {})
    righe.append(f"  Windows: {si.get('windows', '?')}")
    righe.append(f"  CPU: {si.get('processore', '?')}, {si.get('nuclei', '?')} cores, {si.get('fili', '?')} threads")
    righe.append(f"  Memory: {si.get('memoria_gb', '?')} GB")
    for g in si.get("schede_video", []):
        righe.append(f"  GPU: {g.get('nome', '?')}, {g.get('memoria_gb', '?')} GB")
    dl = m.get("decklink", {})
    righe.append(f"  Desktop Video: {dl.get('versione') or 'not installed'}")
    for x in dl.get("porte", []):
        verso = "input" if x.get("ingresso") else "output only"
        uhd = ", captures UHD 2160p25" if x.get("u25") else ""
        righe.append(f"    {x.get('nome', '?')} ({x.get('modello', '?')}): {x.get('connessioni', '?')} {verso}, {x.get('interfaccia', '')}{uhd}")
    for c in m.get("pcie", []):
        if c.get("acquisizione"):
            righe.append(f"  PCIe capture card: {c.get('nome', '?')}, x{c.get('larghezza', '?')} of x{c.get('larghezza_max', '?')}, "
                         f"{c.get('gts', '?')} of {c.get('gts_max', '?')} GT/s")
    for d in m.get("dischi", []):
        mis = f", measured {d['misurato_mb_s']} MB/s" if d.get("misurato_mb_s") else ""
        righe.append(f"  Drive {d.get('lettera', '?')} {d.get('modello', '?')} ({d.get('bus') or '?'}, {en(d.get('tipo', '?'))}), "
                     f"{d.get('libero_gb', '?')} of {d.get('totale_gb', '?')} GB free{mis}{', system' if d.get('sistema') else ''}")
    rete = m.get("rete", {})
    for chi in ("atem", "hyperdeck"):
        x = rete.get(chi) or {}
        if x:
            righe.append(f"  {chi.upper() if chi == 'atem' else 'HyperDeck'} on the network: "
                         f"{'responding' if x.get('risponde') else 'not responding'}, {'same' if x.get('stessa_rete') else 'different'} subnet")
    vr, vw = versione_resolve(), versione_webview2()
    righe.append(f"  DaVinci Resolve found: {'yes' if m.get('resolve') else 'no'}" + (f", Resolve.exe version {vr}" if vr else ""))
    righe.append(f"  WebView2 Runtime: {vw or 'not installed (the recorder opens in the browser)'}")
    righe.append(f"  System check of {m.get('data', '?')}: {en(m.get('verdetto', '?'))}")
    for s in m.get("passi", []):
        righe.append(f"    [{en(s.get('esito', '?'))}] {s.get('nome', '?')}: {s.get('nota', '')}")
    for s in m.get("da_fare", []):
        righe.append(f"    to do ({en(s.get('livello', '?'))}): {s.get('titolo', '')}")
    prova = m.get("prova") or {}
    for c in prova.get("codec", []):
        righe.append(f"  Encoding test {codec(c.get('mbit'))}: {c.get('tracce')}{'+' if c.get('almeno') else ''} tracks with 30% margin "
                     f"({c.get('fps')} fps in total, {c.get('provate')} encodes at once)")
    # nella copia che viaggia: niente nome del computer, indirizzi della rete, etichette dei dischi
    m = json.loads(json.dumps(m))
    m.get("sistema", {}).pop("computer", None)
    for s in m.get("rete", {}).get("schede", []):
        s.pop("ip", None)
    for chi in ("atem", "hyperdeck"):
        (m.get("rete", {}).get(chi) or {}).pop("ip", None)
    for d in m.get("dischi", []):
        d.pop("etichetta", None)
    return [p(r) for r in righe], pulisci_json(m, p)


def parte_impostazioni(p):
    i = leggi_json(DATI / "impostazioni.json") or {}
    voci = [v for v in (i.get("tracce") or "").split("|") if v]
    armate = [v for v in voci if not v.startswith("-")]
    setup = list((DATI / "setup").glob("*.json")) if (DATI / "setup").is_dir() else []
    corta = {
        "format": i.get("modo"), "dnxhd_mbit": i.get("mbit"), "audio_channels": i.get("audio"),
        "resolve_project": i.get("resolve"), "atem_set": bool(i.get("atem")), "hyperdeck_set": bool(i.get("hyperdeck")),
        "tracks": len(voci), "tracks_armed": len(armate),
        "track_ports": [v.lstrip("-").split("=")[0] for v in voci],
        "delays": i.get("ritardi", ""), "saved_setups": len(setup),
        "hyperdeck_assignments": len([v for v in (i.get("abbinamenti_hd") or "").split("|") if v]),
    }
    righe = ["SETTINGS",
             f"  {corta['format']}, {codec(corta['dnxhd_mbit'])}, {corta['audio_channels']} audio channels, "
             f"Resolve project {'on' if corta['resolve_project'] else 'off'}",
             f"  ATEM address {'set' if corta['atem_set'] else 'not set'}, HyperDeck address {'set' if corta['hyperdeck_set'] else 'not set'}",
             f"  {corta['tracks']} tracks in the list, {corta['tracks_armed']} armed: {', '.join(corta['track_ports']) or '-'}",
             f"  Delays: {corta['delays'] or 'none'}; saved setups: {corta['saved_setups']}"]
    return [p(r) for r in righe], pulisci_json(corta, p)


def parte_mappa(p):
    """La mappa porta → aux (mappa_porte.py): senza l'indirizzo dell'ATEM e senza i nomi delle sorgenti."""
    m = leggi_json(DATI / "porte.json")
    if not m:
        return ["INPUT MAP", "  Not made: no input linked to an ATEM aux."], None
    porte = sorted((m.get("porte") or {}).items(), key=lambda x: x[1].get("aux", 0))
    righe = ["INPUT MAP", f"  Made {str(m.get('creata', '?'))[:16].replace('T', ' ')}, {len(porte)} inputs linked to an ATEM aux"]
    righe += [f"    AUX {v.get('aux', '?')} -> {k}" for k, v in porte]
    copia = {"creata": m.get("creata"), "porte": dict(porte),
             "aux_allora": {a: {"sorgente": x.get("sorgente")} for a, x in (m.get("aux_allora") or {}).items()}}
    return [p(r) for r in righe], pulisci_json(copia, p)


# Le righe dei registri che dicono qualcosa: avvisi ed errori. Il resto viaggia nello zip, non nel testo.
SEGNI_AVVISO = re.compile(r"⚠|error|failed|cannot|could not|not responding|dropped|exception|crash", re.I)
RIGHE_REGISTRO = 400


def parte_registri(p):
    """Gli ultimi due registri della sessione (registro.cpp), le ultime righe, ripuliti come il resto."""
    c = DATI / "logs"
    registri = sorted(c.glob("*.log"))[-2:] if c.is_dir() else []
    if not registri:
        return ["RECORDER LOG", "  No session log on this machine (versions before 0.9 beta 3 did not write one)."], {}
    righe, file = ["RECORDER LOG"], {}
    for k, r in enumerate(reversed(registri), 1):
        tutte = leggi_testo(r).splitlines()
        coda = [p(x) for x in tutte[-RIGHE_REGISTRO:]]
        avvisi = [x for x in coda if SEGNI_AVVISO.search(x)]
        quando = r.stem.replace("mtsrec ", "").replace("WinISORecorder ", "").rsplit(" ", 1)[0]
        righe.append(f"  Session {k}, started {quando}: {len(tutte)} lines, {len(avvisi)} warnings or errors in the last {len(coda)}")
        righe += ["    " + x for x in avvisi[-20:]]
        file[f"log/session {k}.log"] = "\n".join(coda) + "\n"
    return righe, file


def registrazioni(ultima):
    """Le ultime registrazioni del registratore, la più recente prima: quella appena fatta e la destinazione."""
    viste = []
    i = leggi_json(DATI / "impostazioni.json") or {}
    radici = [Path(ultima)] if ultima else []
    if i.get("cartella"):
        try:
            radici += [x for x in Path(i["cartella"]).iterdir() if x.is_dir()]
        except OSError:
            pass
    for r in radici:
        try:
            if (r / "registrazione.json").is_file() and r.resolve() not in viste:
                viste.append(r.resolve())
        except OSError:
            continue
    viste.sort(key=lambda x: (x / "registrazione.json").stat().st_mtime, reverse=True)
    return viste[:MAX_REGISTRAZIONI]


def parte_registrazione(n, cartella, p):
    r = leggi_json(cartella / "registrazione.json") or {}
    fps = Cadenza.da_registrazione(cartella)          # dal 18 novembre 2026 anche 29,97 e 59,94 (sezione 9.79)
    # prima il percorso intero e la cartella di destinazione: coi soli nomi un percorso con gli spazi restava
    # scoperto a metà nei registri, «<path> ISO - prove\...\recording 1» (trovato il 18 novembre 2026)
    p.aggiungi(str(cartella), f"<recording {n} folder>")
    p.aggiungi(str(cartella.parent), "<destination>")
    p.aggiungi(cartella.name, f"recording {n}")
    tracce = r.get("tracce", [])
    # i nomi dei file possono essere nomi di sorgenti o di persone: nel rapporto resta la porta
    for t in tracce:
        if t.get("file") and t.get("file") != t.get("porta"):
            p.aggiungi(t["file"], t.get("porta", "track"))
    quando = datetime.datetime.fromtimestamp((cartella / "registrazione.json").stat().st_mtime).strftime("%Y-%m-%d %H:%M")
    a, b = tc_fotogrammi(r.get("inizio"), fps), tc_fotogrammi(r.get("fine"), fps)
    durata = (b - a) % fps.giorno if a is not None and b is not None else None
    righe = [f"RECORDING {n}, ended {quando}",
             f"  {r.get('modo', '?')}{' drop-frame' if fps.drop else ''}, {codec(r.get('mbit', '?'))}, {r.get('canali_audio', '?')} audio channels, "
             f"Broadcast WAV {'yes' if r.get('wav') else 'no'}, {len(tracce)} tracks",
             f"  Timecode {r.get('inizio', '?')} to {r.get('fine', '?')}" + (f", {durata_testo(durata, fps)}" if durata is not None else "")]
    atem = modello_atem(cartella)
    righe.append(f"  ATEM: {atem or ('connected' if r.get('atem') else 'not used')}")
    t = conta_tagli(cartella)
    if t:
        righe.append(f"  ATEM edit: {t[0]} cuts, {t[1]} dissolves")
    mk = conta_righe(cartella / "marker.csv")
    if mk is not None:
        righe.append(f"  Markers: {mk}")
    if r.get("hyperdeck"):
        righe.append(f"  HyperDeck record answer: {r.get('hyperdeck_record') or 'none'}")
    car = leggi_json(cartella / "carico.json")
    if car:
        gpu = f", GPU max {car['gpu_max']}%" if car.get("gpu_max", -1) >= 0 else ""
        righe.append(f"  Load while recording: CPU average {car.get('cpu_media')}%, max {car.get('cpu_max')}%{gpu}, memory max {car.get('ram_max')}%")
    peso = 0
    for f in cartella.iterdir():
        if f.suffix.lower() in (".mov", ".wav"):
            try:
                peso += f.stat().st_size
            except OSError:
                pass
    if peso:
        mb_s = peso / 1e6 / fps.secondi(durata) if durata else 0
        righe.append(f"  Written: {peso / 1e9:.1f} GB" + (f", {mb_s:.0f} MB/s" if mb_s else ""))
    righe.append("  Tracks:")
    for riga in leggi_testo(cartella / "rapporto.txt").splitlines():
        if riga.strip():
            righe.append("    " + riga.strip())
    for x in tracce:
        tc = timecode_dal_segnale(cartella / (x.get("file", "") + ".csv"))
        if tc and tc[0]:
            hdmi = " (HDMI input: timecode taken from an SDI input)" if "HDMI" in x.get("porta", "") else ""
            righe.append(f"    {x.get('file')}: timecode from its own signal on {tc[1]} of {tc[0]} frames, signal present on {tc[2]}{hdmi}")
    esito = leggi_json(cartella / "progetto.json")
    if esito:
        righe.append(f"  Resolve project: {'built' if esito.get('ok') else 'NOT built'}: {esito.get('testo', '')}")
    uscite = {"Resolve project (.drp)": list(cartella.glob("*.drp")), "FCPXML": list(cartella.glob("*.fcpxml")),
              "Premiere XML": list(cartella.glob("*Premiere.xml")), "EDL": list(cartella.glob("*.edl"))}
    righe.append("  Files: " + ", ".join(f"{k} {'yes' if v else 'no'}" for k, v in uscite.items()))
    if (cartella / "hyperdeck.json").is_file():
        righe.append("  HyperDeck clips used in the project: yes")
    if (cartella / "interrotti").is_dir():
        righe.append("  Repaired after an interruption: yes")
    righe = [p(x) for x in righe]
    allegati = {
        "registrazione.json": json.dumps(pulisci_json({k: v for k, v in r.items() if k not in ("atem", "hyperdeck")}, p), indent=2),
        "rapporto.txt": p(leggi_testo(cartella / "rapporto.txt")),
    }
    if car:
        allegati["carico.json"] = json.dumps(car, indent=2)
    if esito:
        allegati["progetto.json"] = json.dumps(pulisci_json(esito, p), indent=2)
    return righe, allegati


# ------------------------------------------------------------------ il rapporto
def componi(versione, ultima):
    p = Pulitore()
    rid = identificativo()
    ora = datetime.datetime.now()
    testa = [f"Multicam Timecode Sync Recorder test report",
             f"Version {versione}, report ID {rid}, created {ora:%Y-%m-%d %H:%M}",
             "",
             "This report contains no video, no IP addresses, no computer or user names and no file paths.",
             "Recording and source names are replaced with the DeckLink port names.",
             ""]
    righe_m, macchina = parte_macchina(p)
    righe_i, imp = parte_impostazioni(p)
    corpo = testa + righe_m + [""] + righe_i + [""]
    file = {"machine.json": json.dumps(macchina, indent=2, ensure_ascii=False),
            "settings.json": json.dumps(imp, indent=2, ensure_ascii=False)}
    regs = registrazioni(ultima)
    if not regs:
        corpo += ["RECORDINGS", "  No recordings found in the destination folder.", ""]
    for n, c in enumerate(regs, 1):
        try:
            righe, allegati = parte_registrazione(n, c, p)
        except OSError as e:
            righe, allegati = [f"RECORDING {n}", f"  could not be read: {p(str(e))}"], {}
        corpo += righe + [""]
        for k, v in allegati.items():
            file[f"recording {n}/{k}"] = v
    righe_p, mappa = parte_mappa(p)
    corpo += righe_p + [""]
    if mappa:
        file["input map.json"] = json.dumps(mappa, indent=2, ensure_ascii=False)
    # i registri per ultimi: a questo punto il pulitore conosce già i nomi delle registrazioni e delle sorgenti
    righe_r, registri = parte_registri(p)
    corpo += righe_r + [""]
    file.update(registri)
    nota = leggi_testo(DATI / "nota rapporto.txt").strip()
    corpo += ["NOTES FROM THE TESTER", "  " + (p(nota).replace("\n", "\n  ") if nota else "(none)")]
    testo = "\n".join(corpo) + "\n"
    return rid, ora, testo, {"report.txt": testo, **file}


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--versione", required=True)
    ap.add_argument("--registrazione", default="")
    ap.add_argument("--uscita", default="")
    a = ap.parse_args()
    rid, ora, testo, file = componi(a.versione, a.registrazione)
    if not a.uscita:
        print(json.dumps({"testo": testo, "file": sorted(file)}, ensure_ascii=False))
        return
    nome = f"mtsrec test report {ora:%Y-%m-%d %H-%M} {rid}.zip"
    uscita = Path(a.uscita)
    if uscita.suffix.lower() != ".zip":
        uscita = uscita / nome
    uscita.parent.mkdir(parents=True, exist_ok=True)
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for k, v in file.items():
            z.writestr(f"{uscita.stem}/{k}", v)
    uscita.write_bytes(buf.getvalue())
    print(json.dumps({"file": str(uscita), "nome": uscita.name, "kb": round(len(buf.getvalue()) / 1024, 1)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
