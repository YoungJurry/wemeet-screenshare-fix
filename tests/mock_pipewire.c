#define _GNU_SOURCE

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/pipewire.h>

struct mock_stream {
  const struct pw_stream_events *events;
  void *event_data;
  struct pw_buffer *buffer;
  bool connected;
  bool destroyed;
  bool buffer_present;
  unsigned queue_calls;
  unsigned invalid_queue_calls;
};

static struct mock_stream mock;

struct pw_stream *pw_stream_new(struct pw_core *core, const char *name,
                                struct pw_properties *properties) {
  (void)core;
  (void)name;
  (void)properties;
  memset(&mock, 0, sizeof(mock));
  return (struct pw_stream *)&mock;
}

void pw_stream_destroy(struct pw_stream *stream) {
  struct mock_stream *state = (struct mock_stream *)stream;
  state->destroyed = true;
  state->connected = false;
  if (state->events != NULL && state->events->destroy != NULL)
    state->events->destroy(state->event_data);
}

void pw_stream_add_listener(struct pw_stream *stream, struct spa_hook *listener,
                            const struct pw_stream_events *events, void *data) {
  (void)listener;
  struct mock_stream *state = (struct mock_stream *)stream;
  state->events = events;
  state->event_data = data;
}

int pw_stream_connect(struct pw_stream *stream, enum pw_direction direction,
                      uint32_t target_id, enum pw_stream_flags flags,
                      const struct spa_pod **params, uint32_t n_params) {
  (void)direction;
  (void)target_id;
  (void)flags;
  (void)params;
  (void)n_params;
  struct mock_stream *state = (struct mock_stream *)stream;
  state->connected = true;
  if (state->events != NULL && state->events->state_changed != NULL)
    state->events->state_changed(state->event_data, PW_STREAM_STATE_UNCONNECTED,
                                 PW_STREAM_STATE_PAUSED, NULL);
  return 0;
}

int pw_stream_disconnect(struct pw_stream *stream) {
  struct mock_stream *state = (struct mock_stream *)stream;
  state->connected = false;
  if (state->events != NULL && state->events->state_changed != NULL)
    state->events->state_changed(state->event_data, PW_STREAM_STATE_STREAMING,
                                 PW_STREAM_STATE_UNCONNECTED, NULL);
  return 0;
}

struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream) {
  struct mock_stream *state = (struct mock_stream *)stream;
  if (!state->connected || state->destroyed || !state->buffer_present) {
    errno = EPIPE;
    return NULL;
  }
  return state->buffer;
}

int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer) {
  struct mock_stream *state = (struct mock_stream *)stream;
  if (!state->connected || state->destroyed || !state->buffer_present ||
      buffer != state->buffer) {
    state->invalid_queue_calls++;
    return -EFAULT;
  }
  state->queue_calls++;
  return 0;
}

void mock_set_buffer(struct pw_stream *stream, struct pw_buffer *buffer) {
  struct mock_stream *state = (struct mock_stream *)stream;
  state->buffer = buffer;
}

void mock_emit_add(struct pw_stream *stream) {
  struct mock_stream *state = (struct mock_stream *)stream;
  state->buffer_present = true;
  if (state->events != NULL && state->events->add_buffer != NULL)
    state->events->add_buffer(state->event_data, state->buffer);
}

void mock_emit_remove(struct pw_stream *stream) {
  struct mock_stream *state = (struct mock_stream *)stream;
  if (state->events != NULL && state->events->remove_buffer != NULL)
    state->events->remove_buffer(state->event_data, state->buffer);
  state->buffer_present = false;
}

void mock_emit_process(struct pw_stream *stream) {
  struct mock_stream *state = (struct mock_stream *)stream;
  if (state->events != NULL && state->events->process != NULL)
    state->events->process(state->event_data);
}

unsigned mock_queue_calls(struct pw_stream *stream) {
  return ((struct mock_stream *)stream)->queue_calls;
}

unsigned mock_invalid_queue_calls(struct pw_stream *stream) {
  return ((struct mock_stream *)stream)->invalid_queue_calls;
}
