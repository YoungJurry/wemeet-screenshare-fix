#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

/*
 * Tencent Meeting 3.26.10 loads PipeWire with dlopen()/dlsym(), so ordinary
 * symbol interposition is insufficient.  This library interposes dlsym() and
 * returns guarded wrappers only for the small pw_stream API used by the
 * screen-share module.
 *
 * The guard has two jobs:
 *   1. Snapshot a dequeued SHM frame while PipeWire still owns a valid mapping,
 *      then temporarily expose a long-lived mirror through spa_data.data.
 *   2. Reject delayed queue_buffer calls after
 * remove_buffer/disconnect/destroy, before libpipewire dereferences stale
 * pw_buffer internals.
 */

#define MAX_STREAMS 32u
#define MAX_BUFFERS 32u
#define MAX_PLANES 4u
#define MIRROR_SLOTS 8u

struct buffer_state {
  struct pw_buffer *key;
  bool present;
  bool dequeued;
  uint32_t n_planes;
  void *original[MAX_PLANES];
  void *mirror[MAX_PLANES];
};

struct stream_state {
  struct pw_stream *key;
  bool used;
  bool alive;
  bool connected;
  unsigned active_calls;
  struct buffer_state buffers[MAX_BUFFERS];
};

struct mirror_generation {
  size_t capacity;
  unsigned cursor;
  void *slots[MIRROR_SLOTS];
  struct mirror_generation *older;
};

struct event_proxy {
  struct pw_stream *stream;
  struct pw_stream_events original;
  struct pw_stream_events wrapped;
  void *original_data;
};

static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t state_idle = PTHREAD_COND_INITIALIZER;
static struct stream_state streams[MAX_STREAMS];
static struct mirror_generation *mirrors;
static _Thread_local struct pw_stream *processing_stream;
static _Thread_local unsigned processing_depth;

static atomic_ulong stat_snapshots;
static atomic_ulong stat_bytes;
static atomic_ulong stat_dropped_queues;
static atomic_ulong stat_streams;
static atomic_ulong stat_copy_failures;

static int guard_enabled;
static int allocation_warning_emitted;

/* Real functions are captured from the exact dlopen handle supplied by Wemeet.
 */
static struct pw_stream *(*real_pw_stream_new)(struct pw_core *, const char *,
                                               struct pw_properties *);
static void (*real_pw_stream_destroy)(struct pw_stream *);
static void (*real_pw_stream_add_listener)(struct pw_stream *,
                                           struct spa_hook *,
                                           const struct pw_stream_events *,
                                           void *);
static int (*real_pw_stream_connect)(struct pw_stream *, enum pw_direction,
                                     uint32_t, enum pw_stream_flags,
                                     const struct spa_pod **, uint32_t);
static int (*real_pw_stream_disconnect)(struct pw_stream *);
static struct pw_buffer *(*real_pw_stream_dequeue_buffer)(struct pw_stream *);
static int (*real_pw_stream_queue_buffer)(struct pw_stream *,
                                          struct pw_buffer *);

static void *(*real_dlsym_fn)(void *, const char *);
static pthread_once_t dlsym_once = PTHREAD_ONCE_INIT;

static void log_literal(const char *text) {
  if (text != NULL)
    (void)write(STDERR_FILENO, text, strlen(text));
}

static void init_real_dlsym(void) {
  void *symbol = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34");
  if (symbol == NULL)
    symbol = dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5");
  if (symbol != NULL)
    memcpy(&real_dlsym_fn, &symbol, sizeof(real_dlsym_fn));
}

static struct stream_state *find_stream_locked(struct pw_stream *stream) {
  for (unsigned i = 0; i < MAX_STREAMS; ++i) {
    if (streams[i].used && streams[i].key == stream)
      return &streams[i];
  }
  return NULL;
}

