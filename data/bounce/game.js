// Pong browser client — 2D canvas.
// Server-authoritative ball + scoring; the client owns its paddle position
// along its assigned edge and renders the broadcast state. Protocol mirrors
// src/PongGame.cpp.

(() => {
'use strict';

const CLIENT_VERSION = 'pong-1';

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

const PALETTE = ['#4ad6ff', '#ff5d5d', '#ffd24a', '#7CFC54', '#c08bff', '#ff9f43'];

let DPR = 1, cssW = 0, cssH = 0;
function resize() {
    DPR = Math.min(window.devicePixelRatio || 1, 2);
    cssW = innerWidth; cssH = innerHeight;
    canvas.width = Math.floor(cssW * DPR); canvas.height = Math.floor(cssH * DPR);
    canvas.style.width = cssW + 'px'; canvas.style.height = cssH + 'px';
    ctx.setTransform(DPR, 0, 0, DPR, 0, 0);
}
addEventListener('resize', resize); resize();

const STATE = {connected: false, id: 0, edge: -1, plen: 0.2, pt: 0.03,
               roundState: 'lobby', centerUntil: 0, fw: '?'};
let scores = [];
let ball = [0.5, 0.5];
let pads = [];          // [[id, edge, v], ...]
let myV = 0.5;
let arena = {ax: 0, ay: 0, asz: 1};

let ws = null;
function connect(name) {
    statusEl.textContent = 'connecting...';
    const url = (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws';
    ws = new WebSocket(url);
    ws.onopen = () => { STATE.connected = true; ws.send(JSON.stringify({t: 'join', name})); };
    ws.onclose = () => { STATE.connected = false; statusEl.textContent = 'connection lost';
        loginEl.classList.remove('hidden'); hudEl.classList.add('hidden'); };
    ws.onerror = () => { statusEl.textContent = 'connect failed'; };
    ws.onmessage = (ev) => onMessage(ev.data);
}
function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

function onMessage(raw) {
    let m; try { m = JSON.parse(raw); } catch (e) { return; }
    switch (m.t) {
    case 'init':
        STATE.id = m.id; STATE.edge = m.edge; STATE.plen = m.plen; STATE.pt = m.pt;
        loginEl.classList.add('hidden'); hudEl.classList.remove('hidden');
        showCenter('GET READY', 1200);
        break;
    case 'sb':
        ball = m.b; pads = m.pd;
        break;
    case 'point':
        if (m.scorer === STATE.id) showCenter('YOU SCORED', 900);
        else if (m.on === STATE.id) showCenter('MISSED!', 900);
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
    case 'leave': break;
    case 'err': statusEl.textContent = 'error: ' + (m.m || 'unknown'); break;
    }
}

function showCenter(t, ms) { centerEl.textContent = t; STATE.centerUntil = ms ? performance.now() + ms : Infinity; }
function formatTime(s) { s = Math.max(0, s | 0); return ('0' + (s / 60 | 0)).slice(-2) + ':' + ('0' + (s % 60)).slice(-2); }
function updateScoreboard() {
    boardEl.innerHTML = '';
    for (const [id, score, name] of scores.slice().sort((a, b) => b[1] - a[1])) {
        const r = document.createElement('div');
        r.className = 'row' + (id === STATE.id ? ' me' : '');
        r.innerHTML = '<span>' + escapeHtml(name) + '</span><span>' + score + '</span>';
        boardEl.appendChild(r);
    }
}
function escapeHtml(s) { return String(s).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c])); }

