#include "compositor/WaylandRuntime.h"

#include <ctype.h>
#include <drm_fourcc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

struct diftray_pixel_buffer {
  struct wlr_buffer base;
  uint32_t *pixels;
  size_t stride;
};

struct diftray_wayland_runtime {
  struct wl_display *display;
  struct wlr_backend *backend;
  struct wlr_renderer *renderer;
  struct wlr_allocator *allocator;
  struct wlr_output_layout *output_layout;
  struct wlr_scene *scene;
  struct wlr_scene_output_layout *scene_layout;
  struct wlr_scene_tree *ncursor_tree;
  struct wlr_scene_tree *gcursor_tree;
  struct wlr_scene_tree *overlay_tree;
  struct wlr_xdg_shell *xdg_shell;
  struct wlr_seat *seat;
  struct wlr_cursor *cursor;
  struct wlr_xcursor_manager *cursor_mgr;
  struct wlr_scene_rect *background;
  struct wlr_scene_rect *command_bar;
  struct wlr_scene_buffer *command_bar_text;
  struct diftray_pixel_buffer *command_bar_buffer;
  struct wlr_scene_buffer *status_text;
  struct diftray_pixel_buffer *status_buffer;
  struct wl_listener new_output;
  struct wl_listener new_input;
  struct wl_listener new_toplevel;
  struct wl_listener new_popup;
  struct wl_listener request_cursor;
  struct wl_listener request_set_selection;
  struct wl_listener cursor_motion;
  struct wl_listener cursor_motion_absolute;
  struct wl_listener cursor_button;
  struct wl_listener cursor_axis;
  struct wl_listener cursor_frame;
  struct wl_list outputs;
  struct wl_list keyboards;
  struct wl_list toplevels;
  void (*frame_handler)(void *);
  void *frame_userdata;
  diftray_text_renderer text_renderer;
  void *text_userdata;
  diftray_wayland_key_handler key_handler;
  void *key_handler_userdata;
  diftray_wayland_toplevel_handler toplevel_handler;
  diftray_wayland_toplevel_focus_handler focus_handler;
  diftray_wayland_toplevel_destroy_handler toplevel_destroy_handler;
  diftray_wayland_toplevel_request_handler toplevel_request_handler;
  void *toplevel_userdata;
  diftray_wayland_output_handler output_handler;
  void *output_userdata;
  bool command_bar_visible;
  bool gcursor_visible;
  char *command_bar_text_copy;
  char *status_line;
  struct diftray_wayland_style style;
  int output_x, output_y;
  int output_width;
  int output_height;
};

struct diftray_output {
  struct wl_list link;
  struct diftray_wayland_runtime *runtime;
  struct wlr_output *output;
  struct wlr_scene_output *scene_output;
  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;
};

struct diftray_keyboard {
  struct wl_list link;
  struct diftray_wayland_runtime *runtime;
  struct wlr_keyboard *keyboard;
  bool consumed_keys[256];
  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
};

struct diftray_toplevel {
  struct wl_list link;
  struct diftray_wayland_runtime *runtime;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *tree;
  struct wlr_scene_tree *surface_tree;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener commit;
  struct wl_listener destroy;
  struct wl_listener request_minimize;
  struct wl_listener request_maximize;
  struct wl_listener request_fullscreen;
  bool mapped;
  int x, y, width, height;
};

struct diftray_popup {
  struct wlr_xdg_popup *popup;
  struct wl_listener commit;
  struct wl_listener destroy;
};

struct diftray_cell_surface {
  struct diftray_wayland_runtime *runtime;
  struct wlr_scene_tree *tree;
  struct wlr_scene_rect *border;
  struct wlr_scene_rect *highlight;
  struct wlr_scene_buffer *buffer_node;
  struct diftray_pixel_buffer *buffer;
  int x;
  int y;
  int width;
  int height;
};

static void pixel_buffer_destroy(struct wlr_buffer *wlr_buffer) {
  struct diftray_pixel_buffer *buffer =
      wl_container_of(wlr_buffer, buffer, base);
  wlr_buffer_finish(wlr_buffer);
  free(buffer->pixels);
  free(buffer);
}

static bool pixel_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
                                               uint32_t flags, void **data,
                                               uint32_t *format,
                                               size_t *stride) {
  struct diftray_pixel_buffer *buffer =
      wl_container_of(wlr_buffer, buffer, base);
  (void)flags;
  *data = buffer->pixels;
  *format = DRM_FORMAT_ARGB8888;
  *stride = buffer->stride;
  return true;
}

static void pixel_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
  (void)wlr_buffer;
}

static const struct wlr_buffer_impl pixel_buffer_impl = {
    .destroy = pixel_buffer_destroy,
    .begin_data_ptr_access = pixel_buffer_begin_data_ptr_access,
    .end_data_ptr_access = pixel_buffer_end_data_ptr_access,
};

static struct diftray_pixel_buffer *pixel_buffer_create(int width, int height) {
  if (width <= 0 || height <= 0) {
    return NULL;
  }
  struct diftray_pixel_buffer *buffer = calloc(1, sizeof(*buffer));
  if (!buffer) {
    return NULL;
  }
  buffer->stride = (size_t)width * sizeof(uint32_t);
  buffer->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
  if (!buffer->pixels) {
    free(buffer);
    return NULL;
  }
  wlr_buffer_init(&buffer->base, &pixel_buffer_impl, width, height);
  return buffer;
}

