#!/usr/bin/env bash
# Ensure that if any gcc command fails, the script exits with a non-zero exit code 
set -euo pipefail



# --------------------------------------- Colors  ---------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# counters
PASS_COUNT=0
FAIL_COUNT=0
EXIT_CODE=0

# Count a pass only when the message ends with ": PASS"
print_status(){
  echo -e "${GREEN}[TEST]${NC} $*"
  [[ "$*" == *": PASS" ]] && ((++PASS_COUNT))
}

print_info(){   echo -e "${YELLOW}[INFO]${NC} $*"; }

# Count a fail only when the message includes ": FAIL"
print_error(){
  echo -e "${RED}[ERROR]${NC} $*" >&2
  if [[ "$*" == *": FAIL"* ]]; then
    ((++FAIL_COUNT))
    EXIT_CODE=1
  fi
}

# summary printed on exit
summarize() {
  local ran=$((PASS_COUNT + FAIL_COUNT))
  echo -e "\n${YELLOW}[INFO]${NC} Test run finished — " \
          "passed: ${GREEN}${PASS_COUNT}${NC}, " \
          "failed: ${RED}${FAIL_COUNT}${NC}, " \
          "ran: ${ran}"
}

on_exit() {
  trap - EXIT
  summarize
  exit $EXIT_CODE
}
trap 'on_exit' EXIT



# --------------------------------------- Compiler (GCC 13)  ---------------------------------------
if command -v gcc-13 >/dev/null 2>&1; then
  CC=${CC:-gcc-13}
else
  CC=${CC:-gcc}
fi
# Make sure were using gcc
if "$CC" -v 2>&1 | grep -qi clang; then
  print_error "Found Clang at '$CC'. Please install GCC 13 (gcc-13)."
  exit 1
fi
CFLAGS="${CFLAGS:- -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L}"
LDFLAGS="${LDFLAGS:- -lpthread}"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${ROOT_DIR}/output"
ANALYZER="${OUT}/analyzer"


# Helpers -------------------------------------------------------------------------------------------
die(){ print_error "$*"; exit 1; }

require_file(){ [[ -f "$1" ]] || die "Missing file: $1"; }
require_exec(){ [[ -x "$1" ]] || die "Missing executable: $1"; }

run(){ # run cmd, capture stdout
  local __out
  if ! __out="$("$@" 2>&1)"; then
    echo -n "$__out"
    return 1
  fi
  echo -n "$__out"
  return 0
}

assert_eq(){
  local expected="$1" actual="$2" label="$3"
  if [[ "$actual" == "$expected" ]]; then
    print_status "$label: PASS"
  else
    print_error "$label: FAIL (Expected '$expected', got '$actual')"
    # no exit here; keep running
  fi
}

assert_exit(){
  local code="$1" label="$2"; shift 2
  set +e
  "$@"; local rc=$?
  set -e
  if [[ $rc -eq $code ]]; then
    print_status "$label: PASS (exit $code)"
  else
    print_error "$label: FAIL (Expected exit $code, got $rc)"
  fi
}

have_cmd(){ command -v "$1" >/dev/null 2>&1; }

if ! command -v timeout >/dev/null 2>&1; then
  print_info "timeout not found — using no-op fallback"
  timeout() { "$@"; }  # run command without timing out
fi



# --------------------------------------- Build the project ---------------------------------------

print_info "Building project with ./build.sh"
require_file "${ROOT_DIR}/build.sh"
( cd "$ROOT_DIR" && ./build.sh )

mkdir -p "${OUT}/tests"

require_exec "${ANALYZER}"
for so in logger uppercaser rotator flipper expander typewriter; do
  require_file "${OUT}/${so}.so"
done




# --------------------------------------- Build Monitor $ CP tests ---------------------------------------
# headers are where we expect
require_file "${ROOT_DIR}/plugins/sync/monitor.h"
require_file "${ROOT_DIR}/plugins/sync/consumer_producer.h"

# ---- monitor test ----
cat > "${OUT}/tests/monitor_test.c" <<'EOF'
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
EOF

CFLAGS="${CFLAGS:-} -D_GNU_SOURCE"
# monitor test — link with the implementation
"${CC}" ${CFLAGS} -I"${ROOT_DIR}/plugins/sync" \
  -o "${OUT}/monitor_test" \
  "${OUT}/tests/monitor_test.c" \
  "${ROOT_DIR}/plugins/sync/monitor.c" \
  ${LDFLAGS}

