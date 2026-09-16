/* Synthetic roundtrip tests for frontend transforms. */
#include "frontend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

static int roundtrip(int mode,
                     int (*ap)(const uint8_t*,size_t,uint8_t**,size_t*,uint8_t**,size_t*),
                     int (*inv)(const uint8_t*,size_t,const uint8_t*,size_t,uint8_t*,size_t),
                     const uint8_t *in, size_t n) {
    uint8_t *t=NULL,*m=NULL; size_t tn=0,mn=0;
    if (ap(in,n,&t,&tn,&m,&mn)) { printf("mode %d declined (unexpected)\n",mode); return -1; }
    uint8_t *out = malloc(n?n:1);
    int rc = inv(t,tn,m,mn,out,n);
    int ok = rc==0 && memcmp(out,in,n)==0;
    free(t); free(m); free(out);
    return ok?0:-1;
}

static void test_sao(void) {
    /* 11 records of 28 bytes: f[6]=28, f[2]=10 (nrec=10) */
    size_t r=11, n=r*28;
    uint8_t *in = calloc(1,n);
    for (size_t rec=0; rec<r; rec++) {
        uint32_t *f = (uint32_t*)(in+rec*28);
        f[0]= (uint32_t)rec; f[1]= (uint32_t)(1000+rec*7); /* sorted coord col */
        f[2]= 10; f[3]= 0xdeadbeef;
        f[4]= (uint32_t)(rec%3); f[5]=(uint32_t)(rec%5); f[6]=28;
    }
    CHECK(roundtrip(9,fe_sao_apply,fe_sao_invert,in,n)==0,"sao roundtrip");
    /* decline: wrong record size field */
    ((uint32_t*)in)[6]=27;
    { uint8_t *t,*m; size_t tn,mn;
      CHECK(fe_sao_apply(in,n,&t,&tn,&m,&mn)!=0,"sao declines bad recsize"); }
    ((uint32_t*)in)[6]=28;
    /* decline: n not multiple of 28 */
    { uint8_t *t,*m; size_t tn,mn;
      CHECK(fe_sao_apply(in,n-1,&t,&tn,&m,&mn)!=0,"sao declines unaligned"); }
    /* overlapping-column stress: col1 unsorted (deltas still exact) */
    for (size_t rec=0; rec<r; rec++) ((uint32_t*)(in+rec*28))[1]=(uint32_t)(9999-rec*131);
    CHECK(roundtrip(9,fe_sao_apply,fe_sao_invert,in,n)==0,"sao unsorted col1");
    /* high-cardinality col4 -> raw fallback path */
    for (size_t rec=0; rec<r; rec++) ((uint32_t*)(in+rec*28))[4]=(uint32_t)(rec*7919+13);
    CHECK(roundtrip(9,fe_sao_apply,fe_sao_invert,in,n)==0,"sao raw-fallback col");
    free(in);
    printf("sao tests done\n");
}

static void test_d16(void) {
    size_t n = 70000; /* even, >= 64KiB */
    uint8_t *in = malloc(n);
    for (size_t i=0;i<n/2;i++){ uint16_t v=(uint16_t)(i*3+ (i>>8)); in[2*i]=(uint8_t)v; in[2*i+1]=(uint8_t)(v>>8); }
    CHECK(roundtrip(10,fe_d16_apply,fe_d16_invert,in,n)==0,"d16 roundtrip");
    { uint8_t *t,*m; size_t tn,mn;
      CHECK(fe_d16_apply(in,1000,&t,&tn,&m,&mn)!=0,"d16 declines small");
      CHECK(fe_d16_apply(in,n-1,&t,&tn,&m,&mn)!=0,"d16 declines odd"); }
    /* wrapping deltas */
    for (size_t i=0;i<n;i++) in[i]=(uint8_t)(i*37+11);
    CHECK(roundtrip(10,fe_d16_apply,fe_d16_invert,in,n)==0,"d16 wrap roundtrip");
    free(in);
    printf("d16 tests done\n");
}

static void test_exe(void) {
    size_t n = 70000;
    uint8_t *in = calloc(1,n);
    /* 300 E8 sites with small rel32, plus overlapping pair */
    for (int k=0;k<300;k++){
        size_t i = 100 + (size_t)k*200;
        in[i]=0xE8;
        int32_t rel = (int32_t)(5000 - k*17);
        in[i+1]=(uint8_t)rel; in[i+2]=(uint8_t)(rel>>8);
        in[i+3]=(uint8_t)(rel>>16); in[i+4]=(uint8_t)(rel>>24);
    }
    /* E9 jump */
    in[60000]=0xE9;
    { int32_t rel=-123456; in[60001]=(uint8_t)rel; in[60002]=(uint8_t)(rel>>8);
      in[60003]=(uint8_t)(rel>>16); in[60004]=(uint8_t)(rel>>24); }
    /* giant rel32 must be rejected as a site but stream still fine */
    in[65000]=0xE8;
    { int32_t rel=0x70000000; in[65001]=(uint8_t)rel; in[65002]=(uint8_t)(rel>>8);
      in[65003]=(uint8_t)(rel>>16); in[65004]=(uint8_t)(rel>>24); }
    /* overlapping: E8 inside previous rel32 */
    in[200+2]=0xE8;
    { int32_t rel=42; in[203]=(uint8_t)rel; in[204]=(uint8_t)(rel>>8);
      in[205]=(uint8_t)(rel>>16); in[206]=(uint8_t)(rel>>24); }
    CHECK(roundtrip(11,fe_exe_apply,fe_exe_invert,in,n)==0,"exe roundtrip");
    /* decline: too few sites */
    uint8_t *plain = calloc(1,n);
    { uint8_t *t,*m; size_t tn,mn;
      CHECK(fe_exe_apply(plain,n,&t,&tn,&m,&mn)!=0,"exe declines nosites"); }
    free(plain);
    /* decline: too small */
    { uint8_t *t,*m; size_t tn,mn;
      CHECK(fe_exe_apply(in,1000,&t,&tn,&m,&mn)!=0,"exe declines small"); }
    free(in);
    printf("exe tests done\n");
}

int main(void){
    test_sao(); test_d16(); test_exe();
    if (fails) { printf("%d FAILURES\n",fails); return 1; }
    printf("ALL FRONTEND TESTS PASS\n");
    return 0;
}