static void render_chrome_text(struct diftray_wayland_runtime *runtime,
    struct diftray_pixel_buffer *buffer, const char *text,
    const float bg[4], const float fg[4]) {
  if (buffer && runtime->text_renderer)
    runtime->text_renderer(runtime->text_userdata, text, buffer->pixels,
        buffer->base.width, buffer->base.height, bg, fg);
}

static struct diftray_toplevel *toplevel_from_xdg(
    struct diftray_wayland_runtime *runtime,
    struct wlr_xdg_toplevel *xdg_toplevel) {
  struct diftray_toplevel *toplevel;
  wl_list_for_each(toplevel, &runtime->toplevels, link) {
    if (toplevel->xdg_toplevel == xdg_toplevel) {
      return toplevel;
    }
  }
  return NULL;
}

static void layout_gcursor(struct diftray_wayland_runtime *runtime,
                           struct diftray_toplevel *toplevel) {
  if (!runtime || !toplevel || !toplevel->tree) {
    return;
  }
  wlr_scene_node_set_position(&toplevel->tree->node, toplevel->x, toplevel->y);
  if (toplevel->xdg_toplevel) {
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
                              toplevel->width > 0 ? toplevel->width : runtime->output_width,
                              toplevel->height > 0 ? toplevel->height : runtime->output_height);
  }
}

static void focus_toplevel(struct diftray_toplevel *toplevel) {
  if (!toplevel || !toplevel->xdg_toplevel || !toplevel->xdg_toplevel->base) {
    return;
  }
  struct wlr_seat *seat = toplevel->runtime->seat;
  struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
  wlr_scene_node_raise_to_top(&toplevel->tree->node);
  wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
  if (keyboard) {
    wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
                                   keyboard->num_keycodes,
                                   &keyboard->modifiers);
  }
}

static void output_frame(struct wl_listener *listener, void *data) {
  struct diftray_output *output = wl_container_of(listener, output, frame);
  (void)data;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (wlr_scene_output_commit(output->scene_output, NULL)) {
    wlr_scene_output_send_frame_done(output->scene_output, &now);
    if (output->runtime->frame_handler) output->runtime->frame_handler(output->runtime->frame_userdata);
  }
}

static void notify_output_geometry(struct diftray_wayland_runtime *runtime);
static void layout_overlay(struct diftray_wayland_runtime *runtime);

static void output_request_state(struct wl_listener *listener, void *data) {
  struct diftray_output *output =
      wl_container_of(listener, output, request_state);
  const struct wlr_output_event_request_state *event = data;
  if (!wlr_output_commit_state(output->output, event->state)) return;
  layout_overlay(output->runtime);
  notify_output_geometry(output->runtime);
}

static void notify_output_geometry(struct diftray_wayland_runtime *runtime) {
  if (runtime->output_handler) {
    runtime->output_handler(runtime->output_userdata, runtime->output_width,
                            runtime->output_height);
  }
}

static void layout_overlay(struct diftray_wayland_runtime *runtime) {
  if (!runtime->command_bar) {
    return;
  }
  const int bar_height = runtime->style.command_bar_height;
  const int width = runtime->output_width > 0 ? runtime->output_width : 1920;
  const int height = runtime->output_height > 0 ? runtime->output_height : 1080;
  struct wlr_box bounds;
  wlr_output_layout_get_box(runtime->output_layout, NULL, &bounds);
  wlr_scene_rect_set_size(runtime->background, bounds.width, bounds.height);
  wlr_scene_node_set_position(&runtime->background->node, bounds.x, bounds.y);
  wlr_scene_rect_set_size(runtime->command_bar, width, bar_height);
  wlr_scene_node_set_position(&runtime->command_bar->node, runtime->output_x,
                              runtime->output_y + height - bar_height);
  if (runtime->command_bar_text) {
    wlr_scene_node_set_position(&runtime->command_bar_text->node, runtime->output_x,
                                runtime->output_y + height - bar_height);
  }
  if (runtime->status_text) {
    wlr_scene_node_set_position(&runtime->status_text->node, runtime->output_x, runtime->output_y);
  }
}

static void output_destroy(struct wl_listener *listener, void *data) {
  struct diftray_output *output = wl_container_of(listener, output, destroy);
  (void)data;
  wl_list_remove(&output->frame.link);
  wl_list_remove(&output->request_state.link);
  wl_list_remove(&output->destroy.link);
  wl_list_remove(&output->link);
  struct diftray_wayland_runtime *runtime = output->runtime;
  wlr_output_layout_remove(runtime->output_layout, output->output);
  free(output);
  notify_output_geometry(runtime);
}

static void new_output(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, new_output);
  struct wlr_output *wlr_output = data;
  if (!wlr_output_init_render(wlr_output, runtime->allocator,
                              runtime->renderer)) {
    return;
  }

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode) {
    wlr_output_state_set_mode(&state, mode);
  }
  bool committed = wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);
  if (!committed) return;

  struct diftray_output *output = calloc(1, sizeof(*output));
  if (!output) return;
  output->runtime = runtime;
  output->output = wlr_output;
  output->frame.notify = output_frame;
  output->request_state.notify = output_request_state;
  output->destroy.notify = output_destroy;
  wl_signal_add(&wlr_output->events.frame, &output->frame);
  wl_signal_add(&wlr_output->events.request_state, &output->request_state);
  wl_signal_add(&wlr_output->events.destroy, &output->destroy);
  wl_list_insert(&runtime->outputs, &output->link);

  struct wlr_output_layout_output *layout_output =
      wlr_output_layout_add_auto(runtime->output_layout, wlr_output);
  output->scene_output = wlr_scene_output_create(runtime->scene, wlr_output);
  wlr_scene_output_layout_add_output(runtime->scene_layout, layout_output,
                                     output->scene_output);

  wlr_output_effective_resolution(wlr_output, &runtime->output_width, &runtime->output_height);
  wlr_cursor_set_xcursor(runtime->cursor, runtime->cursor_mgr, "default");
  layout_overlay(runtime);
  notify_output_geometry(runtime);
}

