#include "nterm/NTermRenderer.hpp"

#include "nterm/NTerm.hpp"

#include <algorithm>
#include <libtsm.h>

namespace {
uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) | b;
}

uint32_t mix(uint32_t background, uint32_t foreground, uint8_t coverage) {
  if (coverage == 0) {
    return background;
  }
  if (coverage == 255) {
    return foreground;
  }
  const uint32_t br = (background >> 16) & 0xff;
  const uint32_t bg = (background >> 8) & 0xff;
  const uint32_t bb = background & 0xff;
  const uint32_t fr = (foreground >> 16) & 0xff;
  const uint32_t fg = (foreground >> 8) & 0xff;
  const uint32_t fb = foreground & 0xff;
  const uint32_t r = (br * (255 - coverage) + fr * coverage) / 255;
  const uint32_t g = (bg * (255 - coverage) + fg * coverage) / 255;
  const uint32_t b = (bb * (255 - coverage) + fb * coverage) / 255;
  return pack_rgb(static_cast<uint8_t>(r), static_cast<uint8_t>(g),
                  static_cast<uint8_t>(b));
}

struct DrawContext {
  NTermRenderer *self = nullptr;
  NTerm *term = nullptr;
  uint32_t *pixels = nullptr;
  int width = 0;
  int height = 0;
  int cell_w = 8;
  int cell_h = 16;
};

int draw_cell(struct tsm_screen *, uint64_t, const uint32_t *ch, size_t len,
              unsigned int cell_width, unsigned int posx, unsigned int posy,
              const struct tsm_screen_attr *attr, tsm_age_t, void *data) {
  auto *ctx = static_cast<DrawContext *>(data);
  if (!ctx || !attr || !ctx->pixels) {
    return 0;
  }
  uint8_t fr = attr->fr;
  uint8_t fg = attr->fg;
  uint8_t fb = attr->fb;
  uint8_t br = attr->br;
  uint8_t bg = attr->bg;
  uint8_t bb = attr->bb;
  if (attr->inverse) {
    std::swap(fr, br);
    std::swap(fg, bg);
    std::swap(fb, bb);
  }
  const uint32_t background = pack_rgb(br, bg, bb);
  const uint32_t foreground = pack_rgb(fr, fg, fb);
  const int origin_x = static_cast<int>(posx) * ctx->cell_w;
  const int origin_y = static_cast<int>(posy) * ctx->cell_h;
  const int span = std::max(1, static_cast<int>(cell_width));
  for (int row = 0; row < ctx->cell_h; ++row) {
    const int y = origin_y + row;
    if (y < 0 || y >= ctx->height) {
      continue;
    }
    uint32_t *line = ctx->pixels + y * ctx->width;
    for (int col = 0; col < ctx->cell_w * span; ++col) {
      const int x = origin_x + col;
      if (x >= 0 && x < ctx->width) {
        line[x] = background;
      }
    }
  }
  uint32_t codepoint = (len > 0 && ch) ? ch[0] : ' ';
  if (codepoint == 0) {
    codepoint = ' ';
  }
  auto *glyph = ctx->self->glyphs() ? ctx->self->glyphs()->glyph(codepoint, attr->bold != 0)
                                    : nullptr;
  if (!glyph) {
    return 0;
  }
  const int baseline = ctx->self->glyphs()->baseline();
  for (int row = 0; row < glyph->height; ++row) {
    const int y = origin_y + baseline - glyph->top + row;
    if (y < 0 || y >= ctx->height) {
      continue;
    }
    uint32_t *line = ctx->pixels + y * ctx->width;
    for (int col = 0; col < glyph->width; ++col) {
      const int x = origin_x + glyph->left + col;
      if (x < 0 || x >= ctx->width) {
        continue;
      }
      const uint8_t coverage =
          glyph->coverage[static_cast<size_t>(row * glyph->width + col)];
      line[x] = mix(line[x], foreground, coverage);
    }
  }
  return 0;
}
}

NTermRenderer::NTermRenderer() : renderer_(std::make_unique<GlyphRenderer>()) {}

bool NTermRenderer::init(const std::string &font_family, int font_size) {
  return renderer_ && renderer_->init(font_family, font_size);
}

void NTermRenderer::render(NTerm *term, std::vector<uint32_t> &pixels, int width,
                           int height, bool selected) {
  if (!term || width <= 0 || height <= 0) {
    return;
  }
  pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height),
                pack_rgb(9, 12, 17));
  DrawContext ctx;
  ctx.self = this;
  ctx.term = term;
  ctx.pixels = pixels.data();
  ctx.width = width;
  ctx.height = height;
  ctx.cell_w = renderer_ ? renderer_->cell_width() : 8;
  ctx.cell_h = renderer_ ? renderer_->cell_height() : 16;
  if (term->screen()) {
    tsm_screen_draw(term->screen(), draw_cell, &ctx);
    const unsigned cx = tsm_screen_get_cursor_x(term->screen());
    const unsigned cy = tsm_screen_get_cursor_y(term->screen());
    const int x0 = static_cast<int>(cx) * ctx.cell_w;
    const int y0 = static_cast<int>(cy) * ctx.cell_h + ctx.cell_h - 2;
    for (int x = x0; x < x0 + ctx.cell_w && x < width; ++x) {
      if (y0 >= 0 && y0 < height) {
        pixels[static_cast<size_t>(y0 * width + x)] = pack_rgb(89, 166, 255);
      }
    }
  }
  if (selected) {
    const uint32_t accent = pack_rgb(255, 184, 46);
    for (int x = 0; x < width; ++x) {
      pixels[static_cast<size_t>(x)] = accent;
      pixels[static_cast<size_t>((height - 1) * width + x)] = accent;
    }
    for (int y = 0; y < height; ++y) {
      pixels[static_cast<size_t>(y * width)] = accent;
      pixels[static_cast<size_t>(y * width + (width - 1))] = accent;
    }
  }
}
