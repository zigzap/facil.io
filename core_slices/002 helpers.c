/* *****************************************************************************
Internal helpers
***************************************************************************** */
#ifndef H_FACIL_IO_H /* development sugar, ignore */
#include "999 dev.h" /* development sugar, ignore */
#endif               /* development sugar, ignore */

#define FIO_QUEUE
#define FIO_SOCK /* should be public? */
#define FIO_STREAM
#include "fio-stl.h"

/* *****************************************************************************
Polling Helpers
***************************************************************************** */

#if !defined(FIO_ENGINE_EPOLL) && !defined(FIO_ENGINE_KQUEUE) &&               \
    !defined(FIO_ENGINE_POLL)
#if defined(HAVE_EPOLL) || __has_include("sys/epoll.h")
#define FIO_ENGINE_EPOLL 1
#elif defined(HAVE_KQUEUE) || __has_include("sys/event.h")
#define FIO_ENGINE_KQUEUE 1
#else
#define FIO_ENGINE_POLL 1
#endif
#endif /* !defined(FIO_ENGINE_EPOLL) ... */

FIO_IFUNC void fio_uuid_monitor_close(void);
FIO_IFUNC void fio_uuid_monitor_init(void);
FIO_SFUNC size_t fio_uuid_monitor_review(void);
FIO_IFUNC void fio_uuid_monitor_add(fio_uuid_s *uuid);
FIO_IFUNC void fio_uuid_monitor_add_read(fio_uuid_s *uuid);
FIO_IFUNC void fio_uuid_monitor_add_write(fio_uuid_s *uuid);
FIO_IFUNC void fio_uuid_monitor_remove(fio_uuid_s *uuid);

/* *****************************************************************************
Queue Helpers
***************************************************************************** */
static fio_queue_s tasks_io_core = FIO_QUEUE_STATIC_INIT(tasks_io_core);
static fio_queue_s tasks_user = FIO_QUEUE_STATIC_INIT(tasks_user);

FIO_IFUNC fio_queue_s *fio_queue_select(uintptr_t flag) {
  fio_queue_s *queues[] = {
      &tasks_user,
      &tasks_io_core,
  };
  return queues[(flag & 1)];
}

/* *****************************************************************************
ENV data maps
***************************************************************************** */

/** An object that can be linked to any facil.io connection (uuid). */
typedef struct {
  void (*on_close)(void *data);
  void *udata;
  uintptr_t flags;
} fio_uuid_env_obj_s;

/* cleanup event task */
static void fio_uuid_env_obj_call_callback_task(void *p, void *udata) {
  union {
    void (*fn)(void *);
    void *p;
  } u;
  u.fn(udata);
  u.p = p;
}

/* cleanup event scheduling */
FIO_IFUNC void fio_uuid_env_obj_call_callback(fio_uuid_env_obj_s o) {
  union {
    void (*fn)(void *);
    void *p;
  } u;
  if (o.on_close) {
    u.fn = o.on_close;
    fio_queue_push_urgent(fio_queue_select(o.flags),
                          fio_uuid_env_obj_call_callback_task,
                          u.p,
                          o.udata);
  }
}

/* for storing env string keys */
#define FIO_STR_SMALL fio_str
#include "fio-stl.h"

#define FIO_UMAP_NAME              fio___uuid_env
#define FIO_MAP_TYPE               fio_uuid_env_obj_s
#define FIO_MAP_TYPE_DESTROY(o)    fio_uuid_env_obj_call_callback((o))
#define FIO_MAP_DESTROY_AFTER_COPY 0

/* destroy discarded keys when overwriting existing data (const_name support) */
#define FIO_MAP_KEY                 fio_str_s /* the small string type */
#define FIO_MAP_KEY_COPY(dest, src) (dest) = (src)
#define FIO_MAP_KEY_DESTROY(k)      fio_str_destroy(&k)
#define FIO_MAP_KEY_DISCARD         FIO_MAP_KEY_DESTROY
#define FIO_MAP_KEY_CMP(a, b)       fio_str_is_eq(&(a), &(b))
#include <fio-stl.h>

/* *****************************************************************************
UUID validity, needed?
***************************************************************************** */

