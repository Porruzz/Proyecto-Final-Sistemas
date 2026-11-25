#include "sim_net.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
static sim_t *s;
void reset(int n,int seed,int loss,int strategy){if(!s)s=malloc(sizeof *s);sim_init(s,n,seed);s->loss_pct=loss;for(int i=0;i<n;i++){s->n[i].C=.45f+.08f*i;s->n[i].M=.45f+.06f*i;s->n[i].core.strategy=strategy;s->n[i].core.rng=seed+i;if(strategy==1)s->n[i].core.w=(cn_weights_t){0,0,0,0,1};}sim_run(s,2500);}
void advance(int ms){sim_run(s,ms);}
void toggle(int i,int on){if(on&&!s->n[i].alive)sim_revive(s,i);if(!on&&s->n[i].alive)sim_kill(s,i);}
void link(int a,int b,int on){s->link[a][b]=s->link[b][a]=on;}
int submit(int i,int kind,const unsigned char *data,int len){uint16_t id=0;if(!s->n[i].alive)return -9;int rc=sim_submit(s,i,kind,data,len,0,0,0,&id);return rc?rc:id;}
int result(int i,int id,unsigned char *out,int *meta){const sim_result_t*r=sim_find_result(s,i,id);if(!r)return -1;memcpy(out,r->data,r->dlen);meta[0]=r->status;meta[1]=r->executor;meta[2]=r->t;return r->dlen;}
int now(void){return s->now;}
int count(void){return s->count;}
void clear_history(void){for(int i=0;i<s->count;i++){s->n[i].nres=0;s->n[i].nev=0;}s->nstarts=0;}
const char *snapshot(void){static char b[262144];int p=0;p+=sprintf(b+p,"{\"time\":%u,\"sent\":%u,\"dropped\":%u,\"nodes\":[",s->now,s->frames_sent,s->frames_dropped);for(int i=0;i<s->count;i++){sim_node_t*n=&s->n[i];cn_state_t st;cn_core_local_state(&n->core,&st);p+=sprintf(b+p,"%s{\"id\":%d,\"alive\":%s,\"cpu_free\":%.3f,\"memory_free\":%.3f,\"load\":%.3f,\"neighbors\":%d,\"done\":%u,\"failed\":%u,\"reassigned\":%u,\"lamport\":%u}",i?",":"",i,n->alive?"true":"false",st.C,st.M,st.L,sim_active_neighbors(s,i),n->core.stats.tasks_done,n->core.stats.tasks_failed,n->core.stats.reassignments,n->core.lamport);}p+=sprintf(b+p,"],\"events\":[");int first=1;for(int i=0;i<s->count;i++){sim_node_t*n=&s->n[i];for(int j=n->nev>50?n->nev-50:0;j<n->nev;j++){sim_event_t*e=&n->ev[j];p+=sprintf(b+p,"%s{\"node\":%d,\"t\":%u,\"lc\":%u,\"event\":\"%s\",\"peer\":%d,\"task\":%u}",first?"":",",i,e->t,e->lamport,cn_event_name(e->type),e->peer,e->id);first=0;}}sprintf(b+p,"]}");return b;}
