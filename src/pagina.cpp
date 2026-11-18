#include "pagina.h"

// Scura come le pagine della regia: al buio si legge meglio. Si adatta anche al telefono.
// Le immagini si chiedono una alla volta, la successiva quando la precedente è arrivata,
// al più 5 al secondo per ingresso: se la macchina è lenta rallentano loro, mai la registrazione.
static const std::string testo = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Multicam Timecode Sync Recorder</title>
<style>
:root { --fondo:#0d0f12; --scheda:#171a1f; --testo:#e6e8eb; --tenue:#8a9099; --rosso:#d33; --verde:#2a8; --bordo:#2a2f37; }
* { box-sizing:border-box; }
body { margin:0; background:var(--fondo); color:var(--testo); font:15px/1.4 system-ui, sans-serif; }
header { position:sticky; top:0; z-index:1; background:#111418; border-bottom:1px solid var(--bordo);
         padding:10px 16px; display:flex; flex-wrap:wrap; gap:8px 18px; align-items:center; }
.rec { color:#fff; background:var(--rosso); border-radius:4px; padding:2px 8px; font-weight:700; }
.rec.ferma { background:#555; }
.num { font-variant-numeric:tabular-nums; font-family:ui-monospace, Consolas, monospace; }
.tenue { color:var(--tenue); }
.comandi { display:flex; flex-wrap:wrap; gap:8px; align-items:center; margin-left:auto; }
input, select, button { font:inherit; color:var(--testo); background:var(--scheda); border:1px solid var(--bordo);
                        border-radius:6px; padding:6px 10px; }
input { width:14em; max-width:50vw; }
button { cursor:pointer; }
#marker { background:#1d4ed8; border-color:#1d4ed8; font-weight:700; padding:6px 18px; }
main { display:grid; grid-template-columns:repeat(auto-fill, minmax(300px, 1fr)); gap:14px; padding:16px; }
.ingresso { background:var(--scheda); border:1px solid var(--bordo); border-radius:8px; overflow:hidden; }
.ingresso img { display:block; width:100%; aspect-ratio:16/9; background:#000; object-fit:cover; }
.ingresso.senza img { opacity:.25; }
.riga { display:flex; justify-content:space-between; gap:8px; padding:8px 10px; }
.stato { font-weight:600; color:var(--verde); }
.senza .stato { color:var(--rosso); }
#avviso { padding:0 16px; color:var(--tenue); }
</style></head><body>
<header>
  <span id="rec" class="rec">REC</span>
  <span class="num" id="tc">--:--:--:--</span>
  <span class="tenue">from <span class="num" id="inizio">--</span></span>
  <span class="tenue">dropped <span class="num" id="persi">0</span></span>
  <span class="tenue">marker <span class="num" id="nmarker">0</span></span>
  <span class="comandi">
    <input id="testo" placeholder="marker text">
    <select id="colore">
      <option value="blue">blue</option><option value="red">red</option><option value="green">green</option>
      <option value="yellow">yellow</option><option value="cyan">cyan</option><option value="purple">purple</option>
      <option value="pink">pink</option><option value="cream">cream</option>
    </select>
    <button id="marker">MARKER</button>
    <button id="immagini">thumbnails: on</button>
  </span>
</header>
<p id="avviso"></p>
<main id="griglia"></main>
<script>
let accese = true, ingressi = [];
const $ = id => document.getElementById(id);

function costruisci(lista) {
  const g = $("griglia");
  g.innerHTML = "";
  lista.forEach((x, i) => {
    const d = document.createElement("div");
    d.className = "ingresso";
    d.innerHTML = `<img alt=""><div class="riga"><span class="nome"></span><span class="num tc"></span></div>
                   <div class="riga"><span class="stato"></span><span class="tenue tracce"></span></div>`;
    d.querySelector(".nome").textContent = x.nome;
    g.appendChild(d);
    const img = d.querySelector("img");
    const prossima = () => {
      if (!accese) return setTimeout(prossima, 500);
      const t0 = Date.now();
      img.onload = img.onerror = () => setTimeout(prossima, Math.max(0, 200 - (Date.now() - t0)));
      img.src = "/miniatura?n=" + i + "&t=" + t0;
    };
    prossima();
  });
}

async function stato() {
  try {
    const s = await (await fetch("/stato")).json();
    $("tc").textContent = s.timecode; $("inizio").textContent = s.inizio;
    $("persi").textContent = s.persi; $("nmarker").textContent = s.marker;
    $("rec").className = s.registra ? "rec" : "rec ferma";
    $("avviso").textContent = s.persi > 0 ? "⚠ dropped frames: " + s.persi : "";
    if (JSON.stringify(s.ingressi.map(x => x.nome)) !== JSON.stringify(ingressi)) {
      ingressi = s.ingressi.map(x => x.nome);
      costruisci(s.ingressi);
    }
    document.querySelectorAll(".ingresso").forEach((d, i) => {
      const x = s.ingressi[i];
      d.classList.toggle("senza", !x.segnale);
      d.querySelector(".tc").textContent = x.tc;
      d.querySelector(".stato").textContent = x.segnale ? "signal" : "NO SIGNAL";
      d.querySelector(".tracce").textContent = x.tracce + (x.tracce === 1 ? " track" : " tracks");
    });
  } catch (e) {
    $("rec").className = "rec ferma";
    $("avviso").textContent = "the recorder is not responding";
  }
  setTimeout(stato, 500);
}

$("marker").onclick = async () => {
  const q = new URLSearchParams({ testo: $("testo").value, colore: $("colore").value });
  const r = await (await fetch("/marker?" + q)).json();
  $("avviso").textContent = "marker " + r.marker + " at " + r.timecode;
  $("testo").value = "";
};
$("testo").addEventListener("keydown", e => { if (e.key === "Enter") $("marker").click(); });
$("immagini").onclick = () => {
  accese = !accese;
  $("immagini").textContent = "thumbnails: " + (accese ? "on" : "off");
};
stato();
</script>
</body></html>
)HTML";

const std::string& pagina_registratore() { return testo; }