#define FIO_UMAP_NAME          fio_uuid_validity_map
#define FIO_MAP_TYPE           fio_uuid_s *
#define FIO_MAP_TYPE           fio_uuid_s *
#define FIO_MAP_HASH_FN(o)     fio_risky_ptr(o)
#define FIO_MAP_TYPE_CMP(a, b) ((a) == (b))
#include <fio-stl.h>
static fio_uuid_validity_map_s fio_uuid_validity_map = FIO_MAP_INIT;

FIO_IFUNC void fio_uuid_set_valid(fio_uuid_s *uuid) {
  fio_uuid_validity_map_set(&fio_uuid_validity_map,
                            fio_risky_ptr(uuid),
                            uuid,
                            NULL);
}

FIO_IFUNC void fio_uuid_set_invalid(fio_uuid_s *uuid) {
  fio_uuid_validity_map_remove(&fio_uuid_validity_map,
                               fio_risky_ptr(uuid),
                               uuid,
                               NULL);
}

FIO_IFUNC fio_uuid_s *fio_uuid_is_valid(fio_uuid_s *uuid) {
  return fio_uuid_validity_map_get(&fio_uuid_validity_map,
                                   fio_risky_ptr(uuid),
                                   uuid);
}

FIO_IFUNC void fio_uuid_invalidate_all() {
  fio_uuid_validity_map_destroy(&fio_uuid_validity_map);
}

/* *****************************************************************************
Global State
***************************************************************************** */

static struct {
  FIO_LIST_HEAD protocols;
  fio_thread_mutex_t env_lock;
  fio___uuid_env_s env;
  struct timespec tick;
  struct {
    int in;
    int out;
  } thread_suspenders, io_wake;
  fio_uuid_s *io_wake_uuid;
#if FIO_OS_WIN
  HANDLE master;
#else
  pid_t master;
#endif
  uint16_t threads;
  uint16_t workers;
  uint8_t is_master;
  uint8_t is_worker;
  volatile uint8_t running;
} fio_data = {
    .thread_suspenders = {-1, -1},
    .io_wake = {-1, -1},
    .env_lock = FIO_THREAD_MUTEX_INIT,
    .is_master = 1,
    .is_worker = 1,
};

FIO_IFUNC void fio_reset_wakeup_pipes(int *i) {
  close(i[0]);
  close(i[1]);
  FIO_ASSERT(!pipe(i),
             "%d - couldn't initiate thread wakeup pipes.",
             (int)getpid());
  fio_sock_set_non_block(i[0]);
  fio_sock_set_non_block(i[1]);
}

FIO_DESTRUCTOR(fio_cleanup_at_exit) {
  fio_state_callback_force(FIO_CALL_AT_EXIT);
  fio_thread_mutex_destroy(&fio_data.env_lock);
  fio___uuid_env_destroy(&fio_data.env);
  fio_data.protocols = FIO_LIST_INIT(fio_data.protocols);
  fio_uuid_monitor_close();
  if (fio_data.io_wake_uuid) {
    fio_uuid_free(fio_data.io_wake_uuid);
    fio_data.io_wake_uuid = NULL;
  }
  while (fio_queue_count(&tasks_io_core) + fio_queue_count(&tasks_user)) {
    fio_queue_perform_all(&tasks_io_core);
    fio_queue_perform_all(&tasks_user);
  }
  close(fio_data.thread_suspenders.in);
  close(fio_data.thread_suspenders.out);
  close(fio_data.io_wake.in);
  close(fio_data.io_wake.out);
  fio_data.thread_suspenders.in = -1;
  fio_data.thread_suspenders.out = -1;
  fio_data.io_wake.in = -1;
  fio_data.io_wake.out = -1;
  fio_uuid_monitor_close();
  fio_cli_end();
  fio_uuid_invalidate_all();
  for (int i = 0; i < FIO_CALL_NEVER; ++i)
    fio_state_callback_clear((callback_type_e)i);
}

static void fio_cleanup_after_fork(void *ignr_);

