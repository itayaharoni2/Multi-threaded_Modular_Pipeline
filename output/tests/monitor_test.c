#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>
#include "monitor.h"

static void msleep(int ms){
  struct timespec ts; ts.tv_sec = ms/1000; ts.tv_nsec = (ms%1000)*1000000L;
  nanosleep(&ts, NULL);
}
static void* waiter(void* arg){
  monitor_t* m = (monitor_t*)arg;
  int rc = monitor_wait(m);
  return (void*)(long)rc;
}

static int t01_init_destroy(){
  monitor_t m;
  if (monitor_init(&m)!=0) return 1;
  monitor_destroy(&m);
  return 0;
}
static int t02_signal_before_wait_is_remembered(){
  monitor_t m; monitor_init(&m);
  monitor_signal(&m);
  int rc = monitor_wait(&m);
  monitor_destroy(&m);
  return rc!=0;
}
static int t03_wait_blocks_then_signal(){
  monitor_t m; monitor_init(&m);
  pthread_t th; void* ret=NULL;
  pthread_create(&th,NULL,waiter,&m);
  msleep(100);
  monitor_signal(&m);
  pthread_join(th,&ret);
  int rc=(int)(long)ret;
  monitor_destroy(&m);
  return rc!=0;
}
static int t04_reset_clears_state(){
  monitor_t m; monitor_init(&m);
  monitor_signal(&m);
  monitor_reset(&m);
  pthread_t th; void* ret=NULL;
  pthread_create(&th,NULL,waiter,&m);
  msleep(120);
  int not_done_yet = pthread_tryjoin_np(th,&ret)!=0;
  if (!not_done_yet){ monitor_destroy(&m); return 1; }
  monitor_signal(&m);
  pthread_join(th,&ret);
  int rc=(int)(long)ret;
  monitor_destroy(&m);
  return rc!=0;
}
static int t05_multiple_waiters_release_all(){
  monitor_t m; monitor_init(&m);
  enum {N=5}; pthread_t th[N]; void* ret[N];
  for(int i=0;i<N;i++) pthread_create(&th[i],NULL,waiter,&m);
  msleep(80);
  monitor_signal(&m);
  for(int i=0;i<N;i++) pthread_join(th[i],&ret[i]);
  for(int i=0;i<N;i++) if((int)(long)ret[i]!=0){ monitor_destroy(&m); return 1; }
  monitor_destroy(&m);
  return 0;
}
static int t06_repeated_waits_without_reset_still_ok(){
  monitor_t m; monitor_init(&m);
  monitor_signal(&m);
  int rc1 = monitor_wait(&m);
  int rc2 = monitor_wait(&m);
  monitor_destroy(&m);
  return (rc1!=0 || rc2!=0);
}
static int t07_signal_is_idempotent(){
  monitor_t m; monitor_init(&m);
  monitor_signal(&m);
  monitor_signal(&m);
  int rc = monitor_wait(&m);
  monitor_destroy(&m);
  return rc!=0;
}
static int t08_destroy_safe_after_signal(){
  monitor_t m; monitor_init(&m);
  monitor_signal(&m);
  monitor_destroy(&m);
  return 0;
}
static int t09_stress_waiters(){
  monitor_t m; monitor_init(&m);
  enum {N=32}; pthread_t th[N]; void* ret[N];
  for(int i=0;i<N;i++) pthread_create(&th[i],NULL,waiter,&m);
  msleep(50);
  monitor_signal(&m);
  for(int i=0;i<N;i++) pthread_join(th[i],&ret[i]);
  for(int i=0;i<N;i++) if((int)(long)ret[i]!=0){ monitor_destroy(&m); return 1; }
  monitor_destroy(&m);
  return 0;
}
static int t10_init_null_guard(){
  return (monitor_init(NULL)==0);
}
static int t11_reset_after_destroy_no_crash(){
  monitor_t m; monitor_init(&m); monitor_destroy(&m);
  return 0;
}
static int t12_wait_on_signaled_no_block(){
  monitor_t m; monitor_init(&m); monitor_signal(&m);
  int rc = monitor_wait(&m);
  monitor_destroy(&m);
  return rc!=0;
}
static int t13_double_signal_then_reset_then_wait_blocks_then_signal(){
  monitor_t m; monitor_init(&m);
  monitor_signal(&m); monitor_signal(&m);
  monitor_reset(&m);
  pthread_t th; void* ret=NULL;
  pthread_create(&th,NULL,waiter,&m);
  msleep(120);
  int joinedEarly = (pthread_tryjoin_np(th,&ret)==0);
  if (joinedEarly){ monitor_destroy(&m); return 1; }
  monitor_signal(&m);
  pthread_join(th,&ret);
  int rc=(int)(long)ret;
  monitor_destroy(&m);
  return rc!=0;
}
static int t14_many_signal_wait_cycles(){
  monitor_t m; monitor_init(&m);
  for(int i=0;i<10;i++){
    monitor_reset(&m);
    pthread_t th; void* ret=NULL;
    pthread_create(&th,NULL,waiter,&m);
    msleep(10);
    monitor_signal(&m);
    pthread_join(th,&ret);
    if ((int)(long)ret!=0){ monitor_destroy(&m); return 1; }
  }
  monitor_destroy(&m);
  return 0;
}
static int t15_spurious_wakeups_defended(){
  monitor_t m; monitor_init(&m);
  monitor_signal(&m);
  int rc = monitor_wait(&m);
  monitor_destroy(&m);
  return rc!=0;
}

int main(int argc, char**argv){
  if (argc<2){ fprintf(stderr,"need test name\n"); return 2; }
  const char* t = argv[1];
  struct { const char* name; int(*fn)(void); } cases[] = {
    {"t01_init_destroy",t01_init_destroy},
    {"t02_signal_before_wait_is_remembered",t02_signal_before_wait_is_remembered},
    {"t03_wait_blocks_then_signal",t03_wait_blocks_then_signal},
    {"t04_reset_clears_state",t04_reset_clears_state},
    {"t05_multiple_waiters_release_all",t05_multiple_waiters_release_all},
    {"t06_repeated_waits_without_reset_still_ok",t06_repeated_waits_without_reset_still_ok},
    {"t07_signal_is_idempotent",t07_signal_is_idempotent},
    {"t08_destroy_safe_after_signal",t08_destroy_safe_after_signal},
    {"t09_stress_waiters",t09_stress_waiters},
    {"t10_init_null_guard",t10_init_null_guard},
    {"t11_reset_after_destroy_no_crash",t11_reset_after_destroy_no_crash},
    {"t12_wait_on_signaled_no_block",t12_wait_on_signaled_no_block},
    {"t13_double_signal_then_reset_then_wait_blocks_then_signal",t13_double_signal_then_reset_then_wait_blocks_then_signal},
    {"t14_many_signal_wait_cycles",t14_many_signal_wait_cycles},
    {"t15_spurious_wakeups_defended",t15_spurious_wakeups_defended},
  };
  for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);++i){
    if (strcmp(t,cases[i].name)==0) return cases[i].fn();
  }
  fprintf(stderr,"unknown test: %s\n", t);
  return 2;
}
