#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#include <pipewire/pipewire.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__,       \
              #condition);                                                     \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)

typedef struct pw_stream *(*new_fn)(struct pw_core *, const char *,
                                    struct pw_properties *);
typedef void (*destroy_fn)(struct pw_stream *);
typedef void (*listener_fn)(struct pw_stream *, struct spa_hook *,
                            const struct pw_stream_events *, void *);
typedef int (*connect_fn)(struct pw_stream *, enum pw_direction, uint32_t,
                          enum pw_stream_flags, const struct spa_pod **,
                          uint32_t);
typedef int (*disconnect_fn)(struct pw_stream *);
typedef struct pw_buffer *(*dequeue_fn)(struct pw_stream *);
typedef int (*queue_fn)(struct pw_stream *, struct pw_buffer *);
typedef void (*set_buffer_fn)(struct pw_stream *, struct pw_buffer *);
typedef void (*emit_fn)(struct pw_stream *);
typedef unsigned (*counter_fn)(struct pw_stream *);

static dequeue_fn guarded_dequeue;
static queue_fn guarded_queue;
static struct pw_stream *callback_stream;
static struct spa_data *callback_data;
static const uint8_t *callback_source;
static size_t callback_size;
static unsigned process_calls;
static bool blocking_process;
static pthread_mutex_t block_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t block_condition = PTHREAD_COND_INITIALIZER;
static bool block_entered;
static bool block_release;
static atomic_bool destroy_started;
static atomic_bool destroy_completed;
static emit_fn threaded_emit_process;
static destroy_fn threaded_destroy;

static void load_symbol(void *handle, const char *name, void *destination,
                        size_t size) {
  void *symbol = dlsym(handle, name);
  CHECK(symbol != NULL);
  memcpy(destination, &symbol, size);
}

static unsigned long environment_number(const char *name,
                                        unsigned long fallback) {
  const char *text = getenv(name);
  if (text == NULL || *text == '\0')
    return fallback;
  char *end = NULL;
  errno = 0;
  const unsigned long value = strtoul(text, &end, 10);
  CHECK(errno == 0 && end != text && *end == '\0' && value > 0);
  return value;
}

static void on_process(void *data) {
  (void)data;
  struct pw_buffer *buffer = guarded_dequeue(callback_stream);
  CHECK(buffer != NULL);
  CHECK(callback_data->data != callback_source);
  CHECK(memcmp(callback_data->data, callback_source, callback_size) == 0);

  if (blocking_process) {
    pthread_mutex_lock(&block_lock);
    block_entered = true;
    pthread_cond_broadcast(&block_condition);
    while (!block_release)
      pthread_cond_wait(&block_condition, &block_lock);
    pthread_mutex_unlock(&block_lock);
    CHECK(guarded_queue(callback_stream, buffer) == -EPIPE);
  } else {
    CHECK(guarded_queue(callback_stream, buffer) == 0);
    CHECK(callback_data->data == callback_source);
  }
  process_calls++;
}

static void *process_thread_main(void *opaque) {
  threaded_emit_process(opaque);
  return NULL;
}

static void *destroy_thread_main(void *opaque) {
  atomic_store_explicit(&destroy_started, true, memory_order_release);
  threaded_destroy(opaque);
  atomic_store_explicit(&destroy_completed, true, memory_order_release);
  return NULL;
}

