// Bomberman browser client — 2D canvas renderer.
// Server-authoritative bombs/explosions/bricks/power-ups/score; the client
// owns its own movement (predicted against the synced grid) and sends x,y.
// Protocol mirrors the firmware in src/BombermanGame.cpp.

(() => {
'use strict';

const CLIENT_VERSION = 'bm-1';

// ---------- DOM ----------
const canvas   = document.getElementById('view');
const ctx      = canvas.getContext('2d');
const loginEl  = document.getElementById('login');
const hudEl    = document.getElementById('hud');
const nameEl   = document.getElementById('name');
const joinBtn  = document.getElementById('join');
const statusEl = document.getElementById('status');
const timerEl  = document.getElementById('hudTimer');
const statsEl  = document.getElementById('hudStats');
const scoreEl  = document.getElementById('hudScore');
const boardEl  = document.getElementById('scoreboard');
const centerEl = document.getElementById('centerMsg');
const diagEl   = document.getElementById('diag');
const stickEl  = document.getElementById('stick');
const knobEl   = document.getElementById('stickKnob');
const bombBtn  = document.getElementById('btnBomb');

// ---------- constants ----------
const BASE_SPEED  = 3.4;     // tiles / second
const SPEED_STEP  = 0.7;
const MAX_SPEED   = 6.5;
const FLAME_MS    = 600;     // must match Config::BM_FLAME_MS
const PLAYER_R    = 0.42;    // collision half-extent in tiles
const SEND_MS     = 60;      // ~15 Hz position updates

const PALETTE = ['#4ad6ff', '#ff5d5d', '#ffd24a', '#7CFC54', '#c08bff', '#ff9f43'];
const PU_COLORS = ['#ff7744', '#ffd24a', '#4ad6ff'];
const PU_LETTER = ['B', 'F', 'S'];

// ---------- canvas sizing ----------
let DPR = 1, cssW = 0, cssH = 0;
function resize() {
    DPR  = Math.min(window.devicePixelRatio || 1, 2);
    cssW = window.innerWidth;
    cssH = window.innerHeight;
    canvas.width  = Math.floor(cssW * DPR);
    canvas.height = Math.floor(cssH * DPR);
    canvas.style.width  = cssW + 'px';
    canvas.style.height = cssH + 'px';
    ctx.setTransform(DPR, 0, 0, DPR, 0, 0);
}
window.addEventListener('resize', resize);
resize();

// ---------- state ----------
const STATE = {
    connected: false,
    id: 0,
    w: 13, h: 11,
    cells: [],          // array of chars: '#' wall, '+' brick, '.' empty
    roundState: 'lobby',
    centerUntil: 0,
    fw: '?',
};

const me = {
    x: 1, y: 1,
    dead: false,
    score: 0,
    maxBombs: 1,
    flame: 2,
    speed: BASE_SPEED,
};

const others   = new Map();  // id -> { x, y, dead, name }
const bombs     = new Map();  // id -> { x, y, range, t0 }
const powerups  = new Map();  // id -> { type, x, y }
const flames    = [];         // { x, y, until }
let   scores    = [];

function cellIdx(x, y) { return y * STATE.w + x; }
function cellAt(x, y) {
    if (x < 0 || y < 0 || x >= STATE.w || y >= STATE.h) return '#';
    return STATE.cells[cellIdx(x, y)] || '.';
}

// ---------- networking ----------
let ws = null;
function connect(name) {
    statusEl.textContent = 'connecting...';
    const url = (location.protocol === 'https:' ? 'wss://' : 'ws://')
                + location.host + '/ws';
    ws = new WebSocket(url);
    ws.onopen = () => { STATE.connected = true; ws.send(JSON.stringify({t: 'join', name})); };
    ws.onclose = () => {
        STATE.connected = false;
        statusEl.textContent = 'connection lost';
        loginEl.classList.remove('hidden');
        hudEl.classList.add('hidden');
    };
    ws.onerror = () => { statusEl.textContent = 'connect failed'; };
    ws.onmessage = (ev) => onMessage(ev.data);
}
function send(obj) { if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj)); }