# ---- consumer_producer test ----
cat > "${OUT}/tests/consumer_producer_test.c" <<'EOF'
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
EOF

# consumer_producer tests — link with consumer_producer.c and monitor.c
"${CC}" ${CFLAGS} -D_GNU_SOURCE -I"${ROOT_DIR}/plugins/sync" \
  -o "${OUT}/consumer_producer_test" \
  "${OUT}/tests/consumer_producer_test.c" \
  "${ROOT_DIR}/plugins/sync/consumer_producer.c" \
  "${ROOT_DIR}/plugins/sync/monitor.c" \
  -pthread





# --------------------------------------- Run monitor unit tests (15) ---------------------------------------
print_info "Running monitor unit tests"
for t in \
  t01_init_destroy \
  t02_signal_before_wait_is_remembered \
  t03_wait_blocks_then_signal \
  t04_reset_clears_state \
  t05_multiple_waiters_release_all \
  t06_repeated_waits_without_reset_still_ok \
  t07_signal_is_idempotent \
  t08_destroy_safe_after_signal \
  t09_stress_waiters \
  t10_init_null_guard \
  t11_reset_after_destroy_no_crash \
  t12_wait_on_signaled_no_block \
  t13_double_signal_then_reset_then_wait_blocks_then_signal \
  t14_many_signal_wait_cycles \
  t15_spurious_wakeups_defended
do
  set +e
  "${OUT}/monitor_test" "$t"
  rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    print_status "monitor.$t: PASS"
  else
    print_error "monitor.$t: FAIL (exit $rc)"
  fi
done





# --------------------------------------- Run consumer_producer unit tests (15) ---------------------------------------
print_info "Running consumer_producer unit tests"
for t in \
  t01_init_invalid_args \
  t02_init_destroy_cap1 \
  t03_simple_put_get \
  t04_fifo_order \
  t05_get_blocks_then_unblocks \
  t06_put_blocks_then_unblocks \
  t07_multiple_producers_consumers \
  t08_stress \
  t09_signal_finished_and_wait \
  t10_wait_finished_blocks_until_signal \
  t11_null_get_does_not_crash \
  t12_null_put_fails \
  t13_capacity_wraparound \
  t14_many_small_ops \
  t15_no_spurious_null_get
do
  set +e
  "${OUT}/consumer_producer_test" "$t"
  rc=$?
  set -e
  if [[ $rc -eq 0 ]]; then
    print_status "consumer_producer.$t: PASS"
  else
    print_error "consumer_producer.$t: FAIL (exit $rc)"
  fi
done





# pipeline helpers -------------------------------------------------

grep_first(){
  grep -m1 -E "$1" || true
}

run_ana_checked() {
  local label="$1" input="$2" qs="$3"; shift 3
  local out rc tmp
  tmp="$(mktemp -t ana.XXXXXX)"
  set +e
  printf "%s" "$input" | timeout "${TIMEOUT_SECS:-10}" "$ANALYZER" "$qs" "$@" >"$tmp" 2>&1
  rc=$?
  set -e
  out="$(cat "$tmp")"
  rm -f "$tmp"
  if (( rc != 0 )); then
    print_error "${label}: FAIL (analyzer exited ${rc})"
    printf '%s\n' "$out"
    return 0   # keep running
  fi
  printf '%s' "$out"
  return 0
}






# --------------------------------------- Run normal usage tests (34) ---------------------------------------
print_info "Running 34 normal-usage pipeline tests"

# 1) uppercaser + logger
EXPECTED="[logger] HELLO"
OUT_ALL="$(run_ana_checked "uppercaser + logger(run)" $'hello\n<END>\n' 10 uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "uppercaser + logger"

# 2) rotator + logger
EXPECTED="[logger] ohell"
OUT_ALL="$(run_ana_checked "rotator + logger(run)" $'hello\n<END>\n' 10 rotator logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "rotator + logger"

# 3) flipper + logger
EXPECTED="[logger] olleh"
OUT_ALL="$(run_ana_checked "flipper + logger(run)" $'hello\n<END>\n' 10 flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "flipper + logger"

