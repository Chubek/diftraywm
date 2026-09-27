#include "nterm/GlyphRenderer.hpp"

#include <algorithm>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <hb.h>

GlyphRenderer::GlyphRenderer() = default;

GlyphRenderer::~GlyphRenderer() {
  if (hb_font_) {
    hb_font_destroy(hb_font_);
  }
  if (face_) {
    FT_Done_Face(face_);
  }
  if (library_) {
    FT_Done_FreeType(library_);
  }
}

bool GlyphRenderer::load_font(const std::string &family, int pixel_size) {
  FcInit();
  FcPattern *pattern = FcPatternCreate();
  FcPatternAddString(pattern, FC_FAMILY,
                     reinterpret_cast<const FcChar8 *>(family.c_str()));
  FcPatternAddInteger(pattern, FC_PIXEL_SIZE, pixel_size);
  FcPatternAddInteger(pattern, FC_SPACING, FC_MONO);
  FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcPattern *match = FcFontMatch(nullptr, pattern, &result);
  FcPatternDestroy(pattern);
  if (!match) {
    return false;
  }
  FcChar8 *file = nullptr;
  if (FcPatternGetString(match, FC_FILE, 0, &file) != FcResultMatch || !file) {
    FcPatternDestroy(match);
    return false;
  }
  const FT_Error error =
      FT_New_Face(library_, reinterpret_cast<const char *>(file), 0, &face_);
  FcPatternDestroy(match);
  if (error != 0 || !face_) {
    return false;
  }
  FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(pixel_size));
  hb_font_ = hb_ft_font_create(face_, nullptr);
  // max_advance includes exceptionally wide glyphs; use the normal grid advance.
  if (FT_Load_Char(face_, '0', FT_LOAD_DEFAULT) == 0) {
    cell_width_ = static_cast<int>((face_->glyph->advance.x + 63) / 64);
  } else {
    cell_width_ = static_cast<int>((face_->size->metrics.max_advance + 63) / 64);
  }
  cell_height_ = static_cast<int>((face_->size->metrics.height + 63) / 64);
  baseline_ = static_cast<int>((face_->size->metrics.ascender + 63) / 64);
  if (cell_width_ < 6) {
    cell_width_ = 8;
  }
  if (cell_height_ < 10) {
    cell_height_ = 16;
  }
  return true;
}

bool GlyphRenderer::init(const std::string &family, int pixel_size) {
  if (FT_Init_FreeType(&library_) != 0) {
    return false;
  }
  const char *candidates[] = {family.c_str(), "monospace", "DejaVu Sans Mono",
                              "Noto Sans Mono", "Liberation Mono"};
  for (const char *candidate : candidates) {
    if (candidate && *candidate && load_font(candidate, pixel_size)) {
      return true;
    }
  }
  return false;
}

const GlyphRenderer::Glyph *GlyphRenderer::rasterize(uint32_t codepoint,
                                                     bool bold) {
  if (!hb_font_ || !face_) {
    return nullptr;
  }
  hb_buffer_t *buffer = hb_buffer_create();
  hb_buffer_add_utf32(buffer, &codepoint, 1, 0, 1);
  hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
  hb_buffer_set_script(buffer, HB_SCRIPT_LATIN);
  hb_buffer_set_language(buffer, hb_language_from_string("en", 2));
  const hb_feature_t liga{HB_TAG('l', 'i', 'g', 'a'), 1, 0, static_cast<unsigned>(-1)};
  hb_shape(hb_font_, buffer, &liga, 1);
  unsigned int count = 0;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
  if (!info || count == 0) {
    hb_buffer_destroy(buffer);
    return nullptr;
  }
  const FT_UInt index = info[0].codepoint;
  hb_buffer_destroy(buffer);
  FT_Int32 flags = FT_LOAD_RENDER;
  if (bold) {
    flags |= FT_LOAD_FORCE_AUTOHINT;
  }
  if (FT_Load_Glyph(face_, index, flags) != 0) {
    return nullptr;
  }
Glyph glyph;
  glyph.width = static_cast<int>(face_->glyph->bitmap.width);
  glyph.height = static_cast<int>(face_->glyph->bitmap.rows);
  glyph.left = face_->glyph->bitmap_left;
  glyph.top = face_->glyph->bitmap_top;
  glyph.advance = static_cast<int>((face_->glyph->advance.x + 63) / 64);
  const size_t bytes = static_cast<size_t>(glyph.width) * static_cast<size_t>(glyph.height);
  glyph.coverage.resize(bytes);
  if (face_->glyph->bitmap.buffer && bytes > 0) {
    if (face_->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
      for (int row = 0; row < glyph.height; ++row) {
        std::copy(face_->glyph->bitmap.buffer +
                      row * face_->glyph->bitmap.pitch,
                  face_->glyph->bitmap.buffer + row * face_->glyph->bitmap.pitch +
                      glyph.width,
                  glyph.coverage.begin() + row * glyph.width);
      }
    }
  }
  const uint64_t key = (static_cast<uint64_t>(codepoint) << 1) | (bold ? 1 : 0);
  cache_[key] = std::move(glyph);
  return &cache_[key];
}

const GlyphRenderer::Glyph *GlyphRenderer::glyph(uint32_t codepoint, bool bold) {
  const uint64_t key = (static_cast<uint64_t>(codepoint) << 1) | (bold ? 1 : 0);
  const auto it = cache_.find(key);
  if (it != cache_.end()) {
    return &it->second;
  }
  return rasterize(codepoint, bold);
}