function onMessage(raw) {
    let m; try { m = JSON.parse(raw); } catch (e) { return; }
    switch (m.t) {
    case 'init': {
        STATE.id = m.id;
        STATE.w = m.w; STATE.h = m.h;
        STATE.cells = m.cells.split('');
        me.x = m.sx; me.y = m.sy;
        me.dead = false;
        me.maxBombs = m.mb || 1;
        me.flame = m.fl || 2;
        me.speed = BASE_SPEED;
        bombs.clear(); powerups.clear(); flames.length = 0;
        if (Array.isArray(m.bombs))
            for (const b of m.bombs) bombs.set(b.id, {x: b.x, y: b.y, range: b.range, t0: performance.now()});
        if (Array.isArray(m.pu))
            for (const p of m.pu) powerups.set(p.id, {type: p.type, x: p.x, y: p.y});
        loginEl.classList.add('hidden');
        hudEl.classList.remove('hidden');
        showCenter('GET READY', 1200);
        break;
    }
    case 'sb': {
        const seen = new Set();
        for (const p of m.p) {
            seen.add(p.id);
            if (p.id === STATE.id) {
                me.maxBombs = p.mb; me.flame = p.fl; me.dead = !!p.d;
                continue;   // never overwrite our own predicted position
            }
            let o = others.get(p.id);
            if (!o) { o = {x: p.x, y: p.y, dead: false, name: ''}; others.set(p.id, o); }
            o.x = p.x; o.y = p.y; o.dead = !!p.d;
        }
        for (const id of [...others.keys()]) if (!seen.has(id)) others.delete(id);
        break;
    }
    case 'bomb':
        bombs.set(m.id, {x: m.x, y: m.y, range: m.range, t0: performance.now()});
        break;
    case 'boom': {
        const now = performance.now();
        for (const [x, y] of m.c) {
            flames.push({x, y, until: now + FLAME_MS});
            for (const [id, b] of bombs) if (b.x === x && b.y === y) bombs.delete(id);
        }
        break;
    }
    case 'brick':
        STATE.cells[cellIdx(m.x, m.y)] = '.';
        break;
    case 'pspawn':
        powerups.set(m.id, {type: m.type, x: m.x, y: m.y});
        break;
    case 'ppickup':
        powerups.delete(m.id);
        if (m.target === STATE.id) {
            if (m.type === 2) me.speed = Math.min(MAX_SPEED, me.speed + SPEED_STEP);
            showCenter('+' + (['BOMB', 'FLAME', 'SPEED'][m.type] || 'PU'), 700);
        }
        break;
    case 'kill':
        if (m.target === STATE.id) { me.dead = true; showCenter('YOU DIED', 1400); }
        else if (m.a === STATE.id) showCenter('GOT ONE!', 700);
        break;
    case 'respawn':
        if (m.id === STATE.id) {
            me.x = m.x; me.y = m.y; me.dead = false; me.speed = BASE_SPEED;
            showCenter('RESPAWN', 700);
        } else {
            const o = others.get(m.id);
            if (o) { o.x = m.x; o.y = m.y; o.dead = false; }
        }
        break;
    case 'score': {
        scores = m.p;
        const mine = scores.find(r => r[0] === STATE.id);
        if (mine) me.score = mine[1];
        for (const [id, , name] of scores) { const o = others.get(id); if (o) o.name = name; }
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
        } else if (m.state === 'lobby') {
            showCenter('WAITING FOR HOST', 1200);
        } else if (m.state === 'paused') {
            showCenter('PAUSED', 0);
        }
        break;
    case 'leave':
        others.delete(m.id);
        break;
    case 'err':
        statusEl.textContent = 'error: ' + (m.m || 'unknown');
        break;
    }
}

