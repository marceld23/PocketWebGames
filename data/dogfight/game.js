// Space Dogfight browser client - Three.js renderer (stage C).
// Loads global THREE from /shared/three.min.js (UMD r155).
// Protocol layer is unchanged from the Canvas2D version; only rendering swaps.

(() => {
'use strict';

// Bumped whenever the client file changes — surfaces in the diag panel so we
// can tell instantly whether the device is serving cached JS.
const CLIENT_VERSION = 'pu-coll-diag-3';

// ---------- config ----------
// false = arcade (stick UP = nose UP)
// true  = flight-sim (stick UP = nose DOWN)
const INVERT_Y = false;
const INVERT_X = false;

// ---------- DOM refs ----------
const canvas    = document.getElementById('view');
const loginEl   = document.getElementById('login');
const hudEl     = document.getElementById('hud');
const nameEl    = document.getElementById('name');
const joinBtn   = document.getElementById('join');
const statusEl  = document.getElementById('status');
const hpEl      = document.getElementById('hudHp');
const scoreEl   = document.getElementById('hudScore');
const timerEl   = document.getElementById('hudTimer');
const boardEl   = document.getElementById('scoreboard');
const centerEl  = document.getElementById('centerMsg');
const stickEl   = document.getElementById('stick');
const knobEl    = document.getElementById('stickKnob');
const fireBtn   = document.getElementById('btnFire');
const boostBtn  = document.getElementById('btnBoost');
const diagEl    = document.getElementById('diag');

// ---------- Three.js scene ----------
const scene = new THREE.Scene();
scene.background = new THREE.Color(0x05080f);
scene.fog = new THREE.Fog(0x05080f, 200, 800);

const camera = new THREE.PerspectiveCamera(72, window.innerWidth / window.innerHeight, 0.1, 4000);

const renderer = new THREE.WebGLRenderer({
    canvas,
    antialias: false,        // off for phone perf
    powerPreference: 'high-performance',
});
renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 1.5));
renderer.setSize(window.innerWidth, window.innerHeight);

function onResize() {
    camera.aspect = window.innerWidth / window.innerHeight;
    camera.updateProjectionMatrix();
    renderer.setSize(window.innerWidth, window.innerHeight);
}
window.addEventListener('resize', onResize);
onResize();

// ---------- lighting ----------
scene.add(new THREE.AmbientLight(0x90a8d0, 0.45));
const sun = new THREE.DirectionalLight(0xffe9b4, 1.4);
sun.position.set(50, 80, 40);
scene.add(sun);
const rim = new THREE.DirectionalLight(0x6a90ff, 0.6);
rim.position.set(-40, -10, -30);
scene.add(rim);

// ---------- procedural textures ----------
function makeCanvas(size = 256) {
    const c = document.createElement('canvas');
    c.width = c.height = size;
    return c;
}

function makeHullTexture(hue) {
    // hue: 'blue' or 'red' tint
    const size = 256;
    const c = makeCanvas(size);
    const ctx = c.getContext('2d');

    // Base panels gradient
    const grad = ctx.createLinearGradient(0, 0, 0, size);
    if (hue === 'red') {
        grad.addColorStop(0, '#3b1410');
        grad.addColorStop(1, '#1a0808');
    } else {
        grad.addColorStop(0, '#1a2840');
        grad.addColorStop(1, '#0a1424');
    }
    ctx.fillStyle = grad;
    ctx.fillRect(0, 0, size, size);

    // Panel seams
    ctx.strokeStyle = 'rgba(0,0,0,0.6)';
    ctx.lineWidth = 2;
    for (let y = 24; y < size; y += 32) {
        ctx.beginPath();
        ctx.moveTo(0, y);
        ctx.lineTo(size, y + (Math.random()-0.5)*2);
        ctx.stroke();
    }
    for (let x = 0; x < size; x += 64) {
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.lineTo(x + (Math.random()-0.5)*2, size);
        ctx.stroke();
    }

    // Highlight panels
    ctx.fillStyle = hue === 'red' ? 'rgba(255,80,40,0.10)' : 'rgba(140,200,255,0.10)';
    for (let i = 0; i < 14; i++) {
        const w = 16 + Math.random()*40, h = 8 + Math.random()*20;
        ctx.fillRect(Math.random()*size, Math.random()*size, w, h);
    }

    // Rivets
    ctx.fillStyle = 'rgba(180,200,220,0.25)';
    for (let i = 0; i < 120; i++) {
        const x = Math.random()*size, y = Math.random()*size;
        ctx.beginPath();
        ctx.arc(x, y, 1.0, 0, Math.PI*2);
        ctx.fill();
    }

    // Dirty streaks
    ctx.strokeStyle = 'rgba(0,0,0,0.15)';
    ctx.lineWidth = 6;
    for (let i = 0; i < 6; i++) {
        const x = Math.random()*size;
        ctx.beginPath();
        ctx.moveTo(x, 0);
        ctx.bezierCurveTo(x + 20, size*0.3, x - 10, size*0.6, x + 15, size);
        ctx.stroke();
    }

    const tex = new THREE.CanvasTexture(c);
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
    tex.anisotropy = 4;
    return tex;
}

function makeAsteroidTexture() {
    const size = 256;
    const c = makeCanvas(size);
    const ctx = c.getContext('2d');

    // Base rock noise
    const img = ctx.createImageData(size, size);
    for (let i = 0; i < img.data.length; i += 4) {
        const n = Math.random();
        const v = (60 + n * 80) | 0;
        img.data[i]   = v + 18;
        img.data[i+1] = v + 4;
        img.data[i+2] = v - 8;
        img.data[i+3] = 255;
    }
    ctx.putImageData(img, 0, 0);

    // Craters
    for (let i = 0; i < 18; i++) {
        const x = Math.random()*size, y = Math.random()*size;
        const r = 6 + Math.random()*24;
        const g = ctx.createRadialGradient(x, y, 0, x, y, r);
        g.addColorStop(0, 'rgba(0,0,0,0.55)');
        g.addColorStop(0.7, 'rgba(0,0,0,0.15)');
        g.addColorStop(1, 'rgba(255,200,160,0.10)');
        ctx.fillStyle = g;
        ctx.beginPath();
        ctx.arc(x, y, r, 0, Math.PI*2);
        ctx.fill();
    }

    // Highlights
    for (let i = 0; i < 40; i++) {
        ctx.fillStyle = 'rgba(255,255,240,' + (Math.random()*0.06) + ')';
        ctx.fillRect(Math.random()*size, Math.random()*size, 2, 2);
    }

    const tex = new THREE.CanvasTexture(c);
    tex.wrapS = tex.wrapT = THREE.RepeatWrapping;
    return tex;
}

function makeGlowTexture() {
    const size = 128;
    const c = makeCanvas(size);
    const ctx = c.getContext('2d');
    const g = ctx.createRadialGradient(size/2, size/2, 0, size/2, size/2, size/2);
    g.addColorStop(0,   'rgba(255,240,180,1.0)');
    g.addColorStop(0.4, 'rgba(255,170, 80,0.6)');
    g.addColorStop(1.0, 'rgba(255, 80, 20,0.0)');
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, size, size);
    return new THREE.CanvasTexture(c);
}

const HULL_TEX_FRIEND = makeHullTexture('blue');
const HULL_TEX_ENEMY  = makeHullTexture('red');
const AST_TEX         = makeAsteroidTexture();
const GLOW_TEX        = makeGlowTexture();