static struct stream_state *register_stream_locked(struct pw_stream *stream) {
  struct stream_state *slot = find_stream_locked(stream);
  if (slot == NULL) {
    for (unsigned i = 0; i < MAX_STREAMS; ++i) {
      if (!streams[i].used || !streams[i].alive) {
        slot = &streams[i];
        break;
      }
    }
  }
  if (slot == NULL)
    return NULL;

  memset(slot, 0, sizeof(*slot));
  slot->key = stream;
  slot->used = true;
  slot->alive = true;
  atomic_fetch_add_explicit(&stat_streams, 1, memory_order_relaxed);
  return slot;
}

static struct buffer_state *find_buffer_locked(struct stream_state *stream,
                                               struct pw_buffer *buffer) {
  for (unsigned i = 0; i < MAX_BUFFERS; ++i) {
    if (stream->buffers[i].key == buffer)
      return &stream->buffers[i];
  }
  return NULL;
}

static struct buffer_state *register_buffer_locked(struct stream_state *stream,
                                                   struct pw_buffer *buffer) {
  struct buffer_state *slot = find_buffer_locked(stream, buffer);
  if (slot == NULL) {
    for (unsigned i = 0; i < MAX_BUFFERS; ++i) {
      if (stream->buffers[i].key == NULL || !stream->buffers[i].present) {
        slot = &stream->buffers[i];
        break;
      }
    }
  }
  if (slot == NULL)
    return NULL;

  memset(slot, 0, sizeof(*slot));
  slot->key = buffer;
  slot->present = true;
  return slot;
}

static size_t page_align(size_t size) {
  const long page = sysconf(_SC_PAGESIZE);
  const size_t alignment = page > 0 ? (size_t)page : 4096u;
  if (size > SIZE_MAX - (alignment - 1u))
    return 0;
  return (size + alignment - 1u) & ~(alignment - 1u);
}

