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
#define MAXAV 16
static float avatars[MAXAV * 3];
static int avCount = 0;

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
    for (int k = 0; k < 3; k++) { gCenter[k] = 0.5f*(lo[k]+hi[k]); float h = 0.5f*(hi[k]-lo[k]); if (h > gRadius) gRadius = h; }

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
        if (keyDown['W']) { camPos[0]+=f[0]*v; camPos[1]+=f[1]*v; camPos[2]+=f[2]*v; }
        if (keyDown['S']) { camPos[0]-=f[0]*v; camPos[1]-=f[1]*v; camPos[2]-=f[2]*v; }
        if (keyDown['D']) { camPos[0]+=r[0]*v; camPos[1]+=r[1]*v; camPos[2]+=r[2]*v; }
        if (keyDown['A']) { camPos[0]-=r[0]*v; camPos[1]-=r[1]*v; camPos[2]-=r[2]*v; }
        if (keyDown['E'] || keyDown[' ']) camPos[1]+=v;
        if (keyDown['Q']) camPos[1]-=v;
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