// ---------- ship factory ----------
function makeShipMesh(palette) {
    const group = new THREE.Group();

    const hullMat = new THREE.MeshStandardMaterial({
        map: palette.hullTex,
        color: palette.hullTint,
        metalness: 0.35,
        roughness: 0.55,
    });
    const accentMat = new THREE.MeshStandardMaterial({
        color: palette.accent,
        metalness: 0.6,
        roughness: 0.35,
    });
    const canopyMat = new THREE.MeshStandardMaterial({
        color: 0x223344,
        metalness: 0.7,
        roughness: 0.15,
        emissive: 0x6699ff,
        emissiveIntensity: 0.2,
    });

    // Fuselage: tapered double-cone-ish shape
    const fuselage = new THREE.Mesh(
        new THREE.CylinderGeometry(0.15, 0.55, 3.6, 10, 1),
        hullMat);
    fuselage.rotation.x = Math.PI / 2;     // long axis along +Z
    fuselage.position.z = 0.4;
    group.add(fuselage);

    // Nose cone (apex along +Z)
    const nose = new THREE.Mesh(new THREE.ConeGeometry(0.15, 0.6, 10), accentMat);
    nose.rotation.x = Math.PI / 2;
    nose.position.z = 2.5;
    group.add(nose);

    // Canopy
    const canopy = new THREE.Mesh(
        new THREE.SphereGeometry(0.4, 8, 6, 0, Math.PI*2, 0, Math.PI/2),
        canopyMat);
    canopy.scale.set(0.7, 0.5, 1.4);
    canopy.position.set(0, 0.25, 0.6);
    group.add(canopy);

    // Vertical tail fin
    const fin = new THREE.Mesh(
        new THREE.BoxGeometry(0.08, 0.7, 0.9),
        hullMat);
    fin.position.set(0, 0.45, -1.1);
    group.add(fin);

    // Center engine bell
    const engine = new THREE.Mesh(
        new THREE.CylinderGeometry(0.35, 0.30, 0.5, 10),
        accentMat);
    engine.rotation.x = Math.PI / 2;
    engine.position.set(0, 0, -1.6);
    group.add(engine);

    // Shared glow material — all engine ports reuse the same texture.
    const glowMat = new THREE.SpriteMaterial({
        map: GLOW_TEX,
        color: 0xffcc66,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
        transparent: true,
    });

    // Center engine glow
    const centerGlow = new THREE.Sprite(glowMat);
    centerGlow.scale.set(1.6, 1.6, 1);
    centerGlow.position.set(0, 0, -1.95);
    group.add(centerGlow);

    // Wings + wingtip pods + wingtip glows, all nested under wing groups so
    // they inherit the sweep automatically.
    const wingGeom   = new THREE.BoxGeometry(1.8, 0.08, 1.2);
    const podGeom    = new THREE.CylinderGeometry(0.18, 0.22, 1.4, 8);
    for (const side of [-1, +1]) {
        const wingGroup = new THREE.Group();
        wingGroup.position.set(side * 1.0, -0.1, -0.4);
        wingGroup.rotation.y = side * -0.3;   // sweep back
        group.add(wingGroup);

        const wing = new THREE.Mesh(wingGeom, hullMat);
        wingGroup.add(wing);

        // Pod at the outer wingtip, extending backward in wing-local frame.
        const pod = new THREE.Mesh(podGeom, accentMat);
        pod.rotation.x = Math.PI / 2;
        pod.position.set(side * 0.85, 0, -0.7);
        wingGroup.add(pod);

        // Engine glow at the pod's rear face.
        const glow = new THREE.Sprite(glowMat);
        glow.scale.set(1.0, 1.0, 1);
        glow.position.set(side * 0.85, 0, -1.45);
        wingGroup.add(glow);
    }

    return group;
}

const FRIEND_PALETTE = {
    hullTex: HULL_TEX_FRIEND,
    hullTint: 0xa8c8f0,
    accent:   0x88c8ff,
};
const ENEMY_PALETTE = {
    hullTex: HULL_TEX_ENEMY,
    hullTint: 0xf0a098,
    accent:   0xff7044,
};

