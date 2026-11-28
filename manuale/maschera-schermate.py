"""Prepara le immagini del manuale (img\m-*.png) dalle catture della finestra del registratore.

    py -3.10 maschera-schermate.py [cartella delle catture]

Le catture sono a 1920x1080, fatte con Chrome senza finestra sul registratore acceso durante una registrazione
di prova. Indirizzi di rete, nome del computer e cartella di destinazione vengono sostituiti con valori d'esempio
nella pagina PRIMA della cattura, quindi qui non c'è più niente da coprire: questo script ritaglia e basta.
Fino al manuale rev 07 copriva a mano i rettangoli con gli indirizzi, da qui il nome.

Le catture grezze non stanno nel progetto: senza argomento si cercano in %TEMP%\\mtsrec-pagine\\schermate."""
import json
import sys
import tempfile
from pathlib import Path

from PIL import Image

D = Path(__file__).parent / "img"
C = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(tempfile.gettempdir()) / "mtsrec-pagine" / "schermate"


def salva(nome, origine, ritaglio=None):
    im = Image.open(C / origine).convert("RGB")
    if ritaglio:
        im = im.crop(ritaglio)
    # 256 colori bastano a una finestra scura con poco colore, e il PDF pesa un terzo
    im.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).save(D / nome, optimize=True)
    print(nome, im.size, (D / nome).stat().st_size // 1024, "KB")


# solo verdetto, passi e cose da fare: più sotto ci sono i nomi dei dischi di rete di chi ha fatto la cattura
salva("m-controllo.png", "system check.png", (0, 0, 1920, 486))
salva("m-registratore.png", "record in registrazione.png")
salva("m-setup.png", "setup.png", (0, 0, 1920, 800))
salva("m-traccia.png", "tracce da vicino.png")
# dalla beta 12 i ritagli di Events e Review li misura schermate.py nella pagina (ritagli.json)
R = json.loads((C / "ritagli.json").read_text(encoding="utf-8"))
salva("m-review.png", "review.png", tuple(round(v) for v in R["review"]))
salva("m-eventi.png", "record eventi.png", tuple(round(v) for v in R["eventi"]))
