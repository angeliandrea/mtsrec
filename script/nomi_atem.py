"""I nomi delle sorgenti sulle aux dell'ATEM, per dare ai file il nome della sorgente (sezione 10).

    python nomi_atem.py --atem 192.168.10.240

Stampa un JSON su una riga:
    {"aux": {"9": {"sorgente": 1, "nome": "Camera 1"}, ...}, "nomi": {"1": "Camera 1", ...}}
Si aggancia, aspetta lo stato iniziale che l'ATEM manda da solo e se ne va: SOLA LETTURA, nell'ATEM
non scrive niente, quindi si può usare anche in diretta. Lo lancia il registratore prima del REC.
"""
import argparse, json, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))   # il Python del pacchetto non guarda la cartella dello script
from mappa_porte import Atem


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--atem", required=True)
    a = ap.parse_args()
    try:
        atem = Atem(a.atem)
    except Exception as e:
        print(json.dumps({"errore": str(e)}, ensure_ascii=False))
        return 1
    aux = {str(n): {"sorgente": s, "nome": atem.nomi.get(s, "")} for n, s in sorted(atem.aux.items())}
    nomi = {str(s): n for s, n in sorted(atem.nomi.items())}
    atem.chiudi()
    print(json.dumps({"aux": aux, "nomi": nomi}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