// ---------- asteroid factory ----------
function mulberry32(seed) {
    let a = seed >>> 0;
    return function() {
        a |= 0; a = (a + 0x6D2B79F5) | 0;
        let t = a;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function makeAsteroidMesh(radius, seed) {
    const geom = new THREE.IcosahedronGeometry(radius, 2);
    const pos  = geom.attributes.position;
    const rnd  = mulberry32(seed | 1);
    for (let i = 0; i < pos.count; i++) {
        const x = pos.getX(i), y = pos.getY(i), z = pos.getZ(i);
        const d = 0.75 + rnd() * 0.55;
        pos.setXYZ(i, x*d, y*d, z*d);
    }
    geom.computeVertexNormals();
    const mat = new THREE.MeshStandardMaterial({
        map: AST_TEX,
        color: 0xb0a090,
        metalness: 0.1,
        roughness: 0.95,
    });
    const mesh = new THREE.Mesh(geom, mat);
    // hp/maxHp are filled in by regenArena to mirror the server.
    mesh.userData.hp    = 0;
    mesh.userData.maxHp = 0;
    return mesh;
}

// ---------- power-up factory ----------
const PU_COLORS = [0x44ff66, 0x44ccff, 0xffe744, 0xff7744];
const PU_NAMES  = ['REPAIR', 'SHIELD', 'RAPID', 'TRIPLE'];

function makePowerupMesh(type) {
    const group = new THREE.Group();
    const color = PU_COLORS[type] || 0xffffff;
    const mat = new THREE.MeshBasicMaterial({ color, transparent: true, opacity: 0.95 });

    let inner;
    if (type === 0) {
        // REPAIR: medical cross
        inner = new THREE.Group();
        inner.add(new THREE.Mesh(new THREE.BoxGeometry(1.5, 0.45, 0.45), mat));
        inner.add(new THREE.Mesh(new THREE.BoxGeometry(0.45, 1.5, 0.45), mat));
    } else if (type === 1) {
        // SHIELD: wireframe icosphere
        inner = new THREE.Mesh(
            new THREE.IcosahedronGeometry(0.9, 1),
            new THREE.MeshBasicMaterial({ color, wireframe: true }));
    } else if (type === 2) {
        // RAPID: lightning bolt (two stacked cones)
        inner = new THREE.Group();
        const c1 = new THREE.Mesh(new THREE.ConeGeometry(0.5, 1.1, 4), mat);
        c1.rotation.z = 0.5;
        c1.position.y = 0.4;
        const c2 = new THREE.Mesh(new THREE.ConeGeometry(0.5, 1.1, 4), mat);
        c2.rotation.z = Math.PI - 0.5;
        c2.position.y = -0.4;
        inner.add(c1); inner.add(c2);
    } else {
        // TRIPLE: three spheres in a triangle
        inner = new THREE.Group();
        for (let i = 0; i < 3; i++) {
            const sph = new THREE.Mesh(new THREE.SphereGeometry(0.4, 8, 6), mat);
            const a = i * Math.PI * 2 / 3 + Math.PI / 2;
            sph.position.set(Math.cos(a) * 0.7, Math.sin(a) * 0.7, 0);
            inner.add(sph);
        }
    }
    group.add(inner);

    // Glow halo so it's visible from far.
    const halo = new THREE.Sprite(new THREE.SpriteMaterial({
        map: GLOW_TEX, color,
        blending: THREE.AdditiveBlending,
        depthWrite: false, transparent: true,
    }));
    halo.scale.set(3.5, 3.5, 1);
    group.add(halo);

    group.userData.inner = inner;
    group.userData.type  = type;
    return group;
}

// Shield bubble for the local player.
const shieldMesh = new THREE.Mesh(
    new THREE.SphereGeometry(3.5, 18, 12),
    new THREE.MeshBasicMaterial({
        color: 0x44ccff,
        transparent: true,
        opacity: 0.18,
        depthWrite: false,
        blending: THREE.AdditiveBlending,
        side: THREE.DoubleSide,
        wireframe: true,
    }));
shieldMesh.visible = false;

// ---------- starfield ----------
function makeStarfield(count) {
    const positions = new Float32Array(count * 3);
    const colors    = new Float32Array(count * 3);
    for (let i = 0; i < count; i++) {
        const u = Math.random() * 2 - 1;
        const a = Math.random() * Math.PI * 2;
        const r = Math.sqrt(1 - u*u);
        const D = 3000;
        positions[i*3]   = Math.cos(a) * r * D;
        positions[i*3+1] = u * D;
        positions[i*3+2] = Math.sin(a) * r * D;
        const b = 0.5 + Math.random() * 0.5;
        colors[i*3]   = b * 0.95;
        colors[i*3+1] = b;
        colors[i*3+2] = Math.min(1, b + Math.random() * 0.2);
    }
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    g.setAttribute('color',    new THREE.BufferAttribute(colors,    3));
    return new THREE.Points(g, new THREE.PointsMaterial({
        size: 3, vertexColors: true, sizeAttenuation: false,
    }));
}

const stars = makeStarfield(900);
scene.add(stars);

// ---------- arena ----------
let arenaSize = 500;
const asteroidGroup = new THREE.Group();
scene.add(asteroidGroup);
let asteroidMeshes = []; // index -> mesh (or null if destroyed)

// Mirror of server's ASTEROID_HP_PER_R - keep in sync with Config.h.
const ASTEROID_HP_PER_R = 5;

function applyAsteroidDamageTint(mesh) {
    if (!mesh || !mesh.userData.maxHp) return;
    const hpFrac = Math.max(0, mesh.userData.hp / mesh.userData.maxHp);
    // Damaged asteroids glow red and darken toward black.
    mesh.material.emissive.setRGB((1 - hpFrac) * 0.4, 0, 0);
    const c = 1 - (1 - hpFrac) * 0.5;
    mesh.material.color.setRGB(0xb0/255 * c, 0xa0/255 * c, 0x90/255 * c);
}

function regenArena(seed, size, deadList, adhpList) {
    arenaSize = size;
    // Wipe previous arena
    for (const m of asteroidMeshes) {
        if (!m) continue;
        m.geometry.dispose();
        m.material.dispose();
    }
    while (asteroidGroup.children.length) asteroidGroup.remove(asteroidGroup.children[0]);
    asteroidMeshes = [];

    const rnd = mulberry32(seed);
    // ASTEROIDS FIRST so server's PRNG stays in sync (server skips stars).
    for (let i = 0; i < 22; i++) {
        const r = 6 + rnd() * 10;
        const x = (rnd()*2-1) * size * 0.45;
        const y = (rnd()*2-1) * size * 0.15;
        const z = (rnd()*2-1) * size * 0.45;
        const mesh = makeAsteroidMesh(r, ((seed ^ i) * 9301 + 49297) & 0x7fffffff);
        mesh.position.set(x, y, z);
        mesh.rotation.set(rnd()*Math.PI*2, rnd()*Math.PI*2, rnd()*Math.PI*2);
        mesh.userData.spin = new THREE.Vector3(
            (rnd()-0.5)*0.4, (rnd()-0.5)*0.4, (rnd()-0.5)*0.4);
        mesh.userData.maxHp = Math.round(r * ASTEROID_HP_PER_R);
        mesh.userData.hp    = mesh.userData.maxHp;
        asteroidGroup.add(mesh);
        asteroidMeshes.push(mesh);
    }
    if (Array.isArray(deadList)) {
        for (const idx of deadList) {
            const m = asteroidMeshes[idx];
            if (m) { asteroidGroup.remove(m); asteroidMeshes[idx] = null; }
        }
    }
    if (Array.isArray(adhpList)) {
        for (const row of adhpList) {
            const idx = row[0], hp = row[1];
            const m = asteroidMeshes[idx];
            if (m) {
                m.userData.hp = hp;
                applyAsteroidDamageTint(m);
            }
        }
    }
}
regenArena(12345, arenaSize, null);

// ---------- player & remote ships ----------
const me = {
    pos:  new THREE.Vector3(),
    quat: new THREE.Quaternion(),
    hp:   100,
    score: 0,
    dead: false,
    mesh: null,
};
me.mesh = makeShipMesh(FRIEND_PALETTE);
me.mesh.add(shieldMesh);
scene.add(me.mesh);

// Active client-side buffs (RAPID, TRIPLE timed; SHIELD is a boolean since the
// server tells us when it's consumed).
const buffs = {
    rapidUntil:  0,
    tripleUntil: 0,
    shieldActive: false,
};

// Power-ups currently visible in the arena. id -> { mesh, type, basePos }.
const powerups = new Map();
const powerupGroup = new THREE.Group();
scene.add(powerupGroup);

function spawnPowerup(id, type, x, y, z) {
    const mesh = makePowerupMesh(type);
    mesh.position.set(x, y, z);
    powerupGroup.add(mesh);
    powerups.set(id, { mesh, type, basePos: new THREE.Vector3(x, y, z) });
}
function despawnPowerup(id) {
    const pu = powerups.get(id);
    if (!pu) return;
    powerupGroup.remove(pu.mesh);
    powerups.delete(id);
}
function clearAllPowerups() {
    for (const id of [...powerups.keys()]) despawnPowerup(id);
}

// id -> { mesh, posPrev, posTarget, quatPrev, quatTarget, t0, t1, hp, dead }
const otherShips = new Map();

function ensureOther(id) {
    let o = otherShips.get(id);
    if (!o) {
        o = {
            mesh: makeShipMesh(ENEMY_PALETTE),
            posPrev: new THREE.Vector3(),
            posTarget: new THREE.Vector3(),
            quatPrev: new THREE.Quaternion(),
            quatTarget: new THREE.Quaternion(),
            t0: performance.now(),
            t1: performance.now() + 100,
            hp: 100,
            dead: false,
        };
        scene.add(o.mesh);
        otherShips.set(id, o);
    }
    return o;
}
function removeOther(id) {
    const o = otherShips.get(id);
    if (!o) return;
    scene.remove(o.mesh);
    otherShips.delete(id);
}

// ---------- lasers + explosions ----------
const LASER_GEOM = new THREE.CylinderGeometry(0.12, 0.12, 1.0, 6);
LASER_GEOM.rotateX(Math.PI / 2);
const laserGroup = new THREE.Group();
scene.add(laserGroup);
const lasers = []; // { mesh, dir, life, owner }

function spawnLaser(x, y, z, dx, dy, dz, owner) {
    const color = owner === STATE.id ? 0xffe066 : 0xff5050;
    const mat = new THREE.MeshBasicMaterial({ color });
    const mesh = new THREE.Mesh(LASER_GEOM, mat);
    mesh.position.set(x, y, z);
    mesh.scale.set(1, 1, 6);
    // Aim along (dx,dy,dz). Geometry was rotated so local +Z is the long axis.
    const target = new THREE.Vector3(x + dx, y + dy, z + dz);
    mesh.lookAt(target);
    laserGroup.add(mesh);

    // Halo sprite ride-along
    const halo = new THREE.Sprite(new THREE.SpriteMaterial({
        map: GLOW_TEX,
        color,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
        transparent: true,
    }));
    halo.scale.set(1.4, 1.4, 1);
    mesh.add(halo);

    lasers.push({ mesh, dir: new THREE.Vector3(dx,dy,dz), life: 0.5, owner });
}

const explosionGroup = new THREE.Group();
scene.add(explosionGroup);
const explosions = []; // { mesh, t0, life, scale0, scaleEnd }

function spawnExplosion(x, y, z, scale = 1, color = 0xffaa44) {
    const mat = new THREE.MeshBasicMaterial({
        color,
        transparent: true,
        opacity: 1,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
    });
    const mesh = new THREE.Mesh(new THREE.SphereGeometry(1, 12, 8), mat);
    mesh.position.set(x, y, z);
    mesh.scale.setScalar(scale * 0.6);
    explosionGroup.add(mesh);
    explosions.push({ mesh, mat, t0: performance.now(), life: 0.8, end: scale * 6 });

    // Sprite flash
    const flashMat = new THREE.SpriteMaterial({
        map: GLOW_TEX,
        color,
        blending: THREE.AdditiveBlending,
        depthWrite: false,
        transparent: true,
    });
    const flash = new THREE.Sprite(flashMat);
    flash.position.set(x, y, z);
    flash.scale.set(scale * 4, scale * 4, 1);
    explosionGroup.add(flash);
    explosions.push({ mesh: flash, mat: flashMat, t0: performance.now(), life: 0.6, end: scale * 8, isSprite: true });
}

// ---------- state container ----------
const STATE = {
    connected: false,
    id: 0,
    seed: 12345,
    arena: 500,
    maxHp: 100,
    roundState: 'lobby',
    remainingSec: 0,
    scores: [],
    fireCooldownUntil: 0,
    boosting: false,
    centerMsgUntil: 0,
    // Diagnostics straight from the server's round broadcast.
    diag: { nA: 0, nP: 0, nC: 0, fw: '?' },
};

// Render the persistent diag panel even before connecting so the user can
// always see which client version is running.
function refreshDiagPanel() {
    let nearestStr = '';
    if (asteroidMeshes && asteroidMeshes.length) {
        let best = Infinity, bi = -1, br = 0;
        for (let i = 0; i < asteroidMeshes.length; i++) {
            const m = asteroidMeshes[i];
            if (!m) continue;
            const d = me.pos.distanceTo(m.position);
            if (d < best) { best = d; bi = i; br = m.geometry.boundingSphere ? m.geometry.boundingSphere.radius : 8; }
        }
        if (bi >= 0) nearestStr = '  near ast#' + bi + ' d=' + best.toFixed(1);
    }
    diagEl.textContent =
        'CLI ' + CLIENT_VERSION +
        '  FW ' + STATE.diag.fw +
        '\nSRV ' + STATE.roundState.toUpperCase() +
        '  P:' + STATE.diag.nC +
        '  AST:' + STATE.diag.nA +
        '  PU:' + STATE.diag.nP +
        nearestStr;
}
diagEl.textContent = 'CLI ' + CLIENT_VERSION + '  FW ?';

let ws = null;
function connect(name) {
    statusEl.textContent = 'connecting...';
    const url = (location.protocol === 'https:' ? 'wss://' : 'ws://')
                + location.host + '/ws';
    ws = new WebSocket(url);
    ws.onopen = () => {
        STATE.connected = true;
        ws.send(JSON.stringify({t:'join', name}));
    };
    ws.onclose = () => {
        STATE.connected = false;
        statusEl.textContent = 'connection lost';
        loginEl.classList.remove('hidden');
        hudEl.classList.add('hidden');
    };
    ws.onerror = () => { statusEl.textContent = 'connect failed'; };
    ws.onmessage = (ev) => onMessage(ev.data);
}
function send(obj) {
    if (ws && ws.readyState === 1) ws.send(JSON.stringify(obj));
}

function onMessage(raw) {
    let m; try { m = JSON.parse(raw); } catch (e) { return; }
    switch (m.t) {
    case 'init': {
        STATE.id    = m.id;
        STATE.seed  = m.seed;
        STATE.arena = m.arena;
        STATE.maxHp = m.hp;
        me.hp = m.hp;
        me.dead = false;
        me.pos.set((Math.random()-0.5)*20, 0, (Math.random()-0.5)*20);
        me.quat.identity();
        // Reset buffs at the start of a fresh round.
        buffs.rapidUntil   = 0;
        buffs.tripleUntil  = 0;
        buffs.shieldActive = false;
        shieldMesh.visible = false;
        clearAllPowerups();
        regenArena(m.seed, m.arena, m.dead, m.adhp);
        if (Array.isArray(m.pu)) {
            for (const pu of m.pu) spawnPowerup(pu.id, pu.type, pu.x, pu.y, pu.z);
        }
        loginEl.classList.add('hidden');
        hudEl.classList.remove('hidden');
        showCenter('GET READY', 1500);
        break;
    }
    case 'adamage': {
        const mesh = asteroidMeshes[m.id];
        if (mesh) {
            mesh.userData.hp = m.hp;
            applyAsteroidDamageTint(mesh);
            spawnExplosion(mesh.position.x, mesh.position.y, mesh.position.z,
                           0.5, 0xc0a880);
        }
        break;
    }
    case 'adestroy': {
        const mesh = asteroidMeshes[m.id];
        if (mesh) {
            spawnExplosion(mesh.position.x, mesh.position.y, mesh.position.z, 1.4, 0xffaa44);
            asteroidGroup.remove(mesh);
            mesh.geometry.dispose();
            mesh.material.dispose();
            asteroidMeshes[m.id] = null;
        }
        break;
    }
    case 'pspawn': {
        spawnPowerup(m.id, m.type, m.x, m.y, m.z);
        break;
    }
    case 'ppickup': {
        const pu = powerups.get(m.id);
        if (pu) {
            spawnExplosion(pu.mesh.position.x, pu.mesh.position.y, pu.mesh.position.z,
                           0.8, PU_COLORS[m.type]);
            despawnPowerup(m.id);
        }
        if (m.target === STATE.id) applyPowerupLocally(m.type);
        break;
    }
    case 'shield': {
        // Server tells us a player's shield absorbed a hit.
        if (m.target === STATE.id) {
            buffs.shieldActive = false;
            shieldMesh.visible = false;
            showCenter('SHIELD ABSORBED', 700);
            flashHit();
        }
        break;
    }
    case 'sb': {
        const now = performance.now();
        const seen = new Set();
        for (const p of m.p) {
            seen.add(p.id);
            if (p.id === STATE.id) {
                me.hp = p.hp;
                me.dead = !!p.r;
                if (me.dead) me.mesh.visible = false;
                continue;
            }
            const o = ensureOther(p.id);
            o.posPrev.copy(o.posTarget);
            o.quatPrev.copy(o.quatTarget);
            o.posTarget.set(p.x, p.y, p.z);
            o.quatTarget.set(p.qx, p.qy, p.qz, p.qw);
            o.t0 = now;
            o.t1 = now + 100;
            o.hp = p.hp;
            o.dead = !!p.r;
            o.mesh.visible = !o.dead;
        }
        for (const id of [...otherShips.keys()]) {
            if (!seen.has(id)) removeOther(id);
        }
        break;
    }
    case 'fb': {
        spawnLaser(m.x, m.y, m.z, m.dx, m.dy, m.dz, m.id);
        break;
    }
    case 'hit': {
        if (m.target === STATE.id) {
            me.hp = m.hp;
            flashHit();
            if (m.a === 0) showCenter('COLLISION', 500);
        } else if (m.a === STATE.id) {
            showCenter('HIT', 350);
        }
        break;
    }
    case 'kill': {
        // Find victim position for explosion
        let vx, vy, vz;
        if (m.target === STATE.id) { vx = me.pos.x; vy = me.pos.y; vz = me.pos.z; }
        else {
            const o = otherShips.get(m.target);
            if (o) { vx = o.posTarget.x; vy = o.posTarget.y; vz = o.posTarget.z; }
        }
        if (vx !== undefined) spawnExplosion(vx, vy, vz, 1.8, 0xffcc44);
        if (m.target === STATE.id) showCenter('YOU DIED', 1500);
        else if (m.a === STATE.id) {
            STATE.scores; // updated via score msg
            showCenter('TARGET DOWN', 800);
        }
        break;
    }
    case 'respawn': {
        if (m.id === STATE.id) {
            me.pos.set(m.x, m.y, m.z);
            me.quat.identity();
            me.hp = STATE.maxHp;
            me.dead = false;
            me.mesh.visible = true;
            showCenter('RESPAWNED', 800);
        } else {
            const o = ensureOther(m.id);
            o.posTarget.set(m.x, m.y, m.z);
            o.posPrev.copy(o.posTarget);
            o.dead = false;
            o.mesh.visible = true;
        }
        break;
    }
    case 'score': {
        STATE.scores = m.p;
        const mine = STATE.scores.find(r => r[0] === STATE.id);
        if (mine) me.score = mine[1];
        updateScoreboard();
        break;
    }
    case 'round': {
        STATE.roundState   = m.state;
        STATE.remainingSec = m.remaining;
        if (typeof m.nA === 'number') STATE.diag.nA = m.nA;
        if (typeof m.nP === 'number') STATE.diag.nP = m.nP;
        if (typeof m.nC === 'number') STATE.diag.nC = m.nC;
        if (typeof m.fw === 'string') STATE.diag.fw = m.fw;
        refreshDiagPanel();
        timerEl.textContent = formatTime(m.remaining);
        if (m.state === 'ended') {
            const winRow = STATE.scores.find(r => r[0] === m.winner);
            showCenter(winRow ? 'WINNER: ' + winRow[2] : 'ROUND OVER', 3000);
        } else if (m.state === 'lobby') {
            showCenter('WAITING FOR HOST', 1500);
        } else if (m.state === 'paused') {
            showCenter('PAUSED', 0);
        }
        break;
    }
    case 'leave': {
        removeOther(m.id);
        break;
    }
    case 'err': {
        statusEl.textContent = 'error: ' + (m.m || 'unknown');
        break;
    }
    }
}

// ---------- UI helpers ----------
function showCenter(text, ms) {
    centerEl.textContent = text;
    STATE.centerMsgUntil = ms ? performance.now() + ms : Infinity;
}
function clearCenterIfDue() {
    if (performance.now() > STATE.centerMsgUntil) centerEl.textContent = '';
}
function flashHit() {
    document.body.animate(
        [{filter:'none'}, {filter:'hue-rotate(-40deg) brightness(1.4)'}, {filter:'none'}],
        {duration: 250});
}
function formatTime(s) {
    s = Math.max(0, s|0);
    return ('0' + (s/60|0)).slice(-2) + ':' + ('0' + (s%60)).slice(-2);
}
function updateScoreboard() {
    boardEl.innerHTML = '';
    const sorted = STATE.scores.slice().sort((a,b) => b[1] - a[1]);
    for (const [id, score, name] of sorted) {
        const row = document.createElement('div');
        row.className = 'row' + (id === STATE.id ? ' me' : '');
        row.innerHTML = '<span>' + escapeHtml(name) + '</span><span>' + score + '</span>';
        boardEl.appendChild(row);
    }
}
function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, c => ({
        '&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'
    }[c]));
}

