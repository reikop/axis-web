/* Verify collision math + impact force (port/sim/axis_collide.c).
 *   cc axis_collide.c axis_collide_test.c -lm -o t && ./t
 */
#include <math.h>
#include <stdio.h>

int  axis_seg_tri(const float*,const float*,const float*,const float*,const float*,float*,float*);
void axis_impact_force(const float *dir, float mag, float *outForce);

static int fails=0; static void ck(const char*w,int c){if(!c){printf("  FAIL %s\n",w);fails++;}}
static int nf(float a,float b){return fabsf(a-b)<1e-4f;}

int main(void){
    /* triangle in the z=0 plane; segment along +z through its centroid */
    float a[3]={0,0,0}, b[3]={2,0,0}, c[3]={0,2,0};
    float t, h[3];

    ck("segment through triangle hits",
       axis_seg_tri((float[]){0.5f,0.5f,-1}, (float[]){0.5f,0.5f,1}, a,b,c,&t,h) == 1);
    ck("hit t = 0.5 (midway)", nf(t,0.5f));
    ck("hit point on plane", nf(h[0],0.5f)&&nf(h[1],0.5f)&&nf(h[2],0.0f));

    /* segment that misses the triangle (outside its area) */
    ck("segment missing triangle -> no hit",
       axis_seg_tri((float[]){5,5,-1}, (float[]){5,5,1}, a,b,c,&t,h) == 0);
    /* segment that stops short of the plane -> no hit (t would be >1) */
    ck("segment too short -> no hit",
       axis_seg_tri((float[]){0.5f,0.5f,-2}, (float[]){0.5f,0.5f,-1}, a,b,c,&t,h) == 0);
    /* segment parallel to the triangle plane -> no hit */
    ck("parallel segment -> no hit",
       axis_seg_tri((float[]){0,0,1}, (float[]){2,0,1}, a,b,c,&t,h) == 0);

    /* impact force: opposite the hit direction, scaled by mag*10 */
    float f[3]; axis_impact_force((float[]){1,0,0}, 2.0f, f);
    ck("impact force = -dir*mag*10", nf(f[0],-20.0f)&&nf(f[1],0)&&nf(f[2],0));

    printf(fails?"\nCOLLIDE: %d FAILED\n":"\nCOLLIDE: ALL PASS\n", fails);
    return fails?1:0;
}
