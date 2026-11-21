#pragma once
#include <Arduino.h>

const char page[] PROGMEM = R"HTML(
<!doctype html>
<html lang="fi">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>EnergyWise</title>
<style>
:root{color-scheme:dark;font:16px system-ui;background:#101918;color:#eaf0ed}
body{max-width:960px;margin:auto;padding:24px}
main{display:grid;gap:18px}
section{background:#172524;border:1px solid #304541;border-radius:14px;padding:20px}
h1{font-size:2.3rem;margin:12px 0 24px} h2{margin:0 0 14px;font-size:1.15rem}
.metrics,.channels{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px}
article,fieldset{background:#101b1a;border:1px solid #304541;border-radius:10px;padding:14px;margin:0;min-width:0}
strong{display:block;font-size:1.35rem;margin-top:5px}
label{display:grid;gap:4px;margin-bottom:10px}
input,select,button{font:inherit;padding:8px;border-radius:6px;border:1px solid #54716a;max-width:100%;box-sizing:border-box}
button{background:#a8e9be;color:#10271b;cursor:pointer;margin-top:10px}
small,.muted{color:#b6c7bf} #message{min-height:1.5em}
canvas{width:100%;height:180px;display:block;margin-top:16px}
.legend{display:flex;flex-wrap:wrap;gap:16px;margin:8px 0 0}
.legend span:first-child{color:#a8e9be}.legend span:last-child{color:#b9c6ff}
[data-on="true"]{color:#a8e9be}
</style>
<h1>EnergyWise</h1>
<main>
<section>
<h2>HAN-mittari</h2>
<p id="meter" class="muted">Odotetaan mittarin tietoja</p>
<div class="metrics">
<article>Ostoteho<strong id="importKw">—</strong></article>
<article>Myyntiteho<strong id="exportKw">—</strong></article>
<article>Ostoenergia<strong id="importKwh">—</strong></article>
</div>
<canvas id="history" role="img" aria-label="Osto- ja myyntitehon historia viimeiseltä kymmeneltä minuutilta"></canvas>
<p class="legend"><span>Ostoteho</span><span>Myyntiteho</span></p>
</section>
<section>
<h2>Sähkön hinta</h2>
<div class="metrics">
<article>Tuntihinta<strong id="hourPrice">—</strong></article>
<article>Varttihinta<strong id="quarterPrice">—</strong></article>
</div>
<p id="clock" class="muted"></p>
<form id="prices">
<label>Hinnasto (JSON)<input id="priceFile" type="file" accept=".json,application/json" required></label>
<button>Lataa hinnasto</button>
</form>
</section>
<section>
<h2>Lähtöjen ohjaus</h2>
<form id="rules"><div class="channels" id="channels"></div>
<button id="save" disabled>Tallenna asetukset</button></form>
<p id="message" role="status"></p>
</section>
</main>
<script>
const byId = id => document.getElementById(id);
const format = new Intl.NumberFormat('fi-FI', {maximumFractionDigits: 3});
for (let i = 0; i < 4; ++i) {
  byId('channels').insertAdjacentHTML('beforeend', `<fieldset>
    <legend>Lähtö ${i + 1}</legend><p id="state${i}">—</p>
    <label>Ohjaus<select id="mode${i}">
      <option value="0">Pois</option><option value="60">Tuntihinta</option>
      <option value="15">Varttihinta</option></select></label>
    <label>Hintaraja (snt/kWh)<input id="limit${i}" type="number" step="any" required></label>
  </fieldset>`);
}
let initialized = false;
let samples = [];
function drawHistory() {
  const canvas = byId('history');
  const width = canvas.clientWidth, height = canvas.clientHeight;
  if (width < 80 || height < 60) return;
  const scale = devicePixelRatio || 1;
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  const ctx = canvas.getContext('2d');
  ctx.scale(scale, scale);
  const left = 50, right = width - 10, top = 12, bottom = height - 25;
  const peak = Math.max(1, ...samples.flat().filter(Number.isFinite));
  ctx.font = '12px system-ui';
  ctx.fillStyle = '#b6c7bf';
  ctx.strokeStyle = '#304541';
  for (let tick = 0; tick <= 4; ++tick) {
    const y = bottom - (bottom - top) * tick / 4;
    ctx.beginPath();
    ctx.moveTo(left, y);
    ctx.lineTo(right, y);
    ctx.stroke();
    ctx.fillText(format.format(peak * tick / 4), 2, y + 4, left - 7);
  }
  ctx.fillText('−10 min', left, height - 5);
  ctx.textAlign = 'right';
  ctx.fillText('Nyt', right, height - 5);
  ctx.textAlign = 'center';
  if (!samples.some(row => row.some(Number.isFinite))) {
    ctx.fillText('Ei mittaushistoriaa', (left + right) / 2, height / 2);
    return;
  }
  ['#a8e9be', '#b9c6ff'].forEach((color, series) => {
    ctx.fillStyle = ctx.strokeStyle = color;
    ctx.lineWidth = 2;
    let previous = null;
    samples.forEach((values, i) => {
      const value = values[series];
      if (!Number.isFinite(value)) {
        previous = null;
        return;
      }
      const x = right - (right - left) * (samples.length - 1 - i) / 59;
      const y = bottom - (bottom - top) * value / peak;
      ctx.beginPath();
      if (previous) {
        ctx.moveTo(...previous);
        ctx.lineTo(x, y);
        ctx.stroke();
      } else {
        ctx.arc(x, y, 2, 0, Math.PI * 2);
        ctx.fill();
      }
      previous = [x, y];
    });
  });
}
addEventListener('resize', drawHistory);
async function refreshHistory() {
  try {
    const res = await fetch('/api/history', {cache: 'no-store'});
    if (!res.ok) throw new Error();
    samples = await res.json();
  } catch {
    samples = [];
  }
  drawHistory();
}
async function refresh() {
  try {
    const res = await fetch('/api/status', {cache: 'no-store'});
    if (!res.ok) throw new Error();
    const data = await res.json();
    byId('meter').textContent = data.meterFresh ? 'Vastaanotto toimii' : 'Ei tuoretta mittaridataa';
    byId('clock').textContent = data.clockReady ? '' : 'Odotetaan kellon synkronointia';
    for (const [key, unit] of Object.entries({
      importKw: 'kW', exportKw: 'kW', importKwh: 'kWh',
      hourPrice: 'snt/kWh', quarterPrice: 'snt/kWh'
    })) {
      byId(key).textContent = data[key] == null ? '—' : format.format(data[key]) + ' ' + unit;
    }
    data.outputs.forEach((o, i) => {
      byId('state' + i).textContent = o.on ? 'Päällä' : 'Pois';
      byId('state' + i).dataset.on = o.on;
      if (!initialized) {
        byId('mode' + i).value = o.minutes;
        byId('limit' + i).value = o.limit;
      }
    });
    initialized = true;
    byId('save').disabled = false;
  } catch {
    byId('meter').textContent = 'Yhteys laitteeseen katkesi';
    for (const id of ['importKw','exportKw','importKwh','hourPrice','quarterPrice',
                      'state0','state1','state2','state3']) byId(id).textContent = '—';
    byId('clock').textContent = '';
    byId('save').disabled = true;
  }
}
async function submit(path, body, success) {
  try {
    const res = await fetch(path, {method: 'POST',
      headers: {'Content-Type': 'application/json'}, body});
    if (!res.ok) throw new Error(await res.text());
    byId('message').textContent = success;
    await refresh();
  } catch (err) {
    byId('message').textContent = err.message;
  }
}
byId('rules').onsubmit = async event => {
  event.preventDefault();
  const rules = Array.from({length: 4}, (_, i) => ({
    minutes: Number(byId('mode' + i).value), limit: byId('limit' + i).valueAsNumber
  }));
  await submit('/api/rules', JSON.stringify(rules), 'Lähtöasetukset tallennettu.');
};
byId('prices').onsubmit = async event => {
  event.preventDefault();
  await submit('/api/prices', await byId('priceFile').files[0].text(), 'Hinnasto ladattu.');
};
(async function poll() {
  await refresh();
  setTimeout(poll, 2000);
})();
(async function pollHistory() {
  await refreshHistory();
  setTimeout(pollHistory, 10000);
})();
</script>
</html>
)HTML";