FIO_CONSTRUCTOR(fio_data_state_init) {
  fio_data.protocols = FIO_LIST_INIT(fio_data.protocols);
  fio_data.master = getpid();
  fio_uuid_monitor_init();
  fio_reset_wakeup_pipes(&fio_data.thread_suspenders.in);
  fio_reset_wakeup_pipes(&fio_data.io_wake.in);
  fio_data.tick = fio_time_real();
  fio_state_callback_add(FIO_CALL_IN_CHILD, fio_cleanup_after_fork, NULL);
}

/* *****************************************************************************
Thread suspension helpers
***************************************************************************** */

FIO_IFUNC void fio_user_thread_wake(void) {
  char buf[1] = {0};
  fio_sock_write(fio_data.thread_suspenders.out, buf, 1);
}

FIO_IFUNC void fio_user_thread_suspent(void) {
  char buf[1];
  fio_sock_read(fio_data.thread_suspenders.in, buf, 1);
}

FIO_IFUNC void fio_user_thread_wake_all(void) {
  fio_reset_wakeup_pipes(&fio_data.thread_suspenders.in);
}

/* *****************************************************************************
Signal Helpers
***************************************************************************** */
#define FIO_SIGNAL
#include "fio-stl.h"
static volatile uint8_t fio_signal_forwarded = 0;
/* Handles signals */
static void fio___stop_signal_handler(int sig, void *ignr_) {
  fio_data.running = 0;
  FIO_LOG_INFO("(%d) received shutdown signal.", getpid());
  if (fio_data.is_master && fio_data.workers && !fio_signal_forwarded) {
    kill(0, sig);
    fio_signal_forwarded = 1;
  }
  (void)ignr_;
}

/* Handles signals */
static void fio___worker_reset_signal_handler(int sig, void *ignr_) {
  if (!fio_data.workers || !fio_data.running)
    return;
  if (fio_data.is_worker) {
    fio_data.running = 0;
    FIO_LOG_INFO("(%d) received worker restart signal.", getpid());
  } else if (!fio_signal_forwarded) {
    kill(0, sig);
    FIO_LOG_INFO("(%d) forwarding worker restart signal.",
                 (int)fio_data.master);
  }
  fio_signal_forwarded = 1;
  (void)ignr_;
}

/* *****************************************************************************
UUID data types
***************************************************************************** */

#ifndef FIO_MAX_ADDR_LEN
#define FIO_MAX_ADDR_LEN 48
#endif

typedef enum {
  FIO_UUID_OPEN = 1,          /* 0b0001 */
  FIO_UUID_SUSPENDED_BIT = 2, /* 0b0010 */
  FIO_UUID_SUSPENDED = 3,     /* 0b0011 */
  FIO_UUID_CLOSED_BIT = 4,    /* 0b0100 */
  FIO_UUID_CLOSED = 6,        /* 0b0110 */
  FIO_UUID_CLOSING = 7,       /* 0b0111 */
} fio_uuid_state_e;

struct fio_uuid_s {
  /* fd protocol */
  fio_protocol_s *protocol;
  /* timeout review linked list */
  FIO_LIST_NODE timeouts;
  /** RW hooks. */
  fio_tls_s *tls;
  /* user udata */
  void *udata;
  /* current data to be send */
  fio_stream_s stream;
  /* Objects linked to the uuid */
  fio___uuid_env_s env;
  /* timer handler */
  int64_t active;
  /* socket */
  int fd;
  /** Connection is open */
  volatile fio_uuid_state_e state;
  /** Task lock */
  fio_lock_i lock;
  /** peer address length */
  uint8_t addr_len;
  /** peer address length */
  uint8_t addr[FIO_MAX_ADDR_LEN];
};
FIO_IFUNC fio_uuid_s *fio_uuid_dup2(fio_uuid_s *uuid);
FIO_IFUNC int fio_uuid_free2(fio_uuid_s *uuid);

FIO_SFUNC void fio___touch(void *uuid_, void *should_free) {
  fio_uuid_s *uuid = uuid_;
  uuid->active = fio_time2milli(fio_data.tick);
  FIO_LIST_REMOVE(&uuid->timeouts);
  FIO_LIST_PUSH(uuid->protocol->reserved.uuids.prev, &uuid->timeouts);
  // FIO_LOG_DEBUG("touched %p (fd %d)", uuid_, uuid->fd);
  FIO_LIST_REMOVE(&uuid->protocol->reserved.protocols);
  FIO_LIST_PUSH(fio_data.protocols.prev, &uuid->protocol->reserved.protocols);
  if (should_free)
    fio_uuid_free2(uuid);
}

