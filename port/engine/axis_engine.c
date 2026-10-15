/* AXIS engine harness — the first slice of the approach-B platform shim: a real
 * Emscripten program (not the standalone decoder reactor) that opens a WebGL
 * (GLES2) context and renders actual AXIS mesh geometry decoded by the verified
 * reconstruction (src/axis_shp.c), driven by an emscripten main loop.
 *
 * Game data is NOT bundled: the page fetches the user's own Data/Shape.dat and
 * hands the bytes to engine_load_mesh(), which finds a record in the archive
 * TOC, decodes it with axis_shp_parse, and uploads it to a GL vertex buffer.
 * This is reconstruction-order step 1 (bootstrap + GL context + real geometry);
 * textures, maps, animation and the simulation build on top from here.
 *
 *   build: see ../build_engine.sh  (emcc, FULL_ES2, links src/axis_shp.c)
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES2/gl2.h>

/* from src/axis_shp.c / src/axis_ptx.c (linked in) */
int axis_shp_parse(const uint8_t *buf, uint32_t off, uint32_t end,
                   float *posOut, float *nrmOut, float *uvOut,
                   uint32_t *triOut, uint32_t *grpOut, uint32_t *counts);
int axis_ptx_decode(const uint8_t *ptx, uint32_t ptxLen, const uint8_t *exe, uint32_t exeLen,
                    uint8_t *rgbaOut, uint32_t *wh);

static GLuint prog, vboPos, vboNrm, vboUV, ibo, texId;
static int indexCount = 0, hasUV = 0, hasTex = 0;
static uint8_t RGBABUF[1024 * 1024 * 4];   /* decoded texture scratch */

/* remote-player avatars (multiplayer) drawn as unit cubes at world positions */
static GLuint vboCube, vboCubeN, iboCube;
/* an optional real AXIS mesh used to draw the demo targets as actual game models
 * instead of placeholder cubes (loaded by engine_load_entity_mesh). */
static GLuint vboEnt, vboEntN, vboEntUV, iboEnt, texEnt;
static int entTri = 0, entHasTex = 0;
static float entCenter[3] = {0,0,0}, entRadius = 1.0f;
#define MAXAV 16
static float avatars[MAXAV * 3];
static int avCount = 0;

/* reconstructed simulation (port/sim/) wired into the engine: run the verified
 * projectile combat sim each frame and render the live projectiles. */
typedef struct { float pos[3], vel[3]; int owner, target; float age, lifetime, speed, turnRate; } axis_proj_t;
int axis_sim_tick(axis_proj_t *projs, int *palive, int nProj,
                  const float *actors, const int *aalive, int nActors,
                  float *forces, float radius, float mag, float maxRange, float dt);
#define MAXPROJ 32
#define MAXTGT 8
static axis_proj_t gProjs[MAXPROJ];
static int gProjAlive[MAXPROJ], gProjN = 0;
static float gTgt[MAXTGT*3], gTgtForce[MAXTGT*3];
static int gTgtAlive[MAXTGT], gTgtN = 0, projDemo = 0;

/* the verified effect/debris layer (port/sim/axis_effect.c) wired in too: a
 * projectile impact spawns a burst of gravity debris that arcs down and expires. */
typedef struct { float pos[3], vel[3]; float age, lifetime, trailTimer; } axis_debris_t;
int axis_debris_step(axis_debris_t *d, float dt,
                     int (*collide)(const float*, float*, float*, void*), void*,
                     unsigned (*draw_pool)(void*), void*,
                     void (*trail)(int, unsigned, void*), void*);
#define MAXDEBRIS 128
static axis_debris_t gDebris[MAXDEBRIS];
static int gDebrisAlive[MAXDEBRIS], gDebrisN = 0;

/* the verified collision layers (axis_broadphase.c broad phase, axis_collide.c
 * narrow phase) run against the map's real merged triangle soup (POS/TRI), so a
 * projectile is consumed when it strikes map geometry, not only a target. */
int axis_seg_aabb(const float *a, const float *b, const float *bmin, const float *bmax, float *tEnter);
int axis_seg_tri(const float *p0, const float *p1, const float *a, const float *b,
                 const float *c, float *outT, float *hit);
int axis_seg_sphere(const float *p0, const float *p1, const float *centre, float radius, float *outT);
static int gScore = 0;   /* targets destroyed by player shots */
static float gMapMin[3] = {0,0,0}, gMapMax[3] = {0,0,0};
static float gProjPrev[MAXPROJ][3];

/* the verified animated-prop layer (axis_prop.c): a proximity gate that opens
 * (phase 0->1, rises) while a projectile is within range and closes otherwise. */
float axis_anim_phase_step(float phase, float *rate, int mode, float dt, int armed, int *callback);
int   axis_prop_inside(float dist2, int rangeSteps);
float axis_prop_nearest_dist2(const float *propPos, const float *actors, const int *active, int n);
static float gPropPos[3] = {0,0,0}, gPropPhase = 0.0f, gPropRate = 0.0f;
static int gPropRange = 0;

/* the verified item-pickup layer (axis_pickup.c): collectible items a projectile
 * (acting as the collecting actor) picks up when it passes within range. */
int axis_pickup_in_range(const float *itemPos, const float *actorPos, float itemRadius);
#define MAXITEMS 12
static float gItemPos[MAXITEMS*3];
static int gItemAlive[MAXITEMS], gItemN = 0, gPickupCount = 0;
static int gImpactCount = 0;   /* monotonic debris/impact bursts, for SFX polling */

/* the verified client input layer (axis_input.c): the engine's key edges feed
 * 16 action channels; channel 0 ("fire", bound to F) is built into an
 * edge-triggered press mask each frame and flashes a marker above the gate. */
void axis_input_build_masks(const int *enabled, int *pressLatch, int *releaseLatch, int n,
                            uint32_t *pressMask, uint32_t *releaseMask);
int axis_input_mask_has(uint32_t mask, int ch);
static int gActEnabled[16] = {1}, gActPress[16] = {0}, gActRel[16] = {0};
/* player shots: a fire (channel 0) press launches a projectile from the camera
 * along its forward vector; it flies (const velocity), collides with the map via
 * the reconstructed broad+narrow phases, and bursts debris on impact. */
#define MAXSHOTS 16
static float gShotPos[MAXSHOTS*3], gShotPrev[MAXSHOTS*3], gShotVel[MAXSHOTS*3], gShotAge[MAXSHOTS];
static int gShotAlive[MAXSHOTS];
static unsigned gDbgSeed = 1u;   /* cheap deterministic spread for debris velocities */
static float dbg_rand(void) { gDbgSeed = gDbgSeed * 1103515245u + 12345u; return (float)((gDbgSeed >> 16) & 0x7fff) / 32767.0f; }

/* map mode: per-object triangle ranges + each object's shape, so every placed
 * object can be drawn with its own SHAPE\<name> skin (texForShape, keyed by the
 * Shape.dat record index) where one exists, flat otherwise. */
static int mapMode = 0;
#define MAXOBJ 4096
static uint32_t objTriStart[MAXOBJ], objTriCount[MAXOBJ];
static int objShape[MAXOBJ], objCount = 0;
static float gCenter[3] = {0, 0, 0}, gRadius = 1.0f;

/* free-fly / first-person camera */
static float camPos[3] = {0, 0, 0}, camYaw = 0.0f, camPitch = 0.0f, camSpeed = 1.0f;
static int camReady = 0;
static unsigned char keyDown[320];
static double lastT = 0.0;