static void toplevel_map(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel = wl_container_of(listener, toplevel, map);
  (void)data;
  toplevel->mapped = true;
  layout_gcursor(toplevel->runtime, toplevel);
  if (toplevel->runtime->gcursor_visible) {
    wlr_scene_node_set_enabled(&toplevel->tree->node, true);
    focus_toplevel(toplevel);
  }
}

static void toplevel_unmap(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel =
      wl_container_of(listener, toplevel, unmap);
  (void)data;
  toplevel->mapped = false;
}

static void toplevel_commit(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel =
      wl_container_of(listener, toplevel, commit);
  (void)data;
  if (toplevel->xdg_toplevel->base->initial_commit) {
    layout_gcursor(toplevel->runtime, toplevel);
  }
}

static void toplevel_request_minimize(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_minimize);
  (void)data;
  if (toplevel->runtime->toplevel_request_handler) {
    toplevel->runtime->toplevel_request_handler(
        toplevel->runtime->toplevel_userdata, toplevel->xdg_toplevel,
        "minimize");
  }
}

static void toplevel_request_maximize(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_maximize);
  (void)data;
  layout_gcursor(toplevel->runtime, toplevel);
}

static void toplevel_request_fullscreen(struct wl_listener *listener,
                                        void *data) {
  struct diftray_toplevel *toplevel =
      wl_container_of(listener, toplevel, request_fullscreen);
  (void)data;
  layout_gcursor(toplevel->runtime, toplevel);
}

static void toplevel_destroy(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel =
      wl_container_of(listener, toplevel, destroy);
  (void)data;
  if (toplevel->runtime->toplevel_destroy_handler) {
    toplevel->runtime->toplevel_destroy_handler(
        toplevel->runtime->toplevel_userdata, toplevel->xdg_toplevel);
  }
  wl_list_remove(&toplevel->map.link);
  wl_list_remove(&toplevel->unmap.link);
  wl_list_remove(&toplevel->commit.link);
  wl_list_remove(&toplevel->destroy.link);
  wl_list_remove(&toplevel->request_minimize.link);
  wl_list_remove(&toplevel->request_maximize.link);
  wl_list_remove(&toplevel->request_fullscreen.link);
  wl_list_remove(&toplevel->link);
  free(toplevel);
}

static int client_pid_of(struct wlr_xdg_toplevel *xdg_toplevel) {
  if (!xdg_toplevel || !xdg_toplevel->base || !xdg_toplevel->base->client ||
      !xdg_toplevel->base->client->client) {
    return -1;
  }
  pid_t pid = -1;
  wl_client_get_credentials(xdg_toplevel->base->client->client, &pid, NULL,
                            NULL);
  return (int)pid;
}

static void new_toplevel(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, new_toplevel);
  struct wlr_xdg_toplevel *xdg_toplevel = data;
  struct diftray_toplevel *toplevel = calloc(1, sizeof(*toplevel));
  toplevel->runtime = runtime;
  toplevel->xdg_toplevel = xdg_toplevel;
  toplevel->tree = wlr_scene_tree_create(runtime->gcursor_tree);
  toplevel->surface_tree =
      wlr_scene_xdg_surface_create(toplevel->tree, xdg_toplevel->base);
  xdg_toplevel->base->data = toplevel->surface_tree;
  wlr_scene_node_set_enabled(&toplevel->tree->node, false);

  toplevel->map.notify = toplevel_map;
  toplevel->unmap.notify = toplevel_unmap;
  toplevel->commit.notify = toplevel_commit;
  toplevel->destroy.notify = toplevel_destroy;
  toplevel->request_minimize.notify = toplevel_request_minimize;
  toplevel->request_maximize.notify = toplevel_request_maximize;
  toplevel->request_fullscreen.notify = toplevel_request_fullscreen;
  wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
  wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
  wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
  wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
  wl_signal_add(&xdg_toplevel->events.request_minimize,
                &toplevel->request_minimize);
  wl_signal_add(&xdg_toplevel->events.request_maximize,
                &toplevel->request_maximize);
  wl_signal_add(&xdg_toplevel->events.request_fullscreen,
                &toplevel->request_fullscreen);
  wl_list_insert(&runtime->toplevels, &toplevel->link);

  if (runtime->toplevel_handler) {
    runtime->toplevel_handler(runtime->toplevel_userdata, xdg_toplevel,
                              client_pid_of(xdg_toplevel));
  }
}

static void popup_commit(struct wl_listener *listener, void *data) {
  struct diftray_popup *popup = wl_container_of(listener, popup, commit);
  (void)data;
  if (popup->popup->base->initial_commit) {
    wlr_xdg_surface_schedule_configure(popup->popup->base);
  }
}

static void popup_destroy(struct wl_listener *listener, void *data) {
  struct diftray_popup *popup = wl_container_of(listener, popup, destroy);
  (void)data;
  wl_list_remove(&popup->commit.link);
  wl_list_remove(&popup->destroy.link);
  free(popup);
}

