#pragma once

#include "nterm/GlyphRenderer.hpp"

class GlyphAtlas {
public:
  const GlyphRenderer::Glyph *get(GlyphRenderer &renderer, uint32_t codepoint,
                                  bool bold) {
    return renderer.glyph(codepoint, bold);
  }
};