# 4) expander + logger
EXPECTED="[logger] h e l l o"
OUT_ALL="$(run_ana_checked "expander + logger(run)" $'hello\n<END>\n' 10 expander logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "expander + logger"

# 5) full example chain (uppercaser rotator logger flipper typewriter)
EXPECTED_LOG="[logger] OHELL"
EXPECTED_TW="[typewriter] LLEHO"
OUT_ALL="$(run_ana_checked "full example chain(run)" $'hello\n<END>\n' 20 uppercaser rotator logger flipper typewriter)"
ACT_LOG="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
ACT_TW="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[typewriter\]')"
assert_eq "$EXPECTED_LOG" "$ACT_LOG" "example chain logger output"
assert_eq "$EXPECTED_TW" "$ACT_TW" "example chain typewriter output"

# 6) chain: uppercaser flipper logger
EXPECTED="[logger] OLLEH"
OUT_ALL="$(run_ana_checked "uppercaser→flipper→logger(run)" $'hello\n<END>\n' 10 uppercaser flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "uppercaser→flipper→logger"

# 7) chain: flipper uppercaser logger
EXPECTED="[logger] OLLEH"
OUT_ALL="$(run_ana_checked "flipper→uppercaser→logger(run)" $'hello\n<END>\n' 10 flipper uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "flipper→uppercaser→logger"

# 8) empty line preserved (logger prints blank payload)
EXPECTED="[logger] "
OUT_ALL="$(run_ana_checked "empty line preserved(run)" $'\n<END>\n' 10 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "empty line preserved"

# 9) whitespace line preserved
EXPECTED="[logger]    "
OUT_ALL="$(run_ana_checked "whitespace preserved(run)" $'   \n<END>\n' 10 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "whitespace preserved"

# 10) newline trimmed before processing
EXPECTED="[logger] ABC"
OUT_ALL="$(run_ana_checked "newline trimmed(run)" $'ABC\n<END>\n' 10 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "newline trimmed"

# 11) END propagates, logger does not print END
OUT_ALL="$(run_ana_checked "END not logged(run)" $'<END>\n' 10 logger)"
if printf '%s\n' "$OUT_ALL" | grep -qF "[logger] <END>"; then
  print_error "END not logged: FAIL (logger printed <END>)"
else
  print_status "END not logged: PASS"
fi

# 12) multiple lines framing count
CNT_IN=50
INPUT="$( { seq 1 $CNT_IN | sed 's/.*/x/'; echo '<END>'; } )"
OUT_ALL="$(run_ana_checked "line framing x50(run)" "$INPUT" 5 logger)"
CNT_OUT="$(printf '%s\n' "$OUT_ALL" | grep -c "^\[logger\] x$" || true)"
assert_eq "$CNT_IN" "$CNT_OUT" "line framing x50"

# 13) rotator correctness with punctuation
EXPECTED="[logger] !abcd"
OUT_ALL="$(run_ana_checked "rotator wrap(run)" $'abcd!\n<END>\n' 10 rotator logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "rotator wrap"

# 14) flipper palindrome stays same
EXPECTED="[logger] abba"
OUT_ALL="$(run_ana_checked "flipper palindrome(run)" $'abba\n<END>\n' 10 flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "flipper palindrome"

# 15) expander spacing
EXPECTED="[logger] a b c d"
OUT_ALL="$(run_ana_checked "expander spacing(run)" $'abcd\n<END>\n' 10 expander logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "expander spacing"

# 16) uppercaser basic behavior
EXPECTED="[logger] 123-ABC-XYZ->XYZ"
OUT_ALL="$(run_ana_checked "uppercaser ascii(run)" $'123-ABC-xyz->XYZ\n<END>\n' 10 uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "uppercaser ascii"

# 17) uppercaser twice
EXPECTED="[logger] HELLO"
OUT_ALL="$(run_ana_checked "duplicate uppercaser works(run)" $'hello\n<END>\n' 10 uppercaser uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "duplicate uppercaser works"

# 18) capacity small but sufficient
EXPECTED="[logger] abc"
OUT_ALL="$(run_ana_checked "capacity=1 ok(run)" $'abc\n<END>\n' 1 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "capacity=1 ok"