static void new_popup(struct wl_listener *listener, void *data) {
  (void)listener;
  struct wlr_xdg_popup *xdg_popup = data;
  struct wlr_xdg_surface *parent =
      wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
  if (!parent || !parent->data) {
    return;
  }
  xdg_popup->base->data =
      wlr_scene_xdg_surface_create(parent->data, xdg_popup->base);
  struct diftray_popup *popup = calloc(1, sizeof(*popup));
  popup->popup = xdg_popup;
  popup->commit.notify = popup_commit;
  popup->destroy.notify = popup_destroy;
  wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
  wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

static void keyboard_modifiers(struct wl_listener *listener, void *data) {
  struct diftray_keyboard *keyboard =
      wl_container_of(listener, keyboard, modifiers);
  (void)data;
  wlr_seat_set_keyboard(keyboard->runtime->seat, keyboard->keyboard);
  wlr_seat_keyboard_notify_modifiers(keyboard->runtime->seat,
                                     &keyboard->keyboard->modifiers);
}

static void keyboard_key(struct wl_listener *listener, void *data) {
  struct diftray_keyboard *keyboard = wl_container_of(listener, keyboard, key);
  struct wlr_keyboard_key_event *event = data;
  const xkb_keysym_t *symbols;
  int count = xkb_state_key_get_syms(keyboard->keyboard->xkb_state,
                                     event->keycode + 8, &symbols);
  uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->keyboard);
  uint32_t unicode = xkb_state_key_get_utf32(keyboard->keyboard->xkb_state,
                                             event->keycode + 8);
  bool handled = false;
  const bool tracked = event->keycode < sizeof(keyboard->consumed_keys);
  if (event->state == WL_KEYBOARD_KEY_STATE_RELEASED && tracked) {
    handled = keyboard->consumed_keys[event->keycode];
    keyboard->consumed_keys[event->keycode] = false;
  } else if (keyboard->runtime->key_handler) {
    // A key may have multiple symbols, but represents a single key event.
    handled = keyboard->runtime->key_handler(
        keyboard->runtime->key_handler_userdata,
        count > 0 ? symbols[0] : 0, modifiers, event->state, unicode,
        event->time_msec, event->keycode);
    if (tracked && event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
      keyboard->consumed_keys[event->keycode] = handled;
    }
  }
  if (!handled && keyboard->runtime->gcursor_visible) {
    wlr_seat_set_keyboard(keyboard->runtime->seat, keyboard->keyboard);
    wlr_seat_keyboard_notify_key(keyboard->runtime->seat, event->time_msec,
                                 event->keycode, event->state);
  }
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
  struct diftray_keyboard *keyboard =
      wl_container_of(listener, keyboard, destroy);
  (void)data;
  wl_list_remove(&keyboard->modifiers.link);
  wl_list_remove(&keyboard->key.link);
  wl_list_remove(&keyboard->destroy.link);
  wl_list_remove(&keyboard->link);
  free(keyboard);
}

static void new_keyboard(struct diftray_wayland_runtime *runtime,
                         struct wlr_input_device *device) {
  struct diftray_keyboard *keyboard = calloc(1, sizeof(*keyboard));
  keyboard->runtime = runtime;
  keyboard->keyboard = wlr_keyboard_from_input_device(device);
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_keymap *keymap =
      xkb_keymap_new_from_names(context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
  wlr_keyboard_set_keymap(keyboard->keyboard, keymap);
  xkb_keymap_unref(keymap);
  xkb_context_unref(context);
  keyboard->modifiers.notify = keyboard_modifiers;
  keyboard->key.notify = keyboard_key;
  keyboard->destroy.notify = keyboard_destroy;
  wl_signal_add(&keyboard->keyboard->events.modifiers, &keyboard->modifiers);
  wl_signal_add(&keyboard->keyboard->events.key, &keyboard->key);
  wl_signal_add(&device->events.destroy, &keyboard->destroy);
  wl_list_insert(&runtime->keyboards, &keyboard->link);
  wlr_seat_set_keyboard(runtime->seat, keyboard->keyboard);
}

static struct wlr_surface *gcursor_surface_at(
    struct diftray_wayland_runtime *runtime, double lx, double ly, double *sx,
    double *sy) {
  if (!runtime->gcursor_visible) {
    return NULL;
  }
  struct wlr_scene_node *node =
      wlr_scene_node_at(&runtime->gcursor_tree->node, lx, ly, sx, sy);
  if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
    return NULL;
  }
  struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
  struct wlr_scene_surface *scene_surface =
      wlr_scene_surface_try_from_buffer(scene_buffer);
  if (!scene_surface) {
    return NULL;
  }
  return scene_surface->surface;
}

static void process_cursor_motion(struct diftray_wayland_runtime *runtime,
                                  uint32_t time_msec) {
  if (!runtime->gcursor_visible) {
    wlr_seat_pointer_clear_focus(runtime->seat);
    wlr_cursor_set_xcursor(runtime->cursor, runtime->cursor_mgr, "default");
    return;
  }
  double sx = 0.0;
  double sy = 0.0;
  struct wlr_surface *surface = gcursor_surface_at(
      runtime, runtime->cursor->x, runtime->cursor->y, &sx, &sy);
  if (surface) {
    wlr_seat_pointer_notify_enter(runtime->seat, surface, sx, sy);
    wlr_seat_pointer_notify_motion(runtime->seat, time_msec, sx, sy);
  } else {
    wlr_seat_pointer_clear_focus(runtime->seat);
    wlr_cursor_set_xcursor(runtime->cursor, runtime->cursor_mgr, "default");
  }
}

static void cursor_motion(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, cursor_motion);
  struct wlr_pointer_motion_event *event = data;
  wlr_cursor_move(runtime->cursor, &event->pointer->base, event->delta_x,
                  event->delta_y);
  process_cursor_motion(runtime, event->time_msec);
}

