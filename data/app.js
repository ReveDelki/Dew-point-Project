// app.js  — Dew chart (top) + combined Temp & Humidity chart (bottom, single 0–100 axis)

// Rolling buffers (1000 points at 5 s ~83 min)
const N = 1000;
let a=new Float32Array(N).fill(NaN), b=new Float32Array(N).fill(NaN);     // dew A/B
let aT=new Float32Array(N).fill(NaN), aH=new Float32Array(N).fill(NaN);   // tempA/RH A
let bT=new Float32Array(N).fill(NaN), bH=new Float32Array(N).fill(NaN);   // tempB/RH B
let dd=new Float32Array(N).fill(NaN), fan=new Uint8Array(N).fill(0);
let ts=new Float64Array(N).fill(NaN);
let head=0;

let fanDisabled = false;

// Helpers
const el=id=>document.getElementById(id);
const fmt=x=>(x==null||Number.isNaN(x))?"--":Number(x).toFixed(2);
const numOrNaN = v => (v==null ? NaN : Number(v));

// fetch with timeout to avoid hanging requests on SoftAP/no-internet networks
function fetchWithTimeout(url, opts={}, ms=5000){ // fetch with 5000ms 
  const ctrl = new AbortController();
  const t = setTimeout(() => ctrl.abort(), ms);
  return fetch(url, { ...opts, signal: ctrl.signal })
    .finally(() => clearTimeout(t));
}

/* ===================== Dew chart (top) ===================== */
const cvs=el('plot'), ctx=cvs.getContext('2d');

function axes(minY,maxY){
  ctx.clearRect(0,0,cvs.width,cvs.height);
  ctx.strokeStyle='#e5e7eb';
  ctx.lineWidth=1;
  ctx.fillStyle='#6b7280';
  for(let k=0;k<=5;k++){
    const y=cvs.height*k/5;
    ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(cvs.width,y); ctx.stroke();
    const val=(maxY-(maxY-minY)*k/5).toFixed(0);
    ctx.fillText(val,4,Math.max(10,y-2));
  }
}
function series(buf,color,minY,maxY){
  const sx=cvs.width/(N-1), sy=v=>cvs.height-((v-minY)/(maxY-minY))*cvs.height;
  ctx.strokeStyle=color; ctx.lineWidth=2; ctx.beginPath();
  let s=false;
  for(let k=0;k<N;k++){
    const idx=(head+k)%N, v=buf[idx];
    if(Number.isNaN(v)) continue;
    const x=k*sx,y=sy(v);
    if(!s){ctx.moveTo(x,y); s=true;} else ctx.lineTo(x,y);
  }
  ctx.stroke();
}
function drawDew(){
  const vals=[...a,...b].filter(v=>!Number.isNaN(v));
  let minY=-10,maxY=40;
  if(vals.length){
    minY=Math.min(...vals); maxY=Math.max(...vals);
    const pad=Math.max(2,(maxY-minY)*.15); minY-=pad; maxY+=pad;
    if(maxY-minY<5) maxY=minY+5;
  }
  axes(minY,maxY);
  series(a,'#2563eb',minY,maxY);   // Dew A (blue)
  series(b,'#10b981',minY,maxY);   // Dew B (green)
}

/* ===== Temp + Humidity chart (bottom, single 0–100 axis) ===== */
const cvsTH = el('thPlot'), ctxTH = cvsTH.getContext('2d');

function axesSimple(minY,maxY){
  ctxTH.clearRect(0,0,cvsTH.width,cvsTH.height);
  ctxTH.strokeStyle='#e5e7eb';
  ctxTH.fillStyle='#6b7280';
  ctxTH.lineWidth=1;
  for(let k=0;k<=5;k++){
    const y=cvsTH.height*k/5;
    ctxTH.beginPath(); ctxTH.moveTo(0,y); ctxTH.lineTo(cvsTH.width,y); ctxTH.stroke();
    const val=(maxY-(maxY-minY)*k/5).toFixed(0);
    ctxTH.fillText(val,4,Math.max(10,y-2));
  }
}
function seriesCommon(buf,color,minY,maxY){
  const sx=cvsTH.width/(N-1), sy=v=>cvsTH.height-((v-minY)/(maxY-minY))*cvsTH.height;
  ctxTH.strokeStyle=color; ctxTH.lineWidth=2; ctxTH.beginPath();
  let s=false;
  for(let k=0;k<N;k++){
    const idx=(head+k)%N, v=buf[idx];
    if(Number.isNaN(v)) continue;
    const x=k*sx, y=sy(v);
    if(!s){ctxTH.moveTo(x,y); s=true;} else ctxTH.lineTo(x,y);
  }
  ctxTH.stroke();
}
function drawTempHum(){
  const minY=0, maxY=100; // unified scale for °C and %RH
  axesSimple(minY,maxY);
  // temps
  seriesCommon(aT,'#ef4444',minY,maxY);  // Temp A (red)
  seriesCommon(bT,'#3b82f6',minY,maxY);  // Temp B (blue)
  // humidity
  seriesCommon(aH,'#ffa500',minY,maxY);   // RH A (orange)
  seriesCommon(bH,'#008b8b',minY,maxY); // RH B (darkcyan)
}

