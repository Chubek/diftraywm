#pragma once

#include <stdbool.h>
#include <stdint.h>

struct wl_display;
struct diftray_wayland_runtime;

typedef void (*diftray_wayland_key_handler)(void *userdata, uint32_t keysym,
                                            uint32_t modifiers, uint32_t state);

struct diftray_wayland_style {
  int border_size;
  int command_bar_height;
  float border_color[4];
  float background_color[4];
  float command_bar_color[4];
};

#ifdef __cplusplus
extern "C" {
#endif

struct diftray_wayland_runtime *diftray_wayland_runtime_create(
    struct wl_display *display, const struct diftray_wayland_style *style);
bool diftray_wayland_runtime_start(struct diftray_wayland_runtime *runtime);
void diftray_wayland_runtime_set_key_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_key_handler handler, void *userdata);
void diftray_wayland_runtime_set_terminal_text(
    struct diftray_wayland_runtime *runtime, const char *text);
void diftray_wayland_runtime_destroy(struct diftray_wayland_runtime *runtime);

#ifdef __cplusplus
}
#endif
