"""Dal registro dei tagli dell'ATEM e dalla cartella del multicorder di vMix
crea in DaVinci Resolve un progetto con i file, la clip multicamera e la
timeline montata con tagli e dissolvenze.

Uso:
  py -3.13 crea_progetto_resolve.py <cartella multicorder> <registro.jsonl> --allestimento allestimento.json
      [--progetto "Nome"] [--solo-xml] [--uscita file.fcpxml]

Python 3.13 perche' fusionscript.dll di Resolve e' legata a quella versione:
con le altre l'import fa morire il processo senza messaggi. Resolve deve essere
aperto con lo scripting esterno attivo (Preferenze > Sistema > Generale).

IL REGISTRO e' un file JSON Lines scritto dal registratore durante la diretta,
un evento per riga:
  {"evento": "multicorder", "stato": "avvio", "ora": "2026-10-10T15:48:24.120"}
  {"evento": "programma", "ora": "...", "ingresso": 3, "transizione": "mix", "durata": 25}
L'ora di un evento "programma" e' quella in cui la transizione COMINCIA:
per un taglio coincide col cambio, per una dissolvenza di d fotogrammi il
punto di taglio sulla timeline cade a meta', cioe' d/2 dopo.

L'ALLESTIMENTO dice quale ingresso dell'ATEM finisce in quale file:
  {"ingressi": {"1": 2, "2": 3}, "taratura_fotogrammi": 0, "nomi": {"1": "Pianoforte"}}
Gli ingressi che non hanno un file — media player, barre, nero — vanno
sull'angolo PROGRAMMA, che e' l'uscita dell'ATEM registrata da vMix.
"""
import argparse, json, sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from genera_fcpxml import TB, leggi_multicorder, scrivi_fcpxml

TIPI = {"cut": "cut", "mix": "mix", "dip": "dip", "wipe": "wipe", "dve": "dve", "sting": "sting"}


def trova_timeline_multicorder(cartella):
    xml = sorted(Path(cartella).glob("MultiCorder - Timeline - *.xml"))
    if not xml:
        sys.exit(f"No MultiCorder timeline in the folder: {cartella}")
    if len(xml) > 1:
        sys.exit("More than one MultiCorder timeline in the same folder: " + ", ".join(x.name for x in xml))
    return xml[0]


def leggi_registro(percorso, allestimento, camere, programma=1):
    """Converte gli eventi dell'ATEM in tagli sulla timeline del multicorder."""
    eventi = [json.loads(r) for r in open(percorso, encoding="utf-8") if r.strip()]
    avvii = [e for e in eventi if e.get("evento") == "multicorder" and e.get("stato") == "avvio"]
    if not avvii:
        sys.exit("The log has no MultiCorder start: cannot tell which frame the cuts refer to")
    t0 = datetime.fromisoformat(avvii[-1]["ora"])
    taratura = int(allestimento.get("taratura_fotogrammi", 0))
    mappa = {int(k): int(v) for k, v in allestimento.get("ingressi", {}).items()}
    nomi = allestimento.get("nomi", {})

    tagli, fuori, senza_file = [], 0, set()
    for e in eventi:
        if e.get("evento") != "programma":
            continue
        tipo = TIPI.get(str(e.get("transizione", "cut")).lower(), "cut")
        dur = int(e.get("durata") or 0) if tipo != "cut" else 0
        inizio = round((datetime.fromisoformat(e["ora"]) - t0).total_seconds() * TB) + taratura
        fr = inizio + dur // 2
        if fr < 0:
            # successo prima dell'avvio: dice cosa era in onda al primo fotogramma
            fuori += 1
            fr, tipo, dur = 0, "cut", 0
        ing = int(e["ingresso"])
        cam = mappa.get(ing, programma)
        if cam not in camere:
            cam = programma
        if ing not in mappa:
            senza_file.add(ing)
        # regola decisa da Andrea il 12 ottobre 2026: in Resolve solo Cut e Fade.
        # Ogni altra transizione diventa un taglio a meta' della sua durata, con un marker
        nota = ""
        if tipo not in ("cut", "mix"):
            nota = f"ATEM {tipo}, {dur} frames: cut here"
            tipo = "cut"
        tagli.append((fr, cam, tipo, dur if tipo == "mix" else 0, nota))

    # due eventi sullo stesso fotogramma: vale l'ultimo
    puliti = {}
    for tg in sorted(tagli, key=lambda x: x[0]):
        puliti[tg[0]] = tg
    tagli = list(puliti.values())
    # un pezzo che si ripete sulla stessa camera non e' un taglio
    finali = []
    for tg in tagli:
        if finali and finali[-1][1] == tg[1]:
            continue
        finali.append(tg)
    return finali, {"prima dell'avvio": fuori, "ingressi senza file": sorted(senza_file), "nomi": nomi}


