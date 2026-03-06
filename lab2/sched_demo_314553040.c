#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <errno.h>
#include <time.h>

// Per-thread argument bundle passed to the worker
typedef struct {
  int id;                        // logical thread id [0..n-1]
  double time_wait;              // busy-wait seconds per loop
  pthread_barrier_t *bar;        // start barrier shared by all workers
} worker_arg_t;

/*
 * Busy-wait for 'sec' seconds of *thread CPU time*.
 * Uses CLOCK_THREAD_CPUTIME_ID so preempted intervals don't advance the clock.
 */
static void busy_wait_seconds(double sec) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t0);
  for (;;) {
    // Tiny useless work to prevent the loop from being optimized away
    for (volatile int i = 0; i < 1000; i++) {}
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t1);
    double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    if (elapsed >= sec) break;
  }
}

/*
 * Worker thread entry:
 * 1) wait at the barrier so all workers start together,
 * 2) repeat 3 times: print a line and busy-wait 'time_wait' seconds (CPU time).
 */
static void *worker(void *arg) {
  worker_arg_t *wa = (worker_arg_t*)arg;
  pthread_barrier_wait(wa->bar);                 // synchronize start
  for (int i = 0; i < 3; i++) {
    printf("Thread %d is running\n", wa->id);   // required output format
    fflush(stdout);                              // avoid buffering effects in diffs
    busy_wait_seconds(wa->time_wait);
  }
  return NULL;
}

/*
 * Parse CSV policies like "NORMAL,FIFO,..." into Linux policies:
 *   NORMAL -> SCHED_OTHER, FIFO -> SCHED_FIFO. Unknown -> SCHED_OTHER.
 * Length is capped at n.
 */
static void parse_csv_policies(const char *s, int *pol, int n) {
  char *tmp = strdup(s);                         // duplicate to safely tokenize
  char *tok = strtok(tmp, ",");
  int idx = 0;
  while (tok && idx < n) {
    if (strcmp(tok, "NORMAL") == 0) pol[idx] = SCHED_OTHER;
    else if (strcmp(tok, "FIFO") == 0) pol[idx] = SCHED_FIFO;
    else pol[idx] = SCHED_OTHER;
    idx++;
    tok = strtok(NULL, ",");
  }
  free(tmp);
}

/*
 * Parse CSV integers like "-1,10,..." into array 'arr' with length n.
 * Unparsed trailing entries are left as zero by caller's calloc.
 */
static void parse_csv_ints(const char *s, int *arr, int n) {
  char *tmp = strdup(s);
  char *tok = strtok(tmp, ",");
  int idx = 0;
  while (tok && idx < n) {
    arr[idx++] = atoi(tok);
    tok = strtok(NULL, ",");
  }
  free(tmp);
}

int main(int argc, char **argv) {
  int opt;
  int nthreads = 1;              // default threads if not provided
  double twait = 0.5;            // default busy-wait seconds
  char *spolicies = NULL, *sprios = NULL;

  // Parse options: -n N -t SEC -s CSV -p CSV
  while ((opt = getopt(argc, argv, "n:t:s:p:")) != -1) {
    if (opt == 'n') nthreads = atoi(optarg);
    else if (opt == 't') twait = atof(optarg);
    else if (opt == 's') spolicies = optarg;
    else if (opt == 'p') sprios = optarg;
  }
  if (!spolicies || !sprios) {
    fprintf(stderr, "usage: %s -n N -t SEC -s NORMAL|FIFO,... -p -1|PRIO,...\n", argv[0]);
    return 1;
  }

  // Allocate policy and priority arrays
  int *pol  = calloc(nthreads, sizeof(int));
  int *prio = calloc(nthreads, sizeof(int));
  parse_csv_policies(spolicies, pol, nthreads);
  parse_csv_ints(sprios, prio, nthreads);

  // Pin the *process* (main thread) to CPU0 to make order deterministic
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(0, &set);
  sched_setaffinity(0, sizeof(set), &set);

  // Prepare threads, per-thread args, and a barrier (count = nthreads)
  pthread_t *ths = malloc(sizeof(pthread_t) * nthreads);
  worker_arg_t *args = malloc(sizeof(worker_arg_t) * nthreads);
  pthread_barrier_t bar;
  pthread_barrier_init(&bar, NULL, nthreads);

  // Create each worker with explicit scheduling attributes and CPU affinity
  for (int i = 0; i < nthreads; i++) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);

    // Use attributes we set, not inherited defaults
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    // Policy: SCHED_OTHER (NORMAL) or SCHED_FIFO (real-time, no timeslice)
    pthread_attr_setschedpolicy(&attr, pol[i]);

    // Priority: only meaningful for FIFO (valid range 1..99)
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    if (pol[i] == SCHED_FIFO) {
      sp.sched_priority = prio[i];         // assume TA input is valid
    } else {
      sp.sched_priority = 0;               // ignored for SCHED_OTHER
    }
    pthread_attr_setschedparam(&attr, &sp);

    // Affinity: also pin this worker to CPU0
    pthread_attr_setaffinity_np(&attr, sizeof(set), &set);

    // Fill worker args
    args[i].id = i;
    args[i].time_wait = twait;
    args[i].bar = &bar;

    // Create thread
    int rc = pthread_create(&ths[i], &attr, worker, &args[i]);
    if (rc != 0) {
      // Common cause if running outside QEMU root or with RT throttling on
      fprintf(stderr, "pthread_create failed: %s\n", strerror(rc));
      return 2;
    }
    pthread_attr_destroy(&attr);
  }

  // Join all workers
  for (int i = 0; i < nthreads; i++) pthread_join(ths[i], NULL);

  // Cleanup
  pthread_barrier_destroy(&bar);
  free(ths); free(args); free(pol); free(prio);
  return 0;
}