static void cursor_motion_absolute(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, cursor_motion_absolute);
  struct wlr_pointer_motion_absolute_event *event = data;
  wlr_cursor_warp_absolute(runtime->cursor, &event->pointer->base, event->x,
                           event->y);
  process_cursor_motion(runtime, event->time_msec);
}

static void cursor_button(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, cursor_button);
  struct wlr_pointer_button_event *event = data;
  if (!runtime->gcursor_visible) {
    return;
  }
  if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
    double sx = 0, sy = 0;
    struct wlr_scene_node *hit = wlr_scene_node_at(
        &runtime->gcursor_tree->node, runtime->cursor->x, runtime->cursor->y,
        &sx, &sy);
    if (hit) {
      struct diftray_toplevel *top;
      wl_list_for_each(top, &runtime->toplevels, link) {
        for (struct wlr_scene_node *node = hit; node; node =
                 node->parent ? &node->parent->node : NULL) {
          if (node == &top->tree->node) {
            focus_toplevel(top);
            if (runtime->focus_handler) {
              runtime->focus_handler(runtime->toplevel_userdata, top->xdg_toplevel);
            }
            break;
          }
        }
      }
    }
  }
  wlr_seat_pointer_notify_button(runtime->seat, event->time_msec,
                                 event->button, event->state);
}

static void cursor_axis(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, cursor_axis);
  struct wlr_pointer_axis_event *event = data;
  if (!runtime->gcursor_visible) {
    return;
  }
  wlr_seat_pointer_notify_axis(runtime->seat, event->time_msec,
                               event->orientation, event->delta,
                               event->delta_discrete, event->source,
                               event->relative_direction);
}

static void cursor_frame(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, cursor_frame);
  (void)data;
  if (runtime->gcursor_visible) {
    wlr_seat_pointer_notify_frame(runtime->seat);
  }
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, request_cursor);
  struct wlr_seat_pointer_request_set_cursor_event *event = data;
  struct wlr_seat_client *focused_client =
      runtime->seat->pointer_state.focused_client;
  if (focused_client == event->seat_client) {
    wlr_cursor_set_surface(runtime->cursor, event->surface, event->hotspot_x,
                           event->hotspot_y);
  }
}

static void seat_request_set_selection(struct wl_listener *listener,
                                       void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, request_set_selection);
  struct wlr_seat_request_set_selection_event *event = data;
  wlr_seat_set_selection(runtime->seat, event->source, event->serial);
}

static void new_input(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, new_input);
  struct wlr_input_device *device = data;
  switch (device->type) {
  case WLR_INPUT_DEVICE_KEYBOARD:
    new_keyboard(runtime, device);
    break;
  case WLR_INPUT_DEVICE_POINTER:
    wlr_cursor_attach_input_device(runtime->cursor, device);
    break;
  default:
    break;
  }
  uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
  if (!wl_list_empty(&runtime->keyboards)) {
    caps |= WL_SEAT_CAPABILITY_KEYBOARD;
  }
  wlr_seat_set_capabilities(runtime->seat, caps);
}

static void refresh_command_bar(struct diftray_wayland_runtime *runtime) {
  if (!runtime->command_bar) {
    return;
  }
  wlr_scene_node_set_enabled(&runtime->command_bar->node,
                             runtime->command_bar_visible);
  const int width = runtime->output_width > 0 ? runtime->output_width : 800;
  const int height = runtime->style.command_bar_height;
  if (!runtime->command_bar_buffer ||
      runtime->command_bar_buffer->base.width != width ||
      runtime->command_bar_buffer->base.height != height) {
    if (runtime->command_bar_buffer) {
      wlr_buffer_drop(&runtime->command_bar_buffer->base);
    }
    runtime->command_bar_buffer = pixel_buffer_create(width, height);
    if (!runtime->command_bar_text) {
      runtime->command_bar_text =
          wlr_scene_buffer_create(runtime->overlay_tree, NULL);
    }
  }
  if (!runtime->command_bar_buffer || !runtime->command_bar_text) {
    return;
  }
  render_chrome_text(runtime, runtime->command_bar_buffer,
                     runtime->command_bar_text_copy,
                     runtime->style.command_bar_color,
                     runtime->style.border_color);
  wlr_scene_buffer_set_buffer(runtime->command_bar_text,
                              &runtime->command_bar_buffer->base);
  wlr_scene_node_set_enabled(&runtime->command_bar_text->node,
                             runtime->command_bar_visible);
  layout_overlay(runtime);
}

static void refresh_status_line(struct diftray_wayland_runtime *runtime) {
  const int width = runtime->output_width > 0 ? runtime->output_width : 800;
  const int height = runtime->style.status_bar_height;
  if (!runtime->status_buffer || runtime->status_buffer->base.width != width ||
      runtime->status_buffer->base.height != height) {
    if (runtime->status_buffer) {
      wlr_buffer_drop(&runtime->status_buffer->base);
    }
    runtime->status_buffer = pixel_buffer_create(width, height);
    if (!runtime->status_text) {
      runtime->status_text =
          wlr_scene_buffer_create(runtime->overlay_tree, NULL);
    }
  }
  if (!runtime->status_buffer || !runtime->status_text) {
    return;
  }
  float bg[4] = {runtime->style.background_color[0],
                 runtime->style.background_color[1],
                 runtime->style.background_color[2], 0.85f};
  render_chrome_text(runtime, runtime->status_buffer, runtime->status_line, bg,
                     runtime->style.border_color);
  wlr_scene_buffer_set_buffer(runtime->status_text,
                              &runtime->status_buffer->base);
  layout_overlay(runtime);
}

