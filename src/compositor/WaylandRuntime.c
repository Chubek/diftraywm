#include "compositor/WaylandRuntime.h"

#include <ctype.h>
#include <drm_fourcc.h>
#include <stdint.h>
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
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

struct diftray_wayland_runtime {
  struct wl_display *display;
  struct wlr_backend *backend;
  struct wlr_renderer *renderer;
  struct wlr_allocator *allocator;
  struct wlr_output_layout *output_layout;
  struct wlr_scene *scene;
  struct wlr_scene_output_layout *scene_layout;
  struct wlr_xdg_shell *xdg_shell;
  struct wlr_seat *seat;
  struct wlr_scene_rect *background;
  struct wlr_scene_rect *command_bar;
  struct wl_listener new_output;
  struct wl_listener new_input;
  struct wl_listener new_toplevel;
  struct wl_listener new_popup;
  struct wl_list outputs;
  struct wl_list keyboards;
  struct wl_list toplevels;
  char *terminal_text;
  diftray_wayland_key_handler key_handler;
  void *key_handler_userdata;
  bool command_bar_visible;
  struct diftray_wayland_style style;
};

struct diftray_output {
  struct wl_list link;
  struct diftray_wayland_runtime *runtime;
  struct wlr_output *output;
  struct wlr_scene_output *scene_output;
  struct wlr_scene_buffer *terminal_scene;
  struct diftray_terminal_buffer *terminal_buffer;
  struct wl_listener frame;
  struct wl_listener request_state;
  struct wl_listener destroy;
};

struct diftray_terminal_buffer {
  struct wlr_buffer base;
  uint32_t *pixels;
  size_t stride;
};

/*
 * A compact 5x7 font is used for the compositor bootstrap terminal. It keeps
 * the output useful before the full HarfBuzz/FreeType glyph pipeline is
 * connected to the scene graph. Lowercase input is rendered using its
 * uppercase glyph, which is sufficient for shell startup and diagnostics.
 */
static const uint8_t terminal_glyphs[128][7] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    ['!'] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x00, 0x04},
    ['#'] = {0x0a, 0x1f, 0x0a, 0x0a, 0x1f, 0x0a, 0x00},
    ['$'] = {0x04, 0x0f, 0x14, 0x0e, 0x05, 0x1e, 0x04},
    ['%'] = {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13},
    ['&'] = {0x0c, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0d},
    ['('] = {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02},
    [')'] = {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08},
    ['*'] = {0x00, 0x04, 0x15, 0x0e, 0x15, 0x04, 0x00},
    ['+'] = {0x00, 0x04, 0x04, 0x1f, 0x04, 0x04, 0x00},
    [','] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x04},
    ['-'] = {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00},
    ['.'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06},
    ['/'] = {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10},
    ['0'] = {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e},
    ['1'] = {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
    ['2'] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f},
    ['3'] = {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e},
    ['4'] = {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02},
    ['5'] = {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e},
    ['6'] = {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e},
    ['7'] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
    ['8'] = {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e},
    ['9'] = {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c},
    [':'] = {0x00, 0x06, 0x06, 0x00, 0x06, 0x06, 0x00},
    [';'] = {0x00, 0x06, 0x06, 0x00, 0x06, 0x04, 0x08},
    ['<'] = {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
    ['='] = {0x00, 0x1f, 0x00, 0x1f, 0x00, 0x00, 0x00},
    ['>'] = {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08},
    ['?'] = {0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04},
    ['A'] = {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
    ['B'] = {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e},
    ['C'] = {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e},
    ['D'] = {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e},
    ['E'] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f},
    ['F'] = {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10},
    ['G'] = {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f},
    ['H'] = {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11},
    ['I'] = {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e},
    ['J'] = {0x01, 0x01, 0x01, 0x01, 0x11, 0x11, 0x0e},
    ['K'] = {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11},
    ['L'] = {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f},
    ['M'] = {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11},
    ['N'] = {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11},
    ['O'] = {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
    ['P'] = {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10},
    ['Q'] = {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d},
    ['R'] = {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11},
    ['S'] = {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e},
    ['T'] = {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
    ['U'] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e},
    ['V'] = {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04},
    ['W'] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a},
    ['X'] = {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11},
    ['Y'] = {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04},
    ['Z'] = {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f},
    ['_'] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f},
    ['|'] = {0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04},
};

