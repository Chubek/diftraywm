#pragma once

#include <cstddef>
#include <cstdint>

class GlyphAtlas {
public:
  struct Key {
    unsigned int glyph_index = 0;
    unsigned int font_size = 0;
    std::uint32_t color = 0;
  };
};