static void camForward(float *f) {
    float cp = cosf(camPitch), sp = sinf(camPitch), sy = sinf(camYaw), cy = cosf(camYaw);
    f[0] = cp * sy; f[1] = sp; f[2] = -cp * cy;
}
static void camInit(void) {
    float f[3] = {0, 0, -1};
    camYaw = 0.0f; camPitch = -0.35f; camForward(f);
    float d = gRadius * 2.4f;
    camPos[0] = gCenter[0] - f[0] * d;
    camPos[1] = gCenter[1] - f[1] * d;
    camPos[2] = gCenter[2] - f[2] * d;
    camSpeed = gRadius * 1.2f;
    camReady = 1;
}

/* merged scene buffers (one mesh, or a whole map's placed objects) */
#define VCAP 900000u
#define TCAP 1800000u
static float POS[VCAP * 3], NRM[VCAP * 3], UVt[VCAP * 2];
static uint32_t TRI[TCAP * 3], GRP[8192 * 2], CNT[4];

/* per-object scratch while merging a map (a single mesh is small) */
#define TVCAP 120000u
#define TTCAP 240000u
static float TPOS[TVCAP * 3], TNRM[TVCAP * 3], TUV[TVCAP * 2];
static uint32_t TTRI[TTCAP * 3], TGRP[8192 * 2], TCNT[4];

/* Shape.dat TOC, with each name normalized the way web/map.html matches them:
 * '\\' and '/' -> '_', ASCII upper -> lower (cp949 high bytes untouched). */
#define MAXTOC 4096
static uint8_t TOCN[MAXTOC][0x40];
static int TOCLEN[MAXTOC];
static uint32_t TOCOFF[MAXTOC], TOCEND[MAXTOC];
static int tocCount = 0;
static GLuint texForShape[MAXTOC];   /* per-map-object skin, keyed by Shape.dat index (0 = none) */

static int norm_name(const uint8_t *s, int max, uint8_t *out) {
    int n = 0; while (n < max && s[n]) n++;
    for (int i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '\\' || c == '/') c = '_';
        else if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);
        out[i] = c;
    }
    return n;
}

static const char *VS =
    "attribute vec3 aPos; attribute vec3 aNrm; attribute vec2 aUV;"
    "uniform mat4 uMVP; uniform mat4 uModel; varying vec3 vN; varying vec2 vUV;"
    "void main(){ vN = mat3(uModel)*aNrm; vUV = aUV; gl_Position = uMVP*vec4(aPos,1.0); }";
static const char *FS =
    "precision mediump float; varying vec3 vN; varying vec2 vUV;"
    "uniform sampler2D uTex; uniform int uHasTex; uniform vec3 uColor;"
    "void main(){ vec3 n=normalize(vN); float d=max(dot(n,normalize(vec3(0.4,0.8,0.6))),0.0);"
    "float l=0.45+0.55*d;"
    "vec3 c = (uHasTex==1) ? texture2D(uTex, vUV).rgb*l : uColor*l;"
    "gl_FragColor=vec4(c,1.0); }";

static GLuint compile(GLenum t, const char *src) {
    GLuint s = glCreateShader(t); glShaderSource(s, 1, &src, 0); glCompileShader(s);
    return s;
}

/* 4x4 column-major helpers */
static void mat_mul(float *r, const float *a, const float *b) {
    float t[16];
    for (int c = 0; c < 4; c++) for (int r2 = 0; r2 < 4; r2++)
        t[c*4+r2] = a[0*4+r2]*b[c*4+0] + a[1*4+r2]*b[c*4+1] + a[2*4+r2]*b[c*4+2] + a[3*4+r2]*b[c*4+3];
    memcpy(r, t, sizeof t);
}
static void mat_persp(float *m, float fovy, float asp, float zn, float zf) {
    float f = 1.0f / tanf(fovy * 0.5f);
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / asp; m[5] = f; m[10] = (zf + zn) / (zn - zf); m[11] = -1.0f; m[14] = (2 * zf * zn) / (zn - zf);
}

/* exported to JS: allocate a byte buffer the page fills with the fetched file */
EMSCRIPTEN_KEEPALIVE uint8_t *engine_alloc(int n) { return (uint8_t *)malloc(n); }
EMSCRIPTEN_KEEPALIVE void engine_free(uint8_t *p) { free(p); }

/* decode record `index` of a Shape.dat archive buffer and upload it to GL */
EMSCRIPTEN_KEEPALIVE int engine_load_mesh(const uint8_t *dat, uint32_t len, uint32_t index) {
    uint32_t count; memcpy(&count, dat, 4);
    if (index >= count) return -1;
    uint32_t off; memcpy(&off, dat + 4 + index * 0x44 + 0x40, 4);
    uint32_t end = len;
    if (index + 1 < count) memcpy(&end, dat + 4 + (index + 1) * 0x44 + 0x40, 4);
    if (axis_shp_parse(dat, off, end, POS, NRM, UVt, TRI, GRP, CNT) != 0) return -2;
    uint32_t vc = CNT[0], tc = CNT[1];
    if (vc == 0 || tc == 0) return -3;

    /* bounds for an auto-framing camera */
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < vc; i++) for (int k = 0; k < 3; k++) {
        float v = POS[i*3+k]; if (v < lo[k]) lo[k] = v; if (v > hi[k]) hi[k] = v;
    }
    gRadius = 0.0001f;
    for (int k = 0; k < 3; k++) { gCenter[k] = 0.5f*(lo[k]+hi[k]); float h = 0.5f*(hi[k]-lo[k]); if (h > gRadius) gRadius = h; }

    glBindBuffer(GL_ARRAY_BUFFER, vboPos); glBufferData(GL_ARRAY_BUFFER, vc*3*sizeof(float), POS, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboNrm); glBufferData(GL_ARRAY_BUFFER, vc*3*sizeof(float), NRM, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboUV);  glBufferData(GL_ARRAY_BUFFER, vc*2*sizeof(float), UVt, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, tc*3*sizeof(uint32_t), TRI, GL_STATIC_DRAW);
    indexCount = (int)(tc * 3);
    hasUV = 1; hasTex = 0; mapMode = 0;   /* texture set separately via engine_set_texture */
    camInit();
    return (int)CNT[2]; /* group count, as a success signal */
}

/* Decode a PTX texture blob (the shape's SHAPE\<name> skin, located by the page)
 * with the verified axis_ptx.c and bind it as the mesh's GL texture. ptxLen==0
 * clears the texture (flat shading). exe is the user's AXIS.EXE (Huffman tables). */
EMSCRIPTEN_KEEPALIVE int engine_set_texture(const uint8_t *exe, uint32_t exeLen, const uint8_t *ptx, uint32_t ptxLen) {
    if (ptxLen == 0) { hasTex = 0; return 0; }
    uint32_t wh[2];
    if (axis_ptx_decode(ptx, ptxLen, exe, exeLen, RGBABUF, wh) != 0) { hasTex = 0; return -1; }
    if (!texId) glGenTextures(1, &texId);
    glBindTexture(GL_TEXTURE_2D, texId);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)wh[0], (GLsizei)wh[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, RGBABUF);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    hasTex = 1;
    return (int)(wh[0] << 16 | wh[1]);
}

/* multiplayer: set the remote players' world positions (xyz triples, up to 16).
 * The page decodes them from the relay with the real type-0 wire codec. */