/* ===================== Draw both charts ===================== */
// Collect all non-NaN values from both series
// Determine minY/maxY from data, add padding
// Draw axes, then both series with different colors
function drawAll(){
  drawDew();
  drawTempHum();
}

/* ===================== UI state ===================== */
// Update fan button + mode to enable/disable fan and switch pill from auto/manual
const setFanUI = () => {
  const b = el('fanBtn');
  const modePill = el('modePill');
  if (fanDisabled) {
    b.textContent = 'Enable Fan';
    modePill.textContent = 'MANUAL OFF';
    modePill.className = 'pill bad';
  } else {
    b.textContent = 'Disable Fan';
    modePill.textContent = 'AUTO MODE';
    modePill.className = 'pill ok';
  }
};

/* ===================== Poll /data ===================== */
// Fetch data from /data endpoint and update UI + buffers
let inFlight = false; // NEW: prevent overlapping tick() calls (common freeze cause)

async function tick(){
  if (inFlight) return; // NEW: skip if previous tick still running
  inFlight = true;

  try{
    // add cache-busting query + timeout to keep polling reliable on SoftAP
    const r = await fetchWithTimeout('/data?ts=' + Date.now(), {cache:'no-store'}, 5000);
    const j = await r.json();

    // Cards
    el('dewA').textContent=fmt(j.dewA);
    el('dewB').textContent=fmt(j.dewB);
    el('tA').textContent=fmt(j.tempA);
    el('hA').textContent=fmt(j.rhA);
    el('tB').textContent=fmt(j.tempB);
    el('hB').textContent=fmt(j.rhB);

    // ΔDew + fan (if not valid -> NaN)
    const d = (j.dewA==null || j.dewB==null) ? NaN : Number(j.dewA - j.dewB);
    el('dd').textContent=fmt(d);

    // Fan status
    const sv=el('sv'); sv.textContent = j.fan ? "ON" : "OFF";
    sv.className = "pill " + (j.fan ? "bad" : "ok");

    fanDisabled = !!j.disabled;
    setFanUI();

    // Store ring buffer (null -> NaN to avoid fake zeros)
    a[head]=numOrNaN(j.dewA);   b[head]=numOrNaN(j.dewB);
    aT[head]=numOrNaN(j.tempA); aH[head]=numOrNaN(j.rhA);
    bT[head]=numOrNaN(j.tempB); bH[head]=numOrNaN(j.rhB);
    dd[head]=d; fan[head]=j.fan?1:0; ts[head]=Date.now();

    head=(head+1)%N;
    drawAll();
  }catch(e){
    // don't hide errors, log it (helps when realtime not updating)
    console.log("tick() failed:", e);
  } finally {
    inFlight = false; // always release lock
  }
}

/* ===================== Controls ===================== */
// Clear buffers
function clearBuf(){
  a.fill(NaN); b.fill(NaN);
  aT.fill(NaN); aH.fill(NaN);
  bT.fill(NaN); bH.fill(NaN);
  dd.fill(NaN); fan.fill(0); ts.fill(NaN);
  drawAll();
}

// Export buffers to CSV
function exportCSV(){
  const sep=';';
  const lines=[`sep=${sep}`,"timestamp"+sep+"dewA"+sep+"dewB"+sep+"tempA"+sep+"rhA"+sep+"tempB"+sep+"rhB"+sep+"deltaDew"+sep+"fan"];

  // format: ISO timestamp; dewA; dewB; tempA; rhA; tempB; rhB; deltaDew; fan (0/1)
  // for NaN/invalid use empty field
  for(let k=0;k<N;k++){
    const idx=(head+k)%N;
    if(Number.isNaN(ts[idx])) continue;
    const t=new Date(ts[idx]).toISOString();
    const row=[t,a[idx],b[idx],aT[idx],aH[idx],bT[idx],bH[idx],dd[idx],fan[idx]]
      .map(v=>(v==null||Number.isNaN(v))?"":String(v));
    lines.push(row.join(sep));
  }

  // Create and download blob
  const blob=new Blob([lines.join("\n")],{type:"text/csv"});
  const url=URL.createObjectURL(blob);
  const aEl=document.createElement('a');
  aEl.href=url; aEl.download="sht4x_log.csv"; aEl.click();
  URL.revokeObjectURL(url);
}

// Poller handle so we can stop it before reboot
let pollHandle = null;

// Button handlers
document.getElementById('clear').onclick=clearBuf;
document.getElementById('export').onclick=exportCSV;
document.getElementById('zipBtn').onclick = async ()=>{
  const day = el('daySel').value;
  try{ await downloadZipForDay(day); }catch(e){ alert("ZIP failed"); }
};
document.getElementById('fanBtn').onclick=async ()=>{
  // timeout-protected fan request so it can't hang forever
  try{ await fetchWithTimeout('/fan?toggle=1&ts=' + Date.now(), {method:'POST', cache:'no-store'}, 5000); }catch(e){}
  tick(); // don't await (avoid blocking UI)
};