def _tc(testo, cad=None):
    from cadenza import Cadenza
    return (cad or Cadenza.di(TB)).da_testo(testo)


# Come Resolve scrive la cadenza della timeline nelle impostazioni del progetto
VALORI_RESOLVE = {(24000, 1001): "23.976", (24, 1): "24", (25, 1): "25", (30000, 1001): "29.97", (30, 1): "30",
                  (50, 1): "50", (60000, 1001): "59.94", (60, 1): "60"}


def crea_in_resolve(resolve, fcpxml, nome_progetto, nome_timeline, note=(), marker=(), larghezza=1920, altezza=1080,
                    cadenza=None, interlacciato=False):
    """Crea il progetto e importa la timeline. Va bene sia da fuori sia dall'MCP integrato.

    note: (fotogramma, testo) dei tagli da guardare, marker gialli «ATEM».
    marker: (fotogramma, colore, nome, nota, durata) dati durante la registrazione (sezione 9.55).
    """
    pm = resolve.GetProjectManager()
    nome, i = nome_progetto, 2
    while not pm.CreateProject(nome):
        nome = f"{nome_progetto} ({i})"
        i += 1
        if i > 20:
            raise RuntimeError("Cannot create the project in Resolve")
    p = pm.GetCurrentProject()
    # il formato va deciso prima di importare qualunque cosa
    # cadenza: quella della registrazione (sezione 9.79); senza, 25 come il multicorder di vMix
    if cadenza is None:
        p.SetSetting("timelineFrameRate", str(TB))
    else:
        p.SetSetting("timelineFrameRate", VALORI_RESOLVE.get((cadenza.num, cadenza.den), str(cadenza.base)))
        p.SetSetting("timelineDropFrameTimecode", "1" if cadenza.drop else "0")
        if interlacciato:
            p.SetSetting("timelineInterlaceProcessing", "1")
    p.SetSetting("timelineResolutionWidth", str(larghezza))
    p.SetSetting("timelineResolutionHeight", str(altezza))
    mp = p.GetMediaPool()
    tl = mp.ImportTimelineFromFile(str(fcpxml), {"timelineName": nome_timeline})
    if not tl:
        raise RuntimeError("Resolve rejected the timeline: " + str(fcpxml))
    p.SetCurrentTimeline(tl)
    # ⚠ Un FCPXML 1080i50 Resolve lo importa come timeline a 50, che conta i semicampi (prova del 18 novembre
    # 2026: progetto a 50, inizio al fotogramma 141578 per 70789 fotogrammi di file). I nostri fotogrammi
    # contano a 25: sulla timeline si moltiplicano per il rapporto, che in progressivo vale 1.
    rapporto = 1
    try:
        fps_tl = float(tl.GetSetting("timelineFrameRate") or 0)
        fps_reg = cadenza.fps if cadenza is not None else float(TB)
        if fps_tl and fps_reg:
            rapporto = max(1, round(fps_tl / fps_reg))
    except (TypeError, ValueError):
        rapporto = 1
    # i fotogrammi dei marker contano dall'inizio della timeline
    # Resolve rifiuta un secondo marker sullo stesso fotogramma: si sposta al primo libero dopo.
    def metti(fr, colore, nome, nota, durata):
        return any(tl.AddMarker(fr * rapporto + k, colore, nome, nota, durata * rapporto) for k in range(5))
    contati = sum(1 for m in marker if metti(*m))
    # I marker valgono per tutte le tracce (Andrea, 31 ottobre): anche su ogni clip del media pool,
    # allo stesso timecode. Le tracce partono insieme, la clip dell'HyperDeck da un altro timecode.
    if marker:
        # l'inizio della timeline dal suo fotogramma, che a 50 per l'interlacciato va diviso per due
        inizio_tl = tl.GetStartFrame() // rapporto if rapporto > 1 else _tc(tl.GetStartTimecode(), cadenza)
        for clip in mp.GetRootFolder().GetClipList():
            pr = clip.GetClipProperty()
            if pr.get("Type") == "Timeline" or not pr.get("Start TC"):
                continue
            # la clip multicamera nata da un FCPXML interlacciato è anche lei a 50: timecode e fotogrammi
            # suoi si leggono alla sua cadenza, e i marker si mettono in fotogrammi suoi
            r_clip = 1
            try:
                if float(pr.get("FPS") or 0) and (cadenza is not None or TB):
                    r_clip = max(1, round(float(pr["FPS"]) / (cadenza.fps if cadenza is not None else float(TB))))
            except (TypeError, ValueError):
                r_clip = 1
            if r_clip > 1:
                from cadenza import Cadenza
                c = cadenza if cadenza is not None else Cadenza.di(TB)
                doppia = Cadenza(c.base * r_clip, c.num * r_clip, c.den, c.drop)
                inizio_clip = doppia.da_testo(pr["Start TC"]) // r_clip
                durata_clip = int(pr.get("Frames") or 0) // r_clip
            else:
                inizio_clip, durata_clip = _tc(pr["Start TC"], cadenza), int(pr.get("Frames") or 0)
            for fr, colore, testo, nota, durata in marker:
                k = inizio_tl + fr - inizio_clip
                if 0 <= k < durata_clip:
                    clip.AddMarker(k * r_clip, colore, testo, nota, durata * r_clip)
    contati += sum(1 for fr, nota in note if metti(fr, "Yellow", "ATEM", nota, 1))
    pezzi = [i for i in tl.GetItemListInTrack("video", 1)]
    pm.SaveProject()
    return {"progetto": nome, "timeline": tl.GetName(),
            "pezzi": sum(1 for i in pezzi if i.GetName() != "Cross Dissolve"),
            "dissolvenze": sum(1 for i in pezzi if i.GetName() == "Cross Dissolve"), "marker": contati}


