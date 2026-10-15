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

/* generous scratch for one decoded mesh */
#define VCAP 300000u
#define TCAP 600000u
static float POS[VCAP * 3], NRM[VCAP * 3], UVt[VCAP * 2];
static uint32_t TRI[TCAP * 3], GRP[8192 * 2], CNT[4];

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