struct diftray_wayland_runtime *diftray_wayland_runtime_create(
    struct wl_display *display, const struct diftray_wayland_style *style) {
  struct diftray_wayland_runtime *runtime = calloc(1, sizeof(*runtime));
  if (!runtime) return NULL;
  runtime->display = display;
  runtime->style = *style;
  if (runtime->style.highlight_color[3] <= 0.0f) {
    runtime->style.highlight_color[0] = 1.0f;
    runtime->style.highlight_color[1] = 0.72f;
    runtime->style.highlight_color[2] = 0.18f;
    runtime->style.highlight_color[3] = 1.0f;
  }
  wl_list_init(&runtime->outputs);
  wl_list_init(&runtime->keyboards);
  wl_list_init(&runtime->toplevels);
  wlr_log_init(WLR_INFO, NULL);
  runtime->backend =
      wlr_backend_autocreate(wl_display_get_event_loop(display), NULL);
  if (!runtime->backend) {
    free(runtime);
    return NULL;
  }
  runtime->renderer = wlr_renderer_autocreate(runtime->backend);
  runtime->allocator =
      runtime->renderer
          ? wlr_allocator_autocreate(runtime->backend, runtime->renderer)
          : NULL;
  if (!runtime->renderer || !runtime->allocator) {
    diftray_wayland_runtime_destroy(runtime);
    return NULL;
  }
  wlr_renderer_init_wl_display(runtime->renderer, display);
  wlr_compositor_create(display, 6, runtime->renderer);
  wlr_subcompositor_create(display);
  wlr_data_device_manager_create(display);
  runtime->output_layout = wlr_output_layout_create(display);
  runtime->scene = wlr_scene_create();
  runtime->scene_layout =
      wlr_scene_attach_output_layout(runtime->scene, runtime->output_layout);
  runtime->background = wlr_scene_rect_create(
      &runtime->scene->tree, 1920, 1080, runtime->style.background_color);
  runtime->ncursor_tree = wlr_scene_tree_create(&runtime->scene->tree);
  runtime->gcursor_tree = wlr_scene_tree_create(&runtime->scene->tree);
  runtime->overlay_tree = wlr_scene_tree_create(&runtime->scene->tree);
  runtime->command_bar = wlr_scene_rect_create(
      runtime->overlay_tree, 1920, runtime->style.command_bar_height,
      runtime->style.command_bar_color);
  wlr_scene_node_set_enabled(&runtime->command_bar->node, false);
  wlr_scene_node_set_enabled(&runtime->gcursor_tree->node, false);
  runtime->xdg_shell = wlr_xdg_shell_create(display, 3);
  runtime->seat = wlr_seat_create(display, "seat0");
  runtime->cursor = wlr_cursor_create();
  wlr_cursor_attach_output_layout(runtime->cursor, runtime->output_layout);
  runtime->cursor_mgr = wlr_xcursor_manager_create(NULL, 24);
  wlr_xcursor_manager_load(runtime->cursor_mgr, 1);

  runtime->new_output.notify = new_output;
  runtime->new_input.notify = new_input;
  runtime->new_toplevel.notify = new_toplevel;
  runtime->new_popup.notify = new_popup;
  runtime->request_cursor.notify = seat_request_cursor;
  runtime->request_set_selection.notify = seat_request_set_selection;
  runtime->cursor_motion.notify = cursor_motion;
  runtime->cursor_motion_absolute.notify = cursor_motion_absolute;
  runtime->cursor_button.notify = cursor_button;
  runtime->cursor_axis.notify = cursor_axis;
  runtime->cursor_frame.notify = cursor_frame;
  wl_signal_add(&runtime->backend->events.new_output, &runtime->new_output);
  wl_signal_add(&runtime->backend->events.new_input, &runtime->new_input);
  wl_signal_add(&runtime->xdg_shell->events.new_toplevel,
                &runtime->new_toplevel);
  wl_signal_add(&runtime->xdg_shell->events.new_popup, &runtime->new_popup);
  wl_signal_add(&runtime->seat->events.request_set_cursor,
                &runtime->request_cursor);
  wl_signal_add(&runtime->seat->events.request_set_selection,
                &runtime->request_set_selection);
  wl_signal_add(&runtime->cursor->events.motion, &runtime->cursor_motion);
  wl_signal_add(&runtime->cursor->events.motion_absolute,
                &runtime->cursor_motion_absolute);
  wl_signal_add(&runtime->cursor->events.button, &runtime->cursor_button);
  wl_signal_add(&runtime->cursor->events.axis, &runtime->cursor_axis);
  wl_signal_add(&runtime->cursor->events.frame, &runtime->cursor_frame);
  runtime->status_line = strdup("DiftrayWM ncursor");
  return runtime;
}

bool diftray_wayland_runtime_start(struct diftray_wayland_runtime *runtime) {
  return runtime && wlr_backend_start(runtime->backend);
}

void diftray_wayland_runtime_set_text_renderer(struct diftray_wayland_runtime *runtime,
    diftray_text_renderer renderer, void *userdata) {
  if (!runtime) return;
  runtime->text_renderer = renderer;
  runtime->text_userdata = userdata;
}

void diftray_wayland_runtime_set_key_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_key_handler handler, void *userdata) {
  if (!runtime) {
    return;
  }
  runtime->key_handler = handler;
  runtime->key_handler_userdata = userdata;
}

void diftray_wayland_runtime_set_toplevel_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_toplevel_handler handler,
    diftray_wayland_toplevel_destroy_handler destroy_handler,
    diftray_wayland_toplevel_request_handler request_handler, void *userdata) {
  if (!runtime) {
    return;
  }
  runtime->toplevel_handler = handler;
  runtime->toplevel_destroy_handler = destroy_handler;
  runtime->toplevel_request_handler = request_handler;
  runtime->toplevel_userdata = userdata;
}

