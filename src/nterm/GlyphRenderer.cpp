#include "nterm/GlyphRenderer.hpp"

#include <algorithm>
#include <memory>
#include <cstdlib>
#include <unistd.h>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
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
  int index = 0;
  FcPatternGetInteger(match, FC_INDEX, 0, &index);
  const FT_Error error =
      FT_New_Face(library_, reinterpret_cast<const char *>(file), index, &face_);
  FcPatternDestroy(match);
  if (error != 0 || !face_) {
    return false;
  }
  if (FT_Set_Pixel_Sizes(face_, 0, static_cast<FT_UInt>(pixel_size)) != 0) {
    FT_Done_Face(face_);
    face_ = nullptr;
    return false;
  }
  hb_font_ = hb_ft_font_create(face_, nullptr);
  // max_advance includes exceptionally wide glyphs; use the normal grid advance.
  hb_buffer_t *measure = hb_buffer_create();
  hb_buffer_add_utf8(measure, "0", 1, 0, 1);
  hb_buffer_guess_segment_properties(measure);
  hb_shape(hb_font_, measure, nullptr, 0);
  unsigned count = 0;
  const auto *positions = hb_buffer_get_glyph_positions(measure, &count);
  cell_width_ = count ? (positions[0].x_advance + 63) / 64 : 8;
  hb_buffer_destroy(measure);
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
  if (pixel_size <= 0 || pixel_size > 512) return false;
  GlyphRenderer next;
  if (!next.initialize(family, pixel_size)) return false;
  std::swap(fontconfig_, next.fontconfig_);
  std::swap(library_, next.library_);
  std::swap(face_, next.face_);
  std::swap(hb_font_, next.hb_font_);
  std::swap(cell_width_, next.cell_width_);
  std::swap(cell_height_, next.cell_height_);
  std::swap(baseline_, next.baseline_);
  cache_.clear();
  cache_bytes_ = 0;
  return true;
}

bool GlyphRenderer::initialize(const std::string &family, int pixel_size) {
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

const GlyphRenderer::Glyph *GlyphRenderer::rasterize(const uint32_t *codepoints,
                                                     std::size_t length, bool bold, bool italic) {
  if (!hb_font_ || !face_) {
    return nullptr;
  }
  FT_Face rendering_face = face_;
  hb_font_t *rendering_font = hb_font_;
  std::unique_ptr<FT_FaceRec_, decltype(&FT_Done_Face)> fallback(nullptr, FT_Done_Face);
  std::unique_ptr<hb_font_t, decltype(&hb_font_destroy)> fallback_font(nullptr, hb_font_destroy);
  bool missing = false;
  for (std::size_t i = 0; i < length; ++i) missing |= FT_Get_Char_Index(face_, codepoints[i]) == 0;
  if (missing) {
    FcPattern *pattern = FcPatternCreate();
    FcCharSet *charset = FcCharSetCreate();
    for (std::size_t i = 0; i < length; ++i) FcCharSetAddChar(charset, codepoints[i]);
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
  hb_buffer_add_utf32(buffer, codepoints, static_cast<int>(length), 0, static_cast<int>(length));
  hb_buffer_guess_segment_properties(buffer);
  const hb_feature_t liga{HB_TAG('l', 'i', 'g', 'a'), 1, 0, static_cast<unsigned>(-1)};
  hb_shape(rendering_font, buffer, &liga, 1);
  unsigned int count = 0;
  hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
  if (!info || count == 0) {
    hb_buffer_destroy(buffer);
    return nullptr;
  }
  const auto *positions = hb_buffer_get_glyph_positions(buffer, nullptr);
  struct Part {
    int x, y, width, height;
    std::vector<uint8_t> coverage;
  };
  std::vector<Part> parts;
  int pen_x = 0, pen_y = 0;
  int left = 0, top = 0, right = 0, bottom = 0;
  bool first = true;
  for (unsigned i = 0; i < count; ++i) {
    if (FT_Load_Glyph(rendering_face, info[i].codepoint, FT_LOAD_DEFAULT) != 0) continue;
    if (bold) FT_GlyphSlot_Embolden(rendering_face->glyph);
    if (italic) FT_GlyphSlot_Oblique(rendering_face->glyph);
    if (FT_Render_Glyph(rendering_face->glyph, FT_RENDER_MODE_NORMAL) != 0) continue;
    const auto &bitmap = rendering_face->glyph->bitmap;
    Part part;
    part.x = (pen_x + positions[i].x_offset) / 64 + rendering_face->glyph->bitmap_left;
    part.y = -(pen_y + positions[i].y_offset) / 64 - rendering_face->glyph->bitmap_top;
    part.width = bitmap.width;
    part.height = bitmap.rows;
    part.coverage.resize(static_cast<std::size_t>(part.width) * part.height);
    for (int row = 0; row < part.height; ++row) {
      const auto *line = bitmap.buffer + (bitmap.pitch >= 0 ? row : part.height - 1 - row) * std::abs(bitmap.pitch);
      for (int col = 0; col < part.width; ++col) {
        uint8_t coverage = 0;
        if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) coverage = line[col];
        else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) coverage = (line[col / 8] & (0x80 >> (col % 8))) ? 255 : 0;
        else if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) coverage = line[col * 4 + 3];
        part.coverage[static_cast<std::size_t>(row) * part.width + col] = coverage;
      }
    }
    if (first) {
      left = part.x; top = part.y; right = part.x + part.width; bottom = part.y + part.height;
      first = false;
    } else {
      left = std::min(left, part.x); top = std::min(top, part.y);
      right = std::max(right, part.x + part.width); bottom = std::max(bottom, part.y + part.height);
    }
    parts.push_back(std::move(part));
    pen_x += positions[i].x_advance;
    pen_y += positions[i].y_advance;
  }
  hb_buffer_destroy(buffer);
  Glyph glyph;
  glyph.left = left;
  glyph.top = -top;
  glyph.width = right - left;
  glyph.height = bottom - top;
  glyph.advance = (pen_x + 63) / 64;
  if (glyph.width < 0 || glyph.height < 0 ||
      static_cast<std::size_t>(glyph.width) * glyph.height > 16 * 1024 * 1024) return nullptr;
  glyph.coverage.resize(static_cast<std::size_t>(glyph.width) * glyph.height);
  for (const auto &part : parts) {
    for (int row = 0; row < part.height; ++row) {
      for (int col = 0; col < part.width; ++col) {
        auto &coverage = glyph.coverage[static_cast<std::size_t>(part.y - top + row) * glyph.width + part.x - left + col];
        const unsigned alpha = part.coverage[static_cast<std::size_t>(row) * part.width + col];
        coverage = static_cast<uint8_t>(alpha + coverage * (255 - alpha) / 255);
      }
    }
  }
  std::u32string key;
  key.push_back((bold ? 1 : 0) | (italic ? 2 : 0));
  for (std::size_t i = 0; i < length; ++i) key.push_back(codepoints[i]);
  // Bound cached raster data for long-lived terminals displaying many symbols.
  if (cache_.size() >= 4096 || cache_bytes_ + glyph.coverage.size() > 16 * 1024 * 1024) {
    cache_.clear();
    cache_bytes_ = 0;
  }
  cache_bytes_ += glyph.coverage.size();
  auto entry = cache_.emplace(std::move(key), std::move(glyph));
  return &entry.first->second;
}

