"""Copre nelle schermate del manuale quello che non deve uscire: indirizzi IP, nome del computer, cartelle di rete.

Le catture grezze (registratore.png, primo-avvio.png) le fa Chrome senza finestra a 1680x1050 sul registratore
acceso, e NON si tengono nel progetto perché contengono indirizzi e nome della macchina:

    chrome --headless=new --hide-scrollbars --force-device-scale-factor=1 --window-size=1680,1050
           --virtual-time-budget=8000 --screenshot=img
egistratore.png http://127.0.0.1:8811/registratore

Le coordinate dei rettangoli valgono per quella grandezza: con un'altra si ricontrollano a vista.
Sezione 9.77 del documento del progetto."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

D = Path(__file__).parent / "img"
F = ImageFont.truetype(r"C:\Windows\Fonts\segoeui.ttf", 11)


def copri(im, box, campione=None, testo=None, colore=(110, 110, 120)):
    d = ImageDraw.Draw(im)
    c = im.getpixel(campione or (box[0] - 2, box[1] + 2))
    d.rectangle(box, fill=c)
    if testo:
        d.text((box[0] + 2, box[1]), testo, font=F, fill=colore)


r = Image.open(D / "registratore.png").convert("RGB")
copri(r, (130, 206, 220, 223), campione=(215, 208))          # campo ATEM
copri(r, (417, 206, 572, 223), campione=(570, 208))          # campo HyperDeck
r.save(D / "m-registratore.png")
r.crop((0, 306, 500, 430)).save(D / "m-traccia.png")
# La barra degli strumenti in due righe una sopra l'altra: intera sarebbe una striscia illeggibile nel PDF
s1, s2 = r.crop((0, 272, 840, 306)), r.crop((840, 272, 1680, 306))
st = Image.new("RGB", (840, 72), r.getpixel((5, 300)))
st.paste(s1, (0, 0)); st.paste(s2, (0, 38))
st.save(D / "m-strumenti.png")

p = Image.open(D / "primo-avvio.png").convert("RGB")
copri(p, (843, 4, 915, 24), campione=(920, 6))                # nome del computer
copri(p, (1354, 296, 1660, 311), campione=(1350, 300), testo="ATEM and HyperDeck responding")
p.crop((0, 0, 1680, 612)).save(D / "m-controllo.png")
print("ok")
