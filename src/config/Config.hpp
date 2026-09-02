#pragma once

#include <string>

struct CompositorConfig {
  int border_size = 3;
  int command_bar_height = 40;
  float border_color[4] = {0.35f, 0.65f, 1.0f, 1.0f};
  float background_color[4] = {0.035f, 0.045f, 0.065f, 1.0f};
  float command_bar_color[4] = {0.08f, 0.12f, 0.18f, 0.98f};
  std::string shell = "/bin/sh";
};

bool load_compositor_config(const std::string &path, CompositorConfig &config,
                            std::string &error);