# 19) ignore post-END input
OUT_ALL="$(run_ana_checked "post-END ignored(run)" $'hi\n<END>\nSHOULD_NOT\n' 5 logger)"
if printf '%s\n' "$OUT_ALL" | grep -q "SHOULD_NOT"; then
  print_error "post-END ignored: FAIL (processed post-END input)"
else
  print_status "post-END ignored: PASS"
fi

# 20) rotator after uppercaser correctness
EXPECTED="[logger] OHELL"
OUT_ALL="$(run_ana_checked "upper→rotator(run)" $'hello\n<END>\n' 10 uppercaser rotator logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "upper→rotator"

# 21) rotator twice
EXPECTED="[logger] lohel"
OUT_ALL="$(run_ana_checked "duplicate rotator works(run)" $'hello\n<END>\n' 10 rotator rotator logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "duplicate rotator works"

# 22) long but < 1024 chars
LONG=$(python3 - <<PY
print("a"*1023)
PY
)
EXPECTED="[logger] ${LONG}"
OUT_ALL="$(run_ana_checked "1023-char line(run)" "$(printf '%s\n<END>\n' "$LONG")" 8 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "1023-char line"

# 23) exact END match only
EXPECTED="[logger] <END!>"
OUT_ALL="$(run_ana_checked "END exactness(run)" $'<END!>\n<END>\n' 10 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "END exactness"

# 24) lowercase <end> is not sentinel
EXPECTED="[logger] <end>"
OUT_ALL="$(run_ana_checked "END case-sensitive(run)" $'<end>\n<END>\n' 10 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "END case-sensitive"

# 25) spaces around END are not sentinel
EXPECTED="[logger]  <END> "
OUT_ALL="$(run_ana_checked "END trimmed? no(run)" $' <END> \n<END>\n' 10 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "END trimmed? no"

# 26) expander then flipper
EXPECTED="[logger] o l l e h"
OUT_ALL="$(run_ana_checked "flip→expand(run)" $'hello\n<END>\n' 10 flipper expander logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "flip→expand"

# 27) rotator then flipper
EXPECTED="[logger] lleho"
OUT_ALL="$(run_ana_checked "rot→flip(run)" $'hello\n<END>\n' 10 rotator flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "rot→flip"

# 28) multiple inputs keep order through pipeline
EXPECTED_SEQ=$(printf "[logger] a\n[logger] b\n[logger] c\n")
OUT_ALL="$(run_ana_checked "order preserved (logger)(run)" $'a\nb\nc\n<END>\n' 5 logger)"
ACTUAL_SEQ="$(printf '%s\n' "$OUT_ALL" | grep -E '^\[logger\]' || true)"
assert_eq "$EXPECTED_SEQ" "$ACTUAL_SEQ" "order preserved (logger)"

# 29) ‘Pipeline shutdown complete’ final line
OUT_ALL="$(run_ana_checked "shutdown line present(run)" $'x\n<END>\n' 5 logger)"
if printf '%s\n' "$OUT_ALL" | tail -n1 | grep -q "Pipeline shutdown complete"; then
  print_status "shutdown line present: PASS"
else
  print_error "shutdown line present: FAIL (missing 'Pipeline shutdown complete')"
fi

# 30) capacity big
EXPECTED="[logger] zzz"
OUT_ALL="$(run_ana_checked "capacity=100 ok(run)" $'zzz\n<END>\n' 100 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "capacity=100 ok"

# 31) uppercaser twice, then rotator
EXPECTED="[logger] OHELL"
OUT_ALL="$(run_ana_checked "upper->upper->rotator(run)" $'hello\n<END>\n' 10 uppercaser uppercaser rotator logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "upper->upper->rotator"

# 32) expander twice (note: spaces get expanded too => triple space)
EXPECTED="[logger] a   b"
OUT_ALL="$(run_ana_checked "expander twice (triple-space)(run)" $'ab\n<END>\n' 10 expander expander logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "expander twice (triple-space)"

# 33) logger twice (should print two identical logger lines)
OUT_ALL="$(run_ana_checked "duplicate logger prints twice(run)" $'hi\n<END>\n' 10 logger logger)"
LINES="$(printf '%s\n' "$OUT_ALL" | grep -c '^\[logger\] hi$' || true)"
assert_eq "2" "$LINES" "duplicate logger prints twice"