EMSCRIPTEN_KEEPALIVE void engine_set_avatars(const float *xyz, int n) {
    if (n > MAXAV) n = MAXAV; if (n < 0) n = 0;
    for (int i = 0; i < n * 3; i++) avatars[i] = xyz[i];
    avCount = n;
}

/* spawn a burst of gravity debris (the verified axis_effect debris layer) at a
 * projectile impact point; each piece gets an upward+outward kick and arcs down. */
static void spawn_debris(const float *at) {
    gImpactCount++;              /* one impact burst — the page plays an explosion SFX per increment */
    for (int k = 0; k < 10; k++) {
        int slot = -1;
        for (int i = 0; i < MAXDEBRIS; i++) { int j = (gDebrisN + i) % MAXDEBRIS; if (!gDebrisAlive[j]) { slot = j; break; } }
        if (slot < 0) return;
        gDebrisN = (slot + 1) % MAXDEBRIS;
        axis_debris_t *d = &gDebris[slot];
        d->pos[0] = at[0]; d->pos[1] = at[1]; d->pos[2] = at[2];
        float sp = gRadius * 0.6f;
        d->vel[0] = (dbg_rand() - 0.5f) * sp;
        d->vel[1] = gRadius * 0.4f + dbg_rand() * gRadius * 0.4f;   /* upward kick */
        d->vel[2] = (dbg_rand() - 0.5f) * sp;
        d->age = 0.0f; d->lifetime = 1.2f; d->trailTimer = 0.1f;
        gDebrisAlive[slot] = 1;
    }
}

/* Query the reconstructed collision pipeline for a projectile's move segment vs
 * the map's real geometry: broad phase (segment vs whole-map AABB) then narrow
 * phase (nearest segment/triangle over the merged soup). Writes the nearest hit
 * point and returns 1 on a strike. */
static int proj_hit_map(const float *prev, const float *cur, float *outHit) {
    if (indexCount <= 0) return 0;
    float te;
    if (!axis_seg_aabb(prev, cur, gMapMin, gMapMax, &te)) return 0;   /* broad phase */
    int nt = indexCount / 3, found = 0; float bestT = 2.0f, t, h[3];
    for (int i = 0; i < nt; i++) {                                    /* narrow phase */
        const float *a = &POS[TRI[i*3+0]*3];
        const float *b = &POS[TRI[i*3+1]*3];
        const float *c = &POS[TRI[i*3+2]*3];
        if (axis_seg_tri(prev, cur, a, b, c, &t, h) && t < bestT) {
            bestT = t; outHit[0] = h[0]; outHit[1] = h[1]; outHit[2] = h[2]; found = 1;
        }
    }
    return found;
}

/* Trigger a fire action programmatically (same latch the F key sets); the frame
 * loop runs it through axis_input_build_masks like any input. Lets the page fire
 * without a synthetic keypress (used by ?fire and for automated verification). */
EMSCRIPTEN_KEEPALIVE void engine_fire(void) { gActPress[0] = 1; }

/* HUD state: how many pickups the reconstructed axis_pickup layer has collected. */
EMSCRIPTEN_KEEPALIVE int engine_pickups(void) { return gPickupCount; }

/* monotonic impact-burst count, polled by the page to play an explosion SFX. */
EMSCRIPTEN_KEEPALIVE int engine_impacts(void) { return gImpactCount; }

/* targets destroyed by player shots (HUD score). */
EMSCRIPTEN_KEEPALIVE int engine_score(void) { return gScore; }

/* Decode one Shape.dat record into a dedicated entity-mesh buffer, so the demo
 * can draw its targets as a real AXIS model instead of a cube. Stores the mesh
 * centre/radius so it can be scaled to a consistent on-screen size. */
EMSCRIPTEN_KEEPALIVE int engine_load_entity_mesh(const uint8_t *dat, uint32_t len, uint32_t index) {
    uint32_t count; memcpy(&count, dat, 4);
    if (index >= count) return -1;
    uint32_t off; memcpy(&off, dat + 4 + index * 0x44 + 0x40, 4);
    uint32_t end = len;
    if (index + 1 < count) memcpy(&end, dat + 4 + (index + 1) * 0x44 + 0x40, 4);
    if (axis_shp_parse(dat, off, end, TPOS, TNRM, TUV, TTRI, TGRP, TCNT) != 0) return -2;
    uint32_t vc = TCNT[0], tc = TCNT[1];
    if (vc == 0 || tc == 0) return -3;
    float lo[3] = {1e30f,1e30f,1e30f}, hi[3] = {-1e30f,-1e30f,-1e30f};
    for (uint32_t i = 0; i < vc; i++) for (int k = 0; k < 3; k++) { float v = TPOS[i*3+k]; if (v<lo[k]) lo[k]=v; if (v>hi[k]) hi[k]=v; }
    entRadius = 0.0001f;
    for (int k = 0; k < 3; k++) { entCenter[k] = 0.5f*(lo[k]+hi[k]); float h = 0.5f*(hi[k]-lo[k]); if (h > entRadius) entRadius = h; }
    if (!vboEnt) { glGenBuffers(1,&vboEnt); glGenBuffers(1,&vboEntN); glGenBuffers(1,&vboEntUV); glGenBuffers(1,&iboEnt); }
    glBindBuffer(GL_ARRAY_BUFFER, vboEnt);  glBufferData(GL_ARRAY_BUFFER, vc*3*sizeof(float), TPOS, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboEntN); glBufferData(GL_ARRAY_BUFFER, vc*3*sizeof(float), TNRM, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboEntUV); glBufferData(GL_ARRAY_BUFFER, vc*2*sizeof(float), TUV, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, iboEnt); glBufferData(GL_ELEMENT_ARRAY_BUFFER, tc*3*sizeof(uint32_t), TTRI, GL_STATIC_DRAW);
    entTri = (int)tc; entHasTex = 0;
    return (int)tc;
}

/* give the entity mesh its real SHAPE skin: decode the PTX (axis_ptx.c) and bind
 * it as the entity texture so the demo targets render textured, not flat. */
EMSCRIPTEN_KEEPALIVE int engine_set_entity_texture(const uint8_t *exe, uint32_t exeLen, const uint8_t *ptx, uint32_t ptxLen) {
    if (ptxLen == 0) { entHasTex = 0; return 0; }
    uint32_t wh[2];
    if (axis_ptx_decode(ptx, ptxLen, exe, exeLen, RGBABUF, wh) != 0) { entHasTex = 0; return -1; }
    if (!texEnt) glGenTextures(1, &texEnt);
    glBindTexture(GL_TEXTURE_2D, texEnt);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)wh[0], (GLsizei)wh[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, RGBABUF);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    entHasTex = 1;
    return (int)(wh[0] << 16 | wh[1]);
}

/* Demo: run the reconstructed projectile combat sim in the engine — place a ring
 * of targets around the loaded map's centre and fire homing projectiles at them
 * from above. The sim (axis_sim_tick) advances them each frame; the engine draws
 * the live projectiles and targets. Proves the verified sim runs in the port. */