RESOLVE_EXE = Path(r"C:\Program Files\Blackmagic Design\DaVinci Resolve\Resolve.exe")


def registro_resolve():
    """Il registro di Resolve: davinci_resolve.log in Resolve 21.2, ResolveDebug.txt nelle versioni di prima."""
    import os
    base = Path(os.environ.get("APPDATA", "")) / "Blackmagic Design" / "DaVinci Resolve" / "Support" / "logs"
    for nome in ("davinci_resolve.log", "ResolveDebug.txt"):
        if (base / nome).exists():
            return base / nome
    return None


def stato_server_script(dal):
    """Come sta il server di script del Resolve aperto a «dal» (secondi epoch), dal registro di Resolve:
    None se non è ancora partito, "in corso" mentre Resolve ci riprova, "caduto" se ha rinunciato, "pronto".

    17 novembre 2026 (sezioni 9.71 e 9.75): Resolve fa partire il suo server di script circa 30 secondi dopo
    la finestra e ci si collega; se intanto uno script lo interroga, «Failed to connect to script server»
    tre volte e da lì nessuno script lo raggiunge più, finché non si riapre. Aperto senza interrogarlo è
    partito al primo colpo. Il registro accumula gli avvii di più giorni: conta solo il primo «Started
    script server» da «dal» in poi, non tutto il file (prima un fallimento vecchio faceva arrendere anche
    un avvio buono)."""
    import re, time
    from datetime import datetime
    registro = registro_resolve()
    if registro is None:
        return None
    try:
        if registro.stat().st_mtime < dal:
            return None
        righe = registro.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return None
    soglia = datetime.fromtimestamp(dal - 2).strftime("%Y-%m-%d %H:%M:%S")
    ultimo, quando = None, None
    for k, r in enumerate(righe):
        if "| Started script server:" in r:
            m = re.search(r"\| (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)", r)
            if m and m.group(1) >= soglia:
                ultimo, quando = k, m.group(1)
                break                   # il primo avvio dopo «dal»: quello aperto da qui
    if ultimo is None:
        return None
    dopo = [r.strip() for r in righe[ultimo + 1:ultimo + 12]]
    if "Failed to connect to script server" in dopo:
        return "caduto"
    if any(r.startswith("Failed to connect to script server") for r in dopo):
        return "in corso"
    # nessun errore: lo si dà per pronto qualche secondo dopo, quando i tentativi sarebbero già scritti
    partito = datetime.strptime(quando, "%Y-%m-%d %H:%M:%S").timestamp()
    return "pronto" if time.time() - partito >= 6 else "in corso"


def chiudi_resolve(proc, attesa=60):
    """Chiude con garbo il Resolve aperto da qui (mai uno aperto da altri): niente progetto dell'utente dentro."""
    import subprocess, time
    subprocess.run(["taskkill", "/PID", str(proc.pid)], capture_output=True)
    limite = time.time() + attesa
    while time.time() < limite and proc.poll() is None:
        time.sleep(1)
    return proc.poll() is not None


