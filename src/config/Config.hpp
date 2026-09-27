#pragma once

#include <string>

struct CompositorConfig {
  int border_size = 3;
  int command_bar_height = 40;
  int status_bar_height = 24;
  float border_color[4] = {0.35f, 0.65f, 1.0f, 1.0f};
  float background_color[4] = {0.035f, 0.045f, 0.065f, 1.0f};
  float command_bar_color[4] = {0.08f, 0.12f, 0.18f, 0.98f};
  std::string shell = "/bin/sh";
  std::string font = "monospace";
  int font_size = 14;
  std::string ncursor_mode = "stack";
  std::string gcursor_mode = "tab";
  std::string word_pool = "/usr/share/dict/words";
  std::string theme;
  std::string help_path;
  std::string help_key_close = "q";
  std::string help_key_search = "/";
  std::string help_key_next = "n";
  std::string help_key_previous = "?";
  std::string help_key_page_down = "space";
  std::string help_key_page_up = "b";
  std::string help_key_line_down = "j";
  std::string help_key_line_up = "k";
};

bool load_compositor_config(const std::string &path, CompositorConfig &config,
                            std::string &error);
