"""Dalla cartella di una registrazione di mtsrec crea il progetto Resolve ed esporta il .drp.

Nel progetto: i file, la clip multicamera per timecode e la timeline coi tagli.
Tutti i file di mtsrec cominciano dallo stesso timecode (sezione 9.51), quindi gli
angoli della multicamera sono allineati senza sfasamenti.

Uso:
  py -3.13 progetto_da_registratore.py <cartella> --atem --sorgenti "1=CAM 1;2=CAM 2" [--hyperdeck-clip file.mov]
  py -3.13 progetto_da_registratore.py <cartella> --tagli-finti 4 [--progetto "Nome"] [--drp file.drp]

--atem           i tagli veri, dal registro atem.jsonl che il registratore scrive con --atem
                 (tagli_da_atem.py, regola del +2 della sezione 9.48). La timeline parte dal
                 timecode della registrazione, cioè quello dell'ATEM
--sorgenti       quale traccia registra quale ingresso dell'ATEM: «numero=pezzo del nome del file».
                 Un ingresso senza traccia va sull'angolo del programma, se c'è, con un marker.
                 Senza, le sorgenti si ricavano da porte.json, la mappa delle porte che il registratore
                 copia nella cartella (mappa_porte.py, sezione 9.58), e dalle aux in atem.jsonl; la
                 traccia che registra il programma del M/E 1 diventa l'angolo del programma
--hyperdeck-clip la clip dell'HyperDeck scaricata dalla scheda: diventa l'angolo del programma,
                 allineata per timecode
hyperdeck.json   se c'è nella cartella (lo scrive il pannello «HyperDeck clips…» della pagina, sezione 9.72),
                 le clip degli HyperDeck abbinate agli ingressi dell'ATEM entrano come angoli in più: le
                 tracce DeckLink hanno la precedenza, e dove una clip non copre un pezzo si usa il programma
--tagli-finti N  per provare senza ATEM: un taglio ogni N secondi, a giro sulle tracce, una
                 dissolvenza ogni cinque. Il nome della timeline lo dichiara.

I marker dati durante la registrazione (marker.csv) vanno sulla timeline col loro colore e
anche in Marker.edl, che Resolve importa sulla timeline pure senza il .drp.

Per Premiere Pro, che l'FCPXML non lo legge, accanto escono anche un Final Cut Pro 7 XML
(«… - Premiere.xml») e un EDL con gli stessi tagli e dissolvenze e l'audio che segue il video (genera_premiere.py). Escono prima
di collegarsi a Resolve, quindi ci sono anche con --senza-resolve o se Resolve non risponde.

Resolve va con lo scripting esterno su Locale; Python 3.13, vedi crea_progetto_resolve.py. Se è chiuso
lo script lo apre e aspetta che sia pronto (16 novembre 2026: allo STOP Resolve era chiuso, il progetto
non nasceva e la pagina restava su «in costruzione»).

L'esito va in progetto.json nella cartella della registrazione, riuscito o no: il registratore lo legge
e la pagina lo mostra. Con --dal-registratore, che è come lo lancia il registratore nella sua finestra,
se qualcosa va storto la finestra resta aperta col motivo finché non si preme Invio.
"""
import argparse, json, re, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from crea_progetto_resolve import collega_resolve, crea_in_resolve
from cadenza import Cadenza
import genera_fcpxml, genera_premiere
from genera_fcpxml import scrivi_fcpxml
from genera_premiere import scrivi_edl, scrivi_xml_premiere
import tagli_da_atem

# La cadenza della registrazione: si legge da registrazione.json all'inizio di crea() (sezione 9.79).
TB = Cadenza()

# I file li legge il registratore stesso, con «mtsrec --sonda» (sezione 9.81): ffprobe.exe non è
# firmato e Smart App Control di Windows 11 lo bloccherebbe, quindi non viaggia più nel pacchetto.
# Nel pacchetto il registratore sta nella cartella sopra gli script; su questa macchina anche nella
# cartella di compilazione. ffprobe resta solo come ripiego per chi lancia gli script senza registratore.
REGISTRATORE = next((p for p in (Path(__file__).resolve().parent.parent / "mtsrec.exe",
                                 Path.home() / "WinISORecorder-build" / "mtsrec.exe") if p.exists()), None)