EMSCRIPTEN_KEEPALIVE void engine_proj_demo(void) {
    float R = gRadius * 0.55f, spd = gRadius * 0.9f;
    gTgtN = MAXTGT; gProjN = MAXTGT;
    for (int i = 0; i < MAXTGT; i++) {
        float a = 6.2831853f * (float)i / MAXTGT;
        gTgt[i*3+0] = gCenter[0] + cosf(a) * R;
        gTgt[i*3+1] = gCenter[1];
        gTgt[i*3+2] = gCenter[2] + sinf(a) * R;
        gTgtAlive[i] = 1; gTgtForce[i*3]=gTgtForce[i*3+1]=gTgtForce[i*3+2]=0;
        /* a projectile launched from above the centre, aimed outward, homing on target i */
        gProjs[i].pos[0] = gCenter[0]; gProjs[i].pos[1] = gCenter[1] + R; gProjs[i].pos[2] = gCenter[2];
        gProjs[i].vel[0] = cosf(a)*spd; gProjs[i].vel[1] = 0; gProjs[i].vel[2] = sinf(a)*spd;
        gProjs[i].owner = -1; gProjs[i].target = i;
        gProjs[i].age = 0; gProjs[i].lifetime = 30.0f; gProjs[i].speed = spd; gProjs[i].turnRate = 2.5f;
        gProjAlive[i] = 1;
    }
    /* a proximity gate just outside the ring (clear of the central buildings),
       driven by the verified axis_prop layer; the nearest ring target is within
       its activation radius, so it opens and holds */
    gPropPos[0] = gCenter[0] + R * 1.25f; gPropPos[1] = gCenter[1]; gPropPos[2] = gCenter[2];
    gPropPhase = 0.0f; gPropRate = 0.0f;
    gPropRange = (int)(gRadius * 0.004f); if (gPropRange < 1) gPropRange = 1;  /* radius = range*100 */
    /* collectible items in a ring between centre and the targets (axis_pickup) */
    gItemN = MAXITEMS; gPickupCount = 0;
    for (int i = 0; i < MAXITEMS; i++) {
        float a = 6.2831853f * (float)i / MAXITEMS + 0.3f;
        gItemPos[i*3+0] = gCenter[0] + cosf(a) * R * 0.8f;
        gItemPos[i*3+1] = gCenter[1];
        gItemPos[i*3+2] = gCenter[2] + sinf(a) * R * 0.8f;
        gItemAlive[i] = 1;
    }
    for (int i = 0; i < MAXSHOTS; i++) gShotAlive[i] = 0;
    projDemo = 1;   /* gScore is cumulative across re-seeds (starts 0 at load) */
}

static inline float rf32e(const uint8_t *p) { float v; __builtin_memcpy(&v, p, 4); return v; }
static inline int finite_e(float v) { uint32_t b; __builtin_memcpy(&b, &v, 4); return (b & 0x7f800000u) != 0x7f800000u; }
static inline float absf_e(float v) { uint32_t b; __builtin_memcpy(&b, &v, 4); b &= 0x7fffffffu; float r; __builtin_memcpy(&r, &b, 4); return r; }

/* Load a whole map: decrypt the .bfd in place (SPEC §2.3/§2.6), and for each
 * placed object match its name to a Shape.dat record the way web/map.html does,
 * decode that mesh (axis_shp_parse), translate it to the object's world position,
 * and merge everything into one scene buffer. Returns the number of objects
 * placed (meshes found), or a negative error. */
EMSCRIPTEN_KEEPALIVE int engine_load_map(const uint8_t *dat, uint32_t datLen, uint8_t *bfd, uint32_t bfdLen) {
    /* build the normalized Shape.dat TOC */
    uint32_t sc; memcpy(&sc, dat, 4);
    tocCount = sc < MAXTOC ? (int)sc : MAXTOC;
    for (int i = 0; i < tocCount; i++) {
        uint32_t e = 4 + (uint32_t)i * 0x44;
        TOCLEN[i] = norm_name(dat + e, 0x40, TOCN[i]);
        memcpy(&TOCOFF[i], dat + e + 0x40, 4);
        TOCEND[i] = (i + 1 < (int)sc) ? ({ uint32_t o; memcpy(&o, dat + 4 + (uint32_t)(i + 1) * 0x44 + 0x40, 4); o; }) : datLen;
    }

    /* decrypt the .bfd (first 4 bytes plaintext version) */
    for (uint32_t i = 4; i < bfdLen; i++) { uint8_t x = (uint8_t)(bfd[i] ^ 0x59); bfd[i] = (uint8_t)((x << 1) | (x >> 7)); }
    uint32_t count = (bfdLen >= 12) ? ({ uint32_t c; memcpy(&c, bfd + 8, 4); c; }) : 0;

    for (int t = 0; t < tocCount; t++) texForShape[t] = 0;   /* new map: drop old skins */
    objCount = 0;
    uint32_t mergedV = 0, mergedT = 0; int placed = 0;
    uint8_t onm[0x88];
    for (uint32_t i = 0; i < count; i++) {
        uint32_t o = 0xC + i * 0xB0;
        if (o + 0xB0 > bfdLen) break;
        int on = norm_name(bfd + o, 0x88, onm);
        /* match against the TOC */
        int j = -1;
        for (int t = 0; t < tocCount; t++) {
            if (TOCLEN[t] != on) continue;
            int k = 0; for (; k < on; k++) if (TOCN[t][k] != onm[k]) break;
            if (k == on) { j = t; break; }
        }
        if (j < 0) continue;
        if (axis_shp_parse(dat, TOCOFF[j], TOCEND[j], TPOS, TNRM, TUV, TTRI, TGRP, TCNT) != 0) continue;
        uint32_t vc = TCNT[0], tc = TCNT[1];
        if (vc == 0 || tc == 0 || vc > TVCAP || tc > TTCAP) continue;
        if (mergedV + vc > VCAP || mergedT + tc > TCAP) break;   /* scene buffer full */

        float px = rf32e(bfd + o + 0x8c), py = rf32e(bfd + o + 0x90), pz = rf32e(bfd + o + 0x94);
        if (!(finite_e(px) && finite_e(py) && finite_e(pz)) || (double)absf_e(px) > 1e7) { px = py = pz = 0.0f; }

        for (uint32_t k = 0; k < vc; k++) {
            POS[(mergedV + k) * 3 + 0] = TPOS[k * 3 + 0] + px;
            POS[(mergedV + k) * 3 + 1] = TPOS[k * 3 + 1] + py;
            POS[(mergedV + k) * 3 + 2] = TPOS[k * 3 + 2] + pz;
            NRM[(mergedV + k) * 3 + 0] = TNRM[k * 3 + 0];
            NRM[(mergedV + k) * 3 + 1] = TNRM[k * 3 + 1];
            NRM[(mergedV + k) * 3 + 2] = TNRM[k * 3 + 2];
            UVt[(mergedV + k) * 2 + 0] = TUV[k * 2 + 0];
            UVt[(mergedV + k) * 2 + 1] = TUV[k * 2 + 1];
        }
        for (uint32_t k = 0; k < tc; k++) {
            TRI[(mergedT + k) * 3 + 0] = TTRI[k * 3 + 0] + mergedV;
            TRI[(mergedT + k) * 3 + 1] = TTRI[k * 3 + 1] + mergedV;
            TRI[(mergedT + k) * 3 + 2] = TTRI[k * 3 + 2] + mergedV;
        }
        if (objCount < MAXOBJ) { objTriStart[objCount] = mergedT; objTriCount[objCount] = tc; objShape[objCount] = j; objCount++; }
        mergedV += vc; mergedT += tc; placed++;
    }
    if (mergedV == 0) return -1;

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < mergedV; i++) for (int k = 0; k < 3; k++) { float v = POS[i*3+k]; if (v < lo[k]) lo[k] = v; if (v > hi[k]) hi[k] = v; }
    gRadius = 0.0001f;
    for (int k = 0; k < 3; k++) { gCenter[k] = 0.5f*(lo[k]+hi[k]); float h = 0.5f*(hi[k]-lo[k]); if (h > gRadius) gRadius = h; gMapMin[k] = lo[k]; gMapMax[k] = hi[k]; }

    glBindBuffer(GL_ARRAY_BUFFER, vboPos); glBufferData(GL_ARRAY_BUFFER, mergedV*3*sizeof(float), POS, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboNrm); glBufferData(GL_ARRAY_BUFFER, mergedV*3*sizeof(float), NRM, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboUV);  glBufferData(GL_ARRAY_BUFFER, mergedV*2*sizeof(float), UVt, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, mergedT*3*sizeof(uint32_t), TRI, GL_STATIC_DRAW);
    indexCount = (int)(mergedT * 3);
    hasUV = 1; hasTex = 0; mapMode = 1;   /* per-object skins applied via engine_map_set_texture */
    camInit();
    return placed;
}

