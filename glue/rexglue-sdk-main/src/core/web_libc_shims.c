/**
 * @file        core/web_libc_shims.c
 * @brief       [web port] Linux thread extensions missing from Emscripten
 *
 * The POSIX threading code uses CPU affinity and thread names, which
 * Emscripten's libc declares but does not implement. A browser schedules
 * workers itself, so affinity is reported as "every CPU" and changes to it
 * (and thread names) are accepted and ignored.
 *
 * @license     BSD 3-Clause License
 */

#define _GNU_SOURCE
#include <emscripten/threading.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>

static void FillAllCpus(size_t size, cpu_set_t* set) {
  memset(set, 0, size);
  int cpus = emscripten_num_logical_cores();
  for (int i = 0; i < cpus && (size_t)i < size * 8; i++) {
    ((unsigned char*)set)[i / 8] |= (unsigned char)(1u << (i % 8));
  }
}

int sched_getaffinity(pid_t pid, size_t size, cpu_set_t* set) {
  (void)pid;
  FillAllCpus(size, set);
  return 0;
}

int __sched_cpucount(size_t size, const cpu_set_t* set) {
  int count = 0;
  for (size_t i = 0; i < size; i++) {
    count += __builtin_popcount(((const unsigned char*)set)[i]);
  }
  return count;
}

int pthread_getaffinity_np(pthread_t thread, size_t size, cpu_set_t* set) {
  (void)thread;
  FillAllCpus(size, set);
  return 0;
}

int pthread_setaffinity_np(pthread_t thread, size_t size, const cpu_set_t* set) {
  (void)thread;
  (void)size;
  (void)set;
  return 0;
}

int pthread_setname_np(pthread_t thread, const char* name) {
  (void)thread;
  (void)name;
  return 0;
}
