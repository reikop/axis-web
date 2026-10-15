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

/* from src/axis_shp.c (linked in) */
int axis_shp_parse(const uint8_t *buf, uint32_t off, uint32_t end,
                   float *posOut, float *nrmOut, float *uvOut,
                   uint32_t *triOut, uint32_t *grpOut, uint32_t *counts);

static GLuint prog, vboPos, vboNrm, ibo;
static int indexCount = 0;
static float gAngle = 0.0f;
static float gCenter[3] = {0, 0, 0}, gRadius = 1.0f;

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
    "attribute vec3 aPos; attribute vec3 aNrm; uniform mat4 uMVP; uniform mat4 uModel;"
    "varying vec3 vN; void main(){ vN = mat3(uModel)*aNrm; gl_Position = uMVP*vec4(aPos,1.0); }";
static const char *FS =
    "precision mediump float; varying vec3 vN;"
    "void main(){ vec3 n=normalize(vN); float d=max(dot(n,normalize(vec3(0.4,0.8,0.6))),0.0);"
    "vec3 c=vec3(0.30,0.55,0.85)*(0.35+0.65*d); gl_FragColor=vec4(c,1.0); }";

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
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, tc*3*sizeof(uint32_t), TRI, GL_STATIC_DRAW);
    indexCount = (int)(tc * 3);
    return (int)CNT[2]; /* group count, as a success signal */
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
        }
        for (uint32_t k = 0; k < tc; k++) {
            TRI[(mergedT + k) * 3 + 0] = TTRI[k * 3 + 0] + mergedV;
            TRI[(mergedT + k) * 3 + 1] = TTRI[k * 3 + 1] + mergedV;
            TRI[(mergedT + k) * 3 + 2] = TTRI[k * 3 + 2] + mergedV;
        }
        mergedV += vc; mergedT += tc; placed++;
    }
    if (mergedV == 0) return -1;

    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < mergedV; i++) for (int k = 0; k < 3; k++) { float v = POS[i*3+k]; if (v < lo[k]) lo[k] = v; if (v > hi[k]) hi[k] = v; }
    gRadius = 0.0001f;
    for (int k = 0; k < 3; k++) { gCenter[k] = 0.5f*(lo[k]+hi[k]); float h = 0.5f*(hi[k]-lo[k]); if (h > gRadius) gRadius = h; }

    glBindBuffer(GL_ARRAY_BUFFER, vboPos); glBufferData(GL_ARRAY_BUFFER, mergedV*3*sizeof(float), POS, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, vboNrm); glBufferData(GL_ARRAY_BUFFER, mergedV*3*sizeof(float), NRM, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo); glBufferData(GL_ELEMENT_ARRAY_BUFFER, mergedT*3*sizeof(uint32_t), TRI, GL_STATIC_DRAW);
    indexCount = (int)(mergedT * 3);
    return placed;
}

static void frame(void) {
    int w, h; emscripten_get_canvas_element_size("#canvas", &w, &h);
    glViewport(0, 0, w, h);
    glClearColor(0.07f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (indexCount > 0) {
        gAngle += 0.01f;
        float proj[16], view[16], model[16], tmp[16], mvp[16];
        mat_persp(proj, 1.0f, h ? (float)w/(float)h : 1.0f, gRadius*0.05f, gRadius*20.0f);
        float d = gRadius * 3.0f;
        /* view = translate(-0,-0,-d) * rotateY(angle) * translate(-center) */
        float ry[16] = { cosf(gAngle),0,-sinf(gAngle),0, 0,1,0,0, sinf(gAngle),0,cosf(gAngle),0, 0,0,0,1 };
        float tr[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, -gCenter[0],-gCenter[1],-gCenter[2],1 };
        float tz[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,-d,1 };
        mat_mul(tmp, ry, tr); mat_mul(view, tz, tmp);
        memcpy(model, ry, sizeof ry);
        mat_mul(mvp, proj, view);
        glUseProgram(prog);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uMVP"), 1, GL_FALSE, mvp);
        glUniformMatrix4fv(glGetUniformLocation(prog, "uModel"), 1, GL_FALSE, model);
        GLint aPos = glGetAttribLocation(prog, "aPos"), aNrm = glGetAttribLocation(prog, "aNrm");
        glBindBuffer(GL_ARRAY_BUFFER, vboPos); glEnableVertexAttribArray(aPos); glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 0, 0);
        glBindBuffer(GL_ARRAY_BUFFER, vboNrm); glEnableVertexAttribArray(aNrm); glVertexAttribPointer(aNrm, 3, GL_FLOAT, GL_FALSE, 0, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, 0);
    }
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
    glGenBuffers(1, &vboPos); glGenBuffers(1, &vboNrm); glGenBuffers(1, &ibo);
    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