FIO_SFUNC void fio___deferred_on_close(void *fn, void *udata) {
  union {
    void *p;
    void (*fn)(void *);
  } u = {.p = fn};
  u.fn(udata);
}

FIO_SFUNC void fio_uuid___init_task(void *uuid_, void *ignr_) {
  fio_uuid_s *const uuid = uuid_;
  fio_uuid_set_valid(uuid);
  fio_uuid_free2(uuid);
  (void)ignr_;
}

FIO_SFUNC void fio_uuid___init(fio_uuid_s *uuid) {
  *uuid = (fio_uuid_s){
      .state = FIO_UUID_OPEN,
      .stream = FIO_STREAM_INIT(uuid->stream),
      .timeouts = FIO_LIST_INIT(uuid->timeouts),
      .active = fio_time2milli(fio_data.tick),
  };
  fio_queue_push_urgent(&tasks_io_core,
                        .fn = fio_uuid___init_task,
                        .udata1 = fio_uuid_dup2(uuid));
  FIO_LOG_DEBUG("UUID %p initialized.", (void *)uuid);
}

FIO_SFUNC void fio_uuid___destroy(fio_uuid_s *uuid) {
  fio_uuid_set_invalid(uuid);
  fio___uuid_env_destroy(&uuid->env);
  fio_stream_destroy(&uuid->stream);
  // o->rw_hooks->cleanup(uuid->rw_udata);
  FIO_LIST_REMOVE(&uuid->timeouts);
  union {
    void *p;
    void (*fn)(void *);
  } u = {.fn = uuid->protocol->on_close};
  fio_queue_push(&tasks_user, fio___deferred_on_close, u.p, uuid->udata);
#if FIO_ENGINE_POLL
  fio_uuid_monitor_remove(uuid);
#endif
  fio_sock_close(uuid->fd);
  if (uuid->protocol->reserved.uuids.next ==
      uuid->protocol->reserved.uuids.prev) {
    FIO_LIST_REMOVE(&uuid->protocol->reserved.protocols);
  }
  FIO_LOG_DEBUG("UUID %p (fd %d) being freed.", (void *)uuid, (int)(uuid->fd));
}

#define FIO_REF_NAME         fio_uuid
#define FIO_REF_INIT(obj)    fio_uuid___init(&(obj))
#define FIO_REF_DESTROY(obj) fio_uuid___destroy(&(obj))
#include <fio-stl.h>

FIO_SFUNC void fio_uuid_free_task(void *uuid, void *ignr) {
  (void)ignr;
  fio_uuid_free2(uuid);
}

fio_uuid_s *fio_uuid_dup(fio_uuid_s *uuid) { return fio_uuid_dup2(uuid); }
void fio_uuid_free(fio_uuid_s *uuid) {
  fio_queue_push(&tasks_io_core, .fn = fio_uuid_free_task, .udata1 = uuid);
}

#define fio_uuid_dup fio_uuid_dup2

FIO_IFUNC void fio_uuid_close(fio_uuid_s *uuid) {
  if (fio_atomic_and(&uuid->state, ~(unsigned)FIO_UUID_OPEN) & FIO_UUID_OPEN) {
    uuid->state |= FIO_UUID_CLOSED;
    fio_uuid_monitor_remove(uuid);
    fio_uuid_free(uuid);
  }
}

/* *****************************************************************************
Event deferring (declarations)
***************************************************************************** */

static void fio_ev_on_shutdown(void *uuid_, void *udata);
static void fio_ev_on_ready(void *uuid_, void *udata);
static void fio_ev_on_data(void *uuid_, void *udata);
static void fio_ev_on_timeout(void *uuid_, void *udata);
static void mock_ping_eternal(fio_uuid_s *uuid, void *udata);

/* *****************************************************************************
Event deferring (mock functions)
***************************************************************************** */