// Reboot button -> show centered line, stop polling, call /reboot, then blank page
document.getElementById('rebootBtn').onclick = async () => {
  const overlay = document.getElementById('rebootOverlay');
  const appRoot = document.getElementById('appRoot');

  // show message
  overlay.style.display = 'flex';
  appRoot.style.display = 'none';

  // stop polling
  if (pollHandle) { clearInterval(pollHandle); pollHandle = null; }

  try {
    await fetchWithTimeout('/reboot?ts=' + Date.now(), { method: 'POST', cache:'no-store' }, 5000);
  } catch (e) {
    // ignore errors during reboot
  }

  // after a short delay, blank the tab (some browsers won't allow window.close)
  setTimeout(() => {
    // Replace document with blank to simulate "shutdown"
    document.body.innerHTML = '';
    document.title = '';
  }, 1200);
};

/* ===================== Start SD logging when page opens ===================== */
/* When page is opened:
   - send current browser epoch time to ESP32
   - ESP32 sets system time
   - ESP32 creates CSV file + enables logging
*/
async function startLoggingOnOpen(){
  const epoch = Math.floor(Date.now()/1000);
  try{
    await fetchWithTimeout('/startLogging?ts=' + Date.now(), {
      method:'POST',
      cache:'no-store',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'epoch=' + encodeURIComponent(epoch)
    }, 3000);
  }catch(e){
    // ignore: if SD missing or server restarting, page still works
  }
}

/* ===================== SD ZIP download ===================== */

async function loadDays(){
  try{
    const r = await fetchWithTimeout('/sd/days?ts=' + Date.now(), {cache:'no-store'}, 5000);
    const j = await r.json();
    if(!j.ok) return;

    const sel = el('daySel');
    // clear existing (keep first option)
    while(sel.options.length > 1) sel.remove(1);

    // sort newest first (DD-MM-YYYY -> compare by yyyy-mm-dd)
    const days = (j.days || []).slice();
    days.sort((d1,d2)=>{
      const a=d1.split('-'); const b=d2.split('-');
      const A=a[2]+a[1]+a[0]; const B=b[2]+b[1]+b[0];
      return (A<B)?1:(A>B)?-1:0;
    });

    for(const d of days){
      const opt=document.createElement('option');
      opt.value=d; opt.textContent=d;
      sel.appendChild(opt);
    }
  }catch(e){}
}

async function downloadZipForDay(day){
  if(!day) return;

  // Need JSZip loaded (jszip.min.js)
  if(typeof JSZip === 'undefined'){
    alert("JSZip not loaded");
    return;
  }

  // 1) list files
  const r = await fetchWithTimeout('/sd/files?day=' + encodeURIComponent(day) + '&ts=' + Date.now(), {cache:'no-store'}, 8000);
  const j = await r.json();
  if(!j.ok){ alert("Cannot list files"); return; }

  const files = j.files || [];
  if(!files.length){ alert("No files in that day"); return; }

  // 2) fetch each file and add to zip
  const zip = new JSZip();
  const folder = zip.folder(day);

  for(const f of files){
    const url = '/sd/raw?day=' + encodeURIComponent(day) + '&file=' + encodeURIComponent(f) + '&ts=' + Date.now();
    const resp = await fetchWithTimeout(url, {cache:'no-store'}, 15000);
    const blob = await resp.blob();
    folder.file(f, blob);
  }

  // 3) generate zip and download
  const zipBlob = await zip.generateAsync({type:"blob"});
  const aEl=document.createElement('a');
  aEl.href = URL.createObjectURL(zipBlob);
  aEl.download = day + ".zip";
  aEl.click();
  URL.revokeObjectURL(aEl.href);
}

/* ===================== SD purge warning (3 days before) ===================== */
async function checkSdWarning(){
  try{
    const r = await fetchWithTimeout('/sd/status?ts=' + Date.now(), {cache:'no-store'}, 5000);
    const j = await r.json();
    if(!j.ok) return;

    const warnBox = el('sdWarn');
    const msg = el('sdWarnMsg');

    if(j.warn){
      warnBox.style.display = 'block';
      msg.textContent = `All SD data will be deleted in ${j.daysLeft} day(s). Please backup your SD card now.`;
    }else{
      warnBox.style.display = 'none';
    }
  }catch(e){}
}

/* ===================== Start ===================== */
console.log("app.js loaded", new Date().toISOString()); // NEW: confirms app.js is actually loaded
startLoggingOnOpen(); //start SD logging only after opening page

loadDays();
checkSdWarning();
setInterval(checkSdWarning, 60*60*1000); // check once per hour

tick();
if (pollHandle) clearInterval(pollHandle);
pollHandle = setInterval(tick,5000);