// ---------- touch controls ----------
const stickInput = { active: false, x: 0, y: 0, id: -1, cx: 0, cy: 0, r: 0 };
function stickStart(ev) {
    const t = ev.changedTouches ? ev.changedTouches[0] : ev;
    const rect = stickEl.getBoundingClientRect();
    stickInput.cx = rect.left + rect.width / 2;
    stickInput.cy = rect.top  + rect.height / 2;
    stickInput.r  = rect.width / 2;
    stickInput.active = true;
    stickInput.id = ev.changedTouches ? t.identifier : -1;
    stickMove(ev);
}
function stickMove(ev) {
    if (!stickInput.active) return;
    let t = null;
    if (ev.changedTouches) {
        for (const c of ev.changedTouches) {
            if (c.identifier === stickInput.id) { t = c; break; }
        }
        if (!t) return;
    } else { t = ev; }
    let dx = t.clientX - stickInput.cx;
    let dy = t.clientY - stickInput.cy;
    const d = Math.hypot(dx, dy);
    if (d > stickInput.r) { dx *= stickInput.r/d; dy *= stickInput.r/d; }
    stickInput.x = dx / stickInput.r;
    stickInput.y = dy / stickInput.r;
    knobEl.style.transform = 'translate(' + (dx) + 'px,' + (dy) + 'px)';
}
function stickEnd(ev) {
    if (ev.changedTouches) {
        let found = false;
        for (const c of ev.changedTouches) {
            if (c.identifier === stickInput.id) { found = true; break; }
        }
        if (!found) return;
    }
    stickInput.active = false;
    stickInput.x = 0; stickInput.y = 0;
    knobEl.style.transform = 'translate(0,0)';
}
stickEl.addEventListener('touchstart', stickStart, {passive:true});
stickEl.addEventListener('touchmove',  stickMove,  {passive:true});
stickEl.addEventListener('touchend',   stickEnd,   {passive:true});
stickEl.addEventListener('touchcancel',stickEnd,   {passive:true});
stickEl.addEventListener('mousedown',  stickStart);
window.addEventListener('mousemove',   stickMove);
window.addEventListener('mouseup',     stickEnd);

