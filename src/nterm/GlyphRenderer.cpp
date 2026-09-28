#include "nterm/GlyphRenderer.hpp"

#include <algorithm>
#include <memory>
#include <cstdlib>
#include <unistd.h>

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
  if (fontconfig_) FcConfigDestroy(fontconfig_);
}

bool GlyphRenderer::load_font(const std::string &family, int pixel_size) {
  FcPattern *pattern = FcPatternCreate();
  FcPatternAddString(pattern, FC_FAMILY,
                     reinterpret_cast<const FcChar8 *>(family.c_str()));
  FcPatternAddInteger(pattern, FC_PIXEL_SIZE, pixel_size);
  FcPatternAddInteger(pattern, FC_SPACING, FC_MONO);
  FcConfigSubstitute(fontconfig_, pattern, FcMatchPattern);
  FcDefaultSubstitute(pattern);
  FcResult result = FcResultNoMatch;
  FcPattern *match = FcFontMatch(fontconfig_, pattern, &result);
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
  // Use the host's font configuration with the vendored library. This avoids
  // embedding the build prefix's font configuration path in installed apps.
  if (!std::getenv("FONTCONFIG_FILE") && !std::getenv("FONTCONFIG_PATH") &&
      ::access("/etc/fonts/fonts.conf", R_OK) == 0) {
    fontconfig_ = FcConfigCreate();
    if (fontconfig_ && (!FcConfigParseAndLoad(fontconfig_,
          reinterpret_cast<const FcChar8 *>("/etc/fonts/fonts.conf"), FcTrue) ||
        !FcConfigBuildFonts(fontconfig_))) {
      FcConfigDestroy(fontconfig_);
      fontconfig_ = nullptr;
    }
  }
  if (!fontconfig_) fontconfig_ = FcInitLoadConfigAndFonts();
  if (!fontconfig_) return false;
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
  FT_Face rendering_face = face_;
  hb_font_t *rendering_font = hb_font_;
  std::unique_ptr<FT_FaceRec_, decltype(&FT_Done_Face)> fallback(nullptr, FT_Done_Face);
  std::unique_ptr<hb_font_t, decltype(&hb_font_destroy)> fallback_font(nullptr, hb_font_destroy);
  if (FT_Get_Char_Index(face_, codepoint) == 0) {
    FcPattern *pattern = FcPatternCreate();
    FcCharSet *charset = FcCharSetCreate();
    FcCharSetAddChar(charset, codepoint);
    FcPatternAddCharSet(pattern, FC_CHARSET, charset);
    FcConfigSubstitute(fontconfig_, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(fontconfig_, pattern, &result);
    FcCharSetDestroy(charset);
    FcPatternDestroy(pattern);
    FcChar8 *file = nullptr;
    int index = 0;
    if (match && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch) {
      FcPatternGetInteger(match, FC_INDEX, 0, &index);
      FT_Face face = nullptr;
      if (FT_New_Face(library_, reinterpret_cast<const char *>(file), index, &face) == 0) {
        fallback.reset(face);
        if (FT_Set_Pixel_Sizes(face, 0, face_->size->metrics.y_ppem) == 0) {
          fallback_font.reset(hb_ft_font_create(face, nullptr));
          rendering_face = face;
          rendering_font = fallback_font.get();
        }
      }
    }
    if (match) FcPatternDestroy(match);
  }
  hb_buffer_t *buffer = hb_buffer_create();
  hb_buffer_add_utf32(buffer, &codepoint, 1, 0, 1);
  hb_buffer_guess_segment_properties(buffer);
  const hb_feature_t liga{HB_TAG('l', 'i', 'g', 'a'), 1, 0, static_cast<unsigned>(-1)};
  hb_shape(rendering_font, buffer, &liga, 1);
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
  if (FT_Load_Glyph(rendering_face, index, flags) != 0) {
    return nullptr;
  }
Glyph glyph;
  glyph.width = static_cast<int>(rendering_face->glyph->bitmap.width);
  glyph.height = static_cast<int>(rendering_face->glyph->bitmap.rows);
  glyph.left = rendering_face->glyph->bitmap_left;
  glyph.top = rendering_face->glyph->bitmap_top;
  glyph.advance = static_cast<int>((rendering_face->glyph->advance.x + 63) / 64);
  const size_t bytes = static_cast<size_t>(glyph.width) * static_cast<size_t>(glyph.height);
  glyph.coverage.resize(bytes);
  if (rendering_face->glyph->bitmap.buffer && bytes > 0) {
    if (rendering_face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
      for (int row = 0; row < glyph.height; ++row) {
        std::copy(rendering_face->glyph->bitmap.buffer +
                      row * rendering_face->glyph->bitmap.pitch,
                  rendering_face->glyph->bitmap.buffer + row * rendering_face->glyph->bitmap.pitch +
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

void GlyphRenderer::draw_text(const char *text, uint32_t *pixels, int width, int height,
                              const float background[4], const float foreground[4]) {
  if (!pixels || width <= 0 || height <= 0) return;
  auto channel = [](float x) { return static_cast<unsigned>(std::clamp(x, 0.0f, 1.0f) * 255 + 0.5f); };
  const unsigned ba = channel(background[3]);
  const unsigned br = channel(background[0]) * ba / 255;
  const unsigned bg = channel(background[1]) * ba / 255;
  const unsigned bb = channel(background[2]) * ba / 255;
  std::fill(pixels, pixels + static_cast<size_t>(width) * height,
            (ba << 24) | (br << 16) | (bg << 8) | bb);
  if (!hb_font_ || !face_ || !text) return;
  hb_buffer_t *buffer = hb_buffer_create();
  hb_buffer_add_utf8(buffer, text, -1, 0, -1);
  hb_buffer_guess_segment_properties(buffer);
  hb_shape(hb_font_, buffer, nullptr, 0);
  unsigned count;
  const auto *info = hb_buffer_get_glyph_infos(buffer, &count);
  const auto *positions = hb_buffer_get_glyph_positions(buffer, nullptr);
  int pen_x = 0, pen_y = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (FT_Load_Glyph(face_, info[i].codepoint, FT_LOAD_RENDER) == 0) {
      const auto &bitmap = face_->glyph->bitmap;
      const int x0 = (pen_x + positions[i].x_offset) / 64 + face_->glyph->bitmap_left;
      const int y0 = std::max(0, (height - cell_height_) / 2) + baseline_ -
          (pen_y + positions[i].y_offset) / 64 - face_->glyph->bitmap_top;
      if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        for (unsigned row = 0; row < bitmap.rows; ++row) {
          const int y = y0 + row;
          if (y < 0 || y >= height) continue;
          const auto *line = bitmap.buffer + row * bitmap.pitch;
          for (unsigned col = 0; col < bitmap.width; ++col) {
            const int x = x0 + col;
            if (x < 0 || x >= width) continue;
            const unsigned alpha = line[col] * channel(foreground[3]) / 255;
            const uint32_t old = pixels[static_cast<size_t>(y) * width + x];
            const unsigned a = alpha + ((old >> 24) * (255 - alpha)) / 255;
            const unsigned r = (channel(foreground[0]) * alpha + ((old >> 16) & 255) * (255 - alpha)) / 255;
            const unsigned g = (channel(foreground[1]) * alpha + ((old >> 8) & 255) * (255 - alpha)) / 255;
            const unsigned b = (channel(foreground[2]) * alpha + (old & 255) * (255 - alpha)) / 255;
            pixels[static_cast<size_t>(y) * width + x] = (a << 24) | (r << 16) | (g << 8) | b;
          }
        }
      }
    }
    pen_x += positions[i].x_advance;
    pen_y += positions[i].y_advance;
  }
  hb_buffer_destroy(buffer);
}
