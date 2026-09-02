#pragma once

#include <string>

struct FT_FaceRec_;
using FT_Face = FT_FaceRec_ *;
struct hb_font_t;

class GlyphRenderer {
public:
  explicit GlyphRenderer(std::string font_path = {});
  ~GlyphRenderer();

private:
  hb_font_t *hb_font_ = nullptr;
  FT_Face face_ = nullptr;
};
