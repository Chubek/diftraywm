// Regression test: compositor-injected content must land in column 0.
//
// NTerm::display() pushes the help pager and notelet frames straight into the
// VT parser, bypassing the tty line discipline that would normally expand "\n"
// to "\r\n".  libtsm treats a bare LF as linefeed-without-carriage-return, so
// every line used to start indented by the width of the line before it, which
// left the page unreadable.  This drives the real NTerm -> tsm_screen path and
// checks the resulting geometry.
#include "nterm/NTerm.hpp"

#include <libtsm.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

struct Grid {
  unsigned rows = 0;
  unsigned cols = 0;
  std::vector<std::string> cells;

  void init(unsigned r, unsigned c) {
    rows = r;
    cols = c;
    cells.assign(static_cast<std::size_t>(r) * c, " ");
  }
  void put(unsigned x, unsigned y, uint32_t ch) {
    if (y >= rows || x >= cols || ch == 0) {
      return;
    }
    cells[static_cast<std::size_t>(y) * cols + x] =
        ch < 0x80 ? std::string(1, static_cast<char>(ch)) : "?";
  }
};

int
draw_cell(tsm_screen *, uint64_t, const uint32_t *ch, size_t len, unsigned,
          unsigned posx, unsigned posy, const struct tsm_screen_attr *,
          tsm_age_t, void *data)
{
  auto *grid = static_cast<Grid *>(data);
  uint32_t value = (len > 0 && ch) ? ch[0] : ' ';
  for (size_t i = 0; i < (len ? len : 1); i++)
    grid->put(posx + static_cast<unsigned>(i), posy, i == 0 ? value : 0);
  return 0;
}

std::string
row_text(const Grid &grid, unsigned y)
{
  std::string line;
  for (unsigned x = 0; x < grid.cols; x++)
    line += grid.cells[static_cast<std::size_t>(y) * grid.cols + x];
  while (!line.empty() && line.back() == ' ')
    line.pop_back();
  return line;
}

// Feed text through the compositor's injection path and read the screen back.
// Each case starts from a cleared screen, the way paint_help_pager() and
// paint_notelet() do, so a trailing newline that scrolls cannot leak between
// cases.
Grid
paint(NTerm &term, const std::string &text, std::size_t rows)
{
  term.resize(40, rows);
  term.display("\x1b[2J\x1b[H");
  term.display(text);
  Grid grid;
  grid.init(static_cast<unsigned>(rows), 40);
  if (term.screen())
    tsm_screen_draw(term.screen(), draw_cell, &grid);
  return grid;
}

} // namespace

int
main() {
  NTerm term;
  if (!term.screen()) {
    std::cerr << "no tsm_screen\n";
    return EXIT_FAILURE;
  }

  // 1. Plain "\n" separators must start every line in column 0.
  {
    const Grid grid = paint(term, "AAA\nBBB\nCCC\n", 6);
    const char *expected[] = {"AAA", "BBB", "CCC"};
    for (unsigned y = 0; y < 3; y++) {
      const std::string got = row_text(grid, y);
      if (got != expected[y]) {
        std::cerr << "line " << y << ": expected \"" << expected[y]
                  << "\", got \"" << got << "\"\n";
        return EXIT_FAILURE;
      }
    }
  }

  // 2. Content already using CRLF must not gain a second carriage return; the
  //    same three lines have to render identically.
  {
    const Grid grid = paint(term, "AAA\r\nBBB\r\nCCC\r\n", 6);
    const char *expected[] = {"AAA", "BBB", "CCC"};
    for (unsigned y = 0; y < 3; y++) {
      const std::string got = row_text(grid, y);
      if (got != expected[y]) {
        std::cerr << "CRLF line " << y << ": expected \"" << expected[y]
                  << "\", got \"" << got << "\"\n";
        return EXIT_FAILURE;
      }
    }
  }

  {
    const Grid grid = paint(term, "AAA\r\nBBB\nCCC\r\n", 6);
    if (row_text(grid, 0) != "AAA" || row_text(grid, 1) != "BBB" || row_text(grid, 2) != "CCC") {
      std::cerr << "mixed line endings misplaced\n"; return EXIT_FAILURE;
    }
  }

  // 3. A leading newline must not be mistaken for an already-terminated one.
  {
    const Grid grid = paint(term, "\nBBB", 6);
    if (row_text(grid, 0) != "" || row_text(grid, 1) != "BBB") {
      std::cerr << "leading newline misplaced: \"" << row_text(grid, 0)
                << "\" / \"" << row_text(grid, 1) << "\"\n";
      return EXIT_FAILURE;
    }
  }

  // 4. A realistic pager frame, in the exact shape HelpPager::render()
  //    produces: each line is content, then EL to clear the tail, then LF.
  {
    const std::string page =
        "\x1b[2J\x1b[H"
        "NAME\x1b[K\n"
        "NAME BODY TEXT\x1b[K\n"
        "\x1b[K\n"
        "SYNOPSIS\x1b[K\n"
        "SYNOPSIS BODY\x1b[K\n"
        "\x1b[K\n"
        "-- page 1/1  q: close\x1b[K";
    const Grid grid = paint(term, page, 8);
    const char *expected[] = {"NAME", "NAME BODY TEXT", "", "SYNOPSIS",
                              "SYNOPSIS BODY", "", "-- page 1/1  q: close"};
    for (unsigned y = 0; y < 7; y++) {
      const std::string got = row_text(grid, y);
      if (got != expected[y]) {
        std::cerr << "pager line " << y << ": expected \"" << expected[y]
                  << "\", got \"" << got << "\"\n";
        return EXIT_FAILURE;
      }
    }
  }

  // A Termscript program can inspect and write this cell without a PTY.
  {
    term.display("\x1b[2J\x1b[H");
    char path[] = "/tmp/diftray-terminal-XXXXXX";
    const int fd = mkstemp(path);
    if (fd < 0) return EXIT_FAILURE;
    close(fd);
    {
      std::ofstream script(path);
      script << "const T = G:load \"diftray.terminal\";\n"
                "T:display \"script\";\n"
                "const accepted = T:send \"input\";\n"
                "const V = G:load \"std.vterm\";\n"
                "const grid = V:new 2 8;\n"
                "V:feed grid \"ok\";\n"
                "const preview = V:text grid;\n"
                "const cursor = T:cursor;\n"
                "const size = T:size;\n"
                "const screen = T:screen;\n"
                "G:puts accepted;\n"
                "G:puts preview;\n"
                "G:puts cursor;\n"
                "G:puts size;\n"
                "G:puts screen;\n";
    }
    const std::string result = term.run_script(path);
    unlink(path);
    if (result.find("false\nok") == std::string::npos ||
        result.find("0,6") == std::string::npos ||
        result.find("8,40") == std::string::npos ||
        result.find("script") == std::string::npos) {
      std::cerr << "terminal script did not control and inspect cell: " << result << '\n';
      return EXIT_FAILURE;
    }
  }

  std::cout << "injected terminal content is laid out correctly\n";
  return EXIT_SUCCESS;
}