/* Give a Shape.dat record (by index) its SHAPE\<name> skin for map rendering:
 * decode the PTX with axis_ptx.c and bind the resulting GL texture to that shape,
 * so every placed object of that shape draws textured. The page locates the PTX. */
EMSCRIPTEN_KEEPALIVE int engine_map_set_texture(int tocIndex, const uint8_t *exe, uint32_t exeLen, const uint8_t *ptx, uint32_t ptxLen) {
    if (tocIndex < 0 || tocIndex >= MAXTOC || ptxLen == 0) return -1;
    uint32_t wh[2];
    if (axis_ptx_decode(ptx, ptxLen, exe, exeLen, RGBABUF, wh) != 0) return -2;
    GLuint t; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)wh[0], (GLsizei)wh[1], 0, GL_RGBA, GL_UNSIGNED_BYTE, RGBABUF);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    texForShape[tocIndex] = t;
    return (int)(wh[0] << 16 | wh[1]);
}

static void mat_lookat(float *m, const float *eye, const float *ctr, const float *up) {
    float f[3] = {ctr[0]-eye[0], ctr[1]-eye[1], ctr[2]-eye[2]};
    float fl = sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); if (fl < 1e-6f) fl = 1e-6f;
    f[0]/=fl; f[1]/=fl; f[2]/=fl;
    float s[3] = {f[1]*up[2]-f[2]*up[1], f[2]*up[0]-f[0]*up[2], f[0]*up[1]-f[1]*up[0]};
    float sl = sqrtf(s[0]*s[0]+s[1]*s[1]+s[2]*s[2]); if (sl < 1e-6f) sl = 1e-6f;
    s[0]/=sl; s[1]/=sl; s[2]/=sl;
    float u[3] = {s[1]*f[2]-s[2]*f[1], s[2]*f[0]-s[0]*f[2], s[0]*f[1]-s[1]*f[0]};
    m[0]=s[0]; m[4]=s[1]; m[8]=s[2];  m[12]=-(s[0]*eye[0]+s[1]*eye[1]+s[2]*eye[2]);
    m[1]=u[0]; m[5]=u[1]; m[9]=u[2];  m[13]=-(u[0]*eye[0]+u[1]*eye[1]+u[2]*eye[2]);
    m[2]=-f[0]; m[6]=-f[1]; m[10]=-f[2]; m[14]=(f[0]*eye[0]+f[1]*eye[1]+f[2]*eye[2]);
    m[3]=0; m[7]=0; m[11]=0; m[15]=1;
}