function applyPowerupLocally(type) {
    const now = performance.now();
    if (type === 0) {                       // REPAIR (server has already healed)
        showCenter('+50 REPAIR', 800);
    } else if (type === 1) {                // SHIELD
        buffs.shieldActive = true;
        shieldMesh.visible = true;
        showCenter('SHIELD UP', 800);
    } else if (type === 2) {                // RAPID
        buffs.rapidUntil = now + 10000;
        showCenter('RAPID FIRE', 800);
    } else if (type === 3) {                // TRIPLE
        buffs.tripleUntil = now + 10000;
        showCenter('TRIPLE SHOT', 800);
    }
}

function fire() {
    const now = performance.now();
    const cooldown = (now < buffs.rapidUntil) ? 90 : 220;
    if (now < STATE.fireCooldownUntil || me.dead) return;
    STATE.fireCooldownUntil = now + cooldown;

    const fwd   = new THREE.Vector3(0, 0, 1).applyQuaternion(me.quat);
    const right = new THREE.Vector3(1, 0, 0).applyQuaternion(me.quat);
    const triple = now < buffs.tripleUntil;

    const dirs = triple
        ? [
            fwd.clone(),
            fwd.clone().add(right.clone().multiplyScalar( 0.10)).normalize(),
            fwd.clone().add(right.clone().multiplyScalar(-0.10)).normalize(),
          ]
        : [fwd];

    for (const d of dirs) {
        spawnLaser(me.pos.x, me.pos.y, me.pos.z, d.x, d.y, d.z, STATE.id);
        send({t:'f',
              x: me.pos.x, y: me.pos.y, z: me.pos.z,
              dx: d.x,     dy: d.y,     dz: d.z});
    }
}
fireBtn.addEventListener('touchstart', e => { e.preventDefault(); fire(); }, {passive:false});
fireBtn.addEventListener('mousedown',  e => { e.preventDefault(); fire(); });
boostBtn.addEventListener('touchstart', e => { e.preventDefault(); STATE.boosting = true; }, {passive:false});
boostBtn.addEventListener('touchend',   () => { STATE.boosting = false; }, {passive:true});
boostBtn.addEventListener('touchcancel',() => { STATE.boosting = false; }, {passive:true});
boostBtn.addEventListener('mousedown',  e => { e.preventDefault(); STATE.boosting = true; });
boostBtn.addEventListener('mouseup',    () => { STATE.boosting = false; });