// ---------- UI helpers ----------
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
    const sorted = scores.slice().sort((a, b) => b[1] - a[1]);
    for (const [id, score, name] of sorted) {
        const row = document.createElement('div');
        row.className = 'row' + (id === STATE.id ? ' me' : '');
        row.innerHTML = '<span>' + escapeHtml(name) + '</span><span>' + score + '</span>';
        boardEl.appendChild(row);
    }
}
function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
    }[c]));
}

// ---------- input ----------
const stick = {active: false, x: 0, y: 0, id: -1, cx: 0, cy: 0, r: 1};
function stickStart(ev) {
    const t = ev.changedTouches ? ev.changedTouches[0] : ev;
    const rect = stickEl.getBoundingClientRect();
    stick.cx = rect.left + rect.width / 2;
    stick.cy = rect.top + rect.height / 2;
    stick.r = rect.width / 2;
    stick.active = true;
    stick.id = ev.changedTouches ? t.identifier : -1;
    stickMove(ev);
}
function stickMove(ev) {
    if (!stick.active) return;
    let t = null;
    if (ev.changedTouches) {
        for (const c of ev.changedTouches) if (c.identifier === stick.id) { t = c; break; }
        if (!t) return;
    } else { t = ev; }
    let dx = t.clientX - stick.cx, dy = t.clientY - stick.cy;
    const d = Math.hypot(dx, dy);
    if (d > stick.r) { dx *= stick.r / d; dy *= stick.r / d; }
    stick.x = dx / stick.r;
    stick.y = dy / stick.r;
    knobEl.style.transform = 'translate(' + dx + 'px,' + dy + 'px)';
}
function stickEnd(ev) {
    if (ev.changedTouches) {
        let found = false;
        for (const c of ev.changedTouches) if (c.identifier === stick.id) { found = true; break; }
        if (!found) return;
    }
    stick.active = false; stick.x = 0; stick.y = 0;
    knobEl.style.transform = 'translate(0,0)';
}
stickEl.addEventListener('touchstart', stickStart, {passive: true});
stickEl.addEventListener('touchmove', stickMove, {passive: true});
stickEl.addEventListener('touchend', stickEnd, {passive: true});
stickEl.addEventListener('touchcancel', stickEnd, {passive: true});
stickEl.addEventListener('mousedown', stickStart);
window.addEventListener('mousemove', stickMove);
window.addEventListener('mouseup', stickEnd);

function placeBomb() { if (!me.dead && STATE.connected) send({t: 'b'}); }
bombBtn.addEventListener('touchstart', e => { e.preventDefault(); placeBomb(); }, {passive: false});
bombBtn.addEventListener('mousedown', e => { e.preventDefault(); placeBomb(); });

const keys = {};
window.addEventListener('keydown', e => {
    keys[e.code] = true;
    if (e.code === 'Space') { e.preventDefault(); placeBomb(); }
});
window.addEventListener('keyup', e => { keys[e.code] = false; });

