"""Registro dell'ATEM per mtsrec: sola lettura in rete, UDP 9910.

Fa l'aggancio, conferma i pacchetti e scrive ogni comando che arriva con l'istante di arrivo
(perf_counter_ns, lo stesso orologio di Windows del registratore), riga per riga e subito su
disco: se la macchina si spegne il registro arriva fino all'ultimo comando ricevuto.
Non manda niente che cambi lo stato dell'ATEM. Protocollo di atem_rete.py (sezione 9.48).

Dalla beta 12 tiene anche lo stato dal vivo per la pagina del registratore (sezione 9.93):
    --vivo FILE    programma, anteprima, mix e aux del M/E 1, riscritto a ogni cambio e ogni secondo
    --tagli FILE   i tagli e le dissolvenze dall'aggancio in poi, coi nomi delle sorgenti
Senza --uscita non scrive il registro grezzo: è l'ascolto da fermo, che dà il tally prima del REC,
e se l'ATEM tace per 5 secondi esce, così il registratore lo riaggancia da capo.

Lo lancia il registratore e lo ferma chiudendogli lo standard input.
Da solo:
    py -3.13 registro_atem.py --uscita atem.jsonl [--ip 192.168.10.240] [--secondi 60]
    py -3.13 registro_atem.py --vivo vivo.json --tagli tagli.json --modo 1080p25 [--secondi 60]
"""
import argparse, json, os, socket, struct, sys, threading, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # il Python del pacchetto non guarda la cartella dello script
from cadenza import Cadenza

PORTA = 9910
EVENTI = ("PrgI", "PrvI", "TrPs", "TlIn", "TlSr")


def pacchetto(flag, sessione, ack=0, id_locale=0, dati=b""):
    return struct.pack(">HHHHHH", (flag << 11) | (12 + len(dati)), sessione, ack, 0, 0, id_locale) + dati


def scrivi_subito(percorso, dati):
    """Il file intero o niente: la pagina non deve mai leggere mezzo JSON. False se il lettore lo tiene aperto."""
    tmp = percorso + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(json.dumps(dati, ensure_ascii=False, separators=(",", ":")))
    try:
        os.replace(tmp, percorso)
        return True
    except OSError:
        return False