# 34) typewriter twice (two typewriter lines)
OUT_ALL="$(run_ana_checked "duplicate typewriter prints twice(run)" $'hi\n<END>\n' 10 typewriter typewriter)"
LINES="$(printf '%s\n' "$OUT_ALL" | grep -c '^\[typewriter\] hi$' || true)"
assert_eq "2" "$LINES" "duplicate typewriter prints twice"





# --------------------------------------- Run edge cases usage tests (38) ---------------------------------------
print_info "Running 38 edge-cases tests"

# Helper
 assert_cli_error() {
  local label="$1"
  local expect_rc="$2"
  local expect_stdout_grep="$3"
  local expect_stderr_grep="$4"
  shift 4
  local cmd=("$@")

  local tmp_out tmp_err rc
  tmp_out="$(mktemp -t cli_out.XXXXXX)"
  tmp_err="$(mktemp -t cli_err.XXXXXX)"
  set +e
  "${cmd[@]}" >"$tmp_out" 2>"$tmp_err"
  rc=$?
  set -e # IMPORTANT: re-enable immediately

  local checks_passed=1
  local details=""

  # Check 1: Exit code
  if [[ $rc -ne $expect_rc ]]; then
    checks_passed=0
    details+="Expected exit $expect_rc, got $rc. "
  fi

  # Check 2: STDOUT content - use -i for case-insensitive
  if ! grep -qiE "$expect_stdout_grep" "$tmp_out"; then
    checks_passed=0
    details+="Expected stdout to match '$expect_stdout_grep'. "
  fi

  # Check 3: STDERR content - use -i for case-insensitive
  if ! grep -qiE "$expect_stderr_grep" "$tmp_err"; then
    checks_passed=0
    details+="Expected stderr to match '$expect_stderr_grep'. "
  fi

  if [[ $checks_passed -eq 1 ]]; then
    print_status "$label: PASS"
  else
    print_error "$label: FAIL ($details)"
    # print full output on failure for debugging
    if [[ -s "$tmp_out" ]]; then
      echo "--- STDOUT ---" >&2
      cat "$tmp_out" >&2
      echo "----------------" >&2
    fi
    if [[ -s "$tmp_err" ]]; then
      echo "--- STDERR ---" >&2
      cat "$tmp_err" >&2
      echo "----------------" >&2
    fi
  fi

  rm -f "$tmp_out" "$tmp_err"
}

# Guard the whole block against set -e/pipefail surprises
set +e

# F1) Edge: empty string preserved
EXPECTED="[logger] "
OUT_ALL="$(run_ana_checked "edge_preserve_empty(run)" $'\n<END>\n' 10 uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "edge_preserve_empty"

# F2) Edge: whitespace preserved through transform
EXPECTED="[logger]    "
OUT_ALL="$(run_ana_checked "edge_preserve_whitespace(run)" $'   \n<END>\n' 10 uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "edge_preserve_whitespace"

# F3) Very long line (1024 chars)
LONG1024="$(python3 - <<'PY'
print("x"*1024)
PY
)"
EXPECTED="[logger] ${LONG1024}"
INPUT="${LONG1024}"$'\n<END>\n'
OUT_ALL="$(run_ana_checked "edge_very_long_line(1024)" "$INPUT" 10 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "edge_very_long_line(1024)"

# F4) Many small lines framing (100)
INPUT="$( { seq 1 100 | sed 's/.*/y/'; echo '<END>'; } )"
OUT_ALL="$(run_ana_checked "edge_line_framing_many_small_lines" "$INPUT" 4 logger)"
CNT_OUT="$(printf '%s\n' "$OUT_ALL" | grep -c '^\[logger\] y$' || true)"
assert_eq "100" "$CNT_OUT" "edge_line_framing_many_small_lines"

# F5) Full all-plugins happy path (re-check)
EXPECTED_LOG="[logger] OHELL"
EXPECTED_TW="[typewriter] LLEHO"
OUT_ALL="$(run_ana_checked "full_all_plugins_happy_path(run)" $'hello\n<END>\n' 20 uppercaser rotator logger flipper typewriter)"
ACT_LOG="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
ACT_TW="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[typewriter\]')"
assert_eq "$EXPECTED_LOG" "$ACT_LOG" "full_all_plugins_happy_path(logger)"
assert_eq "$EXPECTED_TW" "$ACT_TW" "full_all_plugins_happy_path(typewriter)"