struct diftray_toplevel {
  struct wl_list link;
  struct diftray_wayland_runtime *runtime;
  struct wlr_xdg_toplevel *xdg_toplevel;
  struct wlr_scene_tree *tree;
  struct wlr_scene_rect *border;
  struct wlr_scene_tree *surface_tree;
  struct wl_listener map;
  struct wl_listener unmap;
  struct wl_listener commit;
  struct wl_listener destroy;
};

struct diftray_keyboard {
  struct wl_list link;
  struct diftray_wayland_runtime *runtime;
  struct wlr_keyboard *keyboard;
  struct wl_listener modifiers;
  struct wl_listener key;
  struct wl_listener destroy;
};

struct diftray_popup {
  struct wlr_xdg_popup *popup;
  struct wl_listener commit;
  struct wl_listener destroy;
};

static uint8_t channel_from_float(float value) {
  if (value <= 0.0f) {
    return 0;
  }
  if (value >= 1.0f) {
    return 255;
  }
  return (uint8_t)(value * 255.0f + 0.5f);
}

static uint32_t pack_color(const float color[4]) {
  const uint8_t alpha = channel_from_float(color[3]);
  const uint8_t red = (uint8_t)((uint16_t)channel_from_float(color[0]) * alpha / 255);
  const uint8_t green = (uint8_t)((uint16_t)channel_from_float(color[1]) * alpha / 255);
  const uint8_t blue = (uint8_t)((uint16_t)channel_from_float(color[2]) * alpha / 255);
  return ((uint32_t)alpha << 24) | ((uint32_t)red << 16) |
         ((uint32_t)green << 8) | blue;
}

static void terminal_buffer_destroy(struct wlr_buffer *wlr_buffer) {
  struct diftray_terminal_buffer *buffer =
      wl_container_of(wlr_buffer, buffer, base);
  wlr_buffer_finish(wlr_buffer);
  free(buffer->pixels);
  free(buffer);
}

static bool terminal_buffer_begin_data_ptr_access(
    struct wlr_buffer *wlr_buffer, uint32_t flags, void **data,
    uint32_t *format, size_t *stride) {
  struct diftray_terminal_buffer *buffer =
      wl_container_of(wlr_buffer, buffer, base);
  if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
    return false;
  }
  *data = buffer->pixels;
  *format = DRM_FORMAT_ARGB8888;
  *stride = buffer->stride;
  return true;
}

static void terminal_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {
  (void)wlr_buffer;
}

static const struct wlr_buffer_impl terminal_buffer_impl = {
    .destroy = terminal_buffer_destroy,
    .begin_data_ptr_access = terminal_buffer_begin_data_ptr_access,
    .end_data_ptr_access = terminal_buffer_end_data_ptr_access,
};

static struct diftray_terminal_buffer *terminal_buffer_create(int width,
                                                               int height) {
  if (width <= 0 || height <= 0) {
    return NULL;
  }
  struct diftray_terminal_buffer *buffer = calloc(1, sizeof(*buffer));
  if (!buffer) {
    return NULL;
  }
  buffer->stride = (size_t)width * sizeof(uint32_t);
  buffer->pixels = calloc((size_t)width * (size_t)height, sizeof(uint32_t));
  if (!buffer->pixels) {
    free(buffer);
    return NULL;
  }
  wlr_buffer_init(&buffer->base, &terminal_buffer_impl, width, height);
  return buffer;
}

static void draw_terminal_pixel(struct diftray_terminal_buffer *buffer,
                                int x, int y, uint32_t color) {
  if (!buffer || x < 0 || y < 0 || x >= buffer->base.width ||
      y >= buffer->base.height) {
    return;
  }
  buffer->pixels[(size_t)y * (buffer->stride / sizeof(uint32_t)) +
                (size_t)x] = color;
}

static void draw_terminal_glyph(struct diftray_terminal_buffer *buffer,
                                unsigned char character, int x, int y,
                                int scale, uint32_t color) {
  unsigned char glyph_character = character;
  if (glyph_character >= 'a' && glyph_character <= 'z') {
    glyph_character = (unsigned char)toupper(glyph_character);
  }
  const uint8_t *rows = terminal_glyphs[glyph_character < 128 ? glyph_character : '?'];
  for (int row = 0; row < 7; ++row) {
    for (int column = 0; column < 5; ++column) {
      if ((rows[row] & (1u << (4 - column))) == 0) {
        continue;
      }
      for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx) {
          draw_terminal_pixel(buffer, x + column * scale + dx,
                              y + row * scale + dy, color);
        }
      }
    }
  }
}