def collega_resolve(apri=True, attesa=240):
    import os
    moduli = r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting\Modules"
    os.environ.setdefault("RESOLVE_SCRIPT_API", str(Path(moduli).parent))
    os.environ.setdefault("RESOLVE_SCRIPT_LIB", r"C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll")
    sys.path.append(moduli)
    import DaVinciResolveScript as dvr
    r = dvr.scriptapp("Resolve")
    if not r and apri:
        # Chiuso: lo apre e aspetta che il Project Manager risponda (16 novembre 2026, allo STOP Resolve era
        # chiuso e il progetto non nasceva). Staccato dalla finestra dello script, così resta aperto dopo.
        import subprocess, time
        exe = RESOLVE_EXE if RESOLVE_EXE.exists() else None
        if exe is None:
            sys.exit(f"Resolve is not running and was not found in {RESOLVE_EXE.parent}")
        caduto = False
        for tentativo in (1, 2):
            print("Resolve is not running: starting it and waiting until it is ready…" if tentativo == 1 else
                  "Resolve started without its scripting server: closing it and starting it again…", flush=True)
            dal = time.time()
            proc = subprocess.Popen([str(exe)], cwd=str(exe.parent), close_fds=True,
                                    creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP)
            limite = dal + attesa
            caduto = False
            while time.time() < limite:
                time.sleep(3)
                # Non lo si interroga finché il suo server di script non è partito: bussare prima lo fa cadere
                # (9.75). Senza registro leggibile si aspetta un minuto, poi si prova come prima.
                stato = stato_server_script(dal)
                if stato == "caduto" and proc.poll() is None:
                    caduto = True
                    break
                if stato != "pronto" and not (registro_resolve() is None and time.time() > dal + 60):
                    continue
                r = dvr.scriptapp("Resolve")
                try:
                    if r and r.GetProjectManager() and r.GetProjectManager().GetCurrentProject():
                        time.sleep(3)          # il progetto vuoto c'è: ancora un attimo perché finisca di caricare
                        print("Resolve ready", flush=True)
                        break
                except Exception:
                    pass
                r = None
            if r or not caduto or tentativo == 2 or not chiudi_resolve(proc):
                break
        if not r and caduto:
            sys.exit("Resolve started but its scripting server did not: close Resolve, start it again and press PROJECT")
        if not r:
            sys.exit(f"Resolve started but not responding after {attesa} seconds: check that Preferences > System > General > External scripting using is set to Local")
    if not r:
        sys.exit("Resolve is not responding: it must be running, with Preferences > System > General > External scripting using set to Local")
    return r


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("cartella")
    ap.add_argument("registro")
    ap.add_argument("--allestimento", required=True)
    ap.add_argument("--progetto")
    ap.add_argument("--uscita")
    ap.add_argument("--programma", type=int, default=1)
    ap.add_argument("--solo-xml", action="store_true")
    a = ap.parse_args()

    cartella = Path(a.cartella)
    timeline_mc = trova_timeline_multicorder(cartella)
    camere, _ = leggi_multicorder(timeline_mc, cartella)
    allest = json.load(open(a.allestimento, encoding="utf-8"))
    tagli, info = leggi_registro(a.registro, allest, camere, a.programma)
    nomi_angoli = {int(v): info["nomi"].get(k) for k, v in allest.get("ingressi", {}).items() if info["nomi"].get(k)}

    data = timeline_mc.stem.replace("MultiCorder - Timeline - ", "").strip()
    nome = a.progetto or f"Live {data}"
    uscita = Path(a.uscita) if a.uscita else cartella / f"ATEM cuts - {data}.fcpxml"
    r = scrivi_fcpxml(camere, tagli, uscita, nome="ATEM edit", multicamera=f"Multicam {data}",
                      programma=a.programma, nomi_angoli=nomi_angoli)
    print(f"{r['pezzi']} clips, {r['dissolvenze']} dissolves, {r['angoli']} angles -> {uscita}")
    if info["prima dell'avvio"]:
        print(f"  {info['prima dell\'avvio']} cuts before the MultiCorder start: the last one opens the timeline")
    if info["ingressi senza file"]:
        print(f"  ATEM inputs without a file, put on PROGRAM: {info['ingressi senza file']}")
    if a.solo_xml:
        return
    esito = crea_in_resolve(collega_resolve(), uscita.resolve(), nome, "ATEM edit", r["note"])
    print(f"Resolve: project “{esito['progetto']}”, {esito['pezzi']} clips, "
          f"{esito['dissolvenze']} dissolves, {esito['marker']} markers")


if __name__ == "__main__":
    main()
