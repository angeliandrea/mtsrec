"""Il formato video dell'ATEM per mtsrec: sola lettura in rete, UDP 9910 (sezione 9.79).

Si aggancia, aspetta lo stato iniziale (InCm) e stampa una riga JSON:
    {"ok": true, "video_mode": 10, "formato": "1080p25", "modello": "...", "media_pool_usati": 0}
«formato» è il nome del registratore (mtsrec\\src\\formato.cpp) o null se il registratore quel
formato non lo registra (720p, 8K, SD). Il registratore registra quello che dice l'ATEM, senza
convertire né scalare (Andrea, 17 novembre 2026).

Non manda niente che cambi lo stato dell'ATEM. Con --imposta cambia il formato: SOLO per le prove fuori
diretta, e cancella il media pool (manuale ATEM Constellation, pagina 31).

    py -3.13 stato_atem.py [--ip 192.168.10.240]
    py -3.13 stato_atem.py --ip 192.168.10.240 --imposta 1080p50
"""
import argparse, json, socket, struct, sys, time

PORTA = 9910

# Il numero del formato nel comando VidM (atem-connection, enum VideoMode) → nome del registratore.
# None: formati che l'ATEM ha e il registratore no.
VIDEO_MODE = {
    0: None, 1: None, 2: None, 3: None,                      # SD
    4: None, 5: None,                                        # 720p50, 720p59.94
    6: "1080i50", 7: "1080i59.94",
    8: "1080p23.98", 9: "1080p24", 10: "1080p25", 11: "1080p29.97", 12: "1080p50", 13: "1080p59.94",
    14: "2160p23.98", 15: "2160p24", 16: "2160p25", 17: "2160p29.97", 18: "2160p50", 19: "2160p59.94",
    20: None, 21: None, 22: None, 23: None, 24: None, 25: None,   # 8K
    26: "1080p30", 27: "1080p60", 28: None, 29: "1080i60",        # 28 = 720p60
}
NUMERO = {v: k for k, v in VIDEO_MODE.items() if v}


def pacchetto(flag, sessione, ack=0, id_locale=0, dati=b""):
    return struct.pack(">HHHHHH", (flag << 11) | (12 + len(dati)), sessione, ack, 0, 0, id_locale) + dati


def comando(nome, dati):
    return struct.pack(">HH", 8 + len(dati), 0) + nome.encode("ascii") + dati


class Sessione:
    def __init__(self, ip):
        self.ip, self.s = ip, socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.settimeout(0.3)
        self.sessione, self.id, self.confermati = None, 0, set()
        self.stato = {"video_mode": None, "modello": None, "media_pool_usati": 0, "media_pool": 0}
        self.pronto = False

    def apri(self, limite=10.0):
        self.s.sendto(pacchetto(0x02, 0x53AB, dati=bytes([1, 0, 0, 0, 0, 0, 0, 0])), (self.ip, PORTA))
        t = time.time()
        while not self.pronto and time.time() - t < limite:
            self.giro()
        return self.pronto

    def giro(self):
        try:
            d, _ = self.s.recvfrom(65535)
        except (socket.timeout, OSError):
            return
        flag, sess = d[0] >> 3, struct.unpack(">H", d[2:4])[0]
        if flag & 0x02:
            if d[12] != 2:
                raise RuntimeError(f"connection refused, code {d[12]}")
            self.s.sendto(pacchetto(0x10, sess, 0), (self.ip, PORTA))
            return
        self.sessione = sess
        if flag & 0x01:
            self.s.sendto(pacchetto(0x10, sess, struct.unpack(">H", d[10:12])[0]), (self.ip, PORTA))
        if flag & 0x10:
            self.confermati.add(struct.unpack(">H", d[4:6])[0])
        lunghezza, i = struct.unpack(">H", d[0:2])[0] & 0x07FF, 12
        while i + 8 <= lunghezza:
            n = struct.unpack(">H", d[i:i + 2])[0]
            if n < 8:
                break
            nome, b = d[i + 4:i + 8].decode("ascii", "replace"), d[i + 8:i + n]
            if nome == "VidM":
                self.stato["video_mode"] = b[0]
            elif nome == "_pin":
                self.stato["modello"] = b[:44].split(b"\0")[0].decode("utf-8", "replace")
            elif nome == "MPfe" and b[0] == 0:                 # un fotogramma fermo del media pool
                self.stato["media_pool"] += 1
                if b[4]:
                    self.stato["media_pool_usati"] += 1
            elif nome == "MPfe" and b[0] != 0 and b[4]:        # fotogrammi delle clip
                self.stato["clip_usate"] = self.stato.get("clip_usate", 0) + 1
            elif nome == "MPCS" and len(b) > 1 and b[1]:         # una clip caricata
                self.stato["clip"] = self.stato.get("clip", 0) + 1
            elif nome == "InCm":
                self.pronto = True
            i += n

    def imposta(self, numero, attesa=8.0):
        self.id += 1
        dati = comando("CVdM", bytes([numero, 0, 0, 0]))
        self.s.sendto(pacchetto(0x01, self.sessione, 0, self.id, dati), (self.ip, PORTA))
        t = time.time()
        while time.time() - t < attesa:
            self.giro()
            if self.stato["video_mode"] == numero:
                return True
        return False

    def chiudi(self):
        self.s.close()


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default="192.168.10.240")
    ap.add_argument("--imposta", help="test only, never during a live event: sets the ATEM video standard")
    a = ap.parse_args()
    s = Sessione(a.ip)
    try:
        if not s.apri():
            print(json.dumps({"ok": False, "errore": f"ATEM {a.ip} did not answer"}))
            return 1
        if a.imposta:
            if a.imposta not in NUMERO:
                print(json.dumps({"ok": False, "errore": f"unknown format {a.imposta}"}))
                return 2
            if not s.imposta(NUMERO[a.imposta]):
                print(json.dumps({"ok": False, "errore": "the ATEM did not confirm the new standard",
                                  "video_mode": s.stato["video_mode"]}))
                return 1
        st = dict(s.stato)
        st["ok"] = True
        st["formato"] = VIDEO_MODE.get(st["video_mode"])
        print(json.dumps(st))
        return 0
    finally:
        s.chiudi()


if __name__ == "__main__":
    sys.exit(main())