/* Mirror mappings are intentionally never unmapped before process exit. */
static void *acquire_mirror_locked(size_t required) {
  if (required == 0)
    return NULL;

  if (mirrors == NULL || mirrors->capacity < required) {
    const size_t capacity = page_align(required);
    if (capacity == 0)
      return NULL;

    struct mirror_generation *generation = calloc(1, sizeof(*generation));
    if (generation == NULL)
      return NULL;

    generation->capacity = capacity;
    for (unsigned i = 0; i < MIRROR_SLOTS; ++i) {
      generation->slots[i] = mmap(NULL, capacity, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (generation->slots[i] == MAP_FAILED) {
        generation->slots[i] = NULL;
        for (unsigned j = 0; j < i; ++j)
          (void)munmap(generation->slots[j], capacity);
        free(generation);
        return NULL;
      }
#ifdef MADV_HUGEPAGE
      (void)madvise(generation->slots[i], capacity, MADV_HUGEPAGE);
#endif
    }
    generation->older = mirrors;
    mirrors = generation;
  }

  void *result = mirrors->slots[mirrors->cursor % MIRROR_SLOTS];
  mirrors->cursor++;
  return result;
}

static void restore_buffer_locked(struct buffer_state *state,
                                  struct pw_buffer *buffer) {
  if (state == NULL || buffer == NULL || buffer->buffer == NULL)
    return;

  struct spa_buffer *spa = buffer->buffer;
  const uint32_t planes =
      state->n_planes < spa->n_datas ? state->n_planes : spa->n_datas;
  for (uint32_t i = 0; i < planes; ++i) {
    struct spa_data *data = &spa->datas[i];
    if (data->data == state->mirror[i])
      data->data = state->original[i];
  }
}

static bool snapshot_buffer_locked(struct stream_state *stream,
                                   struct pw_buffer *buffer) {
  if (stream == NULL || buffer == NULL || buffer->buffer == NULL)
    return false;

  struct spa_buffer *spa = buffer->buffer;
  if (spa->datas == NULL || spa->n_datas == 0)
    return false;

  struct buffer_state *state = find_buffer_locked(stream, buffer);
  if (state == NULL)
    state = register_buffer_locked(stream, buffer);
  if (state == NULL)
    return false;

  state->present = true;
  state->dequeued = true;
  state->n_planes = spa->n_datas < MAX_PLANES ? spa->n_datas : MAX_PLANES;

  bool replaced_any = false;
  for (uint32_t i = 0; i < state->n_planes; ++i) {
    struct spa_data *data = &spa->datas[i];
    state->original[i] = data->data;
    state->mirror[i] = NULL;

    if (data->data == NULL || data->maxsize == 0)
      continue;

    void *mirror = acquire_mirror_locked(data->maxsize);
    if (mirror == NULL) {
      atomic_fetch_add_explicit(&stat_copy_failures, 1, memory_order_relaxed);
      if (!allocation_warning_emitted) {
        allocation_warning_emitted = 1;
        log_literal("[wemeet-pw-guard] mirror allocation failed; frame left "
                    "unguarded\n");
      }
      continue;
    }

    size_t offset = 0;
    size_t length = data->maxsize;
    if (data->chunk != NULL) {
      offset = data->chunk->offset % data->maxsize;
      length = data->chunk->size;
      if (length == 0 || length > data->maxsize)
        length = data->maxsize;
    }

    if (offset + length <= data->maxsize) {
      memcpy((uint8_t *)mirror + offset, (const uint8_t *)data->data + offset,
             length);
    } else {
      const size_t first = data->maxsize - offset;
      memcpy((uint8_t *)mirror + offset, (const uint8_t *)data->data + offset,
             first);
      memcpy(mirror, data->data, length - first);
    }

    state->mirror[i] = mirror;
    data->data = mirror;
    replaced_any = true;
    atomic_fetch_add_explicit(&stat_bytes, length, memory_order_relaxed);
  }

  if (replaced_any)
    atomic_fetch_add_explicit(&stat_snapshots, 1, memory_order_relaxed);
  return replaced_any;
}

static bool begin_stream_call(struct pw_stream *stream) {
  bool allowed = false;
  pthread_mutex_lock(&state_lock);
  struct stream_state *state = find_stream_locked(stream);
  if (state != NULL && state->alive) {
    state->active_calls++;
    allowed = true;
  }
  pthread_mutex_unlock(&state_lock);
  return allowed;
}

static void end_stream_call(struct pw_stream *stream) {
  pthread_mutex_lock(&state_lock);
  struct stream_state *state = find_stream_locked(stream);
  if (state != NULL && state->active_calls > 0) {
    state->active_calls--;
    if (state->active_calls == 0)
      pthread_cond_broadcast(&state_idle);
  }
  pthread_mutex_unlock(&state_lock);
}

static void proxy_destroy(void *opaque) {
  struct event_proxy *proxy = opaque;
  if (proxy->original.destroy != NULL)
    proxy->original.destroy(proxy->original_data);
}

static void proxy_state_changed(void *opaque, enum pw_stream_state old_state,
                                enum pw_stream_state new_state,
                                const char *error) {
  struct event_proxy *proxy = opaque;
  pthread_mutex_lock(&state_lock);
  struct stream_state *state = find_stream_locked(proxy->stream);
  if (state != NULL) {
    state->connected = new_state != PW_STREAM_STATE_UNCONNECTED &&
                       new_state != PW_STREAM_STATE_ERROR;
  }
  pthread_mutex_unlock(&state_lock);

  if (proxy->original.state_changed != NULL)
    proxy->original.state_changed(proxy->original_data, old_state, new_state,
                                  error);
}

static void proxy_control_info(void *opaque, uint32_t id,
                               const struct pw_stream_control *control) {
  struct event_proxy *proxy = opaque;
  if (proxy->original.control_info != NULL)
    proxy->original.control_info(proxy->original_data, id, control);
}

static void proxy_io_changed(void *opaque, uint32_t id, void *area,
                             uint32_t size) {
  struct event_proxy *proxy = opaque;
  if (proxy->original.io_changed != NULL)
    proxy->original.io_changed(proxy->original_data, id, area, size);
}

static void proxy_param_changed(void *opaque, uint32_t id,
                                const struct spa_pod *param) {
  struct event_proxy *proxy = opaque;
  if (proxy->original.param_changed != NULL)
    proxy->original.param_changed(proxy->original_data, id, param);
}

static void proxy_add_buffer(void *opaque, struct pw_buffer *buffer) {
  struct event_proxy *proxy = opaque;
  pthread_mutex_lock(&state_lock);
  struct stream_state *stream = find_stream_locked(proxy->stream);
  if (stream != NULL)
    (void)register_buffer_locked(stream, buffer);
  pthread_mutex_unlock(&state_lock);

  if (proxy->original.add_buffer != NULL)
    proxy->original.add_buffer(proxy->original_data, buffer);
}

static void proxy_remove_buffer(void *opaque, struct pw_buffer *buffer) {
  struct event_proxy *proxy = opaque;
  pthread_mutex_lock(&state_lock);
  struct stream_state *stream = find_stream_locked(proxy->stream);
  struct buffer_state *state =
      stream != NULL ? find_buffer_locked(stream, buffer) : NULL;
  if (state != NULL) {
    restore_buffer_locked(state, buffer);
    state->present = false;
    state->dequeued = false;
  }
  pthread_mutex_unlock(&state_lock);

  if (proxy->original.remove_buffer != NULL)
    proxy->original.remove_buffer(proxy->original_data, buffer);
}

static void proxy_process(void *opaque) {
  struct event_proxy *proxy = opaque;
  if (proxy->original.process == NULL)
    return;

  if (!begin_stream_call(proxy->stream))
    return;

  struct pw_stream *previous = processing_stream;
  const unsigned previous_depth = processing_depth;
  processing_stream = proxy->stream;
  processing_depth = previous_depth + 1;
  proxy->original.process(proxy->original_data);
  processing_stream = previous;
  processing_depth = previous_depth;
  end_stream_call(proxy->stream);
}

static void proxy_drained(void *opaque) {
  struct event_proxy *proxy = opaque;
  if (proxy->original.drained != NULL)
    proxy->original.drained(proxy->original_data);
}

static void proxy_command(void *opaque, const struct spa_command *command) {
  struct event_proxy *proxy = opaque;
  if (proxy->original.command != NULL)
    proxy->original.command(proxy->original_data, command);
}

static void proxy_trigger_done(void *opaque) {
  struct event_proxy *proxy = opaque;
  if (proxy->original.trigger_done != NULL)
    proxy->original.trigger_done(proxy->original_data);
}

static struct pw_stream *guard_pw_stream_new(struct pw_core *core,
                                             const char *name,
                                             struct pw_properties *properties) {
  if (real_pw_stream_new == NULL)
    return NULL;
  struct pw_stream *stream = real_pw_stream_new(core, name, properties);
  if (stream != NULL) {
    pthread_mutex_lock(&state_lock);
    (void)register_stream_locked(stream);
    pthread_mutex_unlock(&state_lock);
  }
  return stream;
}

static void guard_pw_stream_destroy(struct pw_stream *stream) {
  if (real_pw_stream_destroy == NULL)
    return;

  pthread_mutex_lock(&state_lock);
  struct stream_state *state = find_stream_locked(stream);
  if (state != NULL) {
    state->alive = false;
    state->connected = false;
    if (!(processing_stream == stream && processing_depth > 0)) {
      while (state->active_calls > 0)
        pthread_cond_wait(&state_idle, &state_lock);
    }
  }
  pthread_mutex_unlock(&state_lock);
  real_pw_stream_destroy(stream);
}

static void guard_pw_stream_add_listener(struct pw_stream *stream,
                                         struct spa_hook *listener,
                                         const struct pw_stream_events *events,
                                         void *data) {
  if (real_pw_stream_add_listener == NULL)
    return;
  if (events == NULL) {
    real_pw_stream_add_listener(stream, listener, events, data);
    return;
  }

  struct event_proxy *proxy = calloc(1, sizeof(*proxy));
  if (proxy == NULL) {
    real_pw_stream_add_listener(stream, listener, events, data);
    return;
  }

  proxy->stream = stream;
  proxy->original = *events;
  proxy->wrapped = *events;
  proxy->original_data = data;
  proxy->wrapped.destroy = proxy_destroy;
  proxy->wrapped.state_changed = proxy_state_changed;
  proxy->wrapped.control_info = proxy_control_info;
  proxy->wrapped.io_changed = proxy_io_changed;
  proxy->wrapped.param_changed = proxy_param_changed;
  proxy->wrapped.add_buffer = proxy_add_buffer;
  proxy->wrapped.remove_buffer = proxy_remove_buffer;
  proxy->wrapped.process = proxy_process;
  proxy->wrapped.drained = proxy_drained;
  proxy->wrapped.command = proxy_command;
  proxy->wrapped.trigger_done = proxy_trigger_done;

  /* Proxy storage is intentionally retained until process exit. */
  real_pw_stream_add_listener(stream, listener, &proxy->wrapped, proxy);
}

static int
guard_pw_stream_connect(struct pw_stream *stream, enum pw_direction direction,
                        uint32_t target_id, enum pw_stream_flags flags,
                        const struct spa_pod **params, uint32_t n_params) {
  if (real_pw_stream_connect == NULL)
    return -ENOSYS;
  const int result = real_pw_stream_connect(stream, direction, target_id, flags,
                                            params, n_params);
  pthread_mutex_lock(&state_lock);
  struct stream_state *state = find_stream_locked(stream);
  if (state != NULL)
    state->connected = result >= 0;
  pthread_mutex_unlock(&state_lock);
  return result;
}

static int guard_pw_stream_disconnect(struct pw_stream *stream) {
  if (real_pw_stream_disconnect == NULL)
    return -ENOSYS;

  pthread_mutex_lock(&state_lock);
  struct stream_state *state = find_stream_locked(stream);
  if (state != NULL) {
    state->connected = false;
    if (!(processing_stream == stream && processing_depth > 0)) {
      while (state->active_calls > 0)
        pthread_cond_wait(&state_idle, &state_lock);
    }
  }
  pthread_mutex_unlock(&state_lock);
  return real_pw_stream_disconnect(stream);
}

static struct pw_buffer *
guard_pw_stream_dequeue_buffer(struct pw_stream *stream) {
  if (real_pw_stream_dequeue_buffer == NULL)
    return NULL;
  if (!begin_stream_call(stream)) {
    errno = EPIPE;
    return NULL;
  }

  struct pw_buffer *buffer = real_pw_stream_dequeue_buffer(stream);
  if (buffer != NULL) {
    pthread_mutex_lock(&state_lock);
    struct stream_state *state = find_stream_locked(stream);
    if (state != NULL && state->alive && state->connected)
      (void)snapshot_buffer_locked(state, buffer);
    pthread_mutex_unlock(&state_lock);
  }
  end_stream_call(stream);
  return buffer;
}

static int guard_pw_stream_queue_buffer(struct pw_stream *stream,
                                        struct pw_buffer *buffer) {
  if (real_pw_stream_queue_buffer == NULL)
    return -ENOSYS;

  pthread_mutex_lock(&state_lock);
  struct stream_state *state = find_stream_locked(stream);
  struct buffer_state *buffer_state =
      state != NULL ? find_buffer_locked(state, buffer) : NULL;
  if (state == NULL || !state->alive || !state->connected ||
      buffer_state == NULL || !buffer_state->present ||
      !buffer_state->dequeued) {
    pthread_mutex_unlock(&state_lock);
    atomic_fetch_add_explicit(&stat_dropped_queues, 1, memory_order_relaxed);
    return -EPIPE;
  }

  state->active_calls++;
  restore_buffer_locked(buffer_state, buffer);
  buffer_state->dequeued = false;
  pthread_mutex_unlock(&state_lock);

  const int result = real_pw_stream_queue_buffer(stream, buffer);
  end_stream_call(stream);
  return result;
}

static void store_function_pointer(void *destination, size_t size,
                                   void *symbol) {
  memcpy(destination, &symbol, size);
}

__attribute__((visibility("default"))) void *dlsym(void *handle,
                                                   const char *name) {
  pthread_once(&dlsym_once, init_real_dlsym);
  if (real_dlsym_fn == NULL)
    return NULL;

  void *real_symbol = real_dlsym_fn(handle, name);
  if (!guard_enabled || real_symbol == NULL)
    return real_symbol;

  if (strcmp(name, "pw_stream_new") == 0) {
    store_function_pointer(&real_pw_stream_new, sizeof(real_pw_stream_new),
                           real_symbol);
    return (void *)(uintptr_t)&guard_pw_stream_new;
  }
  if (strcmp(name, "pw_stream_destroy") == 0) {
    store_function_pointer(&real_pw_stream_destroy,
                           sizeof(real_pw_stream_destroy), real_symbol);
    return (void *)(uintptr_t)&guard_pw_stream_destroy;
  }
  if (strcmp(name, "pw_stream_add_listener") == 0) {
    store_function_pointer(&real_pw_stream_add_listener,
                           sizeof(real_pw_stream_add_listener), real_symbol);
    return (void *)(uintptr_t)&guard_pw_stream_add_listener;
  }
  if (strcmp(name, "pw_stream_connect") == 0) {
    store_function_pointer(&real_pw_stream_connect,
                           sizeof(real_pw_stream_connect), real_symbol);
    return (void *)(uintptr_t)&guard_pw_stream_connect;
  }
  if (strcmp(name, "pw_stream_disconnect") == 0) {
    store_function_pointer(&real_pw_stream_disconnect,
                           sizeof(real_pw_stream_disconnect), real_symbol);
    return (void *)(uintptr_t)&guard_pw_stream_disconnect;
  }
  if (strcmp(name, "pw_stream_dequeue_buffer") == 0) {
    store_function_pointer(&real_pw_stream_dequeue_buffer,
                           sizeof(real_pw_stream_dequeue_buffer), real_symbol);
    return (void *)(uintptr_t)&guard_pw_stream_dequeue_buffer;
  }
  if (strcmp(name, "pw_stream_queue_buffer") == 0) {
    store_function_pointer(&real_pw_stream_queue_buffer,
                           sizeof(real_pw_stream_queue_buffer), real_symbol);
    return (void *)(uintptr_t)&guard_pw_stream_queue_buffer;
  }
  return real_symbol;
}

__attribute__((constructor)) static void guard_init(void) {
  const char *value = getenv("WEMEET_PW_GUARD");
  guard_enabled = value != NULL && strcmp(value, "1") == 0;
  if (guard_enabled)
    log_literal("[wemeet-pw-guard] enabled\n");
}

__attribute__((destructor)) static void guard_fini(void) {
  if (!guard_enabled)
    return;
  char message[256];
  const int length =
      snprintf(message, sizeof(message),
               "[wemeet-pw-guard] streams=%lu snapshots=%lu bytes=%lu "
               "dropped_queues=%lu copy_failures=%lu\n",
               atomic_load_explicit(&stat_streams, memory_order_relaxed),
               atomic_load_explicit(&stat_snapshots, memory_order_relaxed),
               atomic_load_explicit(&stat_bytes, memory_order_relaxed),
               atomic_load_explicit(&stat_dropped_queues, memory_order_relaxed),
               atomic_load_explicit(&stat_copy_failures, memory_order_relaxed));
  if (length > 0)
    (void)write(STDERR_FILENO, message,
                (size_t)length < sizeof(message) ? (size_t)length
                                                 : sizeof(message) - 1u);
}