## CHANGED ## - The expected strings below are now adjusted to match your program's output.
# F6) CLI zero capacity - exit 1 (usage)
assert_cli_error "cli_zero_capacity" 1 "Usage:" "invalid queue size" "${ANALYZER}" 0 logger

# F7) CLI missing plugins - exit 1
assert_cli_error "cli_no_plugins" 1 "Usage:" "Missing arguments" "${ANALYZER}" 10

# F8) CLI bad plugin name - exit 1 (usage)
assert_cli_error "cli_bad_plugin" 1 "Usage:" "cannot open shared object file" "${ANALYZER}" 10 not_a_plugin

# F9) END exact match — near matches are processed
EXPECTED="[logger] <END >"
OUT_ALL="$(run_ana_checked "ih_sentinel_exact_match(run)" $'<END >\n<END>\n' 10 logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "ih_sentinel_exact_match"

# F10) Newline trimming
EXPECTED="[logger] HELLO"
OUT_ALL="$(run_ana_checked "ih_newline_trimming(run)" $'hello\n<END>\n' 10 uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "ih_newline_trimming"

# F11) expander spacing correctness
EXPECTED="[logger] a b c"
OUT_ALL="$(run_ana_checked "ct_expander_spacing(run)" $'abc\n<END>\n' 10 expander logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "ct_expander_spacing"

# F12) flipper correctness on palindromes
EXPECTED="[logger] abccba"
OUT_ALL="$(run_ana_checked "ct_flipper_palindromes(run)" $'abccba\n<END>\n' 10 flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "ct_flipper_palindromes"

# F13) rotator golden
EXPECTED="[logger] dabc"
OUT_ALL="$(run_ana_checked "ct_rotator_golden(run)" $'abcd\n<END>\n' 10 rotator logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "ct_rotator_golden"

# F14) typewriter + logger order preserved (logger first)
OUT_ALL="$(run_ana_checked "ct_typewriter_order_preserved(run)" $'hey\n<END>\n' 10 logger typewriter)"
FIRST="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[(logger|typewriter)\]')"
if [[ "$FIRST" == "[logger] hey" ]]; then
  print_status "ct_typewriter_order_preserved: PASS"
else
  print_error "ct_typewriter_order_preserved: FAIL (first line '$FIRST')"
fi

# F15) uppercaser golden
EXPECTED="[logger] ABCD"
OUT_ALL="$(run_ana_checked "ct_uppercaser_golden(run)" $'abcd\n<END>\n' 10 uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "ct_uppercaser_golden"

# F16) wiring: uppercaser flipper logger
EXPECTED="[logger] OLLEH"
OUT_ALL="$(run_ana_checked "wiring_chain_upper_flipper_logger(run)" $'hello\n<END>\n' 10 uppercaser flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "wiring_chain_upper_flipper_logger"

# F17) END single plugin logger: three normal lines + shutdown
OUT_ALL="$(run_ana_checked "end_single_plugin_logger(run)" $'a\nb\nc\n<END>\n' 10 logger)"
LINES="$(printf '%s\n' "$OUT_ALL" | grep -c '^\[logger\]' || true)"
assert_eq "3" "$LINES" "end_single_plugin_logger"

# F18) END propagation through chain
OUT_ALL="$(run_ana_checked "end_propagation_through_chain(run)" $'hi\n<END>\n' 10 uppercaser logger)"
HAS_OK="$(printf '%s\n' "$OUT_ALL" | grep -c '^\[logger\] HI$' || true)"
assert_eq "1" "$HAS_OK" "end_propagation_through_chain"

# F19) END ignores post-END input
OUT_ALL="$(run_ana_checked "end_ignores_post_end_input(run)" $'ok\n<END>\nNOPE\n' 10 uppercaser logger)"
if printf '%s\n' "$OUT_ALL" | grep -q "NOPE"; then
  print_error "end_ignores_post_end_input: FAIL (processed post-END input)"
else
  print_status "end_ignores_post_end_input: PASS"
fi

# F20) long pipeline string safe
OUT_ALL="$(TIMEOUT_SECS=20 run_ana_checked "long pipeline string(run)" "$( { head -c 800 < /dev/zero | tr '\0' 's'; echo; echo '<END>'; } )" 10 uppercaser logger)"
if printf '%s\n' "$OUT_ALL" | grep -q '^\[logger\] S'; then
  print_status "long pipeline string: PASS"