const keys = {};
window.addEventListener('keydown', e => { keys[e.code] = true; if (e.code === 'Space') fire(); });
window.addEventListener('keyup',   e => { keys[e.code] = false; });

// ---------- main loop ----------
const BASE_SPEED   = 14;
const BOOST_SPEED  = 28;
const TURN_RATE    = 1.6;
const LASER_SPEED  = 500;

let lastT  = performance.now();
let lastSend = 0;

// Reusable temporaries
const _vY = new THREE.Vector3();
const _vX = new THREE.Vector3();
const _vZ = new THREE.Vector3();
const _q  = new THREE.Quaternion();
const _cam = new THREE.Vector3();

function step(dt) {
    if (!me.dead) {
        let yaw = stickInput.x, pitch = stickInput.y;
        if (keys['ArrowLeft'])  yaw   -= 1;
        if (keys['ArrowRight']) yaw   += 1;
        if (keys['ArrowUp'])    pitch -= 1;
        if (keys['ArrowDown'])  pitch += 1;
        yaw   = Math.max(-1, Math.min(1, yaw));
        pitch = Math.max(-1, Math.min(1, pitch));

        const yawSign   = INVERT_X ? +1 : -1;
        const pitchSign = INVERT_Y ? -1 : +1;
        if (yaw !== 0) {
            _vY.set(0,1,0).applyQuaternion(me.quat);
            _q.setFromAxisAngle(_vY, yawSign * yaw * TURN_RATE * dt);
            me.quat.premultiply(_q).normalize();
        }
        if (pitch !== 0) {
            _vX.set(1,0,0).applyQuaternion(me.quat);
            _q.setFromAxisAngle(_vX, pitchSign * pitch * TURN_RATE * dt);
            me.quat.premultiply(_q).normalize();
        }
        // Forward motion
        _vZ.set(0,0,1).applyQuaternion(me.quat);
        const sp = STATE.boosting || keys['ShiftLeft'] ? BOOST_SPEED : BASE_SPEED;
        me.pos.addScaledVector(_vZ, sp * dt);

        const half = arenaSize * 0.5;
        me.pos.x = Math.max(-half, Math.min(half, me.pos.x));
        me.pos.y = Math.max(-half*0.4, Math.min(half*0.4, me.pos.y));
        me.pos.z = Math.max(-half, Math.min(half, me.pos.z));
    }

    me.mesh.position.copy(me.pos);
    me.mesh.quaternion.copy(me.quat);

    // Interpolate remote ships
    const now = performance.now();
    for (const [, o] of otherShips) {
        if (o.dead) continue;
        let t = (now - o.t0) / Math.max(1, (o.t1 - o.t0));
        t = Math.max(0, Math.min(1.4, t));
        o.mesh.position.copy(o.posPrev).lerp(o.posTarget, t);
        o.mesh.quaternion.copy(o.quatPrev).slerp(o.quatTarget, Math.min(1, t));
    }

    // Asteroids: slow spin
    for (const m of asteroidMeshes) {
        if (!m) continue;
        const s = m.userData.spin;
        m.rotation.x += s.x * dt;
        m.rotation.y += s.y * dt;
        m.rotation.z += s.z * dt;
    }

    // Power-ups: spin + bob
    for (const [, pu] of powerups) {
        pu.mesh.userData.inner.rotation.y += dt * 1.6;
        pu.mesh.userData.inner.rotation.x += dt * 0.5;
        pu.mesh.position.y = pu.basePos.y + Math.sin(now * 0.002 + pu.basePos.x) * 0.4;
    }

    // Auto-expire timed buffs (visual cue only — server doesn't track these).
    if (buffs.rapidUntil  && now > buffs.rapidUntil)  buffs.rapidUntil  = 0;
    if (buffs.tripleUntil && now > buffs.tripleUntil) buffs.tripleUntil = 0;

    // Lasers
    for (let i = lasers.length - 1; i >= 0; i--) {
        const l = lasers[i];
        l.mesh.position.addScaledVector(l.dir, LASER_SPEED * dt);
        l.life -= dt;
        if (l.life <= 0) {
            laserGroup.remove(l.mesh);
            l.mesh.material.dispose();
            lasers.splice(i, 1);
        }
    }

    // Explosions
    for (let i = explosions.length - 1; i >= 0; i--) {
        const e = explosions[i];
        const age = (now - e.t0) / 1000;
        const k   = age / e.life;
        if (k >= 1) {
            explosionGroup.remove(e.mesh);
            e.mat.dispose();
            if (e.mesh.geometry) e.mesh.geometry.dispose();
            explosions.splice(i, 1);
            continue;
        }
        const s = THREE.MathUtils.lerp(0.6, e.end, k);
        if (e.isSprite) e.mesh.scale.set(s, s, 1);
        else            e.mesh.scale.setScalar(s);
        e.mat.opacity = Math.max(0, 1 - k);
    }

    // Camera: chase cam behind/above ship
    _cam.set(0, 1.5, -7).applyQuaternion(me.quat).add(me.pos);
    camera.position.copy(_cam);
    _vZ.set(0, 0, 5).applyQuaternion(me.quat).add(me.pos);
    camera.up.set(0, 1, 0).applyQuaternion(me.quat);
    camera.lookAt(_vZ);

    // Send state @ ~15 Hz
    if (now - lastSend > 66 && STATE.connected) {
        lastSend = now;
        send({
            t: 's',
            x:  me.pos.x, y:  me.pos.y, z:  me.pos.z,
            qx: me.quat.x, qy: me.quat.y, qz: me.quat.z, qw: me.quat.w,
        });
    }

    // HUD
    hpEl.textContent    = 'HP ' + Math.max(0, me.hp|0);
    scoreEl.textContent = me.score | 0;
    clearCenterIfDue();
}

