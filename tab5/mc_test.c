#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <math.h>
#include "tab5_platform.h"
static uint8_t nor[0x100000];
static pthread_t th; static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t cv=PTHREAD_COND_INITIALIZER;
static void (*volatile fn)(uint32_t); static volatile uint32_t arg, job, done; static int helper_id;
static uint32_t cnum(void){ return pthread_equal(pthread_self(),th)?1u:0u; }
static void *helper(void*a){ uint32_t d=0; for(;;){ pthread_mutex_lock(&mu); while(d==job) pthread_cond_wait(&cv,&mu); pthread_mutex_unlock(&mu); fn(arg); d++; __atomic_store_n(&done,d,__ATOMIC_RELEASE);} return 0; }
static int nrun; static int run(void(*f)(uint32_t),uint32_t a){ nrun++; pthread_mutex_lock(&mu); fn=f; arg=a; job++; pthread_cond_signal(&cv); pthread_mutex_unlock(&mu); return 1; }
static int wait(void){ while(__atomic_load_n(&done,__ATOMIC_ACQUIRE)!=job); return 0; }
int main(int argc,char**argv){
    int two=argc>1;
    memset(nor,0xFF,sizeof nor); tab5_set_nor(nor);
    if(two){ pthread_create(&th,0,helper,0); tab5_mc_run_hook=run; tab5_mc_wait_hook=wait; tab5_cnum_hook=cnum; }
    tab5_power_on(); tab5_test_pitched_track();
    { static int32_t w[2*256]; for(int i=0;i<44100*35/10/256;i++) tab5_audio_read(w,256); }
    static int32_t st[2*256]; uint32_t h=2166136261u; double pk=0;
    tab5_master(700); tab5_keys(0x4A1);                                         /* a chord */
    for(int b=0;b<400;b++){ tab5_audio_read(st,256); for(int k=0;k<512;k++){ h^=(uint32_t)st[k]; h*=16777619u; double v=fabs(st[k]/8388608.0); if(v>pk)pk=v; } }
    printf("%s: hash %08x peak %.3f helper jobs %d\n", two?"two cores":"one core", h, pk, nrun); return 0; }