static void render_terminal_buffer(struct diftray_wayland_runtime *runtime,
                                   struct diftray_output *output) {
  if (!runtime || !output || !output->output || !output->terminal_scene) {
    return;
  }
  const int width = output->output->width;
  const int height = output->output->height;
  if (width <= 0 || height <= 0) {
    return;
  }
  if (!output->terminal_buffer ||
      output->terminal_buffer->base.width != width ||
      output->terminal_buffer->base.height != height) {
    if (output->terminal_buffer) {
      wlr_buffer_drop(&output->terminal_buffer->base);
    }
    output->terminal_buffer = terminal_buffer_create(width, height);
    if (!output->terminal_buffer) {
      return;
    }
  }

  const uint32_t background = pack_color(runtime->style.background_color);
  const uint32_t foreground = pack_color(runtime->style.border_color);
  const size_t pixels = (size_t)width * (size_t)height;
  for (size_t index = 0; index < pixels; ++index) {
    output->terminal_buffer->pixels[index] = background;
  }

  const int scale = 2;
  const int advance = 12 * scale / 2;
  const int line_height = 10 * scale;
  int x = 24;
  int y = 24;
  bool escape = false;
  const char *text = runtime->terminal_text;
  if (!text || !*text) {
    text = "DiftrayWM";
  }
  for (const unsigned char *cursor = (const unsigned char *)text; *cursor;
       ++cursor) {
    const unsigned char character = *cursor;
    if (escape) {
      if ((character >= 'a' && character <= 'z') ||
          (character >= 'A' && character <= 'Z')) {
        escape = false;
      }
      continue;
    }
    if (character == 0x1b) {
      escape = true;
      continue;
    }
    if (character == '\r') {
      x = 24;
      continue;
    }
    if (character == '\n') {
      x = 24;
      y += line_height;
      continue;
    }
    if (character == '\t') {
      x += advance * 4;
      continue;
    }
    if (character < 0x20) {
      continue;
    }
    if (x + 5 * scale >= width - 24) {
      x = 24;
      y += line_height;
    }
    if (y + 7 * scale >= height - 24) {
      break;
    }
    draw_terminal_glyph(output->terminal_buffer, character, x, y, scale,
                        foreground);
    x += advance;
  }
  if (x >= 24 && y + 7 * scale < height - 24) {
    for (int cursor_x = x; cursor_x < x + 8 && cursor_x < width - 24;
         ++cursor_x) {
      draw_terminal_pixel(output->terminal_buffer, cursor_x,
                          y + 7 * scale + 2, foreground);
    }
  }
  wlr_scene_buffer_set_buffer_with_damage(output->terminal_scene,
                                           &output->terminal_buffer->base,
                                           NULL);
}

static void focus_toplevel(struct diftray_toplevel *toplevel) {
  struct wlr_seat *seat = toplevel->runtime->seat;
  struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;
  struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
  wlr_scene_node_raise_to_top(&toplevel->tree->node);
  wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
  if (keyboard) {
    wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes,
                                   keyboard->num_keycodes, &keyboard->modifiers);
  }
}

static void output_frame(struct wl_listener *listener, void *data) {
  struct diftray_output *output = wl_container_of(listener, output, frame);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (wlr_scene_output_commit(output->scene_output, NULL)) {
    wlr_scene_output_send_frame_done(output->scene_output, &now);
  }
}