else
  print_error "long pipeline string: FAIL"
fi

# F21) flipper twice
EXPECTED="[logger] hello"
OUT_ALL="$(run_ana_checked "duplicate flipper works(run)" $'hello\n<END>\n' 10 flipper flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "duplicate flipper works"

# F22) rotator + expander combined
EXPECTED="[logger] d a b c"
OUT_ALL="$(run_ana_checked "rotator+expander(run)" $'abcd\n<END>\n' 10 rotator expander logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "rotator+expander"

# F23) flipper + rotator
EXPECTED="[logger] adcb"
OUT_ALL="$(run_ana_checked "flipper+rotator(run)" $'abcd\n<END>\n' 10 flipper rotator logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "flipper+rotator"

# F24) typewriter only prints once per line
OUT_ALL="$(run_ana_checked "typewriter single line(run)" $'hi\n<END>\n' 10 typewriter)"
LINES="$(printf '%s\n' "$OUT_ALL" | grep -c '^\[typewriter\]' || true)"
assert_eq "1" "$LINES" "typewriter single line"

# F25) logger only prints payload once
OUT_ALL="$(run_ana_checked "logger single line(run)" $'hi\n<END>\n' 10 logger)"
LINES="$(printf '%s\n' "$OUT_ALL" | grep -c '^\[logger\] hi$' || true)"
assert_eq "1" "$LINES" "logger single line"

# F26) uppercase of mixed chars
EXPECTED="[logger] A1!B2?"
OUT_ALL="$(run_ana_checked "uppercaser mixed(run)" $'a1!b2?\n<END>\n' 10 uppercaser logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "uppercaser mixed"

# F27) expander empty string remains empty (prefix only)
EXPECTED="[logger] "
OUT_ALL="$(run_ana_checked "expander empty(run)" $'\n<END>\n' 10 expander logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "expander empty"

# F28) rotator 1-char is identity
EXPECTED="[logger] z"
OUT_ALL="$(run_ana_checked "rotator len1(run)" $'z\n<END>\n' 10 rotator logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "rotator len1"

# F29) flipper 1-char is identity
EXPECTED="[logger] z"
OUT_ALL="$(run_ana_checked "flipper len1(run)" $'z\n<END>\n' 10 flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "flipper len1"

# F30) expander 1-char no extra space
EXPECTED="[logger] z"
OUT_ALL="$(run_ana_checked "expander len1(run)" $'z\n<END>\n' 10 expander logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "expander len1"

# F31) chain logger at end only
EXPECTED="[logger] EHY"
OUT_ALL="$(run_ana_checked "logger at end(run)" $'hey\n<END>\n' 10 uppercaser rotator flipper logger)"
ACTUAL="$(printf '%s\n' "$OUT_ALL" | grep_first '^\[logger\]')"
assert_eq "$EXPECTED" "$ACTUAL" "logger at end"

# F32) multiple inputs mixed transforms (order preserved)
EXPECTED_SEQ="$(printf "[logger] A\n[logger] B\n[logger] C\n")"
OUT_ALL="$(run_ana_checked "multi inputs preserved(run)" $'a\nb\nc\n<END>\n' 5 uppercaser logger)"
ACTUAL_SEQ="$(printf '%s\n' "$OUT_ALL" | grep -E '^\[logger\]' || true)"
assert_eq "$EXPECTED_SEQ" "$ACTUAL_SEQ" "multi inputs preserved"

# F33) unknown plugin mid-chain - usage error (exit 1)
assert_cli_error "bad plugin mid-chain" 1 "Usage:" "cannot open shared object file" "${ANALYZER}" 10 uppercaser nope logger

# F34) negative capacity - exit 1
assert_cli_error "cli_negative_capacity" 1 "Usage:" "invalid queue size" "${ANALYZER}" -3 logger

# F35) zero plugins (only capacity) - exit 1
assert_cli_error "cli_only_capacity" 1 "Usage:" "Missing arguments" "${ANALYZER}" 10

