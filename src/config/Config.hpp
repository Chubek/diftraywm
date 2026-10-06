#pragma once

#include "config/ConfigProgram.hpp"
#include <string>
#include <vector>
#include <map>

struct MonitorConfig {
  std::string name;
  int rotation = 0; // Degrees counter-clockwise, matching Wayland transforms.
  float scale = 1.0f;
  bool positioned = false;
  int x = 0, y = 0; // Logical coordinates after rotation and scaling.
};

bool parse_monitor_settings(const std::map<std::string, std::string> &settings,
                            MonitorConfig &monitor, std::string &error);

struct CompositorConfig {
  std::shared_ptr<const ConfigProgram> program;
  std::vector<MonitorConfig> monitors;
  int border_size = 3;
  int command_bar_height = 40;
  int status_bar_height = 24;
  int launcher_bar_height = 32;
  float border_color[4] = {0.35f, 0.65f, 1.0f, 1.0f};
  float background_color[4] = {0.035f, 0.045f, 0.065f, 1.0f};
  float command_bar_color[4] = {0.08f, 0.12f, 0.18f, 0.98f};
  float launcher_bar_color[4] = {0.06f, 0.09f, 0.14f, 0.96f};
  std::string shell = "libshell";
  std::string font = "monospace";
  int font_size = 14;
  bool font_ligatures = true;
  std::string ncursor_mode = "stack";
  std::string gcursor_mode = "tab";
  std::string word_pool = "/usr/share/dict/words";
  std::string theme;
  std::string help_path;
  // Every key-related aspect of DiftrayWM -- the Meta prefix chord, the
  // built-in bindings, the help pager keys and the optional system-wide evdev
  // remap -- lives in this INI file rather than in the config itself. Empty
  // means "no keymap", which leaves the compositor on its built-in keys.
  std::string keymap = "keymap.ini";
  // The launcher taskbar is anchored under the status bar. Locked, it stays
  // visible and always paints above the cells and graphical windows.
  bool launcher_locked = false;
};

bool load_compositor_config(const std::string &path, CompositorConfig &config,
                            std::string &error);