static void frame(void) {
    double now = emscripten_get_now(); float dt = lastT ? (float)((now - lastT) / 1000.0) : 0.016f; lastT = now;
    if (dt > 0.1f) dt = 0.1f;
    if (camReady) {
        float look = 1.6f * dt;                              /* arrows / IJKL look */
        if (keyDown['J'] || keyDown[37]) camYaw -= look;     /* 37=Left 39=Right 38=Up 40=Down */
        if (keyDown['L'] || keyDown[39]) camYaw += look;
        if (keyDown['I'] || keyDown[38]) camPitch += look;
        if (keyDown['K'] || keyDown[40]) camPitch -= look;
        if (camPitch > 1.5f) camPitch = 1.5f; if (camPitch < -1.5f) camPitch = -1.5f;
        float f[3]; camForward(f);
        float wup[3] = {0,1,0};
        float r[3] = {f[1]*wup[2]-f[2]*wup[1], f[2]*wup[0]-f[0]*wup[2], f[0]*wup[1]-f[1]*wup[0]};
        float rl = sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]); if (rl<1e-6f) rl=1e-6f; r[0]/=rl; r[1]/=rl; r[2]/=rl;
        float v = camSpeed * dt * ((keyDown[16]) ? 3.0f : 1.0f);   /* Shift = sprint */
        float d[3] = {0,0,0};
        if (keyDown['W']) { d[0]+=f[0]*v; d[1]+=f[1]*v; d[2]+=f[2]*v; }
        if (keyDown['S']) { d[0]-=f[0]*v; d[1]-=f[1]*v; d[2]-=f[2]*v; }
        if (keyDown['D']) { d[0]+=r[0]*v; d[1]+=r[1]*v; d[2]+=r[2]*v; }
        if (keyDown['A']) { d[0]-=r[0]*v; d[1]-=r[1]*v; d[2]-=r[2]*v; }
        if (keyDown['E'] || keyDown[' ']) d[1]+=v;
        if (keyDown['Q']) d[1]-=v;
        /* resolve walking against the real map geometry via the reconstructed
           broad+narrow collision (proj_hit_map): slide per axis so a wall blocks
           only the component into it and you can't walk through buildings */
        if (mapMode && indexCount > 0) {
            float margin = gRadius * 0.03f, hp[3];
            for (int k = 0; k < 3; k++) {
                if (d[k] == 0.0f) continue;
                float to[3] = { camPos[0], camPos[1], camPos[2] };
                to[k] += d[k] + (d[k] > 0.0f ? margin : -margin);
                if (!proj_hit_map(camPos, to, hp)) camPos[k] += d[k];   /* free step, else blocked */
            }
        } else {
            camPos[0]+=d[0]; camPos[1]+=d[1]; camPos[2]+=d[2];
        }
    }
    int w, h; emscripten_get_canvas_element_size("#canvas", &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.07f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (indexCount > 0 && camReady) {
        float proj[16], view[16], mvp[16];
        mat_persp(proj, 1.1f, h ? (float)w/(float)h : 1.0f, gRadius*0.02f, gRadius*40.0f);
        float fwd[3]; camForward(fwd);
        float ctr[3] = {camPos[0]+fwd[0], camPos[1]+fwd[1], camPos[2]+fwd[2]}, up[3] = {0,1,0};
        mat_lookat(view, camPos, ctr, up);
        mat_mul(mvp, proj, view);
        float ident[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"), 1, GL_FALSE, ident);
        GLint aPos = glGetAttribLocation(prog, "aPos"), aNrm = glGetAttribLocation(prog, "aNrm"), aUV = glGetAttribLocation(prog, "aUV");
        glBindBuffer(GL_ARRAY_BUFFER, vboPos); glEnableVertexAttribArray(aPos); glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vboNrm); glEnableVertexAttribArray(aNrm); glVertexAttribPointer(aNrm, 3, GL_FLOAT, GL_FALSE, 0, 0);
        if (aUV >= 0) {
            if (hasUV) { glBindBuffer(GL_ARRAY_BUFFER, vboUV); glEnableVertexAttribArray(aUV); glVertexAttribPointer(aUV, 2, GL_FLOAT, GL_FALSE, 0, 0); }
            else glDisableVertexAttribArray(aUV);
        }
        GLint uHasTexL = glGetUniformLocation(prog, "uHasTex"), uTexL = glGetUniformLocation(prog, "uTex"), uColorL = glGetUniformLocation(prog, "uColor");
        glUniform3f(uColorL, 0.30f, 0.55f, 0.85f);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        if (mapMode) {
            /* draw each placed object with its own SHAPE skin where one exists */
            for (int o = 0; o < objCount; o++) {
                GLuint t = texForShape[objShape[o]];
                glUniform1i(uHasTexL, t ? 1 : 0);
                if (t) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t); glUniform1i(uTexL, 0); }
                glDrawElements(GL_TRIANGLES, (GLsizei)(objTriCount[o] * 3), GL_UNSIGNED_INT, (const void *)(uintptr_t)(objTriStart[o] * 3 * sizeof(uint32_t)));
            }
        } else {
            glUniform1i(uHasTexL, hasTex ? 1 : 0);
            if (hasTex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texId); glUniform1i(uTexL, 0); }
            glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
        }

        /* remote-player avatars: colored cubes at their world positions */
        if (avCount > 0) {
            float s = gRadius * 0.03f; if (s < 1.0f) s = 1.0f;
            glUniform1i(glGetUniformLocation(prog, "uHasTex"), 0);
            glUniform3f(glGetUniformLocation(prog, "uColor"), 0.95f, 0.55f, 0.15f);
            if (aUV >= 0) glDisableVertexAttribArray(aUV);
            glBindBuffer(GL_ARRAY_BUFFER, vboCube);  glEnableVertexAttribArray(aPos); glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 0, 0);
            glBindBuffer(GL_ARRAY_BUFFER, vboCubeN); glEnableVertexAttribArray(aNrm); glVertexAttribPointer(aNrm, 3, GL_FLOAT, GL_FALSE, 0, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, iboCube);
            GLint uMVPloc = glGetUniformLocation(prog, "uMVP"), uModelloc = glGetUniformLocation(prog, "uModel");
            for (int a = 0; a < avCount; a++) {
                float model[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0, avatars[a*3], avatars[a*3+1], avatars[a*3+2], 1 };
                float mvpa[16]; mat_mul(mvpa, mvp, model);
                glUniformMatrix4fv(uMVPloc, 1, GL_FALSE, mvpa);
                glUniformMatrix4fv(uModelloc, 1, GL_FALSE, model);
                glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
            }
        }

        /* reconstructed projectile sim running in the engine (?proj) */
        if (projDemo) {
            float sdt = dt > 0.05f ? 0.05f : dt;      /* clamp hitches so homing can't overshoot */
            int wasAlive[MAXPROJ];
            for (int i = 0; i < gProjN; i++) {
                wasAlive[i] = gProjAlive[i];
                if (gProjAlive[i]) { gProjPrev[i][0]=gProjs[i].pos[0]; gProjPrev[i][1]=gProjs[i].pos[1]; gProjPrev[i][2]=gProjs[i].pos[2]; }
            }
            for (int t = 0; t < gTgtN; t++) { gTgtForce[t*3]=0; gTgtForce[t*3+1]=0; gTgtForce[t*3+2]=0; }
            axis_sim_tick(gProjs, gProjAlive, gProjN, gTgt, gTgtAlive, gTgtN, gTgtForce,
                          gRadius * 0.06f, 1.0f, 0.0f, sdt);
            /* a homing projectile that struck a target this tick (verified sim applied
               knockback -> nonzero force) destroys it and scores */
            for (int t = 0; t < gTgtN; t++) {
                if (!gTgtAlive[t]) continue;
                float *fo = &gTgtForce[t*3];
                if (fo[0]*fo[0] + fo[1]*fo[1] + fo[2]*fo[2] > 0.0f) { gTgtAlive[t] = 0; gScore++; spawn_debris(&gTgt[t*3]); }
            }
            /* a projectile still flying that struck map geometry this step is
               consumed at the impact point (reconstructed collision pipeline) */
            for (int i = 0; i < gProjN; i++) {
                if (!gProjAlive[i]) continue;
                float hp[3];
                if (proj_hit_map(gProjPrev[i], gProjs[i].pos, hp)) {
                    gProjs[i].pos[0]=hp[0]; gProjs[i].pos[1]=hp[1]; gProjs[i].pos[2]=hp[2];
                    gProjAlive[i] = 0;
                }
            }
            /* a projectile that just resolved (target hit or wall hit) spawns debris */
            for (int i = 0; i < gProjN; i++) if (wasAlive[i] && !gProjAlive[i]) spawn_debris(gProjs[i].pos);
            /* advance the debris (verified axis_effect layer); gravity arcs them down */
            for (int i = 0; i < MAXDEBRIS; i++) {
                if (!gDebrisAlive[i]) continue;
                if (axis_debris_step(&gDebris[i], sdt, 0, 0, 0, 0, 0, 0) == 2) gDebrisAlive[i] = 0;
            }
            /* proximity gate (verified axis_prop layer): open while a projectile
               is within range of the gate, close otherwise; projectiles are the
               "actors" fed to the nearest-distance scan */
            {
                /* actors = the ring targets (persistent) + live projectiles, so the
                   gate's nearest-actor proximity test has stable input to open on */
                float actorBuf[(MAXTGT+MAXPROJ)*3]; int actAlive[MAXTGT+MAXPROJ]; int na = 0;
                for (int i = 0; i < gTgtN; i++, na++) {
                    actorBuf[na*3]=gTgt[i*3]; actorBuf[na*3+1]=gTgt[i*3+1]; actorBuf[na*3+2]=gTgt[i*3+2];
                    actAlive[na]=gTgtAlive[i];
                }
                for (int i = 0; i < gProjN; i++, na++) {
                    actorBuf[na*3]=gProjs[i].pos[0]; actorBuf[na*3+1]=gProjs[i].pos[1]; actorBuf[na*3+2]=gProjs[i].pos[2];
                    actAlive[na]=gProjAlive[i];
                }
                float d2 = axis_prop_nearest_dist2(gPropPos, actorBuf, actAlive, na);
                int cb; gPropRate = axis_prop_inside(d2, gPropRange) ? 2.0f : -2.0f;
                gPropPhase = axis_anim_phase_step(gPropPhase, &gPropRate, 1 /*CLAMP*/, sdt, 0, &cb);
            }
            /* item pickups (verified axis_pickup layer): a live projectile acting
               as the collecting actor consumes any item it passes within range of */
            {
                float ir = gRadius * 0.04f;   /* item radius; reach = ir*1.5 */
                for (int k = 0; k < gItemN; k++) {
                    if (!gItemAlive[k]) continue;
                    for (int i = 0; i < gProjN; i++) {
                        if (!gProjAlive[i]) continue;
                        if (axis_pickup_in_range(&gItemPos[k*3], gProjs[i].pos, ir)) {
                            gItemAlive[k] = 0; gPickupCount++;
                            break;
                        }
                    }
                }
            }
            /* client input (verified axis_input layer): build the edge-triggered
               action mask from the key latches; a fire (channel 0) press flashes
               a marker above the gate */
            {
                uint32_t pm, rm;
                axis_input_build_masks(gActEnabled, gActPress, gActRel, 16, &pm, &rm);
                if (axis_input_mask_has(pm, 0)) {           /* fire: launch a shot from the camera */
                    for (int s = 0; s < MAXSHOTS; s++) if (!gShotAlive[s]) {
                        float fwd[3]; camForward(fwd);
                        float spd = gRadius * 1.4f;
                        gShotPos[s*3+0]=camPos[0]; gShotPos[s*3+1]=camPos[1]; gShotPos[s*3+2]=camPos[2];
                        gShotVel[s*3+0]=fwd[0]*spd; gShotVel[s*3+1]=fwd[1]*spd; gShotVel[s*3+2]=fwd[2]*spd;
                        gShotAge[s]=0.0f; gShotAlive[s]=1;
                        break;
                    }
                }
                /* advance player shots: const-velocity step, map collision, debris on hit */
                for (int s = 0; s < MAXSHOTS; s++) {
                    if (!gShotAlive[s]) continue;
                    float *p = &gShotPos[s*3], *pv = &gShotPrev[s*3], *vv = &gShotVel[s*3];
                    pv[0]=p[0]; pv[1]=p[1]; pv[2]=p[2];
                    p[0]+=vv[0]*sdt; p[1]+=vv[1]*sdt; p[2]+=vv[2]*sdt;
                    gShotAge[s]+=sdt;
                    float hp[3], tt;
                    /* hit a target? (verified axis_seg_sphere) -> destroy it + score */
                    int hitTgt = -1;
                    for (int t = 0; t < gTgtN; t++) {
                        if (!gTgtAlive[t]) continue;
                        if (axis_seg_sphere(pv, p, &gTgt[t*3], gRadius * 0.12f, &tt)) { hitTgt = t; break; }
                    }
                    if (hitTgt >= 0) { gTgtAlive[hitTgt] = 0; gScore++; spawn_debris(&gTgt[hitTgt*3]); gShotAlive[s] = 0; }
                    else if (proj_hit_map(pv, p, hp)) { spawn_debris(hp); gShotAlive[s]=0; }
                    else if (gShotAge[s] > 2.5f) gShotAlive[s]=0;
                }
            }
            int anyLive = 0; for (int i = 0; i < gProjN; i++) if (gProjAlive[i]) anyLive = 1;
            int anyTgt = 0; for (int i = 0; i < gTgtN; i++) if (gTgtAlive[i]) anyTgt = 1;
            if (!anyLive || !anyTgt) engine_proj_demo();     /* loop the demo / rearm when cleared */
            if (aUV >= 0) glDisableVertexAttribArray(aUV);
            glBindBuffer(GL_ARRAY_BUFFER, vboCube);  glEnableVertexAttribArray(aPos); glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 0, 0);
            glBindBuffer(GL_ARRAY_BUFFER, vboCubeN); glEnableVertexAttribArray(aNrm); glVertexAttribPointer(aNrm, 3, GL_FLOAT, GL_FALSE, 0, 0);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, iboCube);
            GLint uM = glGetUniformLocation(prog, "uMVP"), uMo = glGetUniformLocation(prog, "uModel");
            glUniform1i(glGetUniformLocation(prog, "uHasTex"), 0);
            float st = gRadius * 0.05f, sp = gRadius * 0.025f;
            if (entTri > 0) {                                /* targets: a real AXIS model */
                glBindBuffer(GL_ARRAY_BUFFER, vboEnt);  glEnableVertexAttribArray(aPos); glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 0, 0);
                glBindBuffer(GL_ARRAY_BUFFER, vboEntN); glEnableVertexAttribArray(aNrm); glVertexAttribPointer(aNrm, 3, GL_FLOAT, GL_FALSE, 0, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, iboEnt);
                int entTexOn = entHasTex && aUV >= 0;
                if (entTexOn) {                               /* real SHAPE skin */
                    glBindBuffer(GL_ARRAY_BUFFER, vboEntUV); glEnableVertexAttribArray(aUV); glVertexAttribPointer(aUV, 2, GL_FLOAT, GL_FALSE, 0, 0);
                    glUniform1i(glGetUniformLocation(prog, "uHasTex"), 1);
                    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, texEnt);
                    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
                } else {
                    glUniform3f(glGetUniformLocation(prog, "uColor"), 0.95f, 0.8f, 0.3f);
                }
                float s = (gRadius * 0.16f) / entRadius;      /* fit the model to a target-sized box */
                for (int i = 0; i < gTgtN; i++) {
                    float m[16] = { s,0,0,0, 0,s,0,0, 0,0,s,0,
                        gTgt[i*3+0] - s*entCenter[0], gTgt[i*3+1] - s*entCenter[1], gTgt[i*3+2] - s*entCenter[2], 1 };
                    float mv[16]; mat_mul(mv, mvp, m);
                    glUniformMatrix4fv(uM, 1, GL_FALSE, mv); glUniformMatrix4fv(uMo, 1, GL_FALSE, m);
                    glDrawElements(GL_TRIANGLES, entTri * 3, GL_UNSIGNED_INT, 0);
                }
                if (entTexOn) { glUniform1i(glGetUniformLocation(prog, "uHasTex"), 0); glDisableVertexAttribArray(aUV); }
                glBindBuffer(GL_ARRAY_BUFFER, vboCube);  glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 0, 0);
                glBindBuffer(GL_ARRAY_BUFFER, vboCubeN); glVertexAttribPointer(aNrm, 3, GL_FLOAT, GL_FALSE, 0, 0);
                glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, iboCube);
            } else for (int i = 0; i < gTgtN; i++) {         /* targets: red cubes (fallback) */
                glUniform3f(glGetUniformLocation(prog, "uColor"), 0.9f, 0.2f, 0.2f);
                float m[16] = { st,0,0,0, 0,st,0,0, 0,0,st,0, gTgt[i*3], gTgt[i*3+1], gTgt[i*3+2], 1 };
                float mv[16]; mat_mul(mv, mvp, m);
                glUniformMatrix4fv(uM, 1, GL_FALSE, mv); glUniformMatrix4fv(uMo, 1, GL_FALSE, m);
                glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
            }
            for (int i = 0; i < gProjN; i++) {               /* live projectiles: yellow cubes */
                if (!gProjAlive[i]) continue;
                glUniform3f(glGetUniformLocation(prog, "uColor"), 0.95f, 0.9f, 0.2f);
                float *p = gProjs[i].pos;
                float m[16] = { sp,0,0,0, 0,sp,0,0, 0,0,sp,0, p[0], p[1], p[2], 1 };
                float mv[16]; mat_mul(mv, mvp, m);
                glUniformMatrix4fv(uM, 1, GL_FALSE, mv); glUniformMatrix4fv(uMo, 1, GL_FALSE, m);
                glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
            }
            float sd = gRadius * 0.012f;
            for (int i = 0; i < MAXDEBRIS; i++) {            /* gravity debris: small orange cubes */
                if (!gDebrisAlive[i]) continue;
                glUniform3f(glGetUniformLocation(prog, "uColor"), 1.0f, 0.55f, 0.1f);
                float *p = gDebris[i].pos;
                float m[16] = { sd,0,0,0, 0,sd,0,0, 0,0,sd,0, p[0], p[1], p[2], 1 };
                float mv[16]; mat_mul(mv, mvp, m);
                glUniformMatrix4fv(uM, 1, GL_FALSE, mv); glUniformMatrix4fv(uMo, 1, GL_FALSE, m);
                glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
            }
            /* the proximity gate: a green pillar rooted at ground level whose
               height grows with its open phase (closed = short, open = tall) */
            {
                float gw = gRadius * 0.05f;
                float H = gRadius * (0.15f + 0.9f * gPropPhase);   /* half-height grows as it opens */
                float py = gPropPos[1] + H;                        /* base sits on the ground */
                glUniform3f(glGetUniformLocation(prog, "uColor"), 0.2f, 0.95f, 0.4f);
                float m[16] = { gw,0,0,0, 0,H,0,0, 0,0,gw,0, gPropPos[0], py, gPropPos[2], 1 };
                float mv[16]; mat_mul(mv, mvp, m);
                glUniformMatrix4fv(uM, 1, GL_FALSE, mv); glUniformMatrix4fv(uMo, 1, GL_FALSE, m);
                glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
            }
            {   /* collectible items: cyan cubes that vanish when a projectile collects them */
                float si = gRadius * 0.03f;
                for (int k = 0; k < gItemN; k++) {
                    if (!gItemAlive[k]) continue;
                    glUniform3f(glGetUniformLocation(prog, "uColor"), 0.2f, 0.85f, 0.95f);
                    float *p = &gItemPos[k*3];
                    float m[16] = { si,0,0,0, 0,si,0,0, 0,0,si,0, p[0], p[1], p[2], 1 };
                    float mv[16]; mat_mul(mv, mvp, m);
                    glUniformMatrix4fv(uM, 1, GL_FALSE, mv); glUniformMatrix4fv(uMo, 1, GL_FALSE, m);
                    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
                }
            }
            {   /* player shots (fired via axis_input): magenta cubes */
                float ss = gRadius * 0.03f;
                for (int s = 0; s < MAXSHOTS; s++) {
                    if (!gShotAlive[s]) continue;
                    glUniform3f(glGetUniformLocation(prog, "uColor"), 0.95f, 0.15f, 0.9f);
                    float *p = &gShotPos[s*3];
                    float m[16] = { ss,0,0,0, 0,ss,0,0, 0,0,ss,0, p[0], p[1], p[2], 1 };
                    float mv[16]; mat_mul(mv, mvp, m);
                    glUniformMatrix4fv(uM, 1, GL_FALSE, mv); glUniformMatrix4fv(uMo, 1, GL_FALSE, m);
                    glDrawElements(GL_TRIANGLES, 36, GL_UNSIGNED_SHORT, 0);
                }
            }
        }
    }
}

