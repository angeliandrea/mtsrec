"""Gli stacchi per misurare il ritardo delle porte (sezione 9.60). Lo lancia il registratore.

    python ritardi_porte.py --atem 192.168.10.240 [--aux 3,9] [--stacchi 8]

Porta le aux indicate sul programma del M/E 1, fa una serie di stacchi fra barre e nero sul programma
e poi rimette tutto com'era: programma, anteprima e aux. Intanto il registratore annota la luminanza di
ogni fotogramma su ogni porta col suo timecode, e confronta quando lo stacco arriva sulla porta da
misurare con quando arriva su una porta che porta il timecode dell'ATEM nel segnale.

⚠ SCRIVE NELL'ATEM e cambia il programma: solo fuori diretta. Prima di scrivere lo stato va su disco,
così se lo script cade si sa com'era. Stampa un JSON su una riga:
    {"ok": true, "stacchi": 8, "rimesso": true}
"""
import argparse, datetime, json, os, random, struct, sys, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # il Python del pacchetto non guarda la cartella dello script
import mappa_porte as mp
from mappa_porte import Atem, BARRE, NERO

PROGRAMMA_ME1 = 10010
ME = 0


class AtemMe(Atem):
    """La sessione di mappa_porte che segue anche programma e anteprima del M/E 1."""

    def __init__(self, ip):
        self.pgm = self.pvw = None
        super().__init__(ip)

    def _ascolta(self):
        while not self.basta:
            try:
                d, _ = self.s.recvfrom(65535)
            except OSError:
                continue
            flag, sess = d[0] >> 3, struct.unpack(">H", d[2:4])[0]
            if flag & 0x02:
                self.s.sendto(mp.pacchetto(0x10, sess, 0), (self.ip, mp.PORTA_ATEM))
                continue
            self.sessione = sess
            if flag & 0x01:
                self.s.sendto(mp.pacchetto(0x10, sess, struct.unpack(">H", d[10:12])[0]), (self.ip, mp.PORTA_ATEM))
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
                elif nome == "PrgI" and b[0] == ME:
                    self.pgm = struct.unpack(">H", b[2:4])[0]
                elif nome == "PrvI" and b[0] == ME:
                    self.pvw = struct.unpack(">H", b[2:4])[0]
                elif nome == "InCm":
                    self.pronto.set()
                i += n

    def manda(self, nome, dati, fatto, attesa=1.0):
        self.id += 1
        pk = mp.comando(nome, dati)
        for _ in range(5):
            self.s.sendto(mp.pacchetto(0x01, self.sessione, 0, self.id, pk), (self.ip, mp.PORTA_ATEM))
            t = time.time()
            while time.time() - t < attesa:
                if self.id in self.confermati and fatto():
                    return True
                time.sleep(0.002)
        return False

    def programma(self, s):
        return self.manda("CPgI", struct.pack(">BBH", ME, 0, s), lambda: self.pgm == s)

    def anteprima(self, s):
        return self.manda("CPvI", struct.pack(">BBH", ME, 0, s), lambda: self.pvw == s)


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--atem", required=True)
    ap.add_argument("--aux", default="", help="aux outputs to switch to program, for example 3,9")
    ap.add_argument("--stacchi", type=int, default=8)
    a = ap.parse_args()
    aux = [int(x) for x in a.aux.split(",") if x.strip()]
    try:
        atem = AtemMe(a.atem)
    except Exception as e:
        print(json.dumps({"errore": str(e)}, ensure_ascii=False))
        return 1
    prima_aux = {x: atem.aux.get(x) for x in aux}
    prima = {"pgm": atem.pgm, "pvw": atem.pvw, "aux": prima_aux}
    if atem.pgm is None or any(v is None for v in prima_aux.values()):
        atem.chiudi()
        print(json.dumps({"errore": "the ATEM did not report program or aux"}, ensure_ascii=False))
        return 1
    cartella = Path(os.path.expandvars(r"%USERPROFILE%\mtsrec"))
    cartella.mkdir(parents=True, exist_ok=True)
    (cartella / f"ATEM before delay measurement {datetime.datetime.now():%Y-%m-%d %H%M%S}.json").write_text(
        json.dumps({"atem": a.atem, **prima}, indent=2), encoding="utf-8")

    fatti, ok = 0, True
    try:
        for x in aux:
            ok = atem.aux_su(x, PROGRAMMA_ME1) and ok
        time.sleep(0.8)                                  # le porte si assestano sul segnale nuovo
        s = NERO if atem.pgm == BARRE else BARRE
        for _ in range(a.stacchi):
            ok = atem.programma(s) and ok
            fatti += 1
            time.sleep(0.7 + random.random() * 0.6)
            s = NERO if s == BARRE else BARRE
    finally:
        atem.programma(prima["pgm"])
        if prima["pvw"] is not None:
            atem.anteprima(prima["pvw"])
        for x, v in prima_aux.items():
            atem.aux_su(x, v)
        time.sleep(0.3)
        atem.chiudi()
    verifica = AtemMe(a.atem)                            # una sessione nuova: lo stato vero
    dopo = {"pgm": verifica.pgm, "pvw": verifica.pvw, "aux": {x: verifica.aux.get(x) for x in aux}}
    verifica.chiudi()
    rimesso = dopo == prima
    print(json.dumps({"ok": ok, "stacchi": fatti, "rimesso": rimesso, "prima": prima, "dopo": dopo}, ensure_ascii=False))
    return 0 if rimesso else 2


if __name__ == "__main__":
    sys.exit(main())
