#pragma once

#include "nterm/GlyphRenderer.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class NTerm;

class NTermRenderer {
public:
  NTermRenderer();
  bool init(const std::string &font_family, int font_size);
  GlyphRenderer *glyphs() { return renderer_.get(); }
  void render(NTerm *term, std::vector<uint32_t> &pixels, int width, int height,
              bool selected);

private:
  std::unique_ptr<GlyphRenderer> renderer_;
};