// ---------- HUD overlay (2D Canvas) ----------
const hud2d = document.getElementById('hud2d');
const hctx  = hud2d.getContext('2d');
let HW = 0, HH = 0, HDPR = 1;
function resizeHud() {
    HDPR = Math.min(window.devicePixelRatio || 1, 1.5);
    HW = window.innerWidth;
    HH = window.innerHeight;
    hud2d.width  = HW * HDPR;
    hud2d.height = HH * HDPR;
    hud2d.style.width  = HW + 'px';
    hud2d.style.height = HH + 'px';
    hctx.setTransform(HDPR, 0, 0, HDPR, 0, 0);
}
window.addEventListener('resize', resizeHud);
resizeHud();

// Constant: same as LASER_SPEED in step() — used for lead computation.
const PROJECTILE_SPEED = 500;

// Re-used temporaries (avoid GC pressure each frame).
const _camDir = new THREE.Vector3();
const _toE    = new THREE.Vector3();
const _camLoc = new THREE.Vector3();
const _invQ   = new THREE.Quaternion();
const _proj   = new THREE.Vector3();
const _leadV  = new THREE.Vector3();

function projectVisible(worldPos) {
    // Returns {sx, sy, visible} for a world point. visible = in front of camera
    // AND within the viewport rectangle.
    _proj.copy(worldPos).project(camera);
    camera.getWorldDirection(_camDir);
    _toE.copy(worldPos).sub(camera.position);
    const inFront = _toE.dot(_camDir) > 0;
    const sx = (_proj.x + 1) * 0.5 * HW;
    const sy = (1 - _proj.y) * 0.5 * HH;
    const visible = inFront && _proj.x >= -1 && _proj.x <= 1 && _proj.y >= -1 && _proj.y <= 1;
    return { sx, sy, visible, inFront };
}

function cameraLocalAngle(worldPos) {
    // Angle in the screen plane from screen center toward worldPos, ignoring
    // distance. Works for points behind the camera as well.
    _camLoc.copy(worldPos).sub(camera.position);
    _invQ.copy(camera.quaternion).invert();
    _camLoc.applyQuaternion(_invQ);
    // In camera-local, +x is right, +y is up, +z is BEHIND (Three.js convention).
    return Math.atan2(-_camLoc.y, _camLoc.x);
}

// Compute lead position where to aim to hit a moving target with a constant-
// speed projectile fired from `shooter`. Returns null if no positive intercept.
function computeLead(shooterPos, targetPos, targetVel, projSpeed) {
    const dx = targetPos.x - shooterPos.x;
    const dy = targetPos.y - shooterPos.y;
    const dz = targetPos.z - shooterPos.z;
    const vx = targetVel.x, vy = targetVel.y, vz = targetVel.z;
    const a = vx*vx + vy*vy + vz*vz - projSpeed*projSpeed;
    const b = 2 * (dx*vx + dy*vy + dz*vz);
    const c = dx*dx + dy*dy + dz*dz;

    let t;
    if (Math.abs(a) < 1e-4) {
        // Almost-zero relative speed - fall back to linear.
        if (Math.abs(b) < 1e-4) return null;
        t = -c / b;
    } else {
        const disc = b*b - 4*a*c;
        if (disc < 0) return null;
        const root = Math.sqrt(disc);
        const t1 = (-b - root) / (2*a);
        const t2 = (-b + root) / (2*a);
        // smallest positive
        t = Number.POSITIVE_INFINITY;
        if (t1 > 0 && t1 < t) t = t1;
        if (t2 > 0 && t2 < t) t = t2;
        if (!isFinite(t)) return null;
    }
    if (t <= 0 || t > 4) return null;       // ignore far-future intercepts
    _leadV.set(targetPos.x + vx*t,
               targetPos.y + vy*t,
               targetPos.z + vz*t);
    return _leadV;
}

function drawCrosshair() {
    const cx = HW * 0.5, cy = HH * 0.5;
    hctx.strokeStyle = 'rgba(120, 220, 255, 0.65)';
    hctx.lineWidth = 1.5;
    hctx.beginPath();
    hctx.arc(cx, cy, 14, 0, Math.PI * 2);
    hctx.stroke();
    hctx.beginPath();
    hctx.moveTo(cx - 26, cy); hctx.lineTo(cx - 8,  cy);
    hctx.moveTo(cx + 26, cy); hctx.lineTo(cx + 8,  cy);
    hctx.moveTo(cx, cy - 26); hctx.lineTo(cx, cy - 8);
    hctx.moveTo(cx, cy + 26); hctx.lineTo(cx, cy + 8);
    hctx.stroke();
    // tiny center dot
    hctx.fillStyle = 'rgba(180, 240, 255, 0.9)';
    hctx.fillRect(cx - 1, cy - 1, 2, 2);
}

function drawRadar() {
    // Top-right radar disc. Forward (ship +Z) is UP on the radar.
    const r = Math.min(HW, HH) * 0.11;
    const cx = HW - r - 14;
    const cy = r + 14;
    const range = 200;   // world units mapped to radar radius

    // Background ring
    hctx.fillStyle   = 'rgba(0, 12, 24, 0.55)';
    hctx.strokeStyle = 'rgba(120, 220, 255, 0.45)';
    hctx.lineWidth = 1.5;
    hctx.beginPath(); hctx.arc(cx, cy, r, 0, Math.PI*2); hctx.fill(); hctx.stroke();
    // Crosshair lines
    hctx.strokeStyle = 'rgba(120, 220, 255, 0.20)';
    hctx.beginPath();
    hctx.moveTo(cx - r, cy); hctx.lineTo(cx + r, cy);
    hctx.moveTo(cx, cy - r); hctx.lineTo(cx, cy + r);
    hctx.stroke();
    // Forward indicator (player nose direction)
    hctx.fillStyle = 'rgba(120, 220, 255, 0.85)';
    hctx.beginPath();
    hctx.moveTo(cx, cy - 5);
    hctx.lineTo(cx - 4, cy + 4);
    hctx.lineTo(cx + 4, cy + 4);
    hctx.closePath();
    hctx.fill();

    _invQ.copy(me.quat).invert();
    for (const [, o] of otherShips) {
        if (o.dead) continue;
        _camLoc.copy(o.mesh.position).sub(me.pos).applyQuaternion(_invQ);
        const lx = _camLoc.x;        // local right
        const lz = _camLoc.z;        // local forward (positive = ahead)
        const dist = Math.hypot(lx, lz);
        // Forward in world becomes "up" on radar.
        let rx = (lx / range) * r;
        let ry = (-lz / range) * r;
        const m = Math.hypot(rx, ry);
        const clipped = m > r - 4;
        if (clipped) {
            rx = rx / m * (r - 4);
            ry = ry / m * (r - 4);
        }
        hctx.fillStyle = clipped ? '#ff8866' : '#ff4040';
        hctx.beginPath();
        hctx.arc(cx + rx, cy + ry, clipped ? 3.5 : 4, 0, Math.PI*2);
        hctx.fill();
    }

    // Power-ups in range (small colored dots).
    for (const [, pu] of powerups) {
        _camLoc.copy(pu.mesh.position).sub(me.pos).applyQuaternion(_invQ);
        const lx = _camLoc.x, lz = _camLoc.z;
        let rx = (lx / range) * r;
        let ry = (-lz / range) * r;
        const m = Math.hypot(rx, ry);
        if (m > r - 4) { rx = rx / m * (r - 4); ry = ry / m * (r - 4); }
        hctx.fillStyle = '#' + PU_COLORS[pu.type].toString(16).padStart(6, '0');
        hctx.fillRect(cx + rx - 2, cy + ry - 2, 4, 4);
    }
}