// ---------- movement (client-authoritative, grid-collided) ----------
function solidForMe(tx, ty) {
    const c = cellAt(tx, ty);
    if (c === '#' || c === '+') return true;
    const onx = Math.round(me.x), ony = Math.round(me.y);
    for (const [, b] of bombs) {
        if (b.x === tx && b.y === ty) {
            if (tx === onx && ty === ony) continue;  // standing on our own bomb
            return true;
        }
    }
    return false;
}
function approach(a, target, step) {
    const d = target - a;
    if (Math.abs(d) <= step) return target;
    return a + Math.sign(d) * step;
}
function move(dt) {
    if (me.dead) return;
    let ix = stick.x, iy = stick.y;
    if (keys['ArrowLeft'] || keys['KeyA']) ix = -1;
    if (keys['ArrowRight'] || keys['KeyD']) ix = 1;
    if (keys['ArrowUp'] || keys['KeyW']) iy = -1;
    if (keys['ArrowDown'] || keys['KeyS']) iy = 1;
    if (Math.abs(ix) < 0.25) ix = 0;
    if (Math.abs(iy) < 0.25) iy = 0;

    let mvx = 0, mvy = 0;
    if (Math.abs(ix) >= Math.abs(iy) && ix !== 0) mvx = Math.sign(ix);
    else if (iy !== 0) mvy = Math.sign(iy);
    if (!mvx && !mvy) return;

    const step = me.speed * dt;
    if (mvx) {
        me.y = approach(me.y, Math.round(me.y), step);
        const ty = Math.round(me.y);
        let nx = me.x + mvx * step;
        const leadTile = Math.round(nx + mvx * PLAYER_R);
        if (solidForMe(leadTile, ty))
            nx = mvx > 0 ? (leadTile - 0.5 - PLAYER_R) : (leadTile + 0.5 + PLAYER_R);
        me.x = nx;
    } else {
        me.x = approach(me.x, Math.round(me.x), step);
        const tx = Math.round(me.x);
        let ny = me.y + mvy * step;
        const leadTile = Math.round(ny + mvy * PLAYER_R);
        if (solidForMe(tx, leadTile))
            ny = mvy > 0 ? (leadTile - 0.5 - PLAYER_R) : (leadTile + 0.5 + PLAYER_R);
        me.y = ny;
    }
}

// ---------- rendering ----------
function boardMetrics() {
    const cell = Math.floor(Math.min(cssW / STATE.w, cssH / STATE.h));
    const ox = Math.floor((cssW - STATE.w * cell) / 2);
    const oy = Math.floor((cssH - STATE.h * cell) / 2);
    return {cell, ox, oy};
}
function sx(wx, m) { return m.ox + (wx + 0.5) * m.cell; }
function sy(wy, m) { return m.oy + (wy + 0.5) * m.cell; }

