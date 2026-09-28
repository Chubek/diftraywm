#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct wl_display;
struct wlr_xdg_toplevel;
struct diftray_wayland_runtime;
struct diftray_cell_surface;

typedef void (*diftray_text_renderer)(void *userdata, const char *text,
    uint32_t *pixels, int width, int height, const float background[4], const float foreground[4]);

typedef bool (*diftray_wayland_key_handler)(void *userdata, uint32_t keysym,
                                            uint32_t modifiers, uint32_t state,
                                            uint32_t unicode, uint32_t time_msec,
                                            uint32_t keycode);

typedef void (*diftray_wayland_toplevel_handler)(void *userdata,
                                                 struct wlr_xdg_toplevel *toplevel,
                                                 int client_pid);
typedef void (*diftray_wayland_toplevel_destroy_handler)(
    void *userdata, struct wlr_xdg_toplevel *toplevel);
typedef void (*diftray_wayland_toplevel_focus_handler)(
    void *userdata, struct wlr_xdg_toplevel *toplevel);
typedef void (*diftray_wayland_toplevel_request_handler)(
    void *userdata, struct wlr_xdg_toplevel *toplevel, const char *request);
typedef void (*diftray_wayland_output_handler)(void *userdata, int width,
                                               int height);

struct diftray_output_geometry {
  const char *name;
  int x, y, width, height;
};

struct diftray_wayland_style {
  int border_size;
  int command_bar_height;
  int status_bar_height;
  float border_color[4];
  float background_color[4];
  float command_bar_color[4];
  float highlight_color[4];
};

#ifdef __cplusplus
extern "C" {
#endif

struct diftray_wayland_runtime *diftray_wayland_runtime_create(
    struct wl_display *display, const struct diftray_wayland_style *style);
bool diftray_wayland_runtime_start(struct diftray_wayland_runtime *runtime);
void diftray_wayland_runtime_destroy(struct diftray_wayland_runtime *runtime);

void diftray_wayland_runtime_set_text_renderer(struct diftray_wayland_runtime *runtime,
    diftray_text_renderer renderer, void *userdata);

void diftray_wayland_runtime_set_key_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_key_handler handler, void *userdata);
void diftray_wayland_runtime_set_toplevel_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_toplevel_handler handler,
    diftray_wayland_toplevel_destroy_handler destroy_handler,
    diftray_wayland_toplevel_request_handler request_handler, void *userdata);
void diftray_wayland_runtime_set_focus_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_toplevel_focus_handler handler);
void diftray_wayland_runtime_set_output_handler(
    struct diftray_wayland_runtime *runtime,
    diftray_wayland_output_handler handler, void *userdata);

bool diftray_wayland_runtime_output_at(struct diftray_wayland_runtime *runtime,
    size_t index, struct diftray_output_geometry *geometry);
void diftray_wayland_runtime_set_chrome_box(struct diftray_wayland_runtime *runtime,
    int x, int y, int width, int height);

bool diftray_wayland_runtime_output_size(struct diftray_wayland_runtime *runtime,
                                         int *width, int *height);
void diftray_wayland_runtime_set_ncursor_visible(
    struct diftray_wayland_runtime *runtime, bool visible);
void diftray_wayland_runtime_set_gcursor_visible(
    struct diftray_wayland_runtime *runtime, bool visible);

struct diftray_cell_surface *diftray_cell_surface_create(
    struct diftray_wayland_runtime *runtime);
void diftray_cell_surface_destroy(struct diftray_cell_surface *surface);
void diftray_cell_surface_place(struct diftray_cell_surface *surface, int x,
                                int y, int width, int height);
bool diftray_cell_surface_update(struct diftray_cell_surface *surface,
                                 const uint32_t *pixels, int width, int height);
void diftray_cell_surface_set_highlight(struct diftray_cell_surface *surface,
                                        bool highlighted);
void diftray_cell_surface_set_visible(struct diftray_cell_surface *surface,
                                      bool visible);

void diftray_wayland_runtime_attach_gcursor(
    struct diftray_wayland_runtime *runtime,
    struct wlr_xdg_toplevel *toplevel);
void diftray_wayland_runtime_set_gcursor_visible_surface(
    struct diftray_wayland_runtime *runtime,
    struct wlr_xdg_toplevel *toplevel, bool visible);
void diftray_wayland_runtime_focus_gcursor(
    struct diftray_wayland_runtime *runtime,
    struct wlr_xdg_toplevel *toplevel);
void diftray_wayland_runtime_layout_gcursor(
    struct diftray_wayland_runtime *runtime, struct wlr_xdg_toplevel *toplevel,
    int x, int y, int width, int height);
void diftray_wayland_runtime_clear_keyboard_focus(
    struct diftray_wayland_runtime *runtime);
void diftray_wayland_runtime_set_style(struct diftray_wayland_runtime *runtime,
                                      const struct diftray_wayland_style *style);

void diftray_wayland_runtime_set_command_bar(
    struct diftray_wayland_runtime *runtime, bool visible, const char *text);
void diftray_wayland_runtime_set_status_line(
    struct diftray_wayland_runtime *runtime, const char *text);

#ifdef __cplusplus
}
#endif