static void attach_listener(struct pw_stream *stream,
                            listener_fn add_listener) {
  static struct spa_hook listener;
  static const struct pw_stream_events events = {
      .version = PW_VERSION_STREAM_EVENTS,
      .process = on_process,
  };
  memset(&listener, 0, sizeof(listener));
  add_listener(stream, &listener, &events, NULL);
}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  CHECK(handle != NULL);

  new_fn stream_new;
  destroy_fn stream_destroy;
  listener_fn stream_add_listener;
  connect_fn stream_connect;
  disconnect_fn stream_disconnect;
  set_buffer_fn set_buffer;
  emit_fn emit_add;
  emit_fn emit_remove;
  emit_fn emit_process;
  counter_fn queue_calls;
  counter_fn invalid_queue_calls;

  /* Resolve in the same order used by Tencent's screen-share module. */
  load_symbol(handle, "pw_stream_new", &stream_new, sizeof(stream_new));
  load_symbol(handle, "pw_stream_destroy", &stream_destroy,
              sizeof(stream_destroy));
  load_symbol(handle, "pw_stream_connect", &stream_connect,
              sizeof(stream_connect));
  load_symbol(handle, "pw_stream_disconnect", &stream_disconnect,
              sizeof(stream_disconnect));
  load_symbol(handle, "pw_stream_add_listener", &stream_add_listener,
              sizeof(stream_add_listener));
  load_symbol(handle, "pw_stream_dequeue_buffer", &guarded_dequeue,
              sizeof(guarded_dequeue));
  load_symbol(handle, "pw_stream_queue_buffer", &guarded_queue,
              sizeof(guarded_queue));
  load_symbol(handle, "mock_set_buffer", &set_buffer, sizeof(set_buffer));
  load_symbol(handle, "mock_emit_add", &emit_add, sizeof(emit_add));
  load_symbol(handle, "mock_emit_remove", &emit_remove, sizeof(emit_remove));
  load_symbol(handle, "mock_emit_process", &emit_process, sizeof(emit_process));
  load_symbol(handle, "mock_queue_calls", &queue_calls, sizeof(queue_calls));
  load_symbol(handle, "mock_invalid_queue_calls", &invalid_queue_calls,
              sizeof(invalid_queue_calls));

  const size_t size =
      (size_t)environment_number("GUARD_TEST_SIZE", 64u * 1024u);
  const unsigned cycles =
      (unsigned)environment_number("GUARD_TEST_CYCLES", 2000u);
  CHECK(size <= UINT32_MAX && cycles > 0);
  uint8_t *source = mmap(NULL, size, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(source != MAP_FAILED);
  for (size_t i = 0; i < size; ++i)
    source[i] = (uint8_t)(i * 37u + 11u);

  struct spa_chunk chunk = {
      .offset = 0,
      .size = (uint32_t)size,
      .stride = 1024,
      .flags = 0,
  };
  struct spa_data spa_data = {
      .type = SPA_DATA_MemFd,
      .flags = SPA_DATA_FLAG_READABLE,
      .fd = -1,
      .mapoffset = 0,
      .maxsize = (uint32_t)size,
      .data = source,
      .chunk = &chunk,
  };
  struct spa_buffer spa_buffer = {
      .n_metas = 0,
      .n_datas = 1,
      .metas = NULL,
      .datas = &spa_data,
  };
  struct pw_buffer buffer = {
      .buffer = &spa_buffer,
  };

  /* Snapshot while dequeued; restore the producer pointer before queue. */
  struct pw_stream *stream = stream_new(NULL, "unit-test", NULL);
  CHECK(stream != NULL);
  attach_listener(stream, stream_add_listener);
  CHECK(stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                       PW_STREAM_FLAG_MAP_BUFFERS, NULL, 0) == 0);
  set_buffer(stream, &buffer);
  emit_add(stream);

  struct pw_buffer *dequeued = guarded_dequeue(stream);
  CHECK(dequeued == &buffer);
  void *first_mirror = spa_data.data;
  CHECK(first_mirror != source);
  CHECK(memcmp(first_mirror, source, size) == 0);
  CHECK(guarded_queue(stream, dequeued) == 0);
  CHECK(spa_data.data == source);
  CHECK(queue_calls(stream) == 1);

  /* The mirror remains readable even if the original SHM mapping vanishes. */
  CHECK(mprotect(source, size, PROT_NONE) == 0);
  volatile uint8_t mirror_byte = ((const uint8_t *)first_mirror)[1234];
  (void)mirror_byte;
  CHECK(mprotect(source, size, PROT_READ | PROT_WRITE) == 0);

  /* remove_buffer invalidates the pw_buffer; a delayed queue must be dropped.
   */
  dequeued = guarded_dequeue(stream);
  CHECK(dequeued == &buffer);
  emit_remove(stream);
  unsigned before = queue_calls(stream);
  CHECK(guarded_queue(stream, dequeued) == -EPIPE);
  CHECK(queue_calls(stream) == before);
  CHECK(invalid_queue_calls(stream) == 0);

  /* Re-add and verify thousands of normal process cycles. */
  spa_data.data = source;
  emit_add(stream);
  callback_stream = stream;
  callback_data = &spa_data;
  callback_source = source;
  callback_size = size;
  process_calls = 0;
  for (unsigned i = 0; i < cycles; ++i) {
    source[i % size] ^= (uint8_t)i;
    emit_process(stream);
  }
  CHECK(process_calls == cycles);
  CHECK(invalid_queue_calls(stream) == 0);

  /* disconnect makes every outstanding completion callback stale. */
  dequeued = guarded_dequeue(stream);
  CHECK(dequeued == &buffer);
  CHECK(stream_disconnect(stream) == 0);
  before = queue_calls(stream);
  CHECK(guarded_queue(stream, dequeued) == -EPIPE);
  CHECK(queue_calls(stream) == before);
  CHECK(invalid_queue_calls(stream) == 0);
  stream_destroy(stream);

  /* destroy waits for an active process callback, while its delayed queue is
   * rejected after the stream has been marked dead. */
  spa_data.data = source;
  stream = stream_new(NULL, "unit-test-destroy-race", NULL);
  attach_listener(stream, stream_add_listener);
  CHECK(stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                       PW_STREAM_FLAG_MAP_BUFFERS, NULL, 0) == 0);
  set_buffer(stream, &buffer);
  emit_add(stream);
  callback_stream = stream;
  blocking_process = true;
  block_entered = false;
  block_release = false;
  atomic_store_explicit(&destroy_started, false, memory_order_relaxed);
  atomic_store_explicit(&destroy_completed, false, memory_order_relaxed);
  threaded_emit_process = emit_process;
  threaded_destroy = stream_destroy;

  pthread_t process_thread;
  pthread_t destroy_thread;
  CHECK(pthread_create(&process_thread, NULL, process_thread_main, stream) ==
        0);
  pthread_mutex_lock(&block_lock);
  while (!block_entered)
    pthread_cond_wait(&block_condition, &block_lock);
  pthread_mutex_unlock(&block_lock);
  CHECK(pthread_create(&destroy_thread, NULL, destroy_thread_main, stream) ==
        0);
  while (!atomic_load_explicit(&destroy_started, memory_order_acquire)) {
  }
  const struct timespec wait_time = {.tv_sec = 0, .tv_nsec = 50000000};
  CHECK(nanosleep(&wait_time, NULL) == 0);
  CHECK(!atomic_load_explicit(&destroy_completed, memory_order_acquire));
  pthread_mutex_lock(&block_lock);
  block_release = true;
  pthread_cond_broadcast(&block_condition);
  pthread_mutex_unlock(&block_lock);
  CHECK(pthread_join(process_thread, NULL) == 0);
  CHECK(pthread_join(destroy_thread, NULL) == 0);
  CHECK(atomic_load_explicit(&destroy_completed, memory_order_acquire));
  CHECK(invalid_queue_calls(stream) == 0);
  blocking_process = false;

  /* A reused stream address is registered as a fresh generation. */
  spa_data.data = source;
  stream = stream_new(NULL, "unit-test-reused", NULL);
  attach_listener(stream, stream_add_listener);
  CHECK(stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                       PW_STREAM_FLAG_MAP_BUFFERS, NULL, 0) == 0);
  set_buffer(stream, &buffer);
  emit_add(stream);
  dequeued = guarded_dequeue(stream);
  CHECK(dequeued == &buffer);
  stream_destroy(stream);
  before = queue_calls(stream);
  CHECK(guarded_queue(stream, dequeued) == -EPIPE);
  CHECK(queue_calls(stream) == before);
  CHECK(invalid_queue_calls(stream) == 0);

  CHECK(munmap(source, size) == 0);
  CHECK(dlclose(handle) == 0);
  puts("all guard tests passed");
  return 0;
}