void diftray_wayland_runtime_set_focus_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_toplevel_focus_handler handler) {
  if (runtime) runtime->focus_handler = handler;
}

void diftray_wayland_runtime_set_output_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_output_handler handler, void *userdata) {
  if (!runtime) {
    return;
  }
  runtime->output_handler = handler;
  runtime->output_userdata = userdata;
  if (runtime->output_width > 0 && runtime->output_height > 0) {
    handler(userdata, runtime->output_width, runtime->output_height);
  }
}

bool diftray_wayland_runtime_output_at(struct diftray_wayland_runtime *runtime,
    size_t index, struct diftray_output_geometry *geometry) {
  if (!runtime || !geometry) return false;
  struct diftray_output *output;
  wl_list_for_each(output, &runtime->outputs, link) {
    if (!output->output->enabled) continue;
    if (index-- != 0) continue;
    struct wlr_box box;
    wlr_output_layout_get_box(runtime->output_layout, output->output, &box);
    *geometry = (struct diftray_output_geometry){output->output->name,
        box.x, box.y, box.width, box.height};
    return true;
  }
  return false;
}

void diftray_wayland_runtime_set_chrome_box(struct diftray_wayland_runtime *runtime,
    int x, int y, int width, int height) {
  if (!runtime) return;
  runtime->output_x = x; runtime->output_y = y;
  runtime->output_width = width; runtime->output_height = height;
  layout_overlay(runtime);
}

bool diftray_wayland_runtime_output_size(struct diftray_wayland_runtime *runtime,
                                         int *width, int *height) {
  if (!runtime) {
    return false;
  }
  if (width) {
    *width = runtime->output_width;
  }
  if (height) {
    *height = runtime->output_height;
  }
  return runtime->output_width > 0 && runtime->output_height > 0;
}

void diftray_wayland_runtime_set_ncursor_visible(
    struct diftray_wayland_runtime *runtime, bool visible) {
  if (!runtime) {
    return;
  }
  wlr_scene_node_set_enabled(&runtime->ncursor_tree->node, visible);
}

void diftray_wayland_runtime_set_gcursor_visible(
    struct diftray_wayland_runtime *runtime, bool visible) {
  if (!runtime) {
    return;
  }
  runtime->gcursor_visible = visible;
  wlr_scene_node_set_enabled(&runtime->gcursor_tree->node, visible);
  if (!visible) {
    wlr_seat_pointer_clear_focus(runtime->seat);
    wlr_seat_keyboard_clear_focus(runtime->seat);
  }
}

struct diftray_cell_surface *diftray_cell_surface_create(
    struct diftray_wayland_runtime *runtime) {
  if (!runtime) {
    return NULL;
  }
  struct diftray_cell_surface *surface = calloc(1, sizeof(*surface));
  surface->runtime = runtime;
  surface->tree = wlr_scene_tree_create(runtime->ncursor_tree);
  surface->border = wlr_scene_rect_create(surface->tree, 1, 1,
                                          runtime->style.border_color);
  surface->highlight = wlr_scene_rect_create(surface->tree, 1, 1,
                                             runtime->style.highlight_color);
  wlr_scene_node_set_enabled(&surface->highlight->node, false);
  surface->buffer_node = wlr_scene_buffer_create(surface->tree, NULL);
  return surface;
}

void diftray_cell_surface_destroy(struct diftray_cell_surface *surface) {
  if (!surface) {
    return;
  }
  if (surface->tree) {
    wlr_scene_node_destroy(&surface->tree->node);
  }
  if (surface->buffer) {
    wlr_buffer_drop(&surface->buffer->base);
  }
  free(surface);
}

void diftray_cell_surface_place(struct diftray_cell_surface *surface, int x,
                                int y, int width, int height) {
  if (!surface) {
    return;
  }
  surface->x = x;
  surface->y = y;
  surface->width = width;
  surface->height = height;
  const int border = surface->runtime->style.border_size;
  wlr_scene_rect_set_color(surface->border, surface->runtime->style.border_color);
  wlr_scene_rect_set_color(surface->highlight, surface->runtime->style.highlight_color);
  wlr_scene_node_set_position(&surface->tree->node, x, y);
  wlr_scene_rect_set_size(surface->border, width, height);
  wlr_scene_rect_set_size(surface->highlight, width, height);
  wlr_scene_node_set_position(&surface->buffer_node->node, border, border);
}

bool diftray_cell_surface_update(struct diftray_cell_surface *surface,
                                 const uint32_t *pixels, int width,
                                 int height) {
  if (!surface || !pixels || width <= 0 || height <= 0) {
    return false;
  }
  if (!surface->buffer || surface->buffer->base.width != width ||
      surface->buffer->base.height != height) {
    if (surface->buffer) {
      wlr_buffer_drop(&surface->buffer->base);
    }
    surface->buffer = pixel_buffer_create(width, height);
    if (!surface->buffer) {
      return false;
    }
  }
  memcpy(surface->buffer->pixels, pixels,
         (size_t)width * (size_t)height * sizeof(uint32_t));
  wlr_scene_buffer_set_buffer(surface->buffer_node, &surface->buffer->base);
  return true;
}

void diftray_cell_surface_set_highlight(struct diftray_cell_surface *surface,
                                        bool highlighted) {
  if (!surface) {
    return;
  }
  wlr_scene_node_set_enabled(&surface->highlight->node, highlighted);
}

