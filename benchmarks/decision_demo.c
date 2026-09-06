#define _POSIX_C_SOURCE 200809L
#include "adapt_ipc.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <pthread.h>
#include <stdatomic.h>
static adapt_ctx_t *g_c;
static _Atomic int g_done;
static void *consumer(void *arg){(void)arg; static unsigned char b[70000];
    while(!atomic_load(&g_done)){ if(adapt_recv(g_c,b,sizeof(b))<=0)
        sched_yield(); }
    return 0; }

int main(void){
    char pp[64],cp[64];
    snprintf(pp,64,"/tmp/sc_p_%ld",(long)getpid());
    snprintf(cp,64,"/tmp/sc_c_%ld",(long)getpid());
    adapt_config_t pc={.local_sock=pp,.peer_sock=cp,.shm_capacity=1u<<20,
                       .policy=ADAPT_POLICY_FULL_ADAPTIVE};
    adapt_config_t cc={.local_sock=cp,.peer_sock=pp,.shm_capacity=1u<<20};
    adapt_ctx_t *p,*c;
    if(adapt_init(ADAPT_ROLE_PRODUCER,&pc,&p))return 2;
    if(adapt_init(ADAPT_ROLE_CONSUMER,&cc,&c))return 2;
    /* concurrent consumer: drains whatever transport the policy
     * picks (a sequential loop deadlocks when the policy escapes to
     * UDS while ring records are still queued) */
    g_c = c;
    pthread_t ct; pthread_create(&ct,0,consumer,0);
    static unsigned char tx[70000];
    memset(tx,0x5a,sizeof(tx));
    /* phases: small control -> bulk -> mixed -> small (route story) */
    for(int phase=0;phase<4;phase++){
        unsigned n = (phase==2)?600:400;
        for(unsigned i=0;i<n;i++){
            size_t sz = 512;
            if(phase==1) sz=16384;
            else if(phase==2) sz = (i%2)?16384:512;
            memcpy(tx,&i,sizeof(i));
            int rc; do{rc=adapt_send(p,tx,sz);}while(rc==-EAGAIN);
            if(rc)return 3;
        }
    }
    atomic_store(&g_done,1);
    pthread_join(ct,0);
    adapt_stats_t st; adapt_get_stats(p,&st);
    fprintf(stderr,"decisions=%llu\n",(unsigned long long)st.decisions);
    adapt_shutdown(p);adapt_shutdown(c);
    return 0;
}