static void output_request_state(struct wl_listener *listener, void *data) {
  struct diftray_output *output = wl_container_of(listener, output, request_state);
  const struct wlr_output_event_request_state *event = data;
  wlr_output_commit_state(output->output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
  struct diftray_output *output = wl_container_of(listener, output, destroy);
  if (output->terminal_scene) {
    wlr_scene_node_destroy(&output->terminal_scene->node);
    output->terminal_scene = NULL;
  }
  if (output->terminal_buffer) {
    wlr_buffer_drop(&output->terminal_buffer->base);
    output->terminal_buffer = NULL;
  }
  wl_list_remove(&output->frame.link);
  wl_list_remove(&output->request_state.link);
  wl_list_remove(&output->destroy.link);
  wl_list_remove(&output->link);
  free(output);
}

static void new_output(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, new_output);
  struct wlr_output *wlr_output = data;
  if (!wlr_output_init_render(wlr_output, runtime->allocator, runtime->renderer)) {
    return;
  }

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
  if (mode) {
    wlr_output_state_set_mode(&state, mode);
  }
  wlr_output_commit_state(wlr_output, &state);
  wlr_output_state_finish(&state);

  struct diftray_output *output = calloc(1, sizeof(*output));
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
  output->terminal_scene =
      wlr_scene_buffer_create(&runtime->scene->tree, NULL);
  if (output->terminal_scene) {
    wlr_scene_node_set_position(&output->terminal_scene->node, 0, 0);
    /* The terminal buffer is created after the static scene nodes. Keep the
     * command bar above it when the bar is toggled on. */
    wlr_scene_node_raise_to_top(&runtime->command_bar->node);
  }
  wlr_scene_rect_set_size(runtime->background, wlr_output->width, wlr_output->height);
  wlr_scene_rect_set_size(runtime->command_bar, wlr_output->width - 24,
                          runtime->style.command_bar_height);
  wlr_scene_node_set_position(&runtime->command_bar->node, 12,
                              wlr_output->height - runtime->style.command_bar_height - 12);
  render_terminal_buffer(runtime, output);
}

static void toplevel_map(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel = wl_container_of(listener, toplevel, map);
  wl_list_insert(&toplevel->runtime->toplevels, &toplevel->link);
  int offset = 24 + (int)wl_list_length(&toplevel->runtime->toplevels) * 24;
  wlr_scene_node_set_position(&toplevel->tree->node, offset, offset);
  focus_toplevel(toplevel);
}

static void toplevel_unmap(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
  wl_list_remove(&toplevel->link);
}

static void toplevel_commit(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel = wl_container_of(listener, toplevel, commit);
  if (toplevel->xdg_toplevel->base->initial_commit) {
    wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 900, 600);
  }
  const struct wlr_box geometry = toplevel->xdg_toplevel->base->geometry;
  if (geometry.width > 0 && geometry.height > 0) {
    const int border = toplevel->runtime->style.border_size;
    wlr_scene_rect_set_size(toplevel->border, geometry.width + border * 2,
                            geometry.height + border * 2);
  }
}

static void toplevel_destroy(struct wl_listener *listener, void *data) {
  struct diftray_toplevel *toplevel = wl_container_of(listener, toplevel, destroy);
  wl_list_remove(&toplevel->map.link);
  wl_list_remove(&toplevel->unmap.link);
  wl_list_remove(&toplevel->commit.link);
  wl_list_remove(&toplevel->destroy.link);
  free(toplevel);
}

static void new_toplevel(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, new_toplevel);
  struct wlr_xdg_toplevel *xdg_toplevel = data;
  struct diftray_toplevel *toplevel = calloc(1, sizeof(*toplevel));
  toplevel->runtime = runtime;
  toplevel->xdg_toplevel = xdg_toplevel;
  toplevel->tree = wlr_scene_tree_create(&runtime->scene->tree);
  const int border = runtime->style.border_size;
  toplevel->border =
      wlr_scene_rect_create(toplevel->tree, 900 + border * 2, 600 + border * 2,
                            runtime->style.border_color);
  toplevel->surface_tree = wlr_scene_xdg_surface_create(toplevel->tree, xdg_toplevel->base);
  wlr_scene_node_set_position(&toplevel->surface_tree->node, border, border);
  xdg_toplevel->base->data = toplevel->surface_tree;

  toplevel->map.notify = toplevel_map;
  toplevel->unmap.notify = toplevel_unmap;
  toplevel->commit.notify = toplevel_commit;
  toplevel->destroy.notify = toplevel_destroy;
  wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
  wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
  wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
  wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);
}

static void popup_commit(struct wl_listener *listener, void *data) {
  struct diftray_popup *popup = wl_container_of(listener, popup, commit);
  if (popup->popup->base->initial_commit) {
    wlr_xdg_surface_schedule_configure(popup->popup->base);
  }
}

static void popup_destroy(struct wl_listener *listener, void *data) {
  struct diftray_popup *popup = wl_container_of(listener, popup, destroy);
  wl_list_remove(&popup->commit.link);
  wl_list_remove(&popup->destroy.link);
  free(popup);
}

