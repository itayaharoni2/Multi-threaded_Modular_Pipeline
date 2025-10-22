#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "consumer_producer.h"

static void msleep(int ms){
  struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (ms%1000)*1000000L;
  nanosleep(&ts, NULL);
}

static void* putter(void* arg){
  consumer_producer_t* q = (consumer_producer_t*)arg;
  consumer_producer_put(q, "X");
  return NULL;
}
static void* getter(void* arg){
  consumer_producer_t* q = (consumer_producer_t*)arg;
  char* s = consumer_producer_get(q);
  int ok = (s && s[0]=='X' && s[1]=='\0');
  free(s);
  return (void*)(long)!ok;
}

static int t01_init_invalid_args(){
  consumer_producer_t q;
  const char* e1 = consumer_producer_init(&q, 0);
  return e1 ? 0 : 1;
}
static int t02_init_destroy_cap1(){
  consumer_producer_t q;
  if (consumer_producer_init(&q,1)!=NULL) return 1;
  consumer_producer_destroy(&q);
  return 0;
}
static int t03_simple_put_get(){
  consumer_producer_t q; consumer_producer_init(&q,4);
  const char* er = consumer_producer_put(&q,"hi");
  if (er) return 1;
  char* s = consumer_producer_get(&q);
  int ok = (s && strcmp(s,"hi")==0);
  free(s); consumer_producer_destroy(&q);
  return !ok;
}
static int t04_fifo_order(){
  consumer_producer_t q; consumer_producer_init(&q,4);
  consumer_producer_put(&q,"a");
  consumer_producer_put(&q,"b");
  char* s1 = consumer_producer_get(&q);
  char* s2 = consumer_producer_get(&q);
  int ok = (strcmp(s1,"a")==0 && strcmp(s2,"b")==0);
  free(s1); free(s2); consumer_producer_destroy(&q);
  return !ok;
}
static int t05_get_blocks_then_unblocks(){
  consumer_producer_t q; consumer_producer_init(&q,1);
  pthread_t th; void* rc=NULL;
  pthread_create(&th,NULL,getter,&q);
  msleep(100);
  consumer_producer_put(&q,"X");
  pthread_join(th,&rc);
  int ok = (int)(long)rc==0;
  consumer_producer_destroy(&q);
  return !ok;
}
static int t06_put_blocks_then_unblocks(){
  consumer_producer_t q; consumer_producer_init(&q,1);
  consumer_producer_put(&q,"A");
  pthread_t th;
  pthread_create(&th,NULL,putter,&q);
  msleep(120);
  int not_done_yet = pthread_tryjoin_np(th,NULL)!=0;
  char* s = consumer_producer_get(&q); free(s);
  pthread_join(th,NULL);
  consumer_producer_destroy(&q);
  return !not_done_yet ? 1 : 0;
}
static int t07_multiple_producers_consumers(){
  consumer_producer_t q; consumer_producer_init(&q,8);
  enum {P=4,C=4};
  pthread_t pt[P], ct[C];
  for(int i=0;i<P;i++) pthread_create(&pt[i],NULL,putter,&q);
  for(int i=0;i<C;i++) pthread_create(&ct[i],NULL,getter,&q);
  for(int i=0;i<P;i++) pthread_join(pt[i],NULL);
  int ok=1;
  for(int i=0;i<C;i++){ void* rc=NULL; pthread_join(ct[i],&rc); ok &= ((int)(long)rc==0); }
  consumer_producer_destroy(&q);
  return !ok;
}
static int t08_stress(){
  consumer_producer_t q; consumer_producer_init(&q,32);
  for(int i=0;i<1000;i++){
    char buf[16]; snprintf(buf,sizeof buf,"v%04d",i);
    char* in = strdup(buf);
    if (!in) { consumer_producer_destroy(&q); return 1; }
    consumer_producer_put(&q, in);
    free(in);
    char* s = consumer_producer_get(&q);
    if (!s){ consumer_producer_destroy(&q); return 1; }
    free(s);
  }
  consumer_producer_destroy(&q); return 0;
}

