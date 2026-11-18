"""Registro dell'ATEM per mtsrec: sola lettura in rete, UDP 9910.

Fa l'aggancio, conferma i pacchetti e scrive ogni comando che arriva con l'istante di arrivo
(perf_counter_ns, lo stesso orologio di Windows del registratore), riga per riga e subito su
disco: se la macchina si spegne il registro arriva fino all'ultimo comando ricevuto.
Non manda niente che cambi lo stato dell'ATEM. Protocollo di atem_rete.py (sezione 9.48).

Lo lancia il registratore con --registro-atem e lo ferma chiudendogli lo standard input.
Da solo:
    py -3.13 registro_atem.py --uscita atem.jsonl [--ip 192.168.10.240] [--secondi 60]
"""
import argparse, json, socket, struct, sys, threading, time

PORTA = 9910


def pacchetto(flag, sessione, ack=0, id_locale=0, dati=b""):
    return struct.pack(">HHHHHH", (flag << 11) | (12 + len(dati)), sessione, ack, 0, 0, id_locale) + dati


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("--uscita", required=True)
    ap.add_argument("--ip", default="192.168.10.240")
    ap.add_argument("--secondi", type=float, default=0, help="0: until standard input is closed")
    a = ap.parse_args()

    stop = threading.Event()
    if a.secondi > 0:
        threading.Timer(a.secondi, stop.set).start()
    else:
        threading.Thread(target=lambda: (sys.stdin.read(), stop.set()), daemon=True).start()

    out = open(a.uscita, "w", encoding="utf-8", buffering=1)          # una riga alla volta
    out.write(json.dumps({"inizio_epoch": time.time(), "inizio_perf_ns": time.perf_counter_ns(), "ip": a.ip}) + "\n")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(0.5)
    s.sendto(pacchetto(0x02, 0x53AB, dati=bytes([1, 0, 0, 0, 0, 0, 0, 0])), (a.ip, PORTA))
    comandi, ultimo, stato = 0, time.time(), False
    try:
        while not stop.is_set():
            try:
                d, _ = s.recvfrom(65535)
            except socket.timeout:
                if time.time() - ultimo > 3:                       # l'ATEM tace: lo si dice nel registro
                    out.write(json.dumps({"perf_ns": time.perf_counter_ns(), "avviso": "ATEM silent for 3 s"}) + "\n")
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
            while i + 8 <= lunghezza:
                n = struct.unpack(">H", d[i:i + 2])[0]
                if n < 8:
                    break
                nome = d[i + 4:i + 8].decode("ascii", "replace")
                out.write(json.dumps({"perf_ns": t, "cmd": nome, "hex": d[i + 8:i + n].hex()}) + "\n")
                comandi += 1
                if nome == "InCm" and not stato:
                    stato = True
                    print("ATEM log: initial state received", flush=True)
                i += n
    except Exception as e:
        out.write(json.dumps({"perf_ns": time.perf_counter_ns(), "errore": repr(e)}) + "\n")
        print("ATEM log: error", repr(e), flush=True)
    finally:
        s.close()
        out.write(json.dumps({"perf_ns": time.perf_counter_ns(), "fine": True, "comandi": comandi}) + "\n")
        out.close()
    print(f"ATEM log: {comandi} commands", flush=True)


if __name__ == "__main__":
    main()
