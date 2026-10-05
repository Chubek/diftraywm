#include "nterm/NTermRenderer.hpp"

#include "nterm/NTerm.hpp"
#include "config/Config.hpp"

#include <algorithm>
#include <libtsm.h>

namespace {
uint32_t pack_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) | b;
}

uint32_t mix(uint32_t background, uint32_t foreground, uint8_t coverage) {
  // Both colors are premultiplied ARGB, as required by the scene buffer.
  if (coverage == 0) return background;
  const uint32_t alpha = (foreground >> 24) * coverage / 255;
  uint32_t result = 0;
  for (unsigned shift : {0U, 8U, 16U}) {
    const uint32_t src = ((foreground >> shift) & 255) * coverage / 255;
    const uint32_t dst = ((background >> shift) & 255) * (255 - alpha) / 255;
    result |= std::min(255U, src + dst) << shift;
  }
  const uint32_t out_alpha = alpha + (background >> 24) * (255 - alpha) / 255;
  return result | (out_alpha << 24);
}

struct DrawRun {
  int x, y, width;
  uint32_t foreground;
  bool bold, italic;
  std::vector<uint32_t> text;
};

struct DrawContext {
  NTermRenderer *self = nullptr;
  uint32_t *pixels = nullptr;
  int width = 0;
  int height = 0;
  int cell_w = 8;
  int cell_h = 16;
  std::vector<DrawRun> runs;
};

int draw_cell(struct tsm_screen *, uint64_t, const uint32_t *ch, size_t len,
              unsigned int cell_width, unsigned int posx, unsigned int posy,
              const struct tsm_screen_attr *attr, tsm_age_t, void *data) {
  auto *ctx = static_cast<DrawContext *>(data);
  if (!ctx || !attr || !ctx->pixels || cell_width == 0) {
    return 0;
  }
  const auto &style = ctx->self->style();
  const auto fg_default = attr->fccode == TSM_COLOR_FOREGROUND && style.override_foreground;
  const auto bg_default = attr->bccode == TSM_COLOR_BACKGROUND;
  uint32_t foreground = fg_default ? style.foreground : pack_rgb(attr->fr, attr->fg, attr->fb);
  uint32_t background = bg_default ? style.background : pack_rgb(attr->br, attr->bg, attr->bb);
  // libtsm has already combined reverse-video, cursor and selection flags.
  if (attr->inverse) std::swap(foreground, background);
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
  if (attr->underline) {
    const int y = std::min(origin_y + ctx->self->glyphs()->baseline() + 1, origin_y + ctx->cell_h - 1);
    if (y >= 0 && y < ctx->height) {
      for (int x = std::max(0, origin_x); x < std::min(ctx->width, origin_x + ctx->cell_w * span); ++x)
        ctx->pixels[static_cast<std::size_t>(y) * ctx->width + x] = mix(background, foreground, 255);
    }
  }
  // Backgrounds are painted first. Shaping the whole compatible text run
  // preserves ligatures and contextual forms, and prevents a wide glyph's
  // continuation cell from painting over its right half.
  if (!ch || len == 0 || ch[0] == 0) return 0;
  if (!ctx->runs.empty()) {
    auto &run = ctx->runs.back();
    if (run.y == origin_y && run.x + run.width == origin_x &&
        run.foreground == foreground && run.bold == static_cast<bool>(attr->bold) &&
        run.italic == static_cast<bool>(attr->italic) && run.text.size() + len <= 128) {
      run.text.insert(run.text.end(), ch, ch + len);
      run.width += ctx->cell_w * span;
      return 0;
    }
  }
  ctx->runs.push_back({origin_x, origin_y, ctx->cell_w * span, foreground,
                       attr->bold != 0, attr->italic != 0, {ch, ch + len}});
  return 0;
}

void draw_runs(DrawContext &ctx) {
  auto *glyphs = ctx.self->glyphs();
  if (!glyphs) return;
  for (const auto &run : ctx.runs) {
    const auto *glyph = glyphs->glyph(run.text.data(), run.text.size(), run.bold, run.italic);
    if (!glyph) continue;
    for (int row = 0; row < glyph->height; ++row) {
      const int y = run.y + glyphs->baseline() - glyph->top + row;
      if (y < run.y || y >= std::min(ctx.height, run.y + ctx.cell_h)) continue;
      auto *line = ctx.pixels + static_cast<std::size_t>(y) * ctx.width;
      for (int col = 0; col < glyph->width; ++col) {
        const int x = run.x + glyph->left + col;
        if (x < std::max(0, run.x) || x >= std::min(ctx.width, run.x + run.width)) continue;
        line[x] = mix(line[x], run.foreground, glyph->coverage[static_cast<std::size_t>(row) * glyph->width + col]);
      }
    }
  }
}
}

NTermRenderer::NTermRenderer() : renderer_(std::make_unique<GlyphRenderer>()) {
  const CompositorConfig defaults;
  auto color = [](const float rgb[4]) {
    return pack_rgb(static_cast<uint8_t>(rgb[0] * 255), static_cast<uint8_t>(rgb[1] * 255),
                    static_cast<uint8_t>(rgb[2] * 255));
  };
  style_.background = color(defaults.background_color);
  style_.cursor = style_.highlight = color(defaults.border_color);
  style_.cursor_thickness = defaults.border_size;
}

bool NTermRenderer::init(const std::string &font_family, int font_size) {
  return renderer_ && renderer_->init(font_family, font_size);
}

void NTermRenderer::render(NTerm *term, std::vector<uint32_t> &pixels, int width,
                           int height, bool selected) {
  if (!term || width <= 0 || height <= 0) {
    return;
  }
  pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height),
                style_.background);
  DrawContext ctx;
  ctx.self = this;
  ctx.pixels = pixels.data();
  ctx.width = width;
  ctx.height = height;
  ctx.cell_w = renderer_ ? renderer_->cell_width() : 8;
  ctx.cell_h = renderer_ ? renderer_->cell_height() : 16;
  if (term->screen()) {
    const bool cursor_visible = !(tsm_screen_get_flags(term->screen()) & TSM_SCREEN_HIDE_CURSOR);
    // Suppress libtsm's built-in reverse-video block while drawing our themed
    // cursor. Application reverse video and selection inversion still apply.
    if (cursor_visible) tsm_screen_set_flags(term->screen(), TSM_SCREEN_HIDE_CURSOR);
    tsm_screen_draw(term->screen(), draw_cell, &ctx);
    if (cursor_visible) tsm_screen_reset_flags(term->screen(), TSM_SCREEN_HIDE_CURSOR);
    draw_runs(ctx);
    if (cursor_visible) {
      const unsigned cx = std::min(tsm_screen_get_cursor_x(term->screen()), static_cast<unsigned>(term->columns() - 1));
      const unsigned cy = std::min(tsm_screen_get_cursor_y(term->screen()), static_cast<unsigned>(term->rows() - 1));
      const int x0 = static_cast<int>(cx) * ctx.cell_w;
      const int bottom = static_cast<int>(cy + 1) * ctx.cell_h;
      const int thickness = std::clamp(style_.cursor_thickness, 0, ctx.cell_h);
      for (int y = std::max(0, bottom - thickness); y < std::min(height, bottom); ++y)
        for (int x = std::max(0, x0); x < std::min(width, x0 + ctx.cell_w); ++x)
          pixels[static_cast<std::size_t>(y) * width + x] = mix(pixels[static_cast<std::size_t>(y) * width + x], style_.cursor, 255);
    }
  }
  if (selected) {
    const uint32_t accent = style_.highlight;
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
