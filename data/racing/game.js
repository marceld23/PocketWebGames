// 3D Racing browser client — Three.js (global THREE from /shared/three.min.js).
// Client-authoritative arcade car (predicted locally); the server owns the
// track, counts checkpoints/laps and decides the winner. Protocol mirrors
// src/RacingGame.cpp.

(() => {
'use strict';

const CLIENT_VERSION = 'race-1';

const canvas   = document.getElementById('view');
const loginEl   = document.getElementById('login');
const hudEl     = document.getElementById('hud');
const nameEl    = document.getElementById('name');
const joinBtn   = document.getElementById('join');
const statusEl  = document.getElementById('status');
const timerEl   = document.getElementById('hudTimer');
const lapEl     = document.getElementById('hudLap');
const posEl     = document.getElementById('hudPos');
const boardEl   = document.getElementById('scoreboard');
const centerEl  = document.getElementById('centerMsg');
const diagEl    = document.getElementById('diag');
const stickEl   = document.getElementById('stick');
const knobEl    = document.getElementById('stickKnob');
const gasBtn    = document.getElementById('btnGas');
const brakeBtn  = document.getElementById('btnBrake');

const PALETTE = [0x4ad6ff, 0xff5d5d, 0xffd24a, 0x7CFC54, 0xc08bff, 0xff9f43];

// physics
const MAX_SPEED = 80, ACCEL = 95, DRAG = 26, BRAKE_DECEL = 130, TURN = 2.9;
const TURN_MIN = 0.4;   // fraction of TURN available even at low speed

const scene = new THREE.Scene();
scene.background = new THREE.Color(0x9fc7e8);
scene.fog = new THREE.Fog(0x9fc7e8, 260, 620);
const camera = new THREE.PerspectiveCamera(70, innerWidth / innerHeight, 0.5, 2000);
const renderer = new THREE.WebGLRenderer({canvas, antialias: false});
renderer.setPixelRatio(Math.min(devicePixelRatio || 1, 1.5));
function onResize() {
    camera.aspect = innerWidth / innerHeight;
    camera.updateProjectionMatrix();
    renderer.setSize(innerWidth, innerHeight);
}
addEventListener('resize', onResize);
renderer.setSize(innerWidth, innerHeight);

scene.add(new THREE.AmbientLight(0xaac4e0, 0.7));
const sun = new THREE.DirectionalLight(0xfff2d0, 1.1);
sun.position.set(80, 160, 40);
scene.add(sun);

const ground = new THREE.Mesh(
    new THREE.PlaneGeometry(2000, 2000),
    new THREE.MeshLambertMaterial({color: 0x2f7d3a}));
ground.rotation.x = -Math.PI / 2;
ground.position.y = -0.05;
scene.add(ground);

let roadMesh = null, startLine = null;
const STATE = {connected: false, id: 0, track: [], width: 18, laps: 3,
               roundState: 'lobby', centerUntil: 0, fw: '?'};

function buildTrack(track, width) {
    if (roadMesh) { scene.remove(roadMesh); roadMesh.geometry.dispose(); }
    if (startLine) scene.remove(startLine);
    const n = track.length, hw = width / 2;
    const verts = [];
    for (let i = 0; i < n; i++) {
        const cur = track[i], nxt = track[(i + 1) % n];
        let dx = nxt[0] - cur[0], dz = nxt[1] - cur[1];
        const l = Math.hypot(dx, dz) || 1; dx /= l; dz /= l;
        const px = dz, pz = -dx;   // right perpendicular
        verts.push(cur[0] + px * hw, 0, cur[1] + pz * hw);
        verts.push(cur[0] - px * hw, 0, cur[1] - pz * hw);
    }
    const idx = [];
    for (let i = 0; i < n; i++) {
        const a = 2 * i, b = 2 * i + 1, c = (2 * i + 2) % (2 * n), d = (2 * i + 3) % (2 * n);
        idx.push(a, b, c, b, d, c);
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.Float32BufferAttribute(verts, 3));
    g.setIndex(idx);
    g.computeVertexNormals();
    roadMesh = new THREE.Mesh(g, new THREE.MeshLambertMaterial({color: 0x2a2d34, side: THREE.DoubleSide}));
    roadMesh.position.y = 0.02;
    scene.add(roadMesh);

    // start/finish stripe across the road at waypoint 0
    startLine = new THREE.Mesh(
        new THREE.PlaneGeometry(width, 4),
        new THREE.MeshBasicMaterial({color: 0xffffff}));
    startLine.rotation.x = -Math.PI / 2;
    startLine.position.set(track[0][0], 0.05, track[0][1]);
    scene.add(startLine);
}

function makeCar(color) {
    const grp = new THREE.Group();
    const body = new THREE.Mesh(new THREE.BoxGeometry(2.2, 0.9, 4.2),
        new THREE.MeshLambertMaterial({color}));
    body.position.y = 0.6;
    grp.add(body);
    const roof = new THREE.Mesh(new THREE.BoxGeometry(1.8, 0.8, 2.0),
        new THREE.MeshLambertMaterial({color: 0x222831}));
    roof.position.set(0, 1.3, -0.2);
    grp.add(roof);
    return grp;
}

const me = {x: 0, z: 0, h: 0, speed: 0, lap: 0, mesh: null};
me.mesh = makeCar(PALETTE[0]);
scene.add(me.mesh);
const others = new Map();  // id -> { mesh }

function ensureOther(id) {
    let o = others.get(id);
    if (!o) { o = {mesh: makeCar(PALETTE[(id - 1) % PALETTE.length])}; scene.add(o.mesh); others.set(id, o); }
    return o;
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
        STATE.id = m.id; STATE.track = m.track; STATE.width = m.width; STATE.laps = m.laps;
        me.mesh.children[0].material.color.setHex(PALETTE[(m.id - 1) % PALETTE.length]);
        buildTrack(m.track, m.width);
        me.x = m.sx; me.z = m.sz; me.h = m.sh; me.speed = 0; me.lap = 0;
        for (const [, o] of others) scene.remove(o.mesh);
        others.clear();
        loginEl.classList.add('hidden'); hudEl.classList.remove('hidden');
        showCenter('GET READY', 1300);
        break;
    case 'sb':
        for (const p of m.p) {
            if (p.id === STATE.id) { me.lap = p.l; continue; }
            const o = ensureOther(p.id);
            o.mesh.position.set(p.x, 0, p.z);
            o.mesh.rotation.y = p.h;
            o.lap = p.l;
        }
        break;
    case 'lap':
        if (m.id === STATE.id) showCenter('LAP ' + m.lap + '/' + STATE.laps, 1000);
        break;
    case 'score': {
        STATE.scores = m.p;
        updateScoreboard(m.p);
        break;
    }
    case 'round':
        STATE.roundState = m.state;
        if (typeof m.fw === 'string') STATE.fw = m.fw;
        timerEl.textContent = formatTime(m.remaining);
        if (m.state === 'ended') {
            const w = (STATE.scores || []).find(r => r[0] === m.winner);
            showCenter(w ? 'WINNER: ' + w[2] : 'RACE OVER', 3000);
        } else if (m.state === 'lobby') showCenter('WAITING FOR HOST', 1200);
        else if (m.state === 'paused') showCenter('PAUSED', 0);
        break;
    case 'leave': {
        const o = others.get(m.id);
        if (o) { scene.remove(o.mesh); others.delete(m.id); }
        break;
    }
    case 'err': statusEl.textContent = 'error: ' + (m.m || 'unknown'); break;
    }
}

function showCenter(t, ms) { centerEl.textContent = t; STATE.centerUntil = ms ? performance.now() + ms : Infinity; }
function formatTime(s) { s = Math.max(0, s | 0); return ('0' + (s / 60 | 0)).slice(-2) + ':' + ('0' + (s % 60)).slice(-2); }
function updateScoreboard(rows) {
    boardEl.innerHTML = '';
    for (const [id, lap, name] of rows.slice().sort((a, b) => b[1] - a[1])) {
        const r = document.createElement('div');
        r.className = 'row' + (id === STATE.id ? ' me' : '');
        r.innerHTML = '<span>' + escapeHtml(name) + '</span><span>L' + lap + '</span>';
        boardEl.appendChild(r);
    }
}
function escapeHtml(s) { return String(s).replace(/[&<>"']/g, c => ({'&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'}[c])); }

// ---------- input ----------
const stick = {active: false, x: 0, id: -1, cx: 0, r: 1};
function sStart(ev) {
    const t = ev.changedTouches ? ev.changedTouches[0] : ev;
    const rect = stickEl.getBoundingClientRect();
    stick.cx = rect.left + rect.width / 2; stick.r = rect.width / 2;
    stick.active = true; stick.id = ev.changedTouches ? t.identifier : -1; sMove(ev);
}
function sMove(ev) {
    if (!stick.active) return;
    let t = null;
    if (ev.changedTouches) { for (const c of ev.changedTouches) if (c.identifier === stick.id) { t = c; break; } if (!t) return; }
    else t = ev;
    let dx = t.clientX - stick.cx;
    if (dx > stick.r) dx = stick.r; if (dx < -stick.r) dx = -stick.r;
    stick.x = dx / stick.r;
    knobEl.style.transform = 'translate(' + dx + 'px,0)';
}
function sEnd(ev) {
    if (ev.changedTouches) { let f = false; for (const c of ev.changedTouches) if (c.identifier === stick.id) { f = true; break; } if (!f) return; }
    stick.active = false; stick.x = 0; knobEl.style.transform = 'translate(0,0)';
}
stickEl.addEventListener('touchstart', sStart, {passive: true});
stickEl.addEventListener('touchmove', sMove, {passive: true});
stickEl.addEventListener('touchend', sEnd, {passive: true});
stickEl.addEventListener('touchcancel', sEnd, {passive: true});
stickEl.addEventListener('mousedown', sStart);
addEventListener('mousemove', sMove);
addEventListener('mouseup', sEnd);

let gasHeld = false, brakeHeld = false;
function bindBtn(el, set) {
    el.addEventListener('touchstart', e => { e.preventDefault(); set(true); }, {passive: false});
    el.addEventListener('touchend', e => { e.preventDefault(); set(false); }, {passive: false});
    el.addEventListener('touchcancel', () => set(false));
    el.addEventListener('mousedown', e => { e.preventDefault(); set(true); });
    el.addEventListener('mouseup', () => set(false));
    el.addEventListener('mouseleave', () => set(false));
}
bindBtn(gasBtn, v => gasHeld = v);
bindBtn(brakeBtn, v => brakeHeld = v);
const keys = {};
addEventListener('keydown', e => keys[e.code] = true);
addEventListener('keyup', e => keys[e.code] = false);

// ---------- track helpers ----------
// Nearest point on the track centreline + distance to it.
function nearestOnTrack(x, z) {
    const t = STATE.track, n = t.length;
    let best = Infinity, bx = x, bz = z;
    for (let i = 0; i < n; i++) {
        const a = t[i], b = t[(i + 1) % n];
        const vx = b[0] - a[0], vz = b[1] - a[1];
        const wx = x - a[0], wz = z - a[1];
        const len2 = vx * vx + vz * vz || 1;
        let s = (wx * vx + wz * vz) / len2;
        s = Math.max(0, Math.min(1, s));
        const px = a[0] + vx * s, pz = a[1] + vz * s;
        const d = (x - px) * (x - px) + (z - pz) * (z - pz);
        if (d < best) { best = d; bx = px; bz = pz; }
    }
    return { x: bx, z: bz, dist: Math.sqrt(best) };
}

// ---------- loop ----------
let lastT = performance.now(), lastSend = 0;
function frame() {
    const now = performance.now();
    let dt = (now - lastT) / 1000; if (dt > 0.1) dt = 0.1; lastT = now;

    if (STATE.connected && STATE.roundState !== 'paused') {
        let steer = stick.x;
        if (keys['ArrowLeft'] || keys['KeyA']) steer = -1;
        if (keys['ArrowRight'] || keys['KeyD']) steer = 1;
        const gas = gasHeld || keys['ArrowUp'] || keys['KeyW'];
        const brake = brakeHeld || keys['ArrowDown'] || keys['KeyS'];

        if (gas) me.speed += ACCEL * dt; else me.speed -= DRAG * dt;
        if (brake) me.speed -= BRAKE_DECEL * dt;
        me.speed = Math.max(0, Math.min(MAX_SPEED, me.speed));
        // Turn rate scales with speed but stays responsive even when slow.
        // Negated so the on-screen left/right match the chase-cam view.
        const turnFactor = Math.max(TURN_MIN, me.speed / MAX_SPEED);
        me.h -= steer * TURN * turnFactor * dt;
        me.x += Math.sin(me.h) * me.speed * dt;
        me.z += Math.cos(me.h) * me.speed * dt;

        // Keep the car on the track: clamp it to the road edge (invisible walls).
        if (STATE.track.length) {
            const near = nearestOnTrack(me.x, me.z);
            const maxOff = STATE.width / 2 - 1.2;
            if (near.dist > maxOff) {
                const k = maxOff / near.dist;
                me.x = near.x + (me.x - near.x) * k;
                me.z = near.z + (me.z - near.z) * k;
                me.speed *= 0.65;   // scrub speed when scraping the edge
            }
        }
    }

    me.mesh.position.set(me.x, 0, me.z);
    me.mesh.rotation.y = me.h;

    // chase cam
    const fx = Math.sin(me.h), fz = Math.cos(me.h);
    camera.position.set(me.x - fx * 16, 9, me.z - fz * 16);
    camera.lookAt(me.x + fx * 8, 1.5, me.z + fz * 8);

    if (now - lastSend > 60 && STATE.connected) {
        lastSend = now;
        send({t: 's', x: me.x, z: me.z, h: me.h});
    }

    lapEl.textContent = 'LAP ' + me.lap + '/' + STATE.laps;
    if (now > STATE.centerUntil) centerEl.textContent = '';
    diagEl.textContent = 'CLI ' + CLIENT_VERSION + '  FW ' + STATE.fw + '\n' + STATE.roundState.toUpperCase();

    renderer.render(scene, camera);
    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);
setInterval(() => { if (STATE.connected) send({t: 'ping'}); }, 3000);

joinBtn.addEventListener('click', () => {
    const name = (nameEl.value || 'RACER').trim().slice(0, 16) || 'RACER';
    if (nameEl.value) localStorage.setItem('race_name', nameEl.value);
    connect(name);
});
nameEl.addEventListener('keydown', e => { if (e.key === 'Enter') joinBtn.click(); });
const saved = localStorage.getItem('race_name');
if (saved) nameEl.value = saved;

})();