static int ptrLocked = 0;
static EM_BOOL onLockChange(int type, const EmscriptenPointerlockChangeEvent *e, void *u) {
    (void)type; (void)u; ptrLocked = e->isActive; return EM_TRUE;
}
static EM_BOOL onMouseMove(int type, const EmscriptenMouseEvent *e, void *u) {
    (void)type; (void)u;
    if (ptrLocked && camReady) {
        camYaw += (float)e->movementX * 0.0025f;
        camPitch -= (float)e->movementY * 0.0025f;
        if (camPitch > 1.5f) camPitch = 1.5f; if (camPitch < -1.5f) camPitch = -1.5f;
    }
    return EM_TRUE;
}

static EM_BOOL onKey(int type, const EmscriptenKeyboardEvent *e, void *u) {
    (void)u;
    int code = -1;
    const char *k = e->key;
    if (k[0] && k[1] == 0) {                 /* single char: letters/space */
        char c = k[0]; if (c >= 'a' && c <= 'z') c -= 32; code = (unsigned char)c;
    } else if (!__builtin_strcmp(k, "ArrowLeft")) code = 37;
    else if (!__builtin_strcmp(k, "ArrowUp")) code = 38;
    else if (!__builtin_strcmp(k, "ArrowRight")) code = 39;
    else if (!__builtin_strcmp(k, "ArrowDown")) code = 40;
    else if (!__builtin_strcmp(k, "Shift")) code = 16;
    else if (!__builtin_strcmp(k, " ")) code = ' ';
    if (code == 'F') {                       /* fire action -> channel 0 (edge-latched) */
        int now = (type == EMSCRIPTEN_EVENT_KEYDOWN) ? 1 : 0, prev = keyDown[code];
        if (now && !prev) gActPress[0] = 1;
        else if (!now && prev) gActRel[0] = 1;
    }
    if (code >= 0 && code < 320) keyDown[code] = (type == EMSCRIPTEN_EVENT_KEYDOWN) ? 1 : 0;
    return EM_TRUE;
}

