"""La cadenza di una registrazione e il suo timecode, per gli script (sezione 9.79).

È la stessa logica di mtsrec\\src\\formato.cpp: i timecode sono fotogrammi dalla mezzanotte
contati davvero, uno per fotogramma. Col drop-frame (29,97 e 59,94) l'etichetta salta 2 o 4 numeri
a ogni minuto non multiplo di dieci, il conto no: così la differenza fra due timecode resta un numero
di fotogrammi a qualunque cadenza.

La cadenza di una registrazione si legge da registrazione.json («cadenza», scritta dal registratore
dal 18 novembre 2026); nelle registrazioni di prima c'è solo «fps» ed erano tutte a 25.

    from cadenza import Cadenza
    c = Cadenza.da_registrazione(cartella)
    c.testo(900025)          # "10:00:01:00"
    c.da_testo("10:00:01;00")
"""
import json, re
from pathlib import Path


class Cadenza:
    def __init__(self, base=25, num=25, den=1, drop=False, interlacciato=False):
        self.base, self.num, self.den, self.drop = int(base), int(num), int(den), bool(drop)
        # 1080i: il fotogramma ha due campi e l'ATEM lavora a campi (sezione 9.80). Non entra nel confronto
        # fra cadenze: il timecode di un 1080i50 è quello di un 1080p25
        self.interlacciato = bool(interlacciato)

    def __repr__(self):
        return f"Cadenza({self.nome()}{' DF' if self.drop else ''})"

    def __eq__(self, o):
        return isinstance(o, Cadenza) and (self.base, self.num, self.den, self.drop) == (o.base, o.num, o.den, o.drop)

    # ------------------------------------------------------------ da dove viene
    @classmethod
    def da_modo(cls, modo, drop=None):
        """Dal nome di un formato del registratore: «1080p25», «1080i59.94», «2160p29.97»."""
        m = re.match(r"^(\d+)([pi])([\d.]+)$", str(modo or ""))
        if not m:
            return cls()
        v = float(m.group(3))
        fps = v / 2 if m.group(2) == "i" else v
        base = round(fps)
        ntsc = abs(fps - base) > 0.001
        c = cls(base, base * 1000 if ntsc else base, 1001 if ntsc else 1, ntsc and base % 30 == 0, m.group(2) == "i")
        if drop is not None and ntsc:
            c.drop = bool(drop)
        return c

    @classmethod
    def da_registrazione(cls, cartella):
        """Da registrazione.json nella cartella; senza, 25 come le registrazioni di prima."""
        p = Path(cartella) / "registrazione.json"
        if not p.exists():
            return cls()
        j = json.loads(p.read_text(encoding="utf-8"))
        c = j.get("cadenza")
        i = bool(j.get("interlacciato")) or "i" in str(j.get("modo", ""))
        if isinstance(c, dict):
            return cls(c.get("base", 25), c.get("num", 25), c.get("den", 1), c.get("drop", False), i)
        if j.get("modo"):
            return cls.da_modo(j["modo"])
        f = j.get("fps") or 25
        return cls(round(f), round(f), 1, False)

    @staticmethod
    def di(x):
        """Una Cadenza da un numero di fotogrammi al secondo intero, per il codice di prima che passava fps=25."""
        return x if isinstance(x, Cadenza) else Cadenza(int(x), int(x), 1, False)

    # ------------------------------------------------------------ numeri
    @property
    def fps(self):
        return self.num / self.den

    @property
    def ntsc(self):
        return self.den == 1001

    @property
    def salto(self):
        return self.base // 15 if self.drop else 0

    @property
    def giorno(self):
        return self.base * 86400 - self.salto * 1296

    def nome(self):
        return str(self.num) if self.den == 1 else f"{self.fps:.2f}"

    # ------------------------------------------------------------ etichette
    def testo(self, f, separatore=None):
        f %= self.giorno
        if self.drop:
            d_, al_minuto, ai_dieci = self.salto, self.base * 60 - self.salto, self.base * 600 - self.salto * 9
            d, m = divmod(f, ai_dieci)
            f += d_ * 9 * d + (d_ * ((m - d_) // al_minuto) if m > d_ else 0)
        b = self.base
        sep = separatore or (";" if self.drop else ":")
        return f"{f // b // 3600:02d}:{f // b // 60 % 60:02d}:{f // b % 60:02d}{sep}{f % b:02d}"

    def da_etichetta(self, h, m, s, f):
        """Fotogrammi dalla mezzanotte; None se l'etichetta non esiste (col drop-frame 00:01:00;00 non c'è)."""
        if not (0 <= h < 24 and 0 <= m < 60 and 0 <= s < 60 and 0 <= f < self.base):
            return None
        d_ = self.salto
        if d_ and s == 0 and m % 10 and f < d_:
            return None
        minuti = h * 60 + m
        return ((h * 60 + m) * 60 + s) * self.base + f - d_ * (minuti - minuti // 10)

    def da_testo(self, t):
        p = re.split(r"[:;.,]", str(t).strip())
        if len(p) != 4:
            raise ValueError(f"timecode not readable: {t!r}")
        r = self.da_etichetta(*(int(x) for x in p))
        if r is None:
            raise ValueError(f"timecode does not exist at {self.nome()}{' DF' if self.drop else ''}: {t!r}")
        return r

    # ------------------------------------------------------------ per gli altri programmi
    def frazione(self, f):
        """I fotogrammi in secondi razionali, come li scrive l'FCPXML: 1001/30000s per uno a 29,97."""
        return f"{f * self.den}/{self.num}s"

    def secondi(self, f):
        return f * self.den / self.num

    def campioni(self, f):
        """I campioni a 48 kHz in f fotogrammi, come li conta il registratore (Cadenza::campioni)."""
        return f * 48000 * self.den // self.num

    def fotogrammi_da_campioni(self, n):
        return n * self.num // (48000 * self.den)


def righe_csv_traccia(percorso):
    """Le righe del CSV di una traccia come dizionari. Il timecode drop-frame contiene il punto e virgola,
    che è anche il separatore: dalla 0.9 beta 5 il registratore lo scrive fra virgolette, e le righe
    spezzate dei CSV di prima (un campo in più) qui si rimettono insieme. Sezione 9.80."""
    import csv
    with open(percorso, encoding="utf-8", newline="") as f:
        lettore = csv.reader(f, delimiter=";")
        intestazione = next(lettore, None)
        if not intestazione:
            return
        n = len(intestazione)
        for campi in lettore:
            if len(campi) == n + 1:
                campi = [campi[0] + ";" + campi[1]] + campi[2:]
            if len(campi) == n:
                yield dict(zip(intestazione, campi))
