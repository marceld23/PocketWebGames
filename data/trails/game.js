// Curve Fever browser client — 2D canvas.
// Server-authoritative: it integrates movement, rasterises trails into an
// occupancy grid and resolves collisions. The client only sends steer input
// (-1/0/+1) and renders the head positions it receives, connecting them into
// trail polylines. Protocol mirrors src/CurveFeverGame.cpp.

(() => {
'use strict';

const CLIENT_VERSION = 'cf-1';

const canvas   = document.getElementById('view');
const ctx      = canvas.getContext('2d');
const loginEl  = document.getElementById('login');
const hudEl    = document.getElementById('hud');
const nameEl   = document.getElementById('name');
const joinBtn  = document.getElementById('join');
const statusEl = document.getElementById('status');
const timerEl  = document.getElementById('hudTimer');
const scoreEl  = document.getElementById('hudScore');
const boardEl  = document.getElementById('scoreboard');
const centerEl = document.getElementById('centerMsg');
const diagEl   = document.getElementById('diag');
const leftBtn  = document.getElementById('btnLeft');
const rightBtn = document.getElementById('btnRight');

const PALETTE = ['#4ad6ff', '#ff5d5d', '#ffd24a', '#7CFC54', '#c08bff', '#ff9f43'];

let DPR = 1, cssW = 0, cssH = 0;
function resize() {
    DPR = Math.min(window.devicePixelRatio || 1, 2);
    cssW = window.innerWidth; cssH = window.innerHeight;
    canvas.width = Math.floor(cssW * DPR);
    canvas.height = Math.floor(cssH * DPR);
    canvas.style.width = cssW + 'px';
    canvas.style.height = cssH + 'px';
    ctx.setTransform(DPR, 0, 0, DPR, 0, 0);
}
window.addEventListener('resize', resize);
resize();

const STATE = {connected: false, id: 0, w: 120, h: 80,
               roundState: 'lobby', centerUntil: 0, fw: '?'};
let scores = [];

// id -> { segs: [[{x,y}...]], cur, alive, hx, hy }
const trails = new Map();
function trail(id) {
    let t = trails.get(id);
    if (!t) { t = {segs: [], cur: null, alive: true, hx: 0, hy: 0}; trails.set(id, t); }
    return t;
}

let ws = null;
function connect(name) {
    statusEl.textContent = 'connecting...';
    const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
    ws = new WebSocket(url);
    ws.onopen = () => { STATE.connected = true; ws.send(JSON.stringify({t: 'join', name})); };
    ws.onclose = () => {
        STATE.connected = false; statusEl.textContent = 'connection lost';
        loginEl.classList.remove('hidden'); hudEl.classList.add('hidden');
    };
    ws.onerror = () => { statusEl.textContent = 'connect failed'; };
    ws.onmessage = (ev) => onMessage(ev.data);
}
function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

function onMessage(raw) {
    let m; try { m = JSON.parse(raw); } catch (e) { return; }
    switch (m.t) {
    case 'init':
        STATE.id = m.id; STATE.w = m.w; STATE.h = m.h;
        trails.clear();
        loginEl.classList.add('hidden'); hudEl.classList.remove('hidden');
        showCenter('GET READY', 1200);
        break;
    case 'clear':
        trails.clear();
        showCenter('GO!', 700);
        break;
    case 'sb':
        for (const p of m.p) {
            const t = trail(p.id);
            t.hx = p.x; t.hy = p.y; t.alive = !!p.a;
            if (!p.a) { t.cur = null; continue; }
            if (p.g) { t.cur = null; }            // gap: break the line
            else {
                if (!t.cur) { t.cur = []; t.segs.push(t.cur); }
                t.cur.push([p.x, p.y]);
            }
        }
        break;
    case 'dead':
        if (m.id === STATE.id) showCenter('CRASHED', 1100);
        { const t = trails.get(m.id); if (t) { t.alive = false; t.cur = null; } }
        break;
    case 'score': {
        scores = m.p;
        const mine = scores.find(r => r[0] === STATE.id);
        if (mine) scoreEl.textContent = mine[1];
        updateScoreboard();
        break;
    }
    case 'round':
        STATE.roundState = m.state;
        if (typeof m.fw === 'string') STATE.fw = m.fw;
        timerEl.textContent = formatTime(m.remaining);
        if (m.state === 'ended') {
            const w = scores.find(r => r[0] === m.winner);
            showCenter(w ? 'WINNER: ' + w[2] : 'ROUND OVER', 3000);
        } else if (m.state === 'lobby') showCenter('WAITING FOR HOST', 1200);
        else if (m.state === 'paused') showCenter('PAUSED', 0);
        break;
    case 'leave':
        trails.delete(m.id);
        break;
    case 'err':
        statusEl.textContent = 'error: ' + (m.m || 'unknown');
        break;
    }
}

function showCenter(text, ms) {
    centerEl.textContent = text;
    STATE.centerUntil = ms ? performance.now() + ms : Infinity;
}
function formatTime(s) {
    s = Math.max(0, s | 0);
    return ('0' + (s / 60 | 0)).slice(-2) + ':' + ('0' + (s % 60)).slice(-2);
}
function updateScoreboard() {
    boardEl.innerHTML = '';
    for (const [id, score, name] of scores.slice().sort((a, b) => b[1] - a[1])) {
        const row = document.createElement('div');
        row.className = 'row' + (id === STATE.id ? ' me' : '');
        row.innerHTML = '<span>' + escapeHtml(name) + '</span><span>' + score + '</span>';
        boardEl.appendChild(row);
    }
}
function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c]));
}

