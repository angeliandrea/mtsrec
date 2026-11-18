"""Il manuale d'uso in PDF, navigabile: segnalibri, indice cliccabile coi numeri di pagina, rimandi interni.

    py -3.10 crea-manuale.py

Da manuale.html (inglese tecnico, sezione 9.77) con Chrome senza finestra, in due passate: la prima dice a che
pagina cade ogni capitolo, la seconda scrive quei numeri nell'indice. I segnalibri li fa Chrome dai titoli
(--generate-pdf-document-outline); se mancano li rimette PyMuPDF, che c'è solo in py -3.10 su questa macchina.

Esce «Multicam Timecode Sync Recorder User Manual.pdf» qui accanto, col nome fisso che il programma apre dal tasto Help e che
CMake copia accanto all'eseguibile. La versione del programma viene da ..\\VERSIONE.txt.
"""
import html as H
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import pymupdf as fitz

QUI = Path(__file__).resolve().parent
CHROME = Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")
USCITA = QUI / "Multicam Timecode Sync Recorder User Manual.pdf"
VERSIONE = (QUI.parent / "VERSIONE.txt").read_text(encoding="utf-8").strip()

sorgente = (QUI / "manuale.html").read_text(encoding="utf-8")
# Titoli dei capitoli (h1) e dei paragrafi (h2) dopo l'indice, con il loro id
titoli = [(liv, id_, re.sub(r"<[^>]+>", "", t).strip())
          for liv, id_, t in re.findall(r'<h([12]) id="([^"]+)">(.*?)</h\1>', sorgente)]


def indice(pagine):
    r = []
    for liv, id_, t in titoli:
        r.append(f'<li class="{"s" if liv == "2" else ""}"><a href="#{id_}">{t}</a><span class="riempi"></span>'
                 f'<span class="pag">{pagine.get(id_, "")}</span></li>')
    return "\n".join(r)


def stampa(pagine, pdf):
    testo = sorgente.replace("{{VERSIONE}}", H.escape(VERSIONE)).replace("{{INDICE}}", indice(pagine))
    tmp = QUI / "_manuale-stampa.html"   # accanto a img\, così le immagini si trovano
    tmp.write_text(testo, encoding="utf-8")
    try:
        with tempfile.TemporaryDirectory() as profilo:
            subprocess.run([str(CHROME), "--headless=new", "--disable-gpu", f"--user-data-dir={profilo}",
                            "--no-pdf-header-footer", "--generate-pdf-document-outline",
                            f"--print-to-pdf={pdf}", tmp.as_uri()],
                           check=True, capture_output=True, timeout=120)
    finally:
        tmp.unlink(missing_ok=True)


def pagine_dei_titoli(pdf):
    """Il numero di pagina di ogni titolo, cercando il testo del titolo dalla pagina dopo l'indice in poi."""
    doc = fitz.open(pdf)
    r, da = {}, 2
    for liv, id_, t in titoli:
        for n in range(da, doc.page_count):
            if doc[n].search_for(t):
                r[id_] = n + 1
                da = n
                break
    return r, doc


prima = QUI / "_passata1.pdf"
stampa({}, prima)
pagine, d1 = pagine_dei_titoli(prima)
d1.close()
prima.unlink()
mancano = [t for _, i, t in titoli if i not in pagine]
if mancano:
    sys.exit("titles not found in the PDF: " + ", ".join(mancano))

stampa(pagine, USCITA)
seconda, doc = pagine_dei_titoli(USCITA)
if seconda != pagine:
    sys.exit("the page numbers moved between the two passes: run it again")

# I segnalibri di Chrome prendono anche il titolo della copertina e «Contents» due volte: si rifanno sempre
# dai titoli, l'indice in testa, capitoli al livello 1 e paragrafi al 2.
doc.set_toc([[1, "Contents", 2]] + [[int(liv), t, pagine[i]] for liv, i, t in titoli])
toc = doc.get_toc()
# Chrome scrive i rimandi interni come destinazioni con nome (LINK_NAMED): contano anche quelli
collegamenti = sum(1 for p in doc for l in p.get_links() if l.get("kind") in (fitz.LINK_GOTO, fitz.LINK_NAMED))
doc.set_metadata({**doc.metadata, "title": "Multicam Timecode Sync Recorder User Manual", "author": "Andrea Angeli",
                  "subject": f"Multicam Timecode Sync Recorder {VERSIONE}"})
doc.saveIncr()
print(f"{USCITA.name}: {doc.page_count} pages, {len(toc)} bookmarks, {collegamenti} internal links, version {VERSIONE}")
