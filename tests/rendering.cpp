#include "nterm/NTerm.hpp"
#include "nterm/NTermRenderer.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
void check(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
}
int main() {
  try {
    NTermRenderer renderer;
    check(renderer.init("monospace", 16), "font initialization failed");
    auto *glyphs = renderer.glyphs();
    const auto old = *glyphs->glyph('M', false);
    check(renderer.init("monospace", 32), "font reload failed");
    const auto bigger = *glyphs->glyph('M', false);
    check(bigger.advance > old.advance && bigger.height > old.height,
          "font reload retained stale glyphs");
    check(!renderer.init("monospace", 0), "invalid font size accepted");
    check(glyphs->glyph('M', false)->coverage == bigger.coverage,
          "failed font reload discarded working font");
    const auto regular = *glyphs->glyph('A', false);
    const auto bold = *glyphs->glyph('A', true);
    check(regular.coverage != bold.coverage, "bold glyph is identical to regular glyph");
    const auto italic = *glyphs->glyph('A', false, true);
    check(regular.coverage != italic.coverage, "italic glyph is identical to regular glyph");
    const uint32_t combining[] = {'x', 0x0301};
    const auto plain = *glyphs->glyph('x', false);
    const auto accented = *glyphs->glyph(combining, 2, false);
    check(plain.coverage != accented.coverage || plain.height != accented.height,
          "combining character was dropped during shaping");

    auto style = renderer.style();
    style.background = 0xff123456;
    style.foreground = 0xffabcdef;
    style.override_foreground = true;
    style.cursor = 0xffff00ff;
    style.cursor_thickness = 2;
    renderer.set_style(style);
    NTerm term;
    term.resize(2, 1);
    const int width = 2 * glyphs->cell_width(), height = glyphs->cell_height();
    std::vector<uint32_t> pixels;
    term.display("\x1b[?25l\x1b[2J\x1b[H");
    renderer.render(&term, pixels, width, height, false);
    check(std::all_of(pixels.begin(), pixels.end(), [&](uint32_t p) { return p == style.background; }),
          "hidden cursor or default background rendered incorrectly");
    term.display("\x1b[?25h");
    renderer.render(&term, pixels, width, height, false);
    check(pixels[(height - 1) * width] == style.cursor, "visible cursor was not drawn");
    check(pixels[0] == style.background, "libtsm block cursor leaked through the themed cursor");
    style.cursor_thickness = 0;
    renderer.set_style(style);
    renderer.render(&term, pixels, width, height, false);
    check(std::all_of(pixels.begin(), pixels.end(), [&](uint32_t p) { return p == style.background; }),
          "zero cursor thickness did not hide the cursor");
    term.display("\x1b[?25l\x1b[H\x1b[4m \x1b[0m");
    renderer.render(&term, pixels, width, height, false);
    check(std::find(pixels.begin(), pixels.end(), style.foreground) != pixels.end(),
          "underlined space was not drawn");
    term.display("\x1b[H\x1b[41m \x1b[0m");
    renderer.render(&term, pixels, width, height, false);
    check(pixels[0] != style.background, "theme overwrote application background color");
    term.display("\x1b[2J\x1b[H\x1b[?5h");
    renderer.render(&term, pixels, width, height, false);
    check(pixels.back() == style.foreground, "reverse-video screen was not honoured");
    style.background = 0x80112233;
    renderer.set_style(style);
    term.display("\x1b[?5l\x1b[2J\x1b[H");
    renderer.render(&term, pixels, width, height, false);
    check(pixels.back() == style.background, "transparent terminal background lost its alpha");
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
