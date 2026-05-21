// Snake Arena browser client — 2D canvas.
// Fully server-authoritative. The client sends a desired direction and
// reconstructs every snake from incremental "step" messages (reliable over
// the WebSocket/TCP). Protocol mirrors src/SnakeGame.cpp.

(() => {
'use strict';

const CLIENT_VERSION = 'snk-1';

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
const stickEl  = document.getElementById('stick');
const knobEl   = document.getElementById('stickKnob');

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

const STATE = {connected: false, id: 0, w: 32, h: 24, roundState: 'lobby',
               centerUntil: 0, fw: '?'};
let scores = [];
const snakes = new Map();   // id -> { cells:[[x,y]...], alive }
const food = [];            // [x,y]

function snake(id) {
    let s = snakes.get(id);
    if (!s) { s = {cells: [], alive: true}; snakes.set(id, s); }
    return s;
}

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
        STATE.id = m.id; STATE.w = m.w; STATE.h = m.h;
        snakes.clear(); food.length = 0;
        for (const sn of m.snakes) snakes.set(sn.id, {cells: sn.c.slice(), alive: !!sn.a});
        for (const f of m.food) food.push([f[0], f[1]]);
        loginEl.classList.add('hidden'); hudEl.classList.remove('hidden');
        showCenter('GET READY', 1200);
        break;
    case 'step':
        for (const [id, hx, hy, g] of m.m) {
            const s = snake(id);
            s.alive = true;
            s.cells.unshift([hx, hy]);
            if (!g) s.cells.pop();
        }
        break;
    case 'dead': {
        const s = snakes.get(m.id);
        if (s) { s.cells = []; s.alive = false; }
        if (m.id === STATE.id) showCenter('CRASHED', 1100);
        break;
    }
    case 'respawn': {
        snakes.set(m.id, {cells: [[m.x, m.y]], alive: true});
        break;
    }
    case 'food': food.push([m.x, m.y]); break;
    case 'eat':
        for (let i = 0; i < food.length; i++)
            if (food[i][0] === m.x && food[i][1] === m.y) { food.splice(i, 1); break; }
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
    case 'leave': snakes.delete(m.id); break;
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

// ---------- input (4 directions) ----------
let lastDx = 0, lastDy = 0;
function setDir(dx, dy) {
    if (dx === lastDx && dy === lastDy) return;
    lastDx = dx; lastDy = dy;
    if (dx || dy) send({t: 'd', x: dx, y: dy});
}
const stick = {active: false, id: -1, cx: 0, cy: 0, r: 1};
function sStart(ev) {
    const t = ev.changedTouches ? ev.changedTouches[0] : ev;
    const rect = stickEl.getBoundingClientRect();
    stick.cx = rect.left + rect.width / 2; stick.cy = rect.top + rect.height / 2;
    stick.r = rect.width / 2; stick.active = true;
    stick.id = ev.changedTouches ? t.identifier : -1; sMove(ev);
}
function sMove(ev) {
    if (!stick.active) return;
    let t = null;
    if (ev.changedTouches) { for (const c of ev.changedTouches) if (c.identifier === stick.id) { t = c; break; } if (!t) return; }
    else t = ev;
    const dx = t.clientX - stick.cx, dy = t.clientY - stick.cy;
    const mag = Math.hypot(dx, dy);
    knobEl.style.transform = 'translate(' + Math.max(-stick.r, Math.min(stick.r, dx)) + 'px,'
        + Math.max(-stick.r, Math.min(stick.r, dy)) + 'px)';
    if (mag < stick.r * 0.3) return;
    if (Math.abs(dx) > Math.abs(dy)) setDir(dx > 0 ? 1 : -1, 0);
    else setDir(0, dy > 0 ? 1 : -1);
}
function sEnd(ev) {
    if (ev.changedTouches) { let f = false; for (const c of ev.changedTouches) if (c.identifier === stick.id) { f = true; break; } if (!f) return; }
    stick.active = false; knobEl.style.transform = 'translate(0,0)';
    lastDx = lastDy = 0;   // allow re-sending same dir next time
}
stickEl.addEventListener('touchstart', sStart, {passive: true});
stickEl.addEventListener('touchmove', sMove, {passive: true});
stickEl.addEventListener('touchend', sEnd, {passive: true});
stickEl.addEventListener('touchcancel', sEnd, {passive: true});
stickEl.addEventListener('mousedown', sStart);
addEventListener('mousemove', sMove);
addEventListener('mouseup', sEnd);
addEventListener('keydown', e => {
    if (e.code === 'ArrowLeft' || e.code === 'KeyA') setDir(-1, 0);
    else if (e.code === 'ArrowRight' || e.code === 'KeyD') setDir(1, 0);
    else if (e.code === 'ArrowUp' || e.code === 'KeyW') setDir(0, -1);
    else if (e.code === 'ArrowDown' || e.code === 'KeyS') setDir(0, 1);
});

// ---------- render ----------
function metrics() {
    const cell = Math.floor(Math.min(cssW / STATE.w, cssH / STATE.h) * 0.96);
    const ox = Math.floor((cssW - STATE.w * cell) / 2);
    const oy = Math.floor((cssH - STATE.h * cell) / 2);
    return {cell, ox, oy};
}
function draw() {
    const now = performance.now();
    ctx.clearRect(0, 0, cssW, cssH);
    const m = metrics(), cell = m.cell;

    ctx.fillStyle = '#0b1428';
    ctx.fillRect(m.ox, m.oy, STATE.w * cell, STATE.h * cell);
    ctx.strokeStyle = '#33507a'; ctx.lineWidth = 2;
    ctx.strokeRect(m.ox, m.oy, STATE.w * cell, STATE.h * cell);

    // food
    for (const [fx, fy] of food) {
        ctx.fillStyle = '#ff5d5d';
        ctx.beginPath();
        ctx.arc(m.ox + (fx + 0.5) * cell, m.oy + (fy + 0.5) * cell, cell * 0.32, 0, Math.PI * 2);
        ctx.fill();
    }

    // snakes
    for (const [id, s] of snakes) {
        const color = PALETTE[(id - 1) % PALETTE.length];
        for (let i = 0; i < s.cells.length; i++) {
            const [x, y] = s.cells[i];
            ctx.fillStyle = (i === 0) ? '#ffffff' : color;
            const pad = (i === 0) ? 1 : 2;
            roundRect(m.ox + x * cell + pad, m.oy + y * cell + pad, cell - 2 * pad, cell - 2 * pad, cell * 0.25);
            ctx.fill();
            if (i === 0) {
                ctx.fillStyle = color;
                roundRect(m.ox + x * cell + 3, m.oy + y * cell + 3, cell - 6, cell - 6, cell * 0.2);
                ctx.fill();
            }
        }
    }

    if (now > STATE.centerUntil) centerEl.textContent = '';
    diagEl.textContent = 'CLI ' + CLIENT_VERSION + '  FW ' + STATE.fw + '\n' + STATE.roundState.toUpperCase();
    requestAnimationFrame(draw);
}
function roundRect(x, y, w, h, r) {
    if (w < 2 * r) r = w / 2; if (h < 2 * r) r = h / 2;
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
}
requestAnimationFrame(draw);
setInterval(() => { if (STATE.connected) send({t: 'ping'}); }, 3000);

joinBtn.addEventListener('click', () => {
    const name = (nameEl.value || 'PLAYER').trim().slice(0, 16) || 'PLAYER';
    if (nameEl.value) localStorage.setItem('snk_name', nameEl.value);
    connect(name);
});
nameEl.addEventListener('keydown', e => { if (e.key === 'Enter') joinBtn.click(); });
const saved = localStorage.getItem('snk_name');
if (saved) nameEl.value = saved;

})();