// ---------- input (steer left / right, hold) ----------
let leftHeld = false, rightHeld = false, lastDir = 0;
function pushDir() {
    const dir = leftHeld ? -1 : (rightHeld ? 1 : 0);
    if (dir !== lastDir) { lastDir = dir; send({t: 'i', d: dir}); }
}
function bindHold(el, setter) {
    el.addEventListener('touchstart', e => { e.preventDefault(); setter(true); pushDir(); }, {passive: false});
    el.addEventListener('touchend',   e => { e.preventDefault(); setter(false); pushDir(); }, {passive: false});
    el.addEventListener('touchcancel', () => { setter(false); pushDir(); });
    el.addEventListener('mousedown', e => { e.preventDefault(); setter(true); pushDir(); });
    el.addEventListener('mouseup',   () => { setter(false); pushDir(); });
    el.addEventListener('mouseleave', () => { setter(false); pushDir(); });
}
bindHold(leftBtn, v => leftHeld = v);
bindHold(rightBtn, v => rightHeld = v);
window.addEventListener('keydown', e => {
    if (e.code === 'ArrowLeft' || e.code === 'KeyA') { leftHeld = true; pushDir(); }
    if (e.code === 'ArrowRight' || e.code === 'KeyD') { rightHeld = true; pushDir(); }
});
window.addEventListener('keyup', e => {
    if (e.code === 'ArrowLeft' || e.code === 'KeyA') { leftHeld = false; pushDir(); }
    if (e.code === 'ArrowRight' || e.code === 'KeyD') { rightHeld = false; pushDir(); }
});

// ---------- render ----------
function metrics() {
    const scale = Math.min(cssW / STATE.w, cssH / STATE.h) * 0.96;
    const ox = (cssW - STATE.w * scale) / 2;
    const oy = (cssH - STATE.h * scale) / 2;
    return {scale, ox, oy};
}
function draw() {
    const now = performance.now();
    ctx.clearRect(0, 0, cssW, cssH);
    const m = metrics();

    // arena wall
    ctx.strokeStyle = '#33507a';
    ctx.lineWidth = 3;
    ctx.strokeRect(m.ox, m.oy, STATE.w * m.scale, STATE.h * m.scale);

    const lw = Math.max(2, m.scale * 1.2);
    for (const [id, t] of trails) {
        const color = PALETTE[(id - 1) % PALETTE.length];
        ctx.strokeStyle = color;
        ctx.lineWidth = lw;
        ctx.lineJoin = 'round';
        ctx.lineCap = 'round';
        for (const seg of t.segs) {
            if (seg.length < 2) continue;
            ctx.beginPath();
            ctx.moveTo(m.ox + seg[0][0] * m.scale, m.oy + seg[0][1] * m.scale);
            for (let i = 1; i < seg.length; i++)
                ctx.lineTo(m.ox + seg[i][0] * m.scale, m.oy + seg[i][1] * m.scale);
            ctx.stroke();
        }
        if (t.alive) {
            const hx = m.ox + t.hx * m.scale, hy = m.oy + t.hy * m.scale;
            ctx.fillStyle = (id === STATE.id) ? '#fff' : color;
            ctx.beginPath();
            ctx.arc(hx, hy, lw * 1.4, 0, Math.PI * 2);
            ctx.fill();
            if (id === STATE.id) {
                ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.stroke();
            }
        }
    }

    if (now > STATE.centerUntil) centerEl.textContent = '';
    diagEl.textContent = 'CLI ' + CLIENT_VERSION + '  FW ' + STATE.fw +
                         '\n' + STATE.roundState.toUpperCase();
    requestAnimationFrame(draw);
}
requestAnimationFrame(draw);

// keep-alive so the server doesn't time us out while we sit still
setInterval(() => { if (STATE.connected) send({t: 'ping'}); }, 3000);

joinBtn.addEventListener('click', () => {
    const name = (nameEl.value || 'PLAYER').trim().slice(0, 16) || 'PLAYER';
    if (nameEl.value) localStorage.setItem('cf_name', nameEl.value);
    connect(name);
});
nameEl.addEventListener('keydown', e => { if (e.key === 'Enter') joinBtn.click(); });
const saved = localStorage.getItem('cf_name');
if (saved) nameEl.value = saved;

})();