FFPROBE = next((str(p) for p in (Path(__file__).resolve().parent.parent / "ffprobe.exe", Path(r"C:\ffmpeg\bin\ffprobe.exe"))
                if p.exists()), "ffprobe")


def sonda(p):
    """Timecode di partenza, numero di fotogrammi e presenza dell'audio di un MOV.

    Il numero dei fotogrammi si legge dall'indice del file (nb_frames), in un decimo di secondo.
    Contarli coi pacchetti (-count_packets) legge tutto il file: con dieci tracce da un'ora, 84 GB
    l'una, il progetto partiva dopo mezz'ora (15 novembre 2026). Si contano solo se l'indice non
    lo dice, come in un MOV a frammenti rimasto aperto. Lo fa il registratore, e solo senza registratore ffprobe."""
    if REGISTRATORE:
        r = json.loads(subprocess.run([str(REGISTRATORE), "--sonda", str(p)], capture_output=True, text=True,
                                      encoding="utf-8", check=True,
                                      creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout)
        return tagli_da_atem.tc_da_testo(r["timecode"], TB), int(r["fotogrammi"]), bool(r["audio"])

    def leggi(conta):
        voce = "nb_read_packets" if conta else "nb_frames"
        r = json.loads(subprocess.run(
            [FFPROBE, "-v", "error", *(["-count_packets"] if conta else []), "-of", "json",
             "-show_entries", f"stream=codec_type,{voce}:stream_tags=timecode", str(p)],
            capture_output=True, text=True, check=True).stdout)["streams"]
        v = next(x for x in r if x["codec_type"] == "video")
        return r, int(v.get(voce) or 0)
    r, n = leggi(False)
    if n <= 0:
        r, n = leggi(True)
    tc = next(x["tags"]["timecode"] for x in r if x.get("tags", {}).get("timecode"))
    return tagli_da_atem.tc_da_testo(tc, TB), n, any(x["codec_type"] == "audio" for x in r)


def leggi_file(cartella):
    """Le tracce della registrazione: timecode di partenza e numero di fotogrammi di ogni MOV."""
    camere = {}
    ordine_naturale = lambda x: [int(s) if s.isdigit() else s for s in re.split(r"(\d+)", x.name)]   # copia 2 prima di copia 10
    for n, p in enumerate(sorted(Path(cartella).glob("*.mov"), key=ordine_naturale), 1):
        tc, durata, ha_audio = sonda(p)
        # "audio" falso tiene l'FCPXML com'era provato in Resolve, che l'audio lo legge dai file da sé;
        # "audio_file" serve all'XML e all'EDL per Premiere (15 novembre 2026)
        camere[n] = {"n": n, "nome": p.name, "percorso": p.resolve(), "inizio": 0,
                     "durata": durata, "tc": tc, "audio": False, "audio_file": ha_audio, "angolo": f"angolo-{n}"}
    return camere


def arrivi_registratore(cartella):
    """Gli arrivi dei fotogrammi dal CSV della prima traccia che ha il timecode dal segnale."""
    for p in sorted(Path(cartella).glob("*.csv")):
        arrivi = tagli_da_atem.arrivi_da_csv(p, TB)
        if arrivi:
            return arrivi
    return None


def porte_dei_file(cartella):
    """Quale porta ha registrato ogni file, da registrazione.json. Dalla pagina del registratore i file
    prendono il nome della sorgente dell'ATEM, «Camera 1», e non più quello della porta (sezione 10).
    Le registrazioni di prima non lo hanno: lì la porta si ricava dal nome del file."""
    p = Path(cartella) / "registrazione.json"
    if not p.exists():
        return {}
    return {t["file"]: t["porta"] for t in json.loads(p.read_text(encoding="utf-8")).get("tracce", [])
            if isinstance(t, dict)}


def sorgenti_da_mappa(cartella, camere):
    """Quale ingresso dell'ATEM registra ogni traccia, senza dirlo a mano (sezione 9.58).

    porte.json, copiata dal registratore alla partenza, dice su quale aux è cablata ogni porta;
    atem.jsonl ha la sorgente di ogni aux nello stato che l'ATEM manda all'aggancio. Restituisce
    {ingresso: traccia}, la traccia che registra il programma del M/E 1 oppure None, e le righe da stampare.
    """
    mappa, registro = cartella / "porte.json", cartella / "atem.jsonl"
    if not mappa.exists() or not registro.exists():
        return {}, None, []
    porte = json.loads(mappa.read_text(encoding="utf-8"))["porte"]
    aux, cambiate, pronto = {}, set(), False
    for riga in open(registro, encoding="utf-8"):
        e = json.loads(riga)
        if e.get("cmd") == "AuxS":
            b = bytes.fromhex(e["hex"])
            n, s = b[0] + 1, int.from_bytes(b[2:4], "big")
            if not pronto:
                aux[n] = s
            elif aux.get(n) != s:
                cambiate.add(n)
        elif e.get("cmd") == "InCm":
            pronto = True
    sorgenti, programma, righe = {}, None, []
    porta_di = porte_dei_file(cartella)
    for n, c in sorted(camere.items()):
        stem = Path(c["nome"]).stem
        porta = porta_di.get(stem, re.sub(r" copia \d+$", "", stem))
        if porta not in porte:
            righe.append(f"  {Path(c['nome']).stem}: ⚠ input not in the input map")
            continue
        a = porte[porta]["aux"]
        s = aux.get(a)
        if s is None:
            righe.append(f"  {Path(c['nome']).stem}: ⚠ AUX {a} is not in the ATEM log")
            continue
        if s == 10010 and programma is None:
            programma = n                                    # il programma del M/E 1: va sotto i tagli
        elif 0 < s < 1000:
            sorgenti.setdefault(s, n)
        righe.append(f"  {Path(c['nome']).stem} ← AUX {a} ← source {s}"
                     + (f"  ⚠ AUX {a} changed during the recording" if a in cambiate else ""))
    return sorgenti, programma, righe


def clip_hyperdeck_abbinate(cartella):
    """Le clip HyperDeck che la pagina ha abbinato agli ingressi dell'ATEM (clip_hyperdeck.py, sezione 9.72):
    hyperdeck.json nella cartella della registrazione, {"cartella": …, "abbinamenti": "file=ingresso|…"},
    con l'ingresso numero dell'ATEM o «program». Le clip senza ingresso non entrano."""
    f = Path(cartella) / "hyperdeck.json"
    if not f.exists():
        return []
    j = json.loads(f.read_text(encoding="utf-8"))
    base = Path(j.get("cartella", ""))
    out = []
    for voce in filter(None, j.get("abbinamenti", "").split("|")):
        file, _, ingresso = voce.rpartition("=")
        if not file or not (ingresso == "program" or ingresso.isdigit()):
            continue
        p = base / file
        if p.exists():
            out.append((p, ingresso))
        else:
            print(f"⚠ HyperDeck clip not found: {p}")
    return out


def aggiungi_hyperdeck(camere, hd_ingressi, sorgenti, programma, nomi):
    """Le clip HyperDeck diventano le sorgenti degli ingressi che nessuna traccia DeckLink registra: le tracce
    del registratore hanno la precedenza. Due clip sullo stesso ingresso: vince quella che copre di più la
    registrazione. Restituisce l'angolo del programma, che una clip «program» dà se non c'è già."""
    fine = min(c["durata"] for c in camere.values() if not c.get("esterna"))
    comune = lambda n: max(0, min(camere[n]["inizio"] + camere[n]["durata"], fine) - max(camere[n]["inizio"], 0))
    da_decklink = set(sorgenti)
    for n in sorted(hd_ingressi, key=comune, reverse=True):
        ing = hd_ingressi[n]
        if comune(n) == 0:
            print(f"⚠ {camere[n]['nome']}: does not overlap the recording, angle only")
        elif ing == "program":
            if programma is None:
                programma = n
                nomi[n] = f"{nomi[n]} - Program"
            else:
                print(f"⚠ {camere[n]['nome']}: there is already a program angle, angle only")
        elif int(ing) in da_decklink:
            print(f"⚠ {camere[n]['nome']}: ATEM input {ing} is already recorded by a DeckLink track, angle only")
        elif int(ing) in sorgenti:
            print(f"⚠ {camere[n]['nome']}: another HyperDeck clip covers ATEM input {ing} longer, angle only")
        else:
            sorgenti[int(ing)] = n
    return programma


def copri_buchi(tagli, camere, programma, fine):
    """Un pezzo della timeline su una clip HyperDeck che non lo copre tutto va sul programma, se c'è e lo
    copre, con una nota: una clip partita tardi o fermata prima non deve lasciare buchi neri."""
    copre = lambda n, a, b: camere[n]["inizio"] <= a and b <= camere[n]["inizio"] + camere[n]["durata"]
    out = []
    for i, (fr, n, tipo, durata, nota) in enumerate(tagli):
        b = tagli[i + 1][0] if i + 1 < len(tagli) else fine
        if camere[n].get("esterna") and not copre(n, fr, b):
            if programma is not None and programma != n and copre(programma, fr, b):
                nota = "; ".join(x for x in (nota, f"{camere[n]['nome']} does not cover this part, program used here") if x)
                n = programma
            else:
                nota = "; ".join(x for x in (nota, f"{camere[n]['nome']} does not cover all of this part") if x)
        out.append((fr, n, tipo, durata, nota))
    return out


def tagli_veri(camere, cartella, sorgenti, programma):
    """I tagli del registro dell'ATEM, in fotogrammi dall'inizio della registrazione.

    sorgenti: {ingresso ATEM: numero della traccia}. programma: numero dell'angolo per gli
    ingressi senza traccia, oppure None. Restituisce anche la tabella da salvare in tagli.csv.
    """
    tracce = [c for c in camere.values() if c["inizio"] == 0 and not c.get("esterna")]
    tagli, nomi, programma_a = tagli_da_atem.leggi(cartella / "atem.jsonl", TB, arrivi_registratore(cartella))
    inizio = min(c["tc"] for c in tracce)
    fine = inizio + min(c["durata"] for c in tracce)
    nome = lambda s: nomi.get(s, (str(s), ""))[0]

    def angolo(s, lista):
        if s in sorgenti:
            return sorgenti[s], ""
        if programma is not None:
            return programma, f"{nome(s)}: no track records it, program used here"
        return (lista[-1][1] if lista else min(camere)), f"{nome(s)}: no track records it"

    lista, tabella = [], []
    a, nota = angolo(programma_a(inizio), lista)
    lista.append((0, a, "cut", 0, nota))
    tabella.append((tagli_da_atem.tc_testo(inizio, TB), "start", "", nome(programma_a(inizio)), camere[a]["nome"]))
    for t in tagli:
        if not inizio < t["tc"] < fine:
            continue
        fr = t["tc"] - inizio + (t["durata"] // 2 if t["tipo"] == "mix" else 0)   # la dissolvenza si dà dal centro
        a, nota = angolo(t["sorgente"], lista)
        nota = "; ".join(x for x in (nota, t["nota"].strip()) if x)
        lista.append((fr, a, t["tipo"], t["durata"], nota))
        tabella.append((tagli_da_atem.tc_testo(t["tc"], TB), t["tipo"], t["durata"] or "", nome(t["sorgente"]),
                        camere[a]["nome"]))
    return lista, tabella, nomi


def leggi_marker(cartella, inizio, durata):
    """I marker dati durante la registrazione, da marker.csv: (fotogramma dall'inizio, colore, nome, nota, durata)."""
    f = cartella / "marker.csv"
    if not f.exists():
        return []
    giorno = TB.giorno
    out = []
    for riga in f.read_text(encoding="utf-8").splitlines()[1:]:
        if not riga.strip():
            continue
        # il timecode può essere fra virgolette, e col drop-frame contiene il punto e virgola: nei file
        # scritti prima della 0.9 beta 5 senza virgolette (sezione 9.80)
        m = re.match(r'^"?(\d+:\d+:\d+[:;.,]\d+)"?;(.*)$', riga)
        if not m:
            continue
        tc = m.group(1)
        colore, dur, fonte, testo = (m.group(2).split(";", 3) + [""] * 4)[:4]
        fr = (tagli_da_atem.tc_da_testo(tc, TB) - inizio) % giorno
        if fr >= durata:
            print(f"⚠ marker at {tc} outside the recording: skipped")
            continue
        out.append((fr, colore or "Blue", testo or f"Marker {len(out) + 1}", f"{fonte} {tc}", max(1, int(dur or 1))))
    return out


def scrivi_edl_marker(marker, inizio, uscita):
    """Gli stessi marker in un EDL che Resolve importa sulla timeline (Timeline › Import › Timeline Markers
    from EDL): servono anche senza il .drp, su un'altra macchina."""
    righe = ["TITLE: mtsrec markers", "FCM: DROP FRAME" if TB.drop else "FCM: NON-DROP FRAME", ""]
    for i, (fr, colore, nome, nota, dur) in enumerate(marker, 1):
        a = tagli_da_atem.tc_testo(inizio + fr, TB)
        b = tagli_da_atem.tc_testo(inizio + fr + 1, TB)
        righe += [f"{i:03d}  001      V     C        {a} {b} {a} {b}  ",
                  f" |C:ResolveColor{colore} |M:{nome} |D:{dur}", ""]
    uscita.write_text("\n".join(righe), encoding="utf-8")


def tagli_finti(camere, ogni_secondi):
    fine = min(c["durata"] for c in camere.values())
    passo, numeri = round(ogni_secondi * TB.fps), sorted(camere)
    return [(fr, numeri[i % len(numeri)], "mix" if i % 5 == 4 else "cut", 12 if i % 5 == 4 else 0, "")
            for i, fr in enumerate(range(0, fine - passo, passo))]


SCRITTI = []          # i file per il montaggio già scritti: se poi Resolve non va, l'esito lo dice


def scrivi_esito(cartella, ok, testo):
    """progetto.json, che il registratore legge per la pagina. Una riga, testo senza virgolette doppie."""
    testo = " ".join(str(testo).replace('"', "'").split())
    try:
        # prima un file a parte e poi lo scambio: il registratore non deve mai leggerlo a metà
        tmp = cartella / "progetto.json.tmp"
        tmp.write_text(json.dumps({"ok": ok, "testo": testo}, ensure_ascii=False) + "\n", encoding="utf-8")
        tmp.replace(cartella / "progetto.json")
    except OSError as e:
        print("⚠ result not written:", e)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("cartella")
    modo = ap.add_mutually_exclusive_group(required=True)
    modo.add_argument("--atem", action="store_true")
    modo.add_argument("--tagli-finti", type=int)
    ap.add_argument("--sorgenti", default="")
    ap.add_argument("--hyperdeck-clip")
    ap.add_argument("--progetto")
    ap.add_argument("--drp")
    ap.add_argument("--senza-resolve", action="store_true", help="files for editing only (FCPXML, Premiere XML, EDL), no Resolve project")
    ap.add_argument("--dal-registratore", action="store_true", help="launched by the recorder at STOP")
    a = ap.parse_args()
    cartella = Path(a.cartella)
    (cartella / "progetto.json").unlink(missing_ok=True)
    try:
        testo = crea(a, cartella)
        scrivi_esito(cartella, True, testo)
        return
    except SystemExit as e:
        motivo = str(e.code) if e.code not in (None, 0) else "exited without giving a reason"
    except Exception as e:
        motivo = f"{type(e).__name__}: {e}"
    altri = f"; the folder still contains {' and '.join(SCRITTI)}" if SCRITTI else ""
    scrivi_esito(cartella, False, f"Resolve project not created: {motivo}{altri}")
    print(f"\n⚠ Resolve project not created: {motivo}{altri}")
    if a.dal_registratore:
        # Prima aspettava Invio per sempre: la finestra restava aperta e teneva occupata la cartella del
        # programma (17 novembre 2026, 9.75). Ora un minuto per leggere, Invio la chiude prima.
        import msvcrt, time
        print("\nThis window closes by itself in 60 seconds, or press Enter.", flush=True)
        fine = time.time() + 60
        while time.time() < fine:
            if msvcrt.kbhit() and msvcrt.getwch() in "\r\n":
                break
            time.sleep(0.2)
    sys.exit(1)


def crea(a, cartella):
    """Fa tutto e restituisce la riga per la pagina del registratore."""
    global TB
    TB = Cadenza.da_registrazione(cartella)
    genera_fcpxml.imposta_cadenza(TB)
    genera_premiere.imposta_cadenza(TB)
    print(f"frame rate: {TB.nome()}{' drop-frame' if TB.drop else ''}")
    camere = leggi_file(cartella)
    if not camere:
        sys.exit("no MOV files in the folder")
    partenze = {c["tc"] for c in camere.values()}
    if len(partenze) > 1:
        print("⚠ the tracks do not start at the same timecode:", sorted(partenze))
    inizio = min(partenze)
    nomi = {n: Path(c["nome"]).stem for n, c in camere.items()}

    programma = hd = None          # hd: la clip dell'HyperDeck, che non è una traccia del registratore
    esterne = set()                # le clip che non sono tracce del registratore: la loro durata non conta
    hd_ingressi = {}               # clip HyperDeck abbinate dalla pagina: {angolo: ingresso ATEM o "program"}
    for p, ingresso in clip_hyperdeck_abbinate(cartella):
        try:
            tc, durata, audio = sonda(p)
        except Exception as e:
            print(f"⚠ HyperDeck clip {p.name} skipped: {e}")
            continue
        n = max(camere) + 1
        scarto = (tc - inizio) % TB.giorno
        if scarto > TB.giorno // 2:
            scarto -= TB.giorno
        camere[n] = {"n": n, "nome": p.name, "percorso": p.resolve(), "inizio": scarto, "durata": durata, "tc": tc,
                     "audio": audio, "audio_file": audio, "angolo": f"angolo-{n}", "esterna": True}
        nomi[n] = f"HyperDeck {p.stem}"
        esterne.add(n)
        hd_ingressi[n] = ingresso
        print(f"HyperDeck: {p.name} → {'program' if ingresso == 'program' else 'ATEM input ' + ingresso},"
              f" from {tagli_da_atem.tc_testo(tc, TB)}, {durata} frames, {scarto:+d} frames relative to the tracks")
    if a.hyperdeck_clip:
        p = Path(a.hyperdeck_clip)
        tc, durata, audio = sonda(p)
        programma = hd = max(camere) + 1
        camere[programma] = {"n": programma, "nome": p.name, "percorso": p.resolve(), "inizio": tc - inizio,
                             "durata": durata, "tc": tc, "audio": audio, "audio_file": audio,
                             "angolo": f"angolo-{programma}", "esterna": True}
        nomi[programma] = "HyperDeck Program"
        esterne.add(hd)
        print(f"HyperDeck: {p.name}, from {tagli_da_atem.tc_testo(tc, TB)}, {durata} frames,"
              f" {tc - inizio:+d} frames relative to the tracks")

    if a.atem:
        sorgenti = {}
        for voce in filter(None, a.sorgenti.split(";")):
            ingresso, pezzo = voce.split("=", 1)
            trovate = [n for n, c in camere.items() if n not in esterne and pezzo.lower() in c["nome"].lower()]
            if len(trovate) != 1:
                sys.exit(f"--sorgenti {voce}: {len(trovate)} tracks match, exactly one is needed")
            sorgenti[int(ingresso)] = trovate[0]
        if not a.sorgenti:
            sorgenti, prog_mappa, righe = sorgenti_da_mappa(cartella, {n: c for n, c in camere.items() if n not in esterne})
            if righe:
                print("sources from the input map:", *righe, sep="\n")
                if programma is None and prog_mappa is not None:
                    programma = prog_mappa
                    nomi[programma] = f"{nomi[programma]} - Program"
            else:
                print("⚠ neither --sorgenti nor porte.json: all cuts go to the program angle, if there is one")
        programma = aggiungi_hyperdeck(camere, hd_ingressi, sorgenti, programma, nomi)
        tagli, tabella, nomi_atem = tagli_veri(camere, cartella, sorgenti, programma)
        tagli = copri_buchi(tagli, camere, programma, min(c["durata"] for n, c in camere.items() if n not in esterne))
        # tagli.csv dice il file che la timeline usa davvero, anche dove copri_buchi ha cambiato angolo
        tabella = [r[:4] + (camere[t[1]]["nome"],) for r, t in zip(tabella, tagli)]
        for ingresso, n in sorgenti.items():
            nomi[n] = f"{nomi[n]} - {nomi_atem.get(ingresso, (str(ingresso),))[0]}"
        with open(cartella / "tagli.csv", "w", encoding="utf-8") as f:
            f.write("timecode;type;duration;ATEM input;file\n")
            for r in tabella:   # il timecode fra virgolette: col drop-frame contiene il separatore (9.80)
                f.write(";".join([f'"{r[0]}"'] + [str(x) for x in r[1:]]) + "\n")
        titolo, file_xml = "Edit from ATEM cuts", "Edit - ATEM cuts.fcpxml"
    else:
        tagli = tagli_finti({n: c for n, c in camere.items() if n not in esterne}, a.tagli_finti)
        titolo, file_xml = "Edit FAKE CUTS", "Edit - FAKE CUTS.fcpxml"

    # Il formato della registrazione: 1080 o UHD (sezione 9.74), progressivo o interlacciato (9.79).
    # Senza registrazione.json, HD come prima.
    reg = cartella / "registrazione.json"
    modo = json.loads(reg.read_text(encoding="utf-8")).get("modo", "") if reg.exists() else ""
    larghezza, altezza = (3840, 2160) if modo.startswith("2160") else (1920, 1080)
    interlacciato = "i" in modo[4:5]
    fcpxml = cartella / file_xml
    r = scrivi_fcpxml(camere, tagli, fcpxml, nome=titolo, multicamera="Multicam",
                      programma=programma if programma is not None else 0, nomi_angoli=nomi, tc_inizio=inizio,
                      larghezza=larghezza, altezza=altezza, interlacciato=interlacciato)
    print(f"FCPXML: {r['pezzi']} clips, {r['dissolvenze']} dissolves, {r['angoli']} angles,"
          f" {len(r['note'])} notes -> {fcpxml}")
    SCRITTI.append("the FCPXML")
    marker = leggi_marker(cartella, inizio, min(c["durata"] for n, c in camere.items() if n not in esterne))
    if marker:
        scrivi_edl_marker(marker, inizio, cartella / "Marker.edl")
        print(f"markers: {len(marker)} from marker.csv, also in Marker.edl")

    # Per Premiere e gli altri programmi: Final Cut Pro 7 XML ed EDL, con gli stessi tagli (genera_premiere.py).
    base = Path(file_xml).stem
    xp = scrivi_xml_premiere(camere, tagli, cartella / f"{base} - Premiere.xml", titolo, inizio, marker,
                             larghezza=larghezza, altezza=altezza, interlacciato=interlacciato)
    ep = scrivi_edl(camere, tagli, cartella / f"{base}.edl", titolo, inizio)
    print(f"Premiere: XML with {xp['pezzi']} clips and {xp['dissolvenze']} dissolves"
          f"{', audio follows video' if xp.get('audio') else ', no audio'}, EDL with {ep['eventi']} events "
          f"(import the EDL into Premiere at {TB.nome()} fps{' drop-frame' if TB.drop else ''})")
    SCRITTI[:] = ["the FCPXML", "the Premiere XML and EDL"]
    if a.senza_resolve:
        return "FCPXML, XML and EDL ready; Resolve project not requested"

    resolve = collega_resolve()
    nome = a.progetto or f"mtsrec - {cartella.name}"
    esito = crea_in_resolve(resolve, fcpxml.resolve(), nome, titolo, r["note"], marker, larghezza, altezza,
                            cadenza=TB, interlacciato=interlacciato)
    print(f"Resolve: project “{esito['progetto']}”, {esito['pezzi']} clips, {esito['dissolvenze']} dissolves,"
          f" {esito['marker']} markers")

    pm = resolve.GetProjectManager()
    p = pm.GetCurrentProject()
    tl = p.GetCurrentTimeline()
    print("timeline:", tl.GetName(), "| start", tl.GetStartTimecode(), "| frames",
          tl.GetEndFrame() - tl.GetStartFrame())
    for c in p.GetMediaPool().GetRootFolder().GetClipList():
        pr = c.GetClipProperty()
        print(f"  {pr.get('Clip Name')}: {pr.get('Video Codec')} | {pr.get('Format')} | TC {pr.get('Start TC')}"
              f" | {pr.get('Frames')} frames | {pr.get('Type')}")
    drp = Path(a.drp) if a.drp else cartella / f"{esito['progetto']}.drp"
    ok = pm.ExportProject(esito["progetto"], str(drp), False)
    print(".drp:", drp if ok else "EXPORT FAILED", f"{drp.stat().st_size / 1e6:.1f} MB" if ok else "")
    def quanti(n, uno, tanti):
        return f"{n} {uno if n == 1 else tanti}"
    return (f"Resolve project ready: “{esito['progetto']}”, {quanti(esito['pezzi'], 'clip', 'clips')}, "
            f"{quanti(esito['dissolvenze'], 'dissolve', 'dissolves')}"
            f"{', ' + quanti(esito['marker'], 'marker', 'markers') if esito['marker'] else ''}"
            f"{'' if ok else '; .drp not exported'}")


if __name__ == "__main__":
    main()