void diftray_cell_surface_set_visible(struct diftray_cell_surface *surface,
                                      bool visible) {
  if (!surface) {
    return;
  }
  wlr_scene_node_set_enabled(&surface->tree->node, visible);
}

void diftray_wayland_runtime_attach_gcursor(
    struct diftray_wayland_runtime *runtime,
    struct wlr_xdg_toplevel *toplevel) {
  struct diftray_toplevel *found = toplevel_from_xdg(runtime, toplevel);
  if (!found) {
    return;
  }
  wlr_scene_node_set_enabled(&found->tree->node, true);
  layout_gcursor(runtime, found);
  focus_toplevel(found);
}

void diftray_wayland_runtime_set_gcursor_visible_surface(
    struct diftray_wayland_runtime *runtime,
    struct wlr_xdg_toplevel *toplevel, bool visible) {
  struct diftray_toplevel *found = toplevel_from_xdg(runtime, toplevel);
  if (!found) {
    return;
  }
  wlr_scene_node_set_enabled(&found->tree->node, visible);
}

void diftray_wayland_runtime_focus_gcursor(
    struct diftray_wayland_runtime *runtime,
    struct wlr_xdg_toplevel *toplevel) {
  struct diftray_toplevel *found = toplevel_from_xdg(runtime, toplevel);
  if (!found) {
    return;
  }
  layout_gcursor(runtime, found);
  focus_toplevel(found);
}

void diftray_wayland_runtime_layout_gcursor(
    struct diftray_wayland_runtime *runtime, struct wlr_xdg_toplevel *toplevel,
    int x, int y, int width, int height) {
  struct diftray_toplevel *found = toplevel_from_xdg(runtime, toplevel);
  if (!found) return;
  found->x = x;
  found->y = y;
  found->width = width;
  found->height = height;
  layout_gcursor(runtime, found);
}

void diftray_wayland_runtime_clear_keyboard_focus(
    struct diftray_wayland_runtime *runtime) {
  if (runtime) {
    wlr_seat_keyboard_clear_focus(runtime->seat);
  }
}

void diftray_wayland_runtime_set_style(struct diftray_wayland_runtime *runtime,
                                      const struct diftray_wayland_style *style) {
  if (!runtime || !style) {
    return;
  }
  runtime->style = *style;
  wlr_scene_rect_set_color(runtime->background, style->background_color);
  wlr_scene_rect_set_color(runtime->command_bar, style->command_bar_color);
  layout_overlay(runtime);
  refresh_command_bar(runtime);
  refresh_status_line(runtime);
}

void diftray_wayland_runtime_set_command_bar(
    struct diftray_wayland_runtime *runtime, bool visible, const char *text) {
  if (!runtime) {
    return;
  }
  runtime->command_bar_visible = visible;
  free(runtime->command_bar_text_copy);
  runtime->command_bar_text_copy = strdup(text ? text : "");
  refresh_command_bar(runtime);
}

void diftray_wayland_runtime_set_status_line(
    struct diftray_wayland_runtime *runtime, const char *text) {
  if (!runtime) {
    return;
  }
  free(runtime->status_line);
  runtime->status_line = strdup(text ? text : "");
  refresh_status_line(runtime);
}

static void disconnect_listener(struct wl_listener *listener) {
  if (listener->link.next) {
    wl_list_remove(&listener->link);
    wl_list_init(&listener->link);
  }
}

void diftray_wayland_runtime_destroy(struct diftray_wayland_runtime *runtime) {
  if (!runtime) {
    return;
  }
  runtime->output_handler = NULL;
  runtime->toplevel_destroy_handler = NULL;
  runtime->toplevel_request_handler = NULL;
  runtime->focus_handler = NULL;
  disconnect_listener(&runtime->new_output);
  disconnect_listener(&runtime->new_input);
  disconnect_listener(&runtime->new_toplevel);
  disconnect_listener(&runtime->new_popup);
  disconnect_listener(&runtime->request_cursor);
  disconnect_listener(&runtime->request_set_selection);
  disconnect_listener(&runtime->cursor_motion);
  disconnect_listener(&runtime->cursor_motion_absolute);
  disconnect_listener(&runtime->cursor_button);
  disconnect_listener(&runtime->cursor_axis);
  disconnect_listener(&runtime->cursor_frame);
  wl_display_destroy_clients(runtime->display);
  if (runtime->command_bar_buffer) {
    wlr_buffer_drop(&runtime->command_bar_buffer->base);
  }
  if (runtime->status_buffer) {
    wlr_buffer_drop(&runtime->status_buffer->base);
  }
  if (runtime->cursor_mgr) {
    wlr_xcursor_manager_destroy(runtime->cursor_mgr);
  }
  if (runtime->cursor) {
    wlr_cursor_destroy(runtime->cursor);
  }
  if (runtime->backend) {
    wlr_backend_destroy(runtime->backend);
  }
  if (runtime->scene) {
    wlr_scene_node_destroy(&runtime->scene->tree.node);
  }
  if (runtime->output_layout) {
    wlr_output_layout_destroy(runtime->output_layout);
  }
  if (runtime->allocator) {
    wlr_allocator_destroy(runtime->allocator);
  }
  if (runtime->renderer) {
    wlr_renderer_destroy(runtime->renderer);
  }
  free(runtime->command_bar_text_copy);
  free(runtime->status_line);
  free(runtime);
}

void diftray_wayland_runtime_set_frame_handler(struct diftray_wayland_runtime *runtime,
    void (*handler)(void *), void *userdata) {
  if (!runtime) return;
  runtime->frame_handler = handler;
  runtime->frame_userdata = userdata;
}