static void new_popup(struct wl_listener *listener, void *data) {
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
  bool handled = false;
  if ((modifiers & WLR_MODIFIER_LOGO) &&
      event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    for (int index = 0; index < count; ++index) {
      if (symbols[index] == XKB_KEY_Escape) {
        wl_display_terminate(keyboard->runtime->display);
        handled = true;
      } else if (symbols[index] == XKB_KEY_colon ||
                 symbols[index] == XKB_KEY_semicolon) {
        keyboard->runtime->command_bar_visible =
            !keyboard->runtime->command_bar_visible;
        wlr_scene_node_set_enabled(&keyboard->runtime->command_bar->node,
                                   keyboard->runtime->command_bar_visible);
        handled = true;
      }
    }
  }
  if (!handled) {
    if (keyboard->runtime->key_handler) {
      for (int index = 0; index < count; ++index) {
        keyboard->runtime->key_handler(
            keyboard->runtime->key_handler_userdata, symbols[index],
            modifiers, event->state);
      }
    }
    wlr_seat_set_keyboard(keyboard->runtime->seat, keyboard->keyboard);
    wlr_seat_keyboard_notify_key(keyboard->runtime->seat, event->time_msec,
                                 event->keycode, event->state);
  }
}

static void keyboard_destroy(struct wl_listener *listener, void *data) {
  struct diftray_keyboard *keyboard =
      wl_container_of(listener, keyboard, destroy);
  wl_list_remove(&keyboard->modifiers.link);
  wl_list_remove(&keyboard->key.link);
  wl_list_remove(&keyboard->destroy.link);
  wl_list_remove(&keyboard->link);
  free(keyboard);
}

static void new_input(struct wl_listener *listener, void *data) {
  struct diftray_wayland_runtime *runtime =
      wl_container_of(listener, runtime, new_input);
  struct wlr_input_device *device = data;
  if (device->type != WLR_INPUT_DEVICE_KEYBOARD) {
    return;
  }
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
  wlr_seat_set_capabilities(runtime->seat, WL_SEAT_CAPABILITY_KEYBOARD);
}

struct diftray_wayland_runtime *diftray_wayland_runtime_create(
    struct wl_display *display, const struct diftray_wayland_style *style) {
  struct diftray_wayland_runtime *runtime = calloc(1, sizeof(*runtime));
  runtime->display = display;
  runtime->style = *style;
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
      runtime->renderer ? wlr_allocator_autocreate(runtime->backend, runtime->renderer)
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
  runtime->background =
      wlr_scene_rect_create(&runtime->scene->tree, 1920, 1080,
                            runtime->style.background_color);
  runtime->command_bar =
      wlr_scene_rect_create(&runtime->scene->tree, 1896,
                            runtime->style.command_bar_height,
                            runtime->style.command_bar_color);
  wlr_scene_node_set_enabled(&runtime->command_bar->node, false);
  runtime->terminal_text = strdup("DiftrayWM\n");
  runtime->xdg_shell = wlr_xdg_shell_create(display, 3);
  runtime->seat = wlr_seat_create(display, "seat0");
  runtime->new_output.notify = new_output;
  runtime->new_input.notify = new_input;
  runtime->new_toplevel.notify = new_toplevel;
  runtime->new_popup.notify = new_popup;
  wl_signal_add(&runtime->backend->events.new_output, &runtime->new_output);
  wl_signal_add(&runtime->backend->events.new_input, &runtime->new_input);
  wl_signal_add(&runtime->xdg_shell->events.new_toplevel, &runtime->new_toplevel);
  wl_signal_add(&runtime->xdg_shell->events.new_popup, &runtime->new_popup);
  return runtime;
}

bool diftray_wayland_runtime_start(struct diftray_wayland_runtime *runtime) {
  return runtime && wlr_backend_start(runtime->backend);
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

void diftray_wayland_runtime_set_terminal_text(
    struct diftray_wayland_runtime *runtime, const char *text) {
  if (!runtime) {
    return;
  }
  char *copy = strdup(text ? text : "");
  if (!copy) {
    return;
  }
  free(runtime->terminal_text);
  runtime->terminal_text = copy;
  struct diftray_output *output;
  wl_list_for_each(output, &runtime->outputs, link) {
    render_terminal_buffer(runtime, output);
  }
}

void diftray_wayland_runtime_destroy(struct diftray_wayland_runtime *runtime) {
  if (!runtime) {
    return;
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
  free(runtime->terminal_text);
  free(runtime);
}
