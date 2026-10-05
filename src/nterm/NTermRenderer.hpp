#pragma once

#include "nterm/GlyphRenderer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class NTerm;

class NTermRenderer {
public:
  struct Style {
    uint32_t background = 0;
    uint32_t foreground = 0;
    uint32_t cursor = 0;
    uint32_t highlight = 0;
    int cursor_thickness = 0;
    bool override_foreground = false;
  };
  NTermRenderer();
  bool init(const std::string &font_family, int font_size);
  GlyphRenderer *glyphs() { return renderer_.get(); }
  const Style &style() const { return style_; }
  void set_style(Style style) { style_ = style; }
  void render(NTerm *term, std::vector<uint32_t> &pixels, int width, int height,
              bool selected);

private:
  std::unique_ptr<GlyphRenderer> renderer_;
  Style style_;
};