function draw() {
    const now = performance.now();
    ctx.clearRect(0, 0, cssW, cssH);
    const m = boardMetrics();
    const cell = m.cell;

    // tiles
    for (let y = 0; y < STATE.h; y++) {
        for (let x = 0; x < STATE.w; x++) {
            const px = m.ox + x * cell, py = m.oy + y * cell;
            const c = cellAt(x, y);
            if (c === '#') {
                ctx.fillStyle = '#46506a';
                ctx.fillRect(px, py, cell, cell);
                ctx.fillStyle = '#5a6688';
                ctx.fillRect(px + 2, py + 2, cell - 4, cell - 6);
                ctx.fillStyle = '#2c3450';
                ctx.fillRect(px + 2, py + cell - 6, cell - 4, 4);
            } else if (c === '+') {
                ctx.fillStyle = '#9a5a2c';
                ctx.fillRect(px + 1, py + 1, cell - 2, cell - 2);
                ctx.strokeStyle = 'rgba(0,0,0,0.35)';
                ctx.lineWidth = 1;
                ctx.strokeRect(px + 3, py + 3, cell - 6, (cell - 6) / 2);
                ctx.strokeRect(px + 3, py + 3 + (cell - 6) / 2, cell - 6, (cell - 6) / 2);
            } else {
                ctx.fillStyle = ((x + y) & 1) ? '#0d1730' : '#0b1428';
                ctx.fillRect(px, py, cell, cell);
            }
        }
    }

    // power-ups
    for (const [, p] of powerups) {
        const cx = sx(p.x, m), cy = sy(p.y, m);
        ctx.fillStyle = PU_COLORS[p.type] || '#fff';
        ctx.beginPath();
        ctx.arc(cx, cy, cell * 0.30, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = '#101';
        ctx.font = 'bold ' + Math.floor(cell * 0.36) + 'px ui-monospace, monospace';
        ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
        ctx.fillText(PU_LETTER[p.type] || '?', cx, cy + 1);
    }

    // bombs (pulsing)
    for (const [, b] of bombs) {
        const cx = sx(b.x, m), cy = sy(b.y, m);
        const pulse = 0.82 + 0.18 * Math.sin((now - b.t0) * 0.012);
        ctx.fillStyle = '#0a0a0a';
        ctx.beginPath();
        ctx.arc(cx, cy, cell * 0.34 * pulse, 0, Math.PI * 2);
        ctx.fill();
        ctx.fillStyle = '#ff5d3a';
        ctx.beginPath();
        ctx.arc(cx + cell * 0.10, cy - cell * 0.18, cell * 0.06, 0, Math.PI * 2);
        ctx.fill();
    }

    // flames
    for (let i = flames.length - 1; i >= 0; i--) {
        const f = flames[i];
        if (now >= f.until) { flames.splice(i, 1); continue; }
        const px = m.ox + f.x * cell, py = m.oy + f.y * cell;
        const k = (f.until - now) / FLAME_MS;
        ctx.fillStyle = 'rgba(255,210,80,' + (0.55 + 0.35 * k) + ')';
        ctx.fillRect(px + 2, py + 2, cell - 4, cell - 4);
        ctx.fillStyle = 'rgba(255,120,30,' + (0.5 * k) + ')';
        ctx.fillRect(px + cell * 0.28, py + cell * 0.28, cell * 0.44, cell * 0.44);
    }

    // other players
    for (const [id, o] of others) {
        if (o.dead) continue;
        drawPlayer(sx(o.x, m), sy(o.y, m), cell, PALETTE[(id - 1) % PALETTE.length], o.name, false);
    }
    // self
    if (!me.dead)
        drawPlayer(sx(me.x, m), sy(me.y, m), cell, PALETTE[(STATE.id - 1) % PALETTE.length], '', true);

    // HUD text
    statsEl.textContent = 'B' + me.maxBombs + ' F' + me.flame;
    scoreEl.textContent = me.score | 0;
    if (now > STATE.centerUntil) centerEl.textContent = '';
    diagEl.textContent = 'CLI ' + CLIENT_VERSION + '  FW ' + STATE.fw +
                         '\n' + STATE.roundState.toUpperCase();
}

function drawPlayer(cx, cy, cell, color, name, isMe) {
    const r = cell * 0.38;
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(cx, cy, r, 0, Math.PI * 2);
    ctx.fill();
    ctx.lineWidth = isMe ? 3 : 2;
    ctx.strokeStyle = isMe ? '#ffffff' : 'rgba(0,0,0,0.5)';
    ctx.stroke();
    // eyes
    ctx.fillStyle = '#0a0a14';
    ctx.beginPath(); ctx.arc(cx - r * 0.32, cy - r * 0.15, r * 0.16, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.arc(cx + r * 0.32, cy - r * 0.15, r * 0.16, 0, Math.PI * 2); ctx.fill();
    if (name) {
        ctx.fillStyle = 'rgba(255,255,255,0.9)';
        ctx.font = Math.floor(cell * 0.26) + 'px ui-monospace, monospace';
        ctx.textAlign = 'center'; ctx.textBaseline = 'bottom';
        ctx.fillText(name, cx, cy - r - 2);
    }
}

// ---------- main loop ----------
let lastT = performance.now();
let lastSend = 0;
function frame() {
    const now = performance.now();
    let dt = (now - lastT) / 1000;
    if (dt > 0.1) dt = 0.1;
    lastT = now;

    move(dt);
    if (now - lastSend > SEND_MS && STATE.connected) {
        lastSend = now;
        send({t: 's', x: me.x, y: me.y});
    }
    draw();
    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// ---------- join ----------
joinBtn.addEventListener('click', () => {
    const name = (nameEl.value || 'PLAYER').trim().slice(0, 16) || 'PLAYER';
    if (nameEl.value) localStorage.setItem('bm_name', nameEl.value);
    connect(name);
});
nameEl.addEventListener('keydown', e => { if (e.key === 'Enter') joinBtn.click(); });
const saved = localStorage.getItem('bm_name');
if (saved) nameEl.value = saved;

})();