function drawEnemyArrowsAndLeads() {
    const now = performance.now();
    const cx = HW * 0.5, cy = HH * 0.5;
    const edgeR = Math.min(HW, HH) * 0.42;   // arrow distance from screen center

    for (const [, o] of otherShips) {
        if (o.dead) continue;

        // Estimate enemy velocity from interp window.
        const dt = Math.max(1, o.t1 - o.t0) / 1000;
        const vel = _leadV.set(
            (o.posTarget.x - o.posPrev.x) / dt,
            (o.posTarget.y - o.posPrev.y) / dt,
            (o.posTarget.z - o.posPrev.z) / dt);

        // Use the visually interpolated position (matches the mesh).
        const epos = o.mesh.position;

        // On-screen check first.
        const info = projectVisible(epos);

        if (info.visible) {
            // Direct marker around the enemy.
            hctx.strokeStyle = 'rgba(255, 80, 80, 0.85)';
            hctx.lineWidth = 1.5;
            hctx.beginPath();
            hctx.arc(info.sx, info.sy, 14, 0, Math.PI*2);
            hctx.stroke();
            // Distance text
            const d = me.pos.distanceTo(epos) | 0;
            hctx.fillStyle = 'rgba(255, 200, 200, 0.85)';
            hctx.font = '11px ui-monospace, Menlo, Consolas, monospace';
            hctx.fillText(d + 'm', info.sx + 18, info.sy + 4);

            // Lead reticle.
            const lead = computeLead(me.pos, epos, vel, PROJECTILE_SPEED);
            if (lead) {
                const lp = projectVisible(lead);
                if (lp.inFront) {
                    hctx.strokeStyle = 'rgba(120, 255, 120, 0.95)';
                    hctx.lineWidth = 2;
                    hctx.beginPath();
                    hctx.moveTo(lp.sx - 9, lp.sy); hctx.lineTo(lp.sx - 3, lp.sy);
                    hctx.moveTo(lp.sx + 3, lp.sy); hctx.lineTo(lp.sx + 9, lp.sy);
                    hctx.moveTo(lp.sx, lp.sy - 9); hctx.lineTo(lp.sx, lp.sy - 3);
                    hctx.moveTo(lp.sx, lp.sy + 3); hctx.lineTo(lp.sx, lp.sy + 9);
                    hctx.stroke();
                    // Thin guide line from enemy to lead point.
                    hctx.strokeStyle = 'rgba(120, 255, 120, 0.30)';
                    hctx.lineWidth = 1;
                    hctx.beginPath();
                    hctx.moveTo(info.sx, info.sy);
                    hctx.lineTo(lp.sx, lp.sy);
                    hctx.stroke();
                }
            }
        } else {
            // Off-screen arrow at the viewport edge.
            const ang = cameraLocalAngle(epos);
            const ax  = cx + Math.cos(ang) * edgeR;
            const ay  = cy + Math.sin(ang) * edgeR;
            hctx.save();
            hctx.translate(ax, ay);
            hctx.rotate(ang);
            hctx.fillStyle = 'rgba(255, 90, 90, 0.95)';
            hctx.beginPath();
            hctx.moveTo(12, 0);
            hctx.lineTo(-6, -7);
            hctx.lineTo(-3, 0);
            hctx.lineTo(-6, 7);
            hctx.closePath();
            hctx.fill();
            hctx.restore();
        }
    }
}

function drawPowerupArrows() {
    const cx = HW * 0.5, cy = HH * 0.5;
    const edgeR = Math.min(HW, HH) * 0.40;   // slightly inside enemy arrows

    for (const [, pu] of powerups) {
        const info = projectVisible(pu.mesh.position);
        if (info.visible) {
            // Faint marker around on-screen power-ups so they're easy to spot.
            const color = PU_COLORS[pu.type];
            hctx.strokeStyle = 'rgba(' +
                ((color>>16)&0xff) + ',' +
                ((color>>8)&0xff)  + ',' +
                (color&0xff)       + ',0.65)';
            hctx.lineWidth = 1.5;
            hctx.beginPath();
            hctx.arc(info.sx, info.sy, 10, 0, Math.PI*2);
            hctx.stroke();
            continue;
        }

        const ang = cameraLocalAngle(pu.mesh.position);
        const ax  = cx + Math.cos(ang) * edgeR;
        const ay  = cy + Math.sin(ang) * edgeR;
        const color = PU_COLORS[pu.type];
        const hex = '#' + color.toString(16).padStart(6, '0');

        hctx.save();
        hctx.translate(ax, ay);
        hctx.rotate(ang);
        hctx.fillStyle = hex;
        hctx.beginPath();
        hctx.moveTo(11, 0);
        hctx.lineTo(-5, -6);
        hctx.lineTo(-2, 0);
        hctx.lineTo(-5, 6);
        hctx.closePath();
        hctx.fill();
        // Small letter for the type.
        hctx.rotate(-ang);
        hctx.fillStyle = '#000';
        hctx.font = 'bold 9px ui-monospace, Menlo, Consolas, monospace';
        hctx.textAlign = 'center';
        hctx.textBaseline = 'middle';
        hctx.fillText(PU_NAMES[pu.type][0], 0, 1);
        hctx.textAlign = 'start';
        hctx.textBaseline = 'alphabetic';
        hctx.restore();
    }
}

function drawBuffBar() {
    const now = performance.now();
    const items = [];
    if (buffs.shieldActive)         items.push(['SHIELD', '#44ccff', 1]);
    if (now < buffs.rapidUntil)     items.push(['RAPID',  '#ffe744',
                                                 (buffs.rapidUntil  - now) / 10000]);
    if (now < buffs.tripleUntil)    items.push(['TRIPLE', '#ff7744',
                                                 (buffs.tripleUntil - now) / 10000]);
    if (items.length === 0) return;

    const x0 = 8, y0 = HH - 8 - items.length * 18;
    hctx.font = '11px ui-monospace, Menlo, Consolas, monospace';
    for (let i = 0; i < items.length; i++) {
        const [name, color, frac] = items[i];
        const y = y0 + i * 18;
        hctx.fillStyle = 'rgba(0,0,0,0.45)';
        hctx.fillRect(x0, y, 120, 14);
        hctx.fillStyle = color;
        hctx.fillRect(x0, y, 120 * Math.max(0, Math.min(1, frac)), 14);
        hctx.fillStyle = '#000';
        hctx.fillText(name, x0 + 6, y + 11);
    }
}

function drawHUD() {
    hctx.clearRect(0, 0, HW, HH);
    if (me.dead) return;
    drawCrosshair();
    drawEnemyArrowsAndLeads();
    drawPowerupArrows();
    drawRadar();
    drawBuffBar();
}

function frame() {
    const now = performance.now();
    let dt = (now - lastT) / 1000;
    if (dt > 0.1) dt = 0.1;
    lastT = now;
    step(dt);
    renderer.render(scene, camera);
    drawHUD();
    requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

// ---------- join ----------
joinBtn.addEventListener('click', () => {
    const name = (nameEl.value || 'PILOT').trim().slice(0, 16) || 'PILOT';
    if (nameEl.value) localStorage.setItem('sf3d_name', nameEl.value);
    connect(name);
});
nameEl.addEventListener('keydown', e => {
    if (e.key === 'Enter') joinBtn.click();
});
const saved = localStorage.getItem('sf3d_name');
if (saved) nameEl.value = saved;

})();