class Vivo:
    """Programma, anteprima, aux e tagli del M/E 1 come li manda l'ATEM. I tagli con la stessa regola di
    tagli_da_atem.py, ma solo dalla rete: il timecode X dell'ultimo Time, più 2 (sezione 9.93)."""

    def __init__(self, vivo, tagli, cad, ip):
        self.file_vivo, self.file_tagli, self.cad, self.ip = vivo, tagli, cad, ip
        self.periodo = 1e9 / cad.fps
        self.pgm = self.pvw = None          # quello che dice l'ATEM, per il tally
        self.programma = None               # quello che usa la regola dei tagli, come in tagli_da_atem.py
        self.mix, self.stile, self.pronto, self.muto = False, 0, False, False
        self.aux, self.nomi, self.tagli = {}, {}, []
        self.n_cut = self.n_mix = self.rev = 0
        self.pgm0 = None                    # il programma quando arriva lo stato iniziale: il primo pezzo della corsia
        self.tempo = None                   # (fotogramma, perf_ns) dell'ultimo Time senza eventi nel pacchetto
        self.scritto = self.scritti_tagli = 0.0
        self.cambiato = self.tagli_cambiati = True
        self.errore = ""

    def _x(self, t, nel_pacchetto):
        if nel_pacchetto is not None:
            return nel_pacchetto - 1
        if self.tempo is None:
            return None
        x, t0 = self.tempo
        return x + int((t - t0) // self.periodo)

    def _taglio(self, tipo, durata, sorgente, t, nel_pacchetto):
        x = self._x(t, nel_pacchetto)
        self.tagli.append({"tc": None if x is None else (x + 2) % self.cad.giorno, "tipo": tipo, "d": durata, "s": sorgente})
        if tipo == "cut":
            self.n_cut += 1
        else:
            self.n_mix += 1
        self.rev += 1
        self.tagli_cambiati = True

    def pacchetto(self, comandi, t):
        """comandi: (nome, dati) di un pacchetto, nell'ordine; t: l'istante di arrivo."""
        nel_pacchetto, eventi = None, False
        for c, b in comandi:
            if c == "Time" and len(b) >= 4:
                x = self.cad.da_etichetta(b[0], b[1], b[2], b[3])
                if x is not None:
                    nel_pacchetto = x
            eventi |= c in EVENTI
        for c, b in comandi:
            if c == "InPr" and len(b) >= 26:
                self.nomi[struct.unpack(">H", b[0:2])[0]] = b[2:22].split(b"\0")[0].decode("utf-8", "replace")
                self.rev += 1
                self.tagli_cambiati = True
            elif c == "AuxS" and len(b) >= 4:
                self.aux[b[0] + 1] = struct.unpack(">H", b[2:4])[0]
                self.cambiato = True
            elif c == "TrSS" and len(b) >= 2 and b[0] == 0:
                self.stile = b[1]
            elif c == "PrvI" and len(b) >= 4 and b[0] == 0:
                self.pvw = struct.unpack(">H", b[2:4])[0]
                self.cambiato = True
            elif c == "InCm":
                self.pronto = True
                if self.pgm0 is None:
                    self.pgm0 = self.programma
                    self.tagli_cambiati = True
                    self.rev += 1
                self.cambiato = True
            elif c == "PrgI" and len(b) >= 4 and b[0] == 0:
                s = struct.unpack(">H", b[2:4])[0]
                if self.pronto and s != self.programma and not self.mix:
                    self._taglio("cut", 0, s, t, nel_pacchetto)
                self.mix = False                    # PrgI arriva a mix finito
                self.programma = self.pgm = s
                self.cambiato = True
            elif c == "TrPs" and len(b) >= 3 and b[0] == 0 and self.pronto:
                if b[1] and not self.mix:
                    self.mix = True
                    self._taglio("mix", b[2] + 1, self.pvw, t, nel_pacchetto)
                    self.programma = self.pvw
                    self.cambiato = True
                elif not b[1] and self.mix:
                    self.mix = False
                    self.cambiato = True
        if nel_pacchetto is not None and not eventi:
            self.tempo = (nel_pacchetto, t)
        if self.muto:
            self.muto = False
            self.cambiato = True

    def scrivi(self, forza=False):
        ora = time.time()
        if self.file_vivo and (forza or (self.cambiato and ora - self.scritto >= 0.05) or ora - self.scritto >= 1.0):
            if scrivi_subito(self.file_vivo, {
                    "t": round(ora, 3), "ip": self.ip, "pronto": self.pronto, "muto": self.muto, "errore": self.errore,
                    "pgm": self.pgm, "pvw": self.pvw, "mix": self.mix,
                    "aux": {str(k): v for k, v in sorted(self.aux.items())},
                    "cut": self.n_cut, "mix_n": self.n_mix, "rev": self.rev}):
                self.scritto, self.cambiato = ora, False
        if self.file_tagli and (forza or (self.tagli_cambiati and ora - self.scritti_tagli >= 0.2)):
            if scrivi_subito(self.file_tagli, {
                    "rev": self.rev, "fps": self.cad.fps, "pgm0": self.pgm0,
                    "nomi": {str(k): v for k, v in sorted(self.nomi.items())},
                    "tagli": self.tagli}):
                self.scritti_tagli, self.tagli_cambiati = ora, False


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--uscita", help="raw log, one command per line; without it nothing is logged")
    ap.add_argument("--vivo", help="live state for the recorder page")
    ap.add_argument("--tagli", help="cuts and dissolves since the connection, for the Program lane")
    ap.add_argument("--modo", default="1080p25", help="recorder format, for the timecode of the cuts")
    ap.add_argument("--ip", default="192.168.10.240")
    ap.add_argument("--secondi", type=float, default=0, help="0: until standard input is closed")
    a = ap.parse_args()
    ascolto = not a.uscita                 # da fermo: solo lo stato dal vivo, ed esce se l'ATEM tace

    stop = threading.Event()
    if a.secondi > 0:
        threading.Timer(a.secondi, stop.set).start()
    else:
        threading.Thread(target=lambda: (sys.stdin.read(), stop.set()), daemon=True).start()

    vivo = Vivo(a.vivo, a.tagli, Cadenza.da_modo(a.modo), a.ip) if (a.vivo or a.tagli) else None
    out = open(a.uscita, "w", encoding="utf-8", buffering=1) if a.uscita else None   # una riga alla volta
    if out:
        out.write(json.dumps({"inizio_epoch": time.time(), "inizio_perf_ns": time.perf_counter_ns(), "ip": a.ip}) + "\n")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    s.sendto(pacchetto(0x02, 0x53AB, dati=bytes([1, 0, 0, 0, 0, 0, 0, 0])), (a.ip, PORTA))
    comandi, ultimo, stato, inizio = 0, time.time(), False, time.time()
    codice = 0
    try:
        while not stop.is_set():
            if vivo:
                vivo.scrivi()
            try:
                d, _ = s.recvfrom(65535)
            except socket.timeout:
                if time.time() - ultimo > 3:                       # l'ATEM tace: lo si dice nel registro
                    if out:
                        out.write(json.dumps({"perf_ns": time.perf_counter_ns(), "avviso": "ATEM silent for 3 s"}) + "\n")
                    if vivo and not vivo.muto:
                        vivo.muto, vivo.cambiato = True, True
                    if ascolto and time.time() - ultimo > 5:
                        codice = 2                                 # il registratore lo rilancia, con un aggancio nuovo
                        break
                    if out:
                        ultimo = time.time()
                continue
            t = time.perf_counter_ns()
            ultimo = time.time()
            flag = d[0] >> 3
            sess = struct.unpack(">H", d[2:4])[0]
            id_remoto = struct.unpack(">H", d[10:12])[0]
            if flag & 0x02:
                if d[12] != 2:
                    raise RuntimeError(f"connection refused, code {d[12]}")
                s.sendto(pacchetto(0x10, sess, 0), (a.ip, PORTA))
                continue
            if flag & 0x01:
                s.sendto(pacchetto(0x10, sess, id_remoto), (a.ip, PORTA))
            lunghezza = struct.unpack(">H", d[0:2])[0] & 0x07FF
            i = 12
            nel = []
            while i + 8 <= lunghezza:
                n = struct.unpack(">H", d[i:i + 2])[0]
                if n < 8:
                    break
                nome = d[i + 4:i + 8].decode("ascii", "replace")
                if out:
                    out.write(json.dumps({"perf_ns": t, "cmd": nome, "hex": d[i + 8:i + n].hex()}) + "\n")
                nel.append((nome, d[i + 8:i + n]))
                comandi += 1
                if nome == "InCm" and not stato:
                    stato = True
                    if out:
                        print("ATEM log: initial state received", flush=True)
                i += n
            if vivo and nel:
                vivo.pacchetto(nel, t)
    except Exception as e:
        if out:
            out.write(json.dumps({"perf_ns": time.perf_counter_ns(), "errore": repr(e)}) + "\n")
            print("ATEM log: error", repr(e), flush=True)
        if vivo:
            vivo.errore = str(e)
        codice = 1
    finally:
        s.close()
        if vivo:
            vivo.muto = vivo.muto or codice != 0
            vivo.scrivi(forza=True)
        if out:
            out.write(json.dumps({"perf_ns": time.perf_counter_ns(), "fine": True, "comandi": comandi}) + "\n")
            out.close()
    if out:
        print(f"ATEM log: {comandi} commands", flush=True)
    return codice


if __name__ == "__main__":
    sys.exit(main())
