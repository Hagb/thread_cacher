#define _GNU_SOURCE
#include "hashtable.h"
#include "queue.h"
#include <assert.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#define SIGKILL_REPLACE_SIG (56)

static int (*pthread_create_)(pthread_t *thread, const pthread_attr_t *attr,
                              void *(*start_routine)(void *), void *arg);
static int (*pthread_join_)(pthread_t thread, void **retval);
static int (*pthread_detach_)(pthread_t thread);
static int (*pthread_kill_)(pthread_t thread, int signo);
static void (*pthread_exit_)(void **retval);
// static int (*pthread_sigmask_)(int how, const sigset_t *set, sigset_t
// *oldset);

static size_t pagesize;
// static int (*pthread_sigmask_)(pthread_t thread, int signo);

static pthread_t main_thread;

static hashtable_t *idle_threadss;
static hashtable_t *busy_threads;
typedef struct {
  pthread_t thread;
  pthread_attr_t *attr;
  void *(*start_routine)(void *);
  void *arg;
  _Bool sig_inited;
  _Bool returned;
  _Bool joined;
  _Bool joined_exit;
  void *retval;
  queue_t *idle_threads;
  pthread_cond_t sig_init_cond;
  pthread_cond_t start_cond;
  pthread_cond_t return_cond;
  pthread_cond_t join_finish_cond;
  pthread_mutex_t after_returned_lock;
  pthread_mutex_t sig_init_lock;
  pthread_mutex_t lock;
  pthread_mutex_t join_lock;
  pthread_mutex_t join_finish_lock;
} thread_info_t;

static void __attribute__((constructor)) init(void) {
  pthread_create_ = dlsym(RTLD_NEXT, "pthread_create");
  pthread_join_ = dlsym(RTLD_NEXT, "pthread_join");
  pthread_kill_ = dlsym(RTLD_NEXT, "pthread_kill");
  pthread_exit_ = dlsym(RTLD_NEXT, "pthread_exit");
  pthread_detach_ = dlsym(RTLD_NEXT, "pthread_detach_");
  // pthread_sigmask_ = dlsym(RTLD_NEXT, "pthread_sigmask");
  idle_threadss = ht_create(0, 4096, NULL);
  busy_threads = ht_create(0, 4096, NULL);
  main_thread = pthread_self();
  pagesize = getpagesize();
}

static int set_busy(thread_info_t *info) {
  return ht_set(busy_threads, &info->thread, sizeof(pthread_t), info,
                sizeof(thread_info_t));
}

static int unset_busy(thread_info_t *info) {
  return ht_delete(busy_threads, &info->thread, sizeof(pthread_t), NULL, NULL);
}

static thread_info_t *get_busy(pthread_t thread) {
  return ht_get(busy_threads, &thread, sizeof(pthread_t), NULL);
}

_Thread_local thread_info_t *thread_local_info;
_Thread_local jmp_buf exit_jmp;