// ---------- input ----------
let lastSent = 0;
function setV(v) {
    myV = Math.max(0, Math.min(1, v));
    const now = performance.now();
    if (now - lastSent > 40) { lastSent = now; send({t: 'p', v: myV}); }
}
function pointerV(px, py) {
    if (STATE.edge === 0 || STATE.edge === 1) return (py - arena.ay) / arena.asz;
    return (px - arena.ax) / arena.asz;
}
let dragging = false;
function pd(e) { dragging = true; pm(e); }
function pm(e) {
    if (!dragging) return;
    const t = e.changedTouches ? e.changedTouches[0] : e;
    setV(pointerV(t.clientX, t.clientY));
}
function pu() { dragging = false; }
canvas.addEventListener('touchstart', e => { e.preventDefault(); pd(e); }, {passive: false});
canvas.addEventListener('touchmove', e => { e.preventDefault(); pm(e); }, {passive: false});
canvas.addEventListener('touchend', pu);
canvas.addEventListener('mousedown', pd);
addEventListener('mousemove', pm);
addEventListener('mouseup', pu);
addEventListener('keydown', e => {
    const horiz = (STATE.edge === 2 || STATE.edge === 3);
    if ((horiz && e.code === 'ArrowLeft') || (!horiz && e.code === 'ArrowUp')) setV(myV - 0.05);
    if ((horiz && e.code === 'ArrowRight') || (!horiz && e.code === 'ArrowDown')) setV(myV + 0.05);
});

// ---------- render ----------
function draw() {
    const now = performance.now();
    ctx.clearRect(0, 0, cssW, cssH);
    const asz = Math.floor(Math.min(cssW, cssH) * 0.9);
    const ax = Math.floor((cssW - asz) / 2), ay = Math.floor((cssH - asz) / 2);
    arena = {ax, ay, asz};

    ctx.fillStyle = '#0b1428';
    ctx.fillRect(ax, ay, asz, asz);
    ctx.strokeStyle = '#33507a'; ctx.lineWidth = 2;
    ctx.strokeRect(ax, ay, asz, asz);

    // walls: edges with no paddle
    const hasPad = [false, false, false, false];
    for (const [, edge] of pads) hasPad[edge] = true;
    ctx.fillStyle = '#46506a';
    const wt = 6;
    if (!hasPad[0]) ctx.fillRect(ax, ay, wt, asz);
    if (!hasPad[1]) ctx.fillRect(ax + asz - wt, ay, wt, asz);
    if (!hasPad[2]) ctx.fillRect(ax, ay, asz, wt);
    if (!hasPad[3]) ctx.fillRect(ax, ay + asz - wt, asz, wt);

    // paddles
    const th = asz * 0.03, L = STATE.plen * asz;
    for (const [id, edge, v] of pads) {
        ctx.fillStyle = (id === STATE.id) ? '#ffffff' : PALETTE[(id - 1) % PALETTE.length];
        if (edge === 0)      ctx.fillRect(ax, ay + v * asz - L / 2, th, L);
        else if (edge === 1) ctx.fillRect(ax + asz - th, ay + v * asz - L / 2, th, L);
        else if (edge === 2) ctx.fillRect(ax + v * asz - L / 2, ay, L, th);
        else if (edge === 3) ctx.fillRect(ax + v * asz - L / 2, ay + asz - th, L, th);
    }

    // ball
    ctx.fillStyle = '#ffe066';
    ctx.beginPath();
    ctx.arc(ax + ball[0] * asz, ay + ball[1] * asz, Math.max(3, asz * 0.018), 0, Math.PI * 2);
    ctx.fill();

    if (now > STATE.centerUntil) centerEl.textContent = '';
    diagEl.textContent = 'CLI ' + CLIENT_VERSION + '  FW ' + STATE.fw +
                         '\nedge ' + STATE.edge + '  ' + STATE.roundState.toUpperCase();
    requestAnimationFrame(draw);
}
requestAnimationFrame(draw);
setInterval(() => { if (STATE.connected) send({t: 'ping'}); }, 3000);

joinBtn.addEventListener('click', () => {
    const name = (nameEl.value || 'PLAYER').trim().slice(0, 16) || 'PLAYER';
    if (nameEl.value) localStorage.setItem('pong_name', nameEl.value);
    connect(name);
});
nameEl.addEventListener('keydown', e => { if (e.key === 'Enter') joinBtn.click(); });
const saved = localStorage.getItem('pong_name');
if (saved) nameEl.value = saved;

})();