int main(void) {
    EmscriptenWebGLContextAttributes attr; emscripten_webgl_init_context_attributes(&attr);
    attr.majorVersion = 1; attr.depth = 1; attr.alpha = 0;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &attr);
    emscripten_webgl_make_context_current(ctx);
    emscripten_webgl_enable_extension(ctx, "OES_element_index_uint");   /* 32-bit mesh indices */
    glEnable(GL_DEPTH_TEST);
    prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, VS));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, FS));
    glLinkProgram(prog);
    glGenBuffers(1, &vboPos); glGenBuffers(1, &vboNrm); glGenBuffers(1, &vboUV); glGenBuffers(1, &ibo);

    /* unit-cube geometry for avatar markers (8 corners, normals = normalized pos) */
    {
        float cp[24], cn[24]; int w = 0;
        for (int sx = -1; sx <= 1; sx += 2) for (int sy = -1; sy <= 1; sy += 2) for (int sz = -1; sz <= 1; sz += 2) {
            cp[w] = (float)sx; cp[w+1] = (float)sy; cp[w+2] = (float)sz;
            float il = 0.57735027f; cn[w] = sx*il; cn[w+1] = sy*il; cn[w+2] = sz*il; w += 3;
        }
        /* corner index by (x,y,z) bits: i = ((x+1)/2)*4 + ((y+1)/2)*2 + ((z+1)/2) */
        static const uint16_t ci[36] = {
            0,1,3, 0,3,2,  4,6,7, 4,7,5,   /* -x, +x */
            0,4,5, 0,5,1,  2,3,7, 2,7,6,   /* -y, +y */
            0,2,6, 0,6,4,  1,5,7, 1,7,3 }; /* -z, +z */
        glGenBuffers(1, &vboCube); glGenBuffers(1, &vboCubeN); glGenBuffers(1, &iboCube);
        glBindBuffer(GL_ARRAY_BUFFER, vboCube); glBufferData(GL_ARRAY_BUFFER, sizeof cp, cp, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, vboCubeN); glBufferData(GL_ARRAY_BUFFER, sizeof cn, cn, GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, iboCube); glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof ci, ci, GL_STATIC_DRAW);
    }
    emscripten_set_keydown_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, onKey);
    emscripten_set_keyup_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, onKey);
    emscripten_set_mousemove_callback(EMSCRIPTEN_EVENT_TARGET_WINDOW, 0, 1, onMouseMove);
    emscripten_set_pointerlockchange_callback(EMSCRIPTEN_EVENT_TARGET_DOCUMENT, 0, 1, onLockChange);
    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