void *thread_wrapper(void *arg) {
  thread_local_info = arg;
  assert(pthread_self() == thread_local_info->thread);
  _Bool long_jmped = 0;
  pthread_mutex_lock(&thread_local_info->sig_init_lock);
  thread_local_info->sig_inited = 1;

  switch (sigsetjmp(exit_jmp, 1)) {
  case 0:
    pthread_cond_broadcast(&thread_local_info->sig_init_cond);
    pthread_mutex_unlock(&thread_local_info->sig_init_lock);
    break;
  case 2:
    thread_local_info->returned = 1;
    thread_local_info->joined = 1;
    thread_local_info->joined_exit = 1;
  case 1:
    long_jmped = 1;
  }

  //   printf("warpper!\n");
  while (1) {
    if (thread_local_info->start_routine && !long_jmped)
      thread_local_info->retval =
          (thread_local_info->start_routine)(thread_local_info->arg);
    long_jmped = 0;

    pthread_mutex_lock(&thread_local_info->after_returned_lock);

    pthread_mutex_lock(&thread_local_info->lock);
    thread_local_info->sig_inited = 1;
    thread_local_info->returned = 1;
    pthread_cond_broadcast(&thread_local_info->return_cond);
    pthread_mutex_unlock(&thread_local_info->lock);

    pthread_mutex_lock(&thread_local_info->join_finish_lock);
    while (!thread_local_info->joined_exit)
      pthread_cond_wait(&thread_local_info->join_finish_cond,
                        &thread_local_info->join_finish_lock);
    thread_local_info->start_routine = NULL;
    unset_busy(thread_local_info);
    queue_push_left(thread_local_info->idle_threads, thread_local_info);
    pthread_mutex_unlock(&thread_local_info->join_finish_lock);

    pthread_mutex_lock(&thread_local_info->lock);
    while (!thread_local_info->start_routine) {
      pthread_cond_wait(&thread_local_info->start_cond,
                        &thread_local_info->lock);
    }
    pthread_mutex_unlock(&thread_local_info->lock);

    pthread_mutex_unlock(&thread_local_info->after_returned_lock);
  }
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start_routine)(void *), void *arg) {
  //   printf("Create!\n");

  pthread_attr_t *attr2 = malloc(sizeof(pthread_attr_t));
  if (!attr) {
    pthread_getattr_default_np(attr2);
  } else {
    *attr2 = *attr;
  }

  size_t stacksize = PTHREAD_STACK_MIN;
  pthread_attr_getstacksize(attr2, &stacksize);
  size_t stack_blocks = (stacksize - 1) / pagesize + 1;
  pthread_attr_setstacksize(attr2, stack_blocks * pagesize);
  // printf("%lu", stack_blocks * pagesize);
  int detachstate = PTHREAD_CREATE_JOINABLE;
  assert(!pthread_attr_getdetachstate(attr2, &detachstate));

  queue_t *idle_threads =
      ht_get(idle_threadss, &stack_blocks, sizeof(size_t), NULL);
  thread_info_t *info = NULL;
  if (idle_threads) {
    if (queue_count(idle_threads) > 6) {
      info = queue_pop_right(idle_threads);
      if (info != NULL) {
        pthread_mutex_lock(&info->lock);
        cpu_set_t cpu_set;
        pthread_attr_getaffinity_np(attr2, sizeof(cpu_set_t), &cpu_set);
        pthread_setaffinity_np(info->thread, sizeof(cpu_set_t), &cpu_set);
        struct sched_param schedparam;
        int policy;
        pthread_attr_getschedpolicy(attr2, &policy);
        pthread_attr_getschedparam(attr2, &schedparam);
        pthread_setschedparam(info->thread, policy, &schedparam);
        info->arg = arg;
        free(info->attr);
        info->attr = attr2;
        info->joined = 0;
        info->joined_exit = 0;
        info->returned = 0;
        set_busy(info);
        info->start_routine = start_routine;
        pthread_cond_signal(&info->start_cond);
        pthread_mutex_unlock(&info->lock);
        *thread = info->thread;
        if (detachstate == PTHREAD_CREATE_DETACHED) {
          // printf("detach!!\n");
          pthread_detach(info->thread);
        }
        return 0;
      }
    }
  } else {
    idle_threads = queue_create();
    ht_set(idle_threadss, &stack_blocks, sizeof(size_t), idle_threads,
           sizeof(void *));
  }

  info = malloc(sizeof(thread_info_t));
  info->idle_threads = idle_threads;
  info->start_routine = start_routine;
  info->arg = arg;
  info->sig_inited = 0;
  info->returned = 0;
  info->joined = 0;
  info->joined_exit = 0;
  info->attr = attr2;
  pthread_cond_init(&info->sig_init_cond, NULL);
  pthread_cond_init(&info->return_cond, NULL);
  pthread_cond_init(&info->start_cond, NULL);
  pthread_cond_init(&info->join_finish_cond, NULL);
  pthread_mutex_init(&info->after_returned_lock, NULL);
  pthread_mutex_init(&info->sig_init_lock, NULL);
  pthread_mutex_init(&info->lock, NULL);
  pthread_mutex_init(&info->join_lock, NULL);
  pthread_mutex_init(&info->join_finish_lock, NULL);
  pthread_mutex_lock(&info->sig_init_lock);
  int ret = pthread_create_(&info->thread, attr, thread_wrapper, info);
  if (ret) {
    pthread_mutex_unlock(&info->sig_init_lock);
    free(info);
    free(attr2);
    // printf("error!%d\n", ret);
    return ret;
  }
  set_busy(info);
  pthread_mutex_unlock(&info->sig_init_lock);
  *thread = info->thread;
  if (detachstate == PTHREAD_CREATE_DETACHED)
    pthread_detach(info->thread);
  return 0;
}