static void mock_on_ev(fio_uuid_s *uuid, void *udata) {
  (void)uuid;
  (void)udata;
}
static void mock_on_data(fio_uuid_s *uuid, void *udata) {
  if ((uuid->state & FIO_UUID_OPEN))
    uuid->state |= FIO_UUID_SUSPENDED;
  (void)udata;
}
static uint8_t mock_on_shutdown(fio_uuid_s *uuid, void *udata) {
  (void)uuid;
  (void)udata;
  return 0;
}
static uint8_t mock_on_shutdown_eternal(fio_uuid_s *uuid, void *udata) {
  (void)uuid;
  (void)udata;
  return (uint8_t)-1;
}
static void mock_timeout(fio_uuid_s *uuid, void *udata) {
  fio_close(uuid);
  (void)uuid;
  (void)udata;
}
static void mock_ping_eternal(fio_uuid_s *uuid, void *udata) {
  fio_touch(uuid);
  (void)udata;
}

FIO_IFUNC void fio_protocol_validate(fio_protocol_s *p) {
  if (p && !(p->reserved.flags & 8)) {
    if (!p->on_data)
      p->on_data = mock_on_data;
    if (!p->on_timeout)
      p->on_timeout = mock_timeout;
    if (!p->on_ready)
      p->on_ready = mock_on_ev;
    if (!p->on_shutdown)
      p->on_shutdown = mock_on_shutdown;
    p->reserved.flags = 8;
    p->reserved.protocols = FIO_LIST_INIT(p->reserved.protocols);
    p->reserved.uuids = FIO_LIST_INIT(p->reserved.uuids);
  }
}

/* *****************************************************************************
IO thread wakeup protocol
***************************************************************************** */

static void fio_io_wakeup_on_data(fio_uuid_s *uuid, void *udata) {
  char buf[1024];
  ssize_t l;
  while ((l = fio_sock_read(uuid->fd, buf, 1024)) > 0)
    ;
  (void)udata;
}

static void fio_io_wakeup_on_close(void *udata) {
  fio_data.io_wake_uuid = NULL;
  (void)udata;
}

static fio_protocol_s FIO_IO_WAKEUP_PROTOCOL = {
    .on_data = fio_io_wakeup_on_data,
    .on_timeout = mock_ping_eternal,
    .on_shutdown = mock_on_shutdown_eternal,
    .on_close = fio_io_wakeup_on_close,
};

FIO_SFUNC void fio_io_wakeup_prep(void) {
  if (fio_data.io_wake_uuid)
    return;
  fio_uuid_s *uuid = fio_data.io_wake_uuid = fio_uuid_new2();
  FIO_ASSERT_ALLOC(uuid);
  uuid->tls = NULL;
  uuid->fd = fio_data.io_wake.in;
  fio_sock_set_non_block(fio_data.io_wake.in);
  fio_sock_set_non_block(fio_data.io_wake.out);
  fio_protocol_validate(&FIO_IO_WAKEUP_PROTOCOL);
  FIO_IO_WAKEUP_PROTOCOL.reserved.flags |= 1; /* system protocol */
  uuid->protocol = &FIO_IO_WAKEUP_PROTOCOL;
  fio___touch(uuid, NULL);
  fio_uuid_monitor_add_read(uuid);
}

FIO_IFUNC void fio_io_thread_wake(void) {
  char buf[1] = {0};
  fio_sock_write(fio_data.io_wake.out, buf, 1);
}

FIO_IFUNC void fio_io_thread_wake_clear(void) {
  char buf[1024];
  ssize_t l;
  while ((l = fio_sock_read(fio_data.io_wake.in, buf, 1024)) > 0)
    ;
}

