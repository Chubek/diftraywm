#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

struct FT_FaceRec_;
using FT_Face = FT_FaceRec_ *;
struct FT_LibraryRec_;
using FT_Library = FT_LibraryRec_ *;
struct hb_font_t;
struct _FcConfig;

class GlyphRenderer {
public:
  struct Glyph {
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
    int advance = 0;
    std::vector<uint8_t> coverage;
  };

  GlyphRenderer();
  ~GlyphRenderer();
  GlyphRenderer(const GlyphRenderer &) = delete;
  GlyphRenderer &operator=(const GlyphRenderer &) = delete;

  bool init(const std::string &family, int pixel_size);
  void draw_text(const char *text, uint32_t *pixels, int width, int height,
                 const float background[4], const float foreground[4]);
  int cell_width() const { return cell_width_; }
  int cell_height() const { return cell_height_; }
  int baseline() const { return baseline_; }
  const Glyph *glyph(uint32_t codepoint, bool bold, bool italic = false);
  const Glyph *glyph(const uint32_t *codepoints, std::size_t length, bool bold, bool italic = false);

private:
  bool load_font(const std::string &family, int pixel_size);
  bool initialize(const std::string &family, int pixel_size);
  const Glyph *rasterize(const uint32_t *codepoints, std::size_t length, bool bold, bool italic);

  _FcConfig *fontconfig_ = nullptr;
  FT_Library library_ = nullptr;
  FT_Face face_ = nullptr;
  hb_font_t *hb_font_ = nullptr;
  int cell_width_ = 8;
  int cell_height_ = 16;
  int baseline_ = 12;
  std::unordered_map<std::u32string, Glyph> cache_;
  std::size_t cache_bytes_ = 0;
};