static int t09_signal_finished_and_wait(){
  consumer_producer_t q; consumer_producer_init(&q,4);
  consumer_producer_signal_finished(&q);
  int rc = consumer_producer_wait_finished(&q);
  consumer_producer_destroy(&q);
  return rc!=0;
}

/* Trampoline to avoid function pointer cast warning */
static void* wait_finished_tramp(void* arg){
  consumer_producer_t* q = (consumer_producer_t*)arg;
  int rc = consumer_producer_wait_finished(q);
  return (void*)(long)rc;
}
static int t10_wait_finished_blocks_until_signal(){
  consumer_producer_t q; consumer_producer_init(&q,4);
  pthread_t th; void* ret=NULL;
  pthread_create(&th,NULL,wait_finished_tramp,&q);
  msleep(120);
  int still_waiting = pthread_tryjoin_np(th,&ret)!=0;
  consumer_producer_signal_finished(&q);
  pthread_join(th,&ret);
  int ok = still_waiting && ((int)(long)ret==0);
  consumer_producer_destroy(&q);
  return !ok;
}
static int t11_null_get_does_not_crash(){
  char* s = consumer_producer_get(NULL);
  (void)s;
  return 0;
}
static int t12_null_put_fails(){
  consumer_producer_t q; consumer_producer_init(&q,2);
  const char* er = consumer_producer_put(&q,NULL);
  consumer_producer_destroy(&q);
  return er?0:1;
}
static int t13_capacity_wraparound(){
  consumer_producer_t q; consumer_producer_init(&q,3);
  consumer_producer_put(&q,"1");
  consumer_producer_put(&q,"2");
  consumer_producer_put(&q,"3");
  char *a=consumer_producer_get(&q), *b=consumer_producer_get(&q); free(a); free(b);
  consumer_producer_put(&q,"4");
  consumer_producer_put(&q,"5");
  char* c=consumer_producer_get(&q); int ok = (c && strcmp(c,"3")==0); free(c);
  consumer_producer_destroy(&q);
  return !ok;
}
static int t14_many_small_ops(){
  consumer_producer_t q; consumer_producer_init(&q,5);
  for(int i=0;i<100;i++){
    consumer_producer_put(&q,"x");
    char* s=consumer_producer_get(&q); free(s);
  }
  consumer_producer_destroy(&q);
  return 0;
}
static int t15_no_spurious_null_get(){
  consumer_producer_t q; consumer_producer_init(&q,1);
  pthread_t th; void* rc=NULL;
  pthread_create(&th,NULL,getter,&q);
  msleep(80);
  consumer_producer_put(&q,"X");
  pthread_join(th,&rc);
  int ok = ((int)(long)rc==0);
  consumer_producer_destroy(&q);
  return !ok;
}

int main(int argc, char**argv){
  if (argc<2){ fprintf(stderr,"need test name\n"); return 2; }
  const char* t = argv[1];
  struct { const char* name; int(*fn)(void); } cases[] = {
    {"t01_init_invalid_args",t01_init_invalid_args},
    {"t02_init_destroy_cap1",t02_init_destroy_cap1},
    {"t03_simple_put_get",t03_simple_put_get},
    {"t04_fifo_order",t04_fifo_order},
    {"t05_get_blocks_then_unblocks",t05_get_blocks_then_unblocks},
    {"t06_put_blocks_then_unblocks",t06_put_blocks_then_unblocks},
    {"t07_multiple_producers_consumers",t07_multiple_producers_consumers},
    {"t08_stress",t08_stress},
    {"t09_signal_finished_and_wait",t09_signal_finished_and_wait},
    {"t10_wait_finished_blocks_until_signal",t10_wait_finished_blocks_until_signal},
    {"t11_null_get_does_not_crash",t11_null_get_does_not_crash},
    {"t12_null_put_fails",t12_null_put_fails},
    {"t13_capacity_wraparound",t13_capacity_wraparound},
    {"t14_many_small_ops",t14_many_small_ops},
    {"t15_no_spurious_null_get",t15_no_spurious_null_get},
  };
  for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
    if (strcmp(t,cases[i].name)==0) return cases[i].fn();
  }
  fprintf(stderr,"unknown test: %s\n", t);
  return 2;
}