/* *****************************************************************************
Housekeeping cycle
***************************************************************************** */
FIO_SFUNC void fio___cycle_housekeeping(void) {
  static int old = 0;
  static time_t last_to_review = 0;
  int c = fio_uuid_monitor_review();
  fio_data.tick = fio_time_real();
  c += fio_signal_review();
  if (!c) {
    if (!old) {
      fio_state_callback_force(FIO_CALL_ON_IDLE);
      fio_signal_forwarded = 0;
    }
#if FIO_OS_POSIX
    if (!fio_data.is_master && fio_data.running &&
        getppid() != fio_data.master) {
      fio_data.running = 0;
      FIO_LOG_FATAL("(%d) parent process (%d != %d) seems to have crashed",
                    getpid(),
                    (int)fio_data.master,
                    (int)getppid());
    }
#endif
  }
  old = c;

  /* test timeouts? */
  if (last_to_review != fio_data.tick.tv_sec) {
    last_to_review = fio_data.tick.tv_sec;
    FIO_LIST_EACH(fio_protocol_s, reserved.protocols, &fio_data.protocols, pr) {
      if (!pr->timeout || pr->timeout > FIO_UUID_TIMEOUT_MAX)
        pr->timeout = FIO_UUID_TIMEOUT_MAX;
      time_t limit =
          fio_time2milli(fio_data.tick) - ((time_t)pr->timeout * 1000);
      FIO_LIST_EACH(fio_uuid_s, timeouts, &pr->reserved.uuids, uuid) {
        if (uuid->active >= limit)
          break;
        fio_queue_push_urgent(fio_queue_select(pr->reserved.flags),
                              .fn = fio_ev_on_timeout,
                              .udata1 = fio_uuid_dup(uuid),
                              .udata2 = NULL);
        FIO_LOG_DEBUG("scheduling timeout for %p", (void *)uuid);
      }
    }
  }
  /* what if there were no other events and timeouts were scheduled?  */
  fio_user_thread_wake();
}

FIO_SFUNC void fio___cycle_housekeeping_running(void) {
  if (!fio_data.io_wake_uuid)
    fio_io_wakeup_prep();
  fio___cycle_housekeeping();
}

/* *****************************************************************************
Cleanup helpers
***************************************************************************** */

static void fio_cleanup_after_fork(void *ignr_) {
  (void)ignr_;
  if (!fio_data.is_master) {
    fio_uuid_monitor_close();
    fio_uuid_monitor_init();
  }

  fio_reset_wakeup_pipes(&fio_data.thread_suspenders.in);
  fio_reset_wakeup_pipes(&fio_data.io_wake.in);

  FIO_LIST_EACH(fio_protocol_s, reserved.protocols, &fio_data.protocols, pr) {
    FIO_LIST_EACH(fio_uuid_s, timeouts, &pr->reserved.uuids, uuid) {
      FIO_LOG_DEBUG("cleanup for fd %d", uuid->fd);
      // fio_sock_close(uuid->fd);
      // uuid->fd = -1;
      uuid->state = FIO_UUID_CLOSED;
      fio_uuid_free2(uuid);
    }
    FIO_LIST_REMOVE(&pr->reserved.protocols);
  }
}

static void fio_cleanup_start_shutdown() {
  FIO_LIST_EACH(fio_protocol_s, reserved.protocols, &fio_data.protocols, pr) {
    FIO_LIST_EACH(fio_uuid_s, timeouts, &pr->reserved.uuids, uuid) {
      fio_queue_push_urgent(fio_queue_select(pr->reserved.flags),
                            .fn = fio_ev_on_shutdown,
                            .udata1 = fio_uuid_dup(uuid),
                            .udata2 = NULL);
    }
  }
}
/* *****************************************************************************
Copy address to string
***************************************************************************** */

FIO_SFUNC void fio_tcp_addr_cpy(fio_uuid_s *uuid,
                                int family,
                                struct sockaddr *addrinfo) {
  const char *result =
      inet_ntop(family,
                family == AF_INET
                    ? (void *)&(((struct sockaddr_in *)addrinfo)->sin_addr)
                    : (void *)&(((struct sockaddr_in6 *)addrinfo)->sin6_addr),
                (char *)uuid->addr,
                sizeof(uuid->addr));
  if (result) {
    uuid->addr_len = strlen((char *)uuid->addr);
  } else {
    uuid->addr_len = 0;
    uuid->addr[0] = 0;
  }
}

/* *****************************************************************************
Misc Helpers
***************************************************************************** */

static inline size_t fio_detect_cpu_cores(void) {
  ssize_t cpu_count = 0;
#ifdef _SC_NPROCESSORS_ONLN
  cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
  if (cpu_count < 0) {
    FIO_LOG_WARNING("CPU core count auto-detection failed.");
    return 0;
  }
#else
  FIO_LOG_WARNING("CPU core count auto-detection failed.");
  cpu_count = FIO_CPU_CORES_FALLBACK;
#endif
  return cpu_count;
}