const GlyphRenderer::Glyph *GlyphRenderer::glyph(uint32_t codepoint, bool bold, bool italic) {
  return glyph(&codepoint, 1, bold, italic);
}

const GlyphRenderer::Glyph *GlyphRenderer::glyph(const uint32_t *codepoints, std::size_t length, bool bold, bool italic) {
  if (!codepoints || length == 0 || length > 1024) return nullptr;
  std::u32string key;
  key.push_back((bold ? 1 : 0) | (italic ? 2 : 0));
  for (std::size_t i = 0; i < length; ++i) key.push_back(codepoints[i]);
  const auto it = cache_.find(key);
  if (it != cache_.end()) return &it->second;
  return rasterize(codepoints, length, bold, italic);
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
  unsigned count = 0;
  const auto *info = hb_buffer_get_glyph_infos(buffer, &count);
  std::vector<uint32_t> codepoints;
  for (unsigned i = 0; i < std::min(count, 1024U); ++i) codepoints.push_back(info[i].codepoint);
  hb_buffer_destroy(buffer);
  // The same shaped-run path as NTerm supplies Unicode fallback for chrome.
  const auto *shaped = glyph(codepoints.data(), codepoints.size(), false);
  if (!shaped) return;
  const int y0 = std::max(0, (height - cell_height_) / 2) + baseline_ - shaped->top;
  const int x0 = shaped->left;
  for (int row = 0; row < shaped->height; ++row) {
    const int y = y0 + row;
    if (y < 0 || y >= height) continue;
    for (int col = 0; col < shaped->width; ++col) {
      const int x = x0 + col;
      if (x < 0 || x >= width) continue;
      const unsigned alpha = shaped->coverage[static_cast<std::size_t>(row) * shaped->width + col] * channel(foreground[3]) / 255;
      const uint32_t old = pixels[static_cast<std::size_t>(y) * width + x];
      const unsigned a = alpha + ((old >> 24) * (255 - alpha)) / 255;
      const unsigned r = (channel(foreground[0]) * alpha + ((old >> 16) & 255) * (255 - alpha)) / 255;
      const unsigned g = (channel(foreground[1]) * alpha + ((old >> 8) & 255) * (255 - alpha)) / 255;
      const unsigned b = (channel(foreground[2]) * alpha + (old & 255) * (255 - alpha)) / 255;
      pixels[static_cast<std::size_t>(y) * width + x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
}