static void print_thread_id(pthread_t id) {
  size_t i;
  for (i = sizeof(i); i; --i)
    printf("%02x", *(((unsigned char *)&id) + i - 1));
}

int pthread_join(pthread_t thread, void **retval) {
  if (thread == main_thread) {
    // printf("Join main thread!\n");
    return pthread_join_(thread, retval);
  }
  thread_info_t *info = get_busy(thread);
  if (!info) {
    // printf("Join no thread\n");
    return ESRCH;
  }
  assert(info->thread == thread);
  if (pthread_mutex_lock(&info->join_lock)) {
    // printf("lock error!\n");
    return info->returned ? ESRCH : EINVAL;
  }
  if (info->joined) {
    // printf("join error!\n");
    pthread_mutex_unlock(&info->join_lock);
    return info->returned ? ESRCH : EINVAL;
  }
  info->joined = 1;
  pthread_mutex_unlock(&info->join_lock);

  pthread_mutex_lock(&info->lock);
  while (!info->returned)
    pthread_cond_wait(&info->return_cond, &info->lock);
  pthread_mutex_unlock(&info->lock);

  pthread_mutex_lock(&info->join_finish_lock);
  if (retval)
    *retval = info->retval;
  info->joined_exit = 1;
  pthread_cond_signal(&info->join_finish_cond);
  pthread_mutex_unlock(&info->join_finish_lock);
  return 0;
}

int pthread_detach(pthread_t thread) {
  // printf("detach!!\n");
  // assert(0);
  if (thread == main_thread) {
    // printf("Detach main thread!\n");
    return pthread_detach_(thread);
  }
  thread_info_t *info = get_busy(thread);
  if (!info) {
    // printf("Detach no thread\n");
    return ESRCH;
  }
  assert(info->thread == thread);
  if (pthread_mutex_lock(&info->join_lock)) {
    // printf("lock error!\n");
    return info->returned ? ESRCH : EINVAL;
  }
  if (info->joined) {
    // printf("join error!\n");
    pthread_mutex_unlock(&info->join_lock);
    return info->returned ? ESRCH : EINVAL;
  }
  info->joined = 1;
  pthread_mutex_unlock(&info->join_lock);

  pthread_mutex_lock(&info->join_finish_lock);
  info->joined_exit = 1;
  pthread_cond_signal(&info->join_finish_cond);
  pthread_mutex_unlock(&info->join_finish_lock);
  return 0;
}

void pthread_exit(void *retval) {
  if (pthread_self() == main_thread) {
    pthread_exit_(retval);
  } else {
    thread_local_info->retval = retval;
    siglongjmp(exit_jmp, 1);
  }
  assert(0);
}

int pthread_kill(pthread_t thread, int signo) {
  if (signo || (thread == main_thread)) {
    return pthread_kill_(thread, signo);
  }

  thread_info_t *info = get_busy(thread);
  return (info && !info->returned) ? 0 : ESRCH;
}