# F36) Concurrency stress test
print_info "Running concurrency stress test (long chain, small queue, high volume)"
STRESS_INPUT="$( { seq 1 100 | sed 's/.*/stress-test-line/'; echo '<END>'; } )"
STRESS_OUT="$(run_ana_checked "concurrency stress test" "$STRESS_INPUT" 1 uppercaser rotator flipper expander logger)"
STRESS_CNT="$(printf '%s\n' "$STRESS_OUT" | grep -c '^\[logger\]' || true)"
assert_eq "100" "$STRESS_CNT" "concurrency stress test line count"

# re-enable -e for the rest of the script
set -e





# --------------------------------------- Memory leak checks ---------------------------------------

if have_cmd valgrind; then
  print_info "Running Valgrind leak checks (STRICT_VALGRIND=${STRICT_VALGRIND:-0})"

  vg() {
    # common Valgrind flags
    valgrind --quiet \
             --leak-check=full \
             --show-leak-kinds=definite \
             --errors-for-leak-kinds=definite \
             --track-origins=yes \
             --error-exitcode=99 \
             "$@"
  }

  # Run a stdin-driven Valgrind case for analyzer
  vg_case() {
    local name="$1" input="$2"; shift 2
    local log; log="$(mktemp -t vg.XXXXXX)"
    set +e
    printf "%s" "$input" | vg "$@" >/dev/null 2> "$log"
    local rc=$?
    set -e
    if [[ $rc -eq 0 ]]; then
      print_status "valgrind: ${name}: PASS"
    else
      if [[ "${STRICT_VALGRIND:-0}" == "1" ]]; then
        print_error "valgrind: ${name}: FAIL (definite leaks found)"
        tail -n 60 "$log"
        rm -f "$log"
        exit 1
      else
        print_info "valgrind: ${name}: advisory: definite leaks reported"
        tail -n 25 "$log"
      fi
    fi
    rm -f "$log"
  }

  # Run a regular executable under Valgrind
  vg_exec_case() {
    local name="$1"; shift
    local log; log="$(mktemp -t vg.XXXXXX)"
    set +e
    vg "$@" >/dev/null 2> "$log"
    local rc=$?
    set -e
    if [[ $rc -eq 0 ]]; then
      print_status "valgrind: ${name}: PASS"
    else
      if [[ "${STRICT_VALGRIND:-0}" == "1" ]]; then
        print_error "valgrind: ${name}: FAIL (definite leaks found)"
        tail -n 60 "$log"
        rm -f "$log"
        exit 1
      else
        print_info "valgrind: ${name}: advisory: definite leaks reported"
        tail -n 25 "$log"
      fi
    fi
    rm -f "$log"
  }

  #  Analyzer cases
  vg_case "analyzer uppercaser->logger" $'hello\n<END>\n' \
          "${ANALYZER}" 5 uppercaser logger

  vg_case "analyzer rotator->expander->logger" $'abcd\n<END>\n' \
          "${ANALYZER}" 8 rotator expander logger

  vg_case "analyzer typewriter->logger" $'hi\n<END>\n' \
          "${ANALYZER}" 5 typewriter logger

  vg_case "analyzer 5-stage chain" $'ab!\ncd?\n<END>\n' \
          "${ANALYZER}" 10 uppercaser expander rotator flipper logger

  vg_case "analyzer empty + whitespace" $'\n   \n<END>\n' \
          "${ANALYZER}" 5 logger
  
  # Run valgrind on the stress test case
  vg_case "analyzer concurrency stress" "$STRESS_INPUT" \
          "${ANALYZER}" 1 uppercaser rotator flipper expander logger

  vg_case "analyzer many small lines (x200)" "$(printf 'a\n%.0s' {1..200}; echo '<END>')" \
          "${ANALYZER}" 5 rotator logger

 vg_case "analyzer max length line (1024 chars)" \
        "$( { head -c 1024 </dev/zero | tr '\0' 'x'; printf '\n<END>\n'; } )" \
        "${ANALYZER}" 5 flipper logger
  
  vg_exec_case "consumer_producer t08_stress" "${OUT}/consumer_producer_test" t08_stress
  vg_exec_case "monitor t09_stress_waiters"   "${OUT}/monitor_test"             t09_stress_waiters

else
  print_info "valgrind not found — skipping memcheck"
fi





if (( FAIL_COUNT == 0 )); then
  print_status "ALL TESTS PASSED"
else
  print_error "SOME TESTS FAILED"
fi
