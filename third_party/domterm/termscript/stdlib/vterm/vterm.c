/* vterm.c -- std.vterm: a VT100/xterm-class parser over a character grid.
 *
 * feed() consumes bytes: UTF-8 text, C0 controls, and the escape sequences an
 * interactive program actually emits.  The parser state lives in the handle, so
 * a sequence split across two feed() calls is still parsed correctly -- the
 * previous implementation kept the state machine in a local, which silently
 * dropped every sequence that straddled a read boundary.
 *
 * Covered:
 *   - C0: BEL BS HT LF VT FF CR SO SI ESC, plus IND/NEL/RI/HTS/RIS/DECALN
 *   - CSI: ICH CUU CUD CUF CUB CNL CPL CHA HPA CUP CHT ED EL IL DL DCH SU SD
 *          ECH CBT HPR VPA VPR REP DA DSR TBC SGR SM/RM (DECSTBM, DECAWM,
 *          DECOM, IRM, DECCKM, DECTCEM, reverse video, alternate screen,
 *          bracketed paste) and save/restore cursor
 *   - ESC: DECSC/DECRC, charset designation (ASCII and the DEC line-drawing
 *          set, selected with SO/SI), and the string introducers OSC/DCS/APC/PM
 *   - SGR: 0-9, 21-29, 30-37, 39, 40-47, 49, 90-97, 100-107, and 38/48 in both
 *          the 5;N and the 2;R;G;B forms
 *
 * Per-cell state is a Unicode scalar, a display width and a foreground /
 * background colour plus style flags, read back through `cell`, `cell_attr`,
 * `fg` and `bg`.  A wide glyph occupies a lead cell followed by a continuation
 * cell.  Lines pushed off the top of a full-screen scroll region accumulate in
 * a scrollback ring that `scrollback` and `scrollback_line` read.  Device
 * replies owed to the host (DSR, DA) queue up in `reply`.
 *
 * Known limits: charset translation covers ASCII and DEC special graphics
 * only; wcwidth approximates the common wide and zero-width ranges rather than
 * the full UAX #11 tables, and zero-width marks do not combine with the base
 * glyph in the grid; the alternate screen keeps no scrollback; and window
 * manipulation (CSI t) is accepted and ignored.
 */
#include "vterm/vterm.h"

#include "common/ts_std_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- cell attributes ---------------- */

#define VT_BOLD         0x0001u
#define VT_DIM          0x0002u
#define VT_ITALIC       0x0004u
#define VT_UNDERLINE    0x0008u
#define VT_BLINK        0x0010u
#define VT_REVERSE      0x0020u
#define VT_HIDDEN       0x0040u
#define VT_STRIKE       0x0080u
#define VT_DOUBLE_UNDER 0x0100u

/* 0-255 are palette slots; VT_DEFAULT_COLOUR means "the terminal default". */
#define VT_DEFAULT_COLOUR 0x100u

typedef struct
{
  uint32_t ch;      /* Unicode scalar; 0 renders as a blank */
  uint8_t  width;   /* 1 normal, 2 wide lead, 0 wide continuation */
  uint16_t fg;      /* 0-255 palette slot, or VT_DEFAULT_COLOUR */
  uint16_t bg;
  uint16_t attr;
} vt_cell_t;

typedef struct
{
  vt_cell_t *cells;
  size_t cols;
} vt_line_t;

/* ---------------- parser states ---------------- */

enum
{
  PS_GROUND = 0,
  PS_ESC,
  PS_CSI_PARAM,
  PS_CSI_INTERMEDIATE,
  PS_CSI_IGNORE,
  PS_OSC_STRING,
  PS_DCS_STRING,
  PS_STRING_IGNORE,   /* SOS / PM / APC: consumed and dropped */
  PS_STRING_ST,       /* ESC seen inside a string: awaiting ST (ESC \\) */
  PS_CHARSET          /* ESC ( / ESC ) / ESC * / ESC + : one more byte */
};

enum
{
  CHARSET_ASCII = 0,
  CHARSET_GRAPHICS
};

#define VT_MAX_PARAMS        32
#define VT_MAX_INTERMEDIATE  4
#define VT_STRING_MAX        1024
#define VT_TITLE_MAX         256

typedef struct
{
  vt_cell_t *cells;
  size_t rows, cols;
  size_t cur_r, cur_c;

  /* Scroll region, 0-based inclusive. */
  size_t scroll_top, scroll_bot;

  /* Saved cursor (DECSC/DECRC and the save done by mode 1049). */
  size_t sv_r, sv_c;
  vt_cell_t sv_pen;
  bool sv_valid;

  /* Current pen: attributes applied to the next printed cell. */
  vt_cell_t pen;

  /* Modes. */
  bool autowrap;        /* DECAWM (7), on by default */
  bool insert_mode;     /* IRM (4) */
  bool origin_mode;     /* DECOM (6) */
  bool app_cursor;      /* DECCKM (1) */
  bool cursor_visible;  /* DECTCEM (25), on by default */
  bool reverse_video;   /* DECSCNM (5) */
  bool newline_mode;    /* LNM (20) */
  bool alt_screen;      /* 1049 */
  bool bracketed_paste; /* 2004 */

  bool wrap_pending;    /* cursor rests in the deferred-wrap column */
  char charset;         /* translation in force, CHARSET_* */
  char g0, g1;          /* G0 / G1 designations */
  char pending_charset; /* set while PS_CHARSET runs */

  unsigned char *tabs;
  bool tabs_valid;

  /* Parser, all persistent across feed() calls. */
  int pstate;
  long params[VT_MAX_PARAMS];
  bool param_colon[VT_MAX_PARAMS];
  size_t nparams;
  char intermediate[VT_MAX_INTERMEDIATE];
  size_t nintermediate;
  char prefix;          /* '?', '>', '<' or 0 */
  char stringbuf[VT_STRING_MAX];
  size_t stringlen;
  bool string_truncated;
  char string_kind;
  char string_param;    /* OSC command letter */
  bool string_have_param;

  /* UTF-8 decoder, also persistent. */
  uint32_t ucs;
  size_t ucs_need;
  size_t ucs_have;

  uint32_t last_printed; /* last printed scalar, for REP (CSI b) */

  /* Alternate screen, allocated on first use. */
  vt_cell_t *alt_cells;
  size_t alt_rows, alt_cols;

  /* Scrollback ring of lines pushed off the top. */
  vt_line_t *scrollback;
  size_t sb_len, sb_cap, sb_limit;

  char title[VT_TITLE_MAX];  /* last OSC 0/2 payload */

  ts_sbuf_t pending;        /* device replies drained by `reply` */
} vterm_t;

static const char vterm_tag_id = 0;

#define VT_MAX_ROWS 2000
#define VT_MAX_COLS 2000

/* ---------------- width and UTF-8 ---------------- */

/* Approximate wcwidth: zero for combining marks and controls, two for the
 * common East Asian wide and fullwidth ranges plus most emoji, one otherwise.
 * Ranges that matter in practice are covered; the full UAX #11 tables are not
 * vendored here. */
static int
vt_wcwidth (uint32_t c)
{
  if (c == 0)
    return 0;
  if (c < 0x20 || (c >= 0x7F && c < 0xA0))
    return 0;
  if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x0483 && c <= 0x0489)
      || (c >= 0x0591 && c <= 0x05BD) || (c >= 0x0610 && c <= 0x061A)
      || (c >= 0x064B && c <= 0x065F) || (c >= 0x0E31 && c <= 0x0E3A)
      || (c >= 0x1AB0 && c <= 0x1AFF) || (c >= 0x1DC0 && c <= 0x1DFF)
      || (c >= 0x20D0 && c <= 0x20F0) || (c >= 0xFE00 && c <= 0xFE0F)
      || (c >= 0xFE20 && c <= 0xFE2F))
    return 0;
  if ((c >= 0x1100 && c <= 0x115F)      /* Hangul Jamo */
      || (c >= 0x2E80 && c <= 0x303E)   /* CJK radicals, Kangxi, punctuation */
      || (c >= 0x3041 && c <= 0x33FF)   /* Kana, Hangul/CJK compatibility */
      || (c >= 0x3400 && c <= 0x4DBF)   /* CJK extension A */
      || (c >= 0x4E00 && c <= 0x9FFF)   /* CJK unified ideographs */
      || (c >= 0xA000 && c <= 0xA4CF)   /* Yi */
      || (c >= 0xAC00 && c <= 0xD7A3)   /* Hangul syllables */
      || (c >= 0xF900 && c <= 0xFAFF)   /* CJK compatibility ideographs */
      || (c >= 0xFE10 && c <= 0xFE19)   /* vertical forms */
      || (c >= 0xFE30 && c <= 0xFE6F)   /* compatibility/small forms */
      || (c >= 0xFF00 && c <= 0xFF60)   /* fullwidth forms */
      || (c >= 0xFFE0 && c <= 0xFFE6)
      || (c >= 0x1F300 && c <= 0x1F64F) /* emoji */
      || (c >= 0x1F900 && c <= 0x1F9FF)
      || (c >= 0x20000 && c <= 0x3FFFD)) /* CJK extensions B and later */
    return 2;
  return 1;
}

static size_t
vt_encode_utf8 (uint32_t c, char *out)
{
  if (c < 0x80)
    {
      out[0] = (char) c;
      return 1;
    }
  if (c < 0x800)
    {
      out[0] = (char) (0xC0 | (c >> 6));
      out[1] = (char) (0x80 | (c & 0x3F));
      return 2;
    }
  if (c < 0x10000)
    {
      out[0] = (char) (0xE0 | (c >> 12));
      out[1] = (char) (0x80 | ((c >> 6) & 0x3F));
      out[2] = (char) (0x80 | (c & 0x3F));
      return 3;
    }
  out[0] = (char) (0xF0 | (c >> 18));
  out[1] = (char) (0x80 | ((c >> 12) & 0x3F));
  out[2] = (char) (0x80 | ((c >> 6) & 0x3F));
  out[3] = (char) (0x80 | (c & 0x3F));
  return 4;
}

/* DEC special graphics: the line-drawing set the alternate charset selects.
 * Indexed by (scalar - 0x5F).  The entries are Unicode scalars, most of them
 * box-drawing characters that need multi-byte UTF-8 encoding. */
static const uint32_t vt_graphics[32] = {
  0x0020, 0x25C6, 0x2592, 0x2592, 0x2592, 0x2592, 0x2592, 0x2592, /* _ ` a-f */
  0x2424, 0x240B, 0x240A, 0x2518, 0x2510, 0x250C, 0x2514, 0x253C, /* g-o */
  0x2534, 0x2500, 0x251C, 0x2524, 0x256E, 0x256F, 0x2570, 0x2572, /* p-w */
  0x2572, 0x2502, 0x2500, 0x2533, 0x2502, 0x23BA, 0x2500, 0x0000  /* x-~ */
};

static uint32_t
vt_translate (const vterm_t *v, uint32_t c)
{
  if (v->charset == CHARSET_GRAPHICS && c >= 0x5F && c <= 0x7E)
    {
      uint32_t mapped = vt_graphics[c - 0x5F];
      return mapped ? mapped : c;
    }
  return c;
}

/* ---------------- grid helpers ---------------- */

static vt_cell_t *
vt_row (vterm_t *v, size_t r)
{
  return v->cells + r * v->cols;
}

static vt_cell_t
vt_pen_cell (void)
{
  vt_cell_t c;
  c.ch = 0;
  c.width = 1;
  c.fg = VT_DEFAULT_COLOUR;
  c.bg = VT_DEFAULT_COLOUR;
  c.attr = 0;
  return c;
}

/* Erasing fills with the current background, as xterm does. */
static vt_cell_t
vt_blank_cell (const vterm_t *v)
{
  vt_cell_t c = vt_pen_cell ();
  c.bg = v->pen.bg;
  return c;
}

static void
vt_erase_span (vterm_t *v, size_t r, size_t from, size_t to)
{
  vt_cell_t blank = vt_blank_cell (v);
  vt_cell_t *row = vt_row (v, r);
  size_t i;
  if (from >= v->cols)
    return;
  if (to > v->cols)
    to = v->cols;
  for (i = from; i < to; i++)
    row[i] = blank;
}

/* ---------------- scrollback ---------------- */

static void
vt_sb_push (vterm_t *v, const vt_cell_t *row, size_t len)
{
  vt_line_t *grown;
  size_t ncap;
  vt_line_t *slot;

  if (v->sb_limit == 0)
    return;
  if (v->sb_len == v->sb_cap)
    {
      if (v->sb_cap >= v->sb_limit)
        {
          /* The ring is full: evict the oldest line to make room rather than
           * growing, which would reallocate to the size we already have. */
          free (v->scrollback[0].cells);
          memmove (v->scrollback, v->scrollback + 1,
                   (v->sb_len - 1) * sizeof *v->scrollback);
          v->sb_len--;
        }
      else
        {
          ncap = v->sb_cap ? v->sb_cap * 2 : 64;
          if (ncap > v->sb_limit)
            ncap = v->sb_limit;
          grown = realloc (v->scrollback, ncap * sizeof *grown);
          if (!grown)
            return;                  /* history is best-effort */
          memset (grown + v->sb_cap, 0, (ncap - v->sb_cap) * sizeof *grown);
          v->scrollback = grown;
          v->sb_cap = ncap;
        }
    }
  slot = &v->scrollback[v->sb_len];
  slot->cells = malloc (len * sizeof *slot->cells);
  if (!slot->cells)
    return;
  memcpy (slot->cells, row, len * sizeof *slot->cells);
  slot->cols = len;
  v->sb_len++;
}

/* Scroll [top, bot] up (n > 0) or down (n < 0) by n lines. */
static void
vt_scroll (vterm_t *v, size_t top, size_t bot, int n)
{
  size_t height, i, r;
  vt_cell_t blank;

  if (bot < top)
    return;
  height = bot - top + 1;
  if (n > (int) height)
    n = (int) height;
  if (n < -(int) height)
    n = -(int) height;
  if (n == 0)
    return;

  blank = vt_blank_cell (v);
  if (n > 0)
    {
      if (top == 0 && bot == v->rows - 1 && !v->alt_screen)
        for (r = 0; r < (size_t) n; r++)
          vt_sb_push (v, vt_row (v, r), v->cols);
      memmove (vt_row (v, top), vt_row (v, top + (size_t) n),
               (height - (size_t) n) * v->cols * sizeof *v->cells);
      for (r = bot + 1 - (size_t) n; r <= bot; r++)
        for (i = 0; i < v->cols; i++)
          vt_row (v, r)[i] = blank;
    }
  else
    {
      size_t k = (size_t) -n;
      memmove (vt_row (v, top + k), vt_row (v, top),
               (height - k) * v->cols * sizeof *v->cells);
      for (r = top; r < top + k; r++)
        for (i = 0; i < v->cols; i++)
          vt_row (v, r)[i] = blank;
    }
}

/* ---------------- cursor ---------------- */

static void
vt_clamp (vterm_t *v)
{
  if (v->rows == 0 || v->cols == 0)
    return;
  if (v->cur_r >= v->rows)
    v->cur_r = v->rows - 1;
  if (v->cur_c >= v->cols)
    v->cur_c = v->cols - 1;
}

static void
vt_linefeed (vterm_t *v)
{
  if (v->cur_r == v->scroll_bot)
    vt_scroll (v, v->scroll_top, v->scroll_bot, 1);
  else if (v->cur_r + 1 < v->rows)
    v->cur_r++;
  v->wrap_pending = false;
}

static void
vt_reverse_index (vterm_t *v)
{
  if (v->cur_r == v->scroll_top)
    vt_scroll (v, v->scroll_top, v->scroll_bot, -1);
  else if (v->cur_r > 0)
    v->cur_r--;
  v->wrap_pending = false;
}

static void
vt_save_cursor (vterm_t *v)
{
  v->sv_r = v->cur_r;
  v->sv_c = v->cur_c;
  v->sv_pen = v->pen;
  v->sv_valid = true;
}

static void
vt_restore_cursor (vterm_t *v)
{
  if (!v->sv_valid)
    return;
  v->cur_r = v->sv_r;
  v->cur_c = v->sv_c;
  v->pen = v->sv_pen;
  v->charset = v->g0;
  v->wrap_pending = false;
  vt_clamp (v);
}

/* ---------------- tabs ---------------- */

static void
vt_tabs_init (vterm_t *v)
{
  size_t i;
  free (v->tabs);
  v->tabs = v->cols ? calloc (v->cols, 1) : NULL;
  if (!v->tabs)
    {
      v->tabs_valid = false;
      return;
    }
  for (i = 8; i < v->cols; i += 8)
    v->tabs[i] = 1;
  v->tabs_valid = true;
}

static void
vt_tab_forward (vterm_t *v)
{
  size_t c = v->cur_c;
  for (;;)
    {
      if (c + 1 >= v->cols)
        {
          c = v->cols - 1;
          break;
        }
      c++;
      if (!v->tabs_valid || v->tabs[c])
        break;
    }
  v->cur_c = c;
  v->wrap_pending = false;
}

static void
vt_tab_backward (vterm_t *v)
{
  size_t c = v->cur_c;
  while (c > 0)
    {
      c--;
      if (!v->tabs_valid || v->tabs[c])
        break;
    }
  v->cur_c = c;
  v->wrap_pending = false;
}

/* ---------------- printing ---------------- */

static void
vt_put (vterm_t *v, uint32_t raw)
{
  uint32_t ucs = vt_translate (v, raw);
  int width = vt_wcwidth (ucs);
  vt_cell_t *row;

  /* Zero-width marks have no cell of their own; they neither move the cursor
   * nor combine with the base glyph in this grid model. */
  if (width == 0)
    return;

  /* A wide glyph never straddles the right margin. */
  if (width == 2 && v->cur_c + 1 >= v->cols)
    {
      if (!v->autowrap)
        return;
      v->cur_c = 0;
      vt_linefeed (v);
    }
  else if (v->wrap_pending)
    {
      if (v->autowrap)
        {
          v->cur_c = 0;
          vt_linefeed (v);
        }
      /* With DECAWM reset the cursor stays in the last column and further
       * characters overwrite it, which is what a real terminal does. */
      v->wrap_pending = false;
    }

  row = vt_row (v, v->cur_r);
  if (v->insert_mode)
    {
      memmove (row + v->cur_c + 1, row + v->cur_c,
               (v->cols - v->cur_c - 1) * sizeof *row);
      row[v->cols - 1] = vt_blank_cell (v);
    }

  row[v->cur_c].ch = ucs;
  row[v->cur_c].width = (uint8_t) width;
  row[v->cur_c].fg = v->pen.fg;
  row[v->cur_c].bg = v->pen.bg;
  row[v->cur_c].attr = v->pen.attr;

  if (width == 2)
    {
      vt_cell_t cont = vt_blank_cell (v);
      cont.ch = 0;
      cont.width = 0;
      row[v->cur_c + 1] = cont;
    }

  v->last_printed = ucs;
  if (v->cur_c + (size_t) width >= v->cols)
    v->wrap_pending = true;     /* deferred: only wraps when more text follows */
  else
    v->cur_c += (size_t) width;
}

/* ---------------- SGR ---------------- */

/* Consume an extended colour at params[i] == 38/48; returns the index of the
 * last parameter used.  Recognises 5;N and 2;R;G;B. */
static size_t
vt_ext_colour (vterm_t *v, size_t i, bool foreground)
{
  long kind;

  if (i + 1 >= v->nparams)
    {
      if (foreground)
        v->pen.fg = VT_DEFAULT_COLOUR;
      else
        v->pen.bg = VT_DEFAULT_COLOUR;
      return i;
    }
  kind = v->params[i + 1];
  if (kind == 5 && i + 2 < v->nparams)
    {
      long n = v->params[i + 2];
      if (n < 0)
        n = 0;
      if (n > 255)
        n = 255;
      if (foreground)
        v->pen.fg = (uint8_t) n;
      else
        v->pen.bg = (uint8_t) n;
      return i + 2;
    }
  if (kind == 2 && i + 4 < v->nparams)
    {
      long r = v->params[i + 2], g = v->params[i + 3],
           b = v->params[i + 4];
      long slot;
      if (r < 0) r = 0;
      if (g < 0) g = 0;
      if (b < 0) b = 0;
      if (r > 255) r = 255;
      if (g > 255) g = 255;
      if (b > 255) b = 255;
      /* The 6x6x6 colour cube is the closest thing in the 256-colour palette
       * to a 24-bit request; truecolour is a host rendering concern. */
      slot = 16 + 36 * (r * 5 / 255) + 6 * (g * 5 / 255) + (b * 5 / 255);
      if (foreground)
        v->pen.fg = (uint8_t) slot;
      else
        v->pen.bg = (uint8_t) slot;
      return i + 4;
    }
  if (foreground)
    v->pen.fg = VT_DEFAULT_COLOUR;
  else
    v->pen.bg = VT_DEFAULT_COLOUR;
  return i + 1;
}

static void
vt_sgr (vterm_t *v)
{
  size_t i = 0;

  if (v->nparams == 0)
    {
      v->pen = vt_pen_cell ();
      return;
    }
  while (i < v->nparams)
    {
      long p = v->params[i];

      if (p == 0)
        v->pen = vt_pen_cell ();
      else if (p == 1)
        v->pen.attr |= VT_BOLD;
      else if (p == 2)
        v->pen.attr |= VT_DIM;
      else if (p == 3)
        v->pen.attr |= VT_ITALIC;
      else if (p == 4)
        {
          v->pen.attr &= (uint16_t) ~(VT_UNDERLINE | VT_DOUBLE_UNDER);
          if (i + 1 < v->nparams && v->params[i + 1] == 2)
            {
              v->pen.attr |= VT_DOUBLE_UNDER;
              i++;
            }
          else
            v->pen.attr |= VT_UNDERLINE;
        }
      else if (p == 5 || p == 6)
        v->pen.attr |= VT_BLINK;
      else if (p == 7)
        v->pen.attr |= VT_REVERSE;
      else if (p == 8)
        v->pen.attr |= VT_HIDDEN;
      else if (p == 9)
        v->pen.attr |= VT_STRIKE;
      else if (p == 21)
        {
          v->pen.attr &= (uint16_t) ~VT_BOLD;
          v->pen.attr |= VT_DOUBLE_UNDER;
        }
      else if (p == 22)
        v->pen.attr &= (uint16_t) ~(VT_BOLD | VT_DIM);
      else if (p == 23)
        v->pen.attr &= (uint16_t) ~VT_ITALIC;
      else if (p == 24)
        v->pen.attr &= (uint16_t) ~(VT_UNDERLINE | VT_DOUBLE_UNDER);
      else if (p == 25)
        v->pen.attr &= (uint16_t) ~VT_BLINK;
      else if (p == 27)
        v->pen.attr &= (uint16_t) ~VT_REVERSE;
      else if (p == 28)
        v->pen.attr &= (uint16_t) ~VT_HIDDEN;
      else if (p == 29)
        v->pen.attr &= (uint16_t) ~VT_STRIKE;
      else if (p >= 30 && p <= 37)
        v->pen.fg = (uint8_t) (p - 30);
      else if (p == 38)
        {
          i = vt_ext_colour (v, i, true);
          i++;
          continue;
        }
      else if (p == 39)
        v->pen.fg = VT_DEFAULT_COLOUR;
      else if (p >= 40 && p <= 47)
        v->pen.bg = (uint8_t) (p - 40);
      else if (p == 48)
        {
          i = vt_ext_colour (v, i, false);
          i++;
          continue;
        }
      else if (p == 49)
        v->pen.bg = VT_DEFAULT_COLOUR;
      else if (p >= 90 && p <= 97)
        v->pen.fg = (uint8_t) (p - 90 + 8);
      else if (p >= 100 && p <= 107)
        v->pen.bg = (uint8_t) (p - 100 + 8);
      i++;
    }
}

/* ---------------- alternate screen ---------------- */

static void
vt_clear_grid (vterm_t *v)
{
  vt_cell_t blank = vt_blank_cell (v);
  size_t i;
  for (i = 0; i < v->rows * v->cols; i++)
    v->cells[i] = blank;
}

static void
vt_set_alt (vterm_t *v, bool on)
{
  vt_cell_t *saved;
  size_t saved_rows = v->rows, saved_cols = v->cols;

  if (on == v->alt_screen)
    return;
  if (on)
    {
      if (!v->alt_cells || v->alt_rows != v->rows || v->alt_cols != v->cols)
        {
          free (v->alt_cells);
          v->alt_cells = malloc (v->rows * v->cols * sizeof *v->alt_cells);
          if (!v->alt_cells)
            {
              v->alt_rows = v->alt_cols = 0;
              return;
            }
          v->alt_rows = v->rows;
          v->alt_cols = v->cols;
        }
      saved = v->cells;
      v->cells = v->alt_cells;
      v->alt_cells = saved;
      v->alt_screen = true;
      vt_clear_grid (v);
      v->cur_r = v->cur_c = 0;
      v->wrap_pending = false;
      return;
    }
  saved = v->cells;
  v->cells = v->alt_cells;
  v->alt_cells = saved;
  v->alt_screen = false;
  /* The alternate grid was sized for the current dimensions; re-clamp the
   * saved primary grid, which never changes size while we are away. */
  v->rows = saved_rows;
  v->cols = saved_cols;
  vt_clamp (v);
}

/* ---------------- modes ---------------- */

static void
vt_set_mode (vterm_t *v, long mode, bool on, bool private_mode)
{
  if (!private_mode)
    {
      switch (mode)
        {
        case 2:      /* KAM: keyboard action, treated as a no-op here */
        case 12:     /* SRM: local echo, host concern */
          return;
        case 4:
          v->insert_mode = on;
          return;
        case 20:
          v->newline_mode = on;
          return;
        default:
          return;
        }
    }
  switch (mode)
    {
    case 1:
      v->app_cursor = on;
      return;
    case 3:
      if (on)
        vt_clear_grid (v);        /* DECSED clears the whole screen */
      v->cur_r = v->cur_c = 0;
      v->wrap_pending = false;
      return;
    case 5:
      v->reverse_video = on;
      return;
    case 6:
      v->origin_mode = on;
      v->cur_r = on ? v->scroll_top : 0;
      v->cur_c = 0;
      v->wrap_pending = false;
      return;
    case 7:
      v->autowrap = on;
      return;
    case 12:
      return;                      /* cursor blink, host concern */
    case 25:
      v->cursor_visible = on;
      return;
    case 47:
    case 1047:
      vt_set_alt (v, on);
      return;
    case 1048:
      if (on)
        vt_save_cursor (v);
      else
        vt_restore_cursor (v);
      return;
    case 1049:
      if (on)
        {
          vt_save_cursor (v);
          vt_set_alt (v, true);
        }
      else
        {
          vt_set_alt (v, false);
          vt_restore_cursor (v);
        }
      return;
    case 2004:
      v->bracketed_paste = on;
      return;
    default:
      return;                      /* mouse modes and the rest: ignored */
    }
}

/* ---------------- CSI dispatch ---------------- */

static long
vt_param (vterm_t *v, size_t i, long dflt)
{
  if (i >= v->nparams || v->params[i] <= 0)
    return dflt;
  return v->params[i];
}

static void
vt_reply (vterm_t *v, const char *fmt, long a, long b)
{
  char buf[64];
  int n;
  if (fmt[0] == '\0')
    return;
  if (b >= 0)
    n = snprintf (buf, sizeof buf, fmt, a, b);
  else
    n = snprintf (buf, sizeof buf, fmt, a);
  if (n > 0)
    ts_sbuf_put (&v->pending, buf, (size_t) n < sizeof buf ? (size_t) n
                                                          : sizeof buf - 1);
}

static void
vt_insert_lines (vterm_t *v, long n)
{
  if (v->cur_r < v->scroll_top || v->cur_r > v->scroll_bot)
    return;
  vt_scroll (v, v->cur_r, v->scroll_bot, (int) n);
}

static void
vt_delete_lines (vterm_t *v, long n)
{
  if (v->cur_r < v->scroll_top || v->cur_r > v->scroll_bot)
    return;
  vt_scroll (v, v->cur_r, v->scroll_bot, -(int) n);
}

static void
vt_insert_chars (vterm_t *v, long n)
{
  vt_cell_t *row = vt_row (v, v->cur_r);
  if (n <= 0)
    return;
  if ((size_t) n >= v->cols)
    {
      vt_erase_span (v, v->cur_r, 0, v->cols);
      return;
    }
  memmove (row + v->cur_c + n, row + v->cur_c,
           (v->cols - v->cur_c - (size_t) n) * sizeof *row);
  vt_erase_span (v, v->cur_r, v->cur_c, v->cur_c + (size_t) n);
}

static void
vt_delete_chars (vterm_t *v, long n)
{
  vt_cell_t *row = vt_row (v, v->cur_r);
  if (n <= 0)
    return;
  if ((size_t) n >= v->cols)
    {
      vt_erase_span (v, v->cur_r, 0, v->cols);
      return;
    }
  memmove (row + v->cur_c, row + v->cur_c + n,
           (v->cols - v->cur_c - (size_t) n) * sizeof *row);
  vt_erase_span (v, v->cur_r, v->cols - (size_t) n, v->cols);
}

static void
vt_rep (vterm_t *v, long n)
{
  long i;
  if (v->last_printed == 0)
    return;
  for (i = 0; i < n && i < (long) (v->rows * v->cols); i++)
    vt_put (v, v->last_printed);
}

static void
vt_csi (vterm_t *v, char final)
{
  long p0 = vt_param (v, 0, 1);
  long p1 = vt_param (v, 1, 1);
  bool priv = v->prefix == '?';

  /* DECALN and the private-mode report are handled elsewhere. */
  switch (final)
    {
    case '@':                      /* ICH */
      vt_insert_chars (v, p0);
      return;
    case 'A':                      /* CUU */
      v->cur_r = p0 > (long) v->cur_r ? 0 : v->cur_r - (size_t) p0;
      v->wrap_pending = false;
      return;
    case 'B':                      /* CUD */
      v->cur_r += (size_t) p0;
      vt_clamp (v);
      v->wrap_pending = false;
      return;
    case 'C':                      /* CUF */
    case 'a':                      /* HPR */
      v->cur_c += (size_t) p0;
      vt_clamp (v);
      v->wrap_pending = false;
      return;
    case 'D':                      /* CUB */
      v->cur_c = p0 > (long) v->cur_c ? 0 : v->cur_c - (size_t) p0;
      v->wrap_pending = false;
      return;
    case 'E':                      /* CNL */
      v->cur_r += (size_t) p0;
      vt_clamp (v);
      v->cur_c = 0;
      v->wrap_pending = false;
      return;
    case 'F':                      /* CPL */
      v->cur_r = p0 > (long) v->cur_r ? 0 : v->cur_r - (size_t) p0;
      v->cur_c = 0;
      v->wrap_pending = false;
      return;
    case 'G':                      /* CHA */
    case '`':                      /* HPA */
      v->cur_c = (size_t) p0 - 1;
      vt_clamp (v);
      v->wrap_pending = false;
      return;
    case 'H':                      /* CUP */
    case 'f':                      /* HVP */
      v->cur_r = (size_t) p0 - 1;
      v->cur_c = (size_t) p1 - 1;
      if (v->origin_mode)
        v->cur_r += v->scroll_top;
      vt_clamp (v);
      v->wrap_pending = false;
      return;
    case 'I':                      /* CHT */
      {
        long i;
        for (i = 0; i < p0; i++)
          vt_tab_forward (v);
      }
      return;
    case 'J':                      /* ED */
      {
        long mode = v->nparams ? v->params[0] : 0;
        size_t r;
        if (mode == 0)
          {
            vt_erase_span (v, v->cur_r, v->cur_c, v->cols);
            for (r = v->cur_r + 1; r < v->rows; r++)
              vt_erase_span (v, r, 0, v->cols);
          }
        else if (mode == 1)
          {
            vt_erase_span (v, v->cur_r, 0, v->cur_c + 1);
            for (r = 0; r < v->cur_r; r++)
              vt_erase_span (v, r, 0, v->cols);
          }
        else if (mode == 2 || mode == 3)
          for (r = 0; r < v->rows; r++)
            vt_erase_span (v, r, 0, v->cols);
        if (mode != 0)
          v->wrap_pending = false;
        return;
      }
    case 'K':                      /* EL */
      {
        long mode = v->nparams ? v->params[0] : 0;
        if (mode == 0)
          vt_erase_span (v, v->cur_r, v->cur_c, v->cols);
        else if (mode == 1)
          vt_erase_span (v, v->cur_r, 0, v->cur_c + 1);
        else
          vt_erase_span (v, v->cur_r, 0, v->cols);
        v->wrap_pending = false;
        return;
      }
    case 'L':                      /* IL */
      vt_insert_lines (v, p0);
      v->cur_c = 0;
      v->wrap_pending = false;
      return;
    case 'M':                      /* DL */
      vt_delete_lines (v, p0);
      v->cur_c = 0;
      v->wrap_pending = false;
      return;
    case 'P':                      /* DCH */
      vt_delete_chars (v, p0);
      v->wrap_pending = false;
      return;
    case 'S':                      /* SU */
      vt_scroll (v, v->scroll_top, v->scroll_bot, (int) p0);
      return;
    case 'T':                      /* SD */
      vt_scroll (v, v->scroll_top, v->scroll_bot, -(int) p0);
      return;
    case 'X':                      /* ECH */
      vt_erase_span (v, v->cur_r, v->cur_c, v->cur_c + (size_t) p0);
      v->wrap_pending = false;
      return;
    case 'Z':                      /* CBT */
      {
        long i;
        for (i = 0; i < p0; i++)
          vt_tab_backward (v);
      }
      return;
    case 'b':                      /* REP */
      vt_rep (v, p0);
      return;
    case 'c':                      /* DA */
      if (!priv)
        vt_reply (v, "\033[?1;2c", 0, -1);
      return;
    case 'd':                      /* VPA */
      v->cur_r = (size_t) p0 - 1;
      if (v->origin_mode)
        v->cur_r += v->scroll_top;
      vt_clamp (v);
      v->wrap_pending = false;
      return;
    case 'e':                      /* VPR */
      v->cur_r += (size_t) p0;
      vt_clamp (v);
      v->wrap_pending = false;
      return;
    case 'g':                      /* TBC */
      if (v->tabs_valid && v->cur_c < v->cols
          && (v->nparams == 0 || v->params[0] == 3))
        v->tabs[v->cur_c] = 0;
      return;
    case 'h':                      /* SM */
    case 'l':                      /* RM */
      {
        size_t i;
        for (i = 0; i < v->nparams; i++)
          vt_set_mode (v, v->params[i], final == 'h', priv);
      }
      return;
    case 'm':                      /* SGR */
      vt_sgr (v);
      return;
    case 'n':                      /* DSR */
      {
        long mode = v->nparams ? v->params[0] : 0;
        if (mode == 5)
          vt_reply (v, "\033[0n", 0, -1);
        else if (mode == 6)
          vt_reply (v, "\033[%ld;%ldR", (long) v->cur_r + 1,
                    (long) v->cur_c + 1);
      }
      return;
    case 'r':                      /* DECSTBM */
      {
        size_t top = v->nparams && v->params[0] > 0
                         ? (size_t) v->params[0] - 1 : 0;
        size_t bot = (v->nparams > 1 && v->params[1] > 0)
                         ? (size_t) v->params[1] - 1 : v->rows - 1;
        if (top < bot && bot < v->rows)
          {
            v->scroll_top = top;
            v->scroll_bot = bot;
          }
        v->cur_r = v->origin_mode ? v->scroll_top : 0;
        v->cur_c = 0;
        v->wrap_pending = false;
      }
      return;
    case 's':                      /* save cursor (SCO) */
      vt_save_cursor (v);
      return;
    case 'u':                      /* restore cursor (SCO) */
      vt_restore_cursor (v);
      return;
    default:
      return;                      /* including 'p', 'q', 't', 'x' */
    }
}

/* ---------------- escape dispatch ---------------- */

/* DECALN (ESC # 8): fill the screen with 'E', the alignment-test pattern. */
static void
vt_dec_aln (vterm_t *v)
{
  size_t r, i;
  for (r = 0; r < v->rows; r++)
    for (i = 0; i < v->cols; i++)
      {
        v->cells[r * v->cols + i] = vt_pen_cell ();
        v->cells[r * v->cols + i].ch = 'E';
      }
}

static void
vt_esc (vterm_t *v, unsigned char c)
{
  switch (c)
    {
    case '(':
    case ')':
    case '*':
    case '+':
      v->pending_charset = (char) c;
      v->pstate = PS_CHARSET;
      return;
    case '#':
      v->nintermediate = 1;
      v->intermediate[0] = '#';
      v->pstate = PS_CSI_INTERMEDIATE;
      return;
    case '%':
      v->nintermediate = 1;
      v->intermediate[0] = '%';
      v->pstate = PS_CSI_INTERMEDIATE;
      return;
    case '7':
      vt_save_cursor (v);
      return;
    case '8':
      vt_restore_cursor (v);
      return;
    case 'D':
      vt_linefeed (v);
      return;
    case 'E':
      vt_linefeed (v);
      v->cur_c = 0;
      return;
    case 'H':
      if (v->tabs_valid && v->cur_c < v->cols)
        v->tabs[v->cur_c] = 1;
      return;
    case 'M':
      vt_reverse_index (v);
      return;
    case 'N':                      /* SS2, single shift: consume one byte */
      v->pstate = PS_STRING_IGNORE;
      v->string_kind = 'N';
      v->stringlen = 0;
      return;
    case 'O':                      /* SS3, single shift: consume one byte */
      v->pstate = PS_STRING_IGNORE;
      v->string_kind = 'O';
      v->stringlen = 0;
      return;
    case 'c':                      /* RIS */
      {
        bool wrap = v->autowrap, vis = v->cursor_visible;
        size_t r, i;
        for (r = 0; r < v->rows; r++)
          for (i = 0; i < v->cols; i++)
            v->cells[r * v->cols + i] = vt_pen_cell ();
        v->pen = vt_pen_cell ();
        v->cur_r = v->cur_c = 0;
        v->scroll_top = 0;
        v->scroll_bot = v->rows ? v->rows - 1 : 0;
        v->autowrap = wrap;
        v->cursor_visible = vis;
        v->insert_mode = false;
        v->origin_mode = false;
        v->app_cursor = false;
        v->reverse_video = false;
        v->newline_mode = false;
        v->bracketed_paste = false;
        v->wrap_pending = false;
        v->g0 = v->g1 = CHARSET_ASCII;
        v->charset = CHARSET_ASCII;
        v->sv_valid = false;
        v->last_printed = 0;
        v->title[0] = '\0';
        ts_sbuf_clear (&v->pending);
        vt_tabs_init (v);
      }
      return;
    case 'n':
      v->pstate = PS_OSC_STRING;
      v->string_kind = ']';
      v->string_param = 0;
      v->string_have_param = false;
      v->stringlen = 0;
      v->string_truncated = false;
      return;
    case 'p':
      v->pstate = PS_OSC_STRING;   /* ESC ] ... handled like OSC */
      v->string_kind = ']';
      v->string_param = 0;
      v->string_have_param = false;
      v->stringlen = 0;
      v->string_truncated = false;
      return;
    case 'P':
      v->pstate = PS_DCS_STRING;
      v->string_kind = 'P';
      v->stringlen = 0;
      v->string_truncated = false;
      return;
    case 'X':
    case '^':
    case '_':
      v->pstate = PS_STRING_IGNORE;
      v->string_kind = (char) c;
      v->stringlen = 0;
      return;
    default:
      return;                      /* two-character escapes: ignored */
    }
}

static void
vt_finish_string (vterm_t *v)
{
  if (v->string_kind == ']')
    {
      if (v->string_param == '0' || v->string_param == '2')
        snprintf (v->title, sizeof v->title, "%.*s", (int) v->stringlen,
                  v->stringbuf);
      else if (v->string_param == '4')
        /* XTerm colour query: answer so a probing program is not left
         * waiting for a response that never arrives. */
        ts_sbuf_put (&v->pending, "\033]4;1;rgb:ffff/ffff/ffff\033\\", 29);
    }
  v->pstate = PS_GROUND;
  v->stringlen = 0;
  v->string_truncated = false;
  v->string_param = 0;
  v->string_have_param = false;
}

static void
vt_string_byte (vterm_t *v, unsigned char c)
{
  if (c == 0x07)                   /* BEL terminates OSC */
    {
      vt_finish_string (v);
      return;
    }
  if (c == 0x1B)
    {
      v->pstate = PS_STRING_ST;    /* expect ST, which is ESC backslash */
      return;
    }
  if (v->pstate == PS_DCS_STRING || v->pstate == PS_STRING_IGNORE)
    return;                       /* payload ignored, still consuming */

  /* OSC is "<command><separator><payload>".  Take the command letter first,
   * then drop exactly one separator, so a ';' inside the payload survives. */
  if (!v->string_have_param)
    {
      if (c >= '0' && c <= '9')
        {
          v->string_param = (char) c;
          v->string_have_param = true;
          return;
        }
      if (c == ';' || c == '?' || c == '=')
        {
          v->string_have_param = true;
          return;
        }
    }
  else if (c == ';')
    return;                       /* the separator itself */
  if (v->stringlen < sizeof v->stringbuf - 1)
    v->stringbuf[v->stringlen++] = (char) c;
  else
    v->string_truncated = true;
}

/* ---------------- the state machine ---------------- */

/* Persistent UTF-8 decoder.  A multi-byte sequence may straddle two feed()
 * calls, so the partial scalar lives in the handle. */
static void
vt_decode (vterm_t *v, unsigned char c)
{
  if (v->ucs_need == 0)
    {
      if (c < 0x80)
        {
          vt_put (v, c);
          return;
        }
      if ((c & 0xE0) == 0xC0)
        v->ucs_need = 1, v->ucs = c & 0x1Fu;
      else if ((c & 0xF0) == 0xE0)
        v->ucs_need = 2, v->ucs = c & 0x0Fu;
      else if ((c & 0xF8) == 0xF0)
        v->ucs_need = 3, v->ucs = c & 0x07u;
      else
        return;                    /* stray continuation or invalid lead */
      v->ucs_have = 0;
      return;
    }
  if ((c & 0xC0) != 0x80)
    {
      /* Malformed: abandon the partial scalar and treat this byte as a lead. */
      v->ucs_need = 0;
      v->ucs_have = 0;
      vt_decode (v, c);
      return;
    }
  v->ucs = (v->ucs << 6) | (c & 0x3Fu);
  v->ucs_have++;
  if (v->ucs_have < v->ucs_need)
    return;
  {
    uint32_t scalar = v->ucs;
    v->ucs = 0;
    v->ucs_need = 0;
    v->ucs_have = 0;
    if (scalar >= 0xD800 && scalar <= 0xDFFF)
      return;                      /* lone surrogate */
    vt_put (v, scalar);
  }
}

/* Entry point for one byte, whatever state the parser is in. */
static void
vt_step (vterm_t *v, unsigned char c)
{
  switch (v->pstate)
    {
    case PS_GROUND:
      if (c == 0x1B)
        {
          v->pstate = PS_ESC;
          v->nparams = 0;
          v->nintermediate = 0;
          v->prefix = 0;
          return;
        }
      if (c == 0x0E)               /* SO: select G1 */
        {
          v->charset = v->g1;
          return;
        }
      if (c == 0x0F)               /* SI: select G0 */
        {
          v->charset = v->g0;
          return;
        }
      if (c == '\n' || c == 0x0B || c == 0x0C)
        {
          vt_linefeed (v);
          if (v->newline_mode)
            v->cur_c = 0;
          return;
        }
      if (c == '\r')
        {
          v->cur_c = 0;
          v->wrap_pending = false;
          return;
        }
      if (c == '\b')
        {
          if (v->wrap_pending)
            v->wrap_pending = false;
          else if (v->cur_c > 0)
            v->cur_c--;
          return;
        }
      if (c == '\t')
        {
          vt_tab_forward (v);
          return;
        }
      if (c < 0x20 || c == 0x7F)
        return;                    /* BEL, NUL, DEL: no grid effect */
      vt_decode (v, c);
      return;

    case PS_ESC:
      if (c == '[')
        {
          v->pstate = PS_CSI_PARAM;
          v->nparams = 0;
          v->nintermediate = 0;
          v->prefix = 0;
          return;
        }
      if (c == ']')
        {
          v->pstate = PS_OSC_STRING;
          v->string_kind = ']';
          v->string_param = 0;
          v->string_have_param = false;
          v->stringlen = 0;
          v->string_truncated = false;
          return;
        }
      if (c == 'P')
        {
          v->pstate = PS_DCS_STRING;
          v->string_kind = 'P';
          v->stringlen = 0;
          v->string_truncated = false;
          return;
        }
      if (c == 'X' || c == '^' || c == '_')
        {
          v->pstate = PS_STRING_IGNORE;
          v->string_kind = (char) c;
          v->stringlen = 0;
          return;
        }
      vt_esc (v, c);
      /* vt_esc may have moved to a sub-state of its own (charset
       * designation, ESC # / ESC % intermediates); do not clobber it. */
      if (v->pstate == PS_ESC)
        v->pstate = PS_GROUND;
      return;

    case PS_CHARSET:
      {
        char code = (char) c;
        char *slot = v->pending_charset == '(' ? &v->g0
                     : v->pending_charset == ')' ? &v->g1
                     : NULL;
        if (slot)
          *slot = (code == '0') ? CHARSET_GRAPHICS : CHARSET_ASCII;
        if (v->pending_charset == '(')
          v->charset = v->g0;
        else if (v->pending_charset == ')')
          v->charset = v->g1;
        v->pstate = PS_GROUND;
      }
      return;

    case PS_CSI_PARAM:
      if (c >= '0' && c <= '9')
        {
          size_t i = v->nparams ? v->nparams - 1 : 0;
          if (v->nparams == 0)
            {
              v->nparams = 1;
              v->params[0] = 0;
              v->param_colon[0] = false;
              i = 0;
            }
          if (v->params[i] < 1000000)
            v->params[i] = v->params[i] * 10 + (c - '0');
          return;
        }
      if (c == ';' || c == ':')
        {
          if (v->nparams == 0)
            {
              v->nparams = 1;
              v->params[0] = 0;
              v->param_colon[0] = false;
            }
          if (v->nparams < VT_MAX_PARAMS)
            {
              v->param_colon[v->nparams] = (c == ':');
              v->params[v->nparams] = 0;
              v->nparams++;
            }
          return;
        }
      if (c == '?' || c == '>' || c == '<')
        {
          if (v->nparams == 0)
            v->prefix = (char) c;
          else
            v->pstate = PS_CSI_IGNORE;
          return;
        }
      if (c >= 0x20 && c <= 0x2F)   /* intermediate byte */
        {
          if (v->nintermediate < VT_MAX_INTERMEDIATE)
            v->intermediate[v->nintermediate++] = (char) c;
          return;
        }
      if (c >= 0x40 && c <= 0x7E)   /* final byte */
        {
          vt_csi (v, (char) c);
          v->pstate = PS_GROUND;
          return;
        }
      v->pstate = PS_CSI_IGNORE;    /* control byte inside CSI: drop */
      return;

    case PS_CSI_INTERMEDIATE:
      if (c >= 0x20 && c <= 0x2F)
        {
          if (v->nintermediate < VT_MAX_INTERMEDIATE)
            v->intermediate[v->nintermediate++] = (char) c;
          return;
        }
      if (c >= 0x30 && c <= 0x3F)
        {
          /* ESC # Pn and ESC % Pn are complete on their parameter byte: there
           * is no separate final byte to follow. */
          if (v->nintermediate == 1 && v->intermediate[0] == '#' && c == '8')
            vt_dec_aln (v);
          v->pstate = PS_GROUND;
          v->nintermediate = 0;
          return;
        }
      if (c >= 0x40 && c <= 0x7E)
        {
          v->pstate = PS_GROUND;
          v->nintermediate = 0;
          return;
        }
      v->pstate = PS_GROUND;
      return;

    case PS_CSI_IGNORE:
      if (c >= 0x40 && c <= 0x7E)
        v->pstate = PS_GROUND;
      return;

    case PS_OSC_STRING:
    case PS_DCS_STRING:
    case PS_STRING_IGNORE:
      vt_string_byte (v, c);
      return;

    case PS_STRING_ST:
      /* ST is ESC backslash; a stray ESC otherwise ends the string too, so a
       * malformed sequence cannot wedge the parser permanently. */
      (void) c;
      vt_finish_string (v);
      return;

    default:
      v->pstate = PS_GROUND;
      return;
    }
}

/* ---------------- feed ---------------- */

static void
vt_feed (vterm_t *v, const char *s)
{
  const unsigned char *p = (const unsigned char *) s;
  size_t i, n = strlen (s);

  for (i = 0; i < n; i++)
    vt_step (v, p[i]);
  /* An unterminated OSC/DCS/UTF-8 sequence is left pending on purpose: the
   * rest arrives in the next feed(). */
}

/* ---------------- lifecycle ---------------- */

static void
vt_free (void *p)
{
  vterm_t *v = p;
  size_t i;
  if (!v)
    return;
  for (i = 0; i < v->sb_len; i++)
    free (v->scrollback[i].cells);
  free (v->scrollback);
  free (v->alt_cells);
  free (v->tabs);
  free (v->cells);
  ts_sbuf_free (&v->pending);
  free (v);
}

static vterm_t *
vt_unwrap (const TS_Value *val, TS_Error *error, const char *what)
{
  return ts_std_handle (val, &vterm_tag_id, error, what);
}

static vterm_t *
vt_alloc (size_t rows, size_t cols)
{
  vterm_t *v = calloc (1, sizeof *v);
  if (!v)
    return NULL;
  v->cells = malloc (rows * cols * sizeof *v->cells);
  if (!v->cells)
    {
      free (v);
      return NULL;
    }
  v->rows = rows;
  v->cols = cols;
  v->scroll_bot = rows - 1;
  v->autowrap = true;
  v->cursor_visible = true;
  v->g0 = v->g1 = CHARSET_ASCII;
  v->pstate = PS_GROUND;
  v->pen = vt_pen_cell ();
  v->sv_pen = v->pen;
  v->title[0] = '\0';
  ts_sbuf_init (&v->pending);
  vt_clear_grid (v);
  vt_tabs_init (v);
  return v;
}

static void
vt_reset_state (vterm_t *v)
{
  v->cur_r = v->cur_c = 0;
  v->scroll_top = 0;
  v->scroll_bot = v->rows - 1;
  v->pen = vt_pen_cell ();
  v->autowrap = true;
  v->cursor_visible = true;
  v->insert_mode = v->origin_mode = v->app_cursor = false;
  v->reverse_video = v->newline_mode = v->bracketed_paste = false;
  v->wrap_pending = false;
  v->g0 = v->g1 = CHARSET_ASCII;
  v->charset = CHARSET_ASCII;
  v->sv_valid = false;
  v->last_printed = 0;
  v->pstate = PS_GROUND;
  v->nparams = v->nintermediate = 0;
  v->stringlen = 0;
  v->ucs = v->ucs_need = v->ucs_have = 0;
  v->title[0] = '\0';
  ts_sbuf_clear (&v->pending);
  vt_clear_grid (v);
  vt_tabs_init (v);
}

static TS_Status
vt_new (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
        TS_Value *ret, TS_Error *error)
{
  int64_t rows = 24, cols = 80;
  vterm_t *v;
  char desc[64];
  (void) ud;
  if (ts_std_argc (vm, argc, 0, 2, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (argc >= 1 && ts_std_int (&argv[0], &rows, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (argc == 2 && ts_std_int (&argv[1], &cols, error, "new") != TS_OK)
    return TS_ERR_INVAL;
  if (rows < 1 || rows > VT_MAX_ROWS || cols < 1 || cols > VT_MAX_COLS)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "new: bad size");
      return TS_ERR_INVAL;
    }
  v = vt_alloc ((size_t) rows, (size_t) cols);
  if (!v)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  snprintf (desc, sizeof desc, "<vterm %lldx%lld>", (long long) rows,
            (long long) cols);
  if (ts_value_make_handle (ret, v, vt_free, desc, &vterm_tag_id) != TS_OK)
    {
      vt_free (v);
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  return TS_OK;
}

static TS_Status
vt_resize (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  int64_t rows, cols;
  vt_cell_t *grown;
  vt_cell_t blank;
  size_t r, i;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "resize") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "resize");
  if (!v)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[1], &rows, error, "resize") != TS_OK
      || ts_std_int (&argv[2], &cols, error, "resize") != TS_OK)
    return TS_ERR_INVAL;
  if (rows < 1 || rows > VT_MAX_ROWS || cols < 1 || cols > VT_MAX_COLS)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "resize: bad size");
      return TS_ERR_INVAL;
    }
  if ((size_t) rows == v->rows && (size_t) cols == v->cols)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }

  blank = vt_blank_cell (v);
  grown = malloc ((size_t) rows * (size_t) cols * sizeof *grown);
  if (!grown)
    {
      ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
      return TS_ERR_NOMEM;
    }
  /* Keep the top-left intersection, blank the rest. */
  for (r = 0; r < (size_t) rows; r++)
    for (i = 0; i < (size_t) cols; i++)
      if (r < v->rows && i < v->cols)
        grown[r * (size_t) cols + i] = v->cells[r * v->cols + i];
      else
        grown[r * (size_t) cols + i] = blank;
  free (v->cells);
  v->cells = grown;
  v->rows = (size_t) rows;
  v->cols = (size_t) cols;
  v->scroll_top = 0;
  v->scroll_bot = v->rows - 1;
  v->cur_r = v->cur_r < v->rows ? v->cur_r : v->rows - 1;
  v->cur_c = v->cur_c < v->cols ? v->cur_c : v->cols - 1;
  v->wrap_pending = false;
  if (v->alt_cells)
    free (v->alt_cells);
  v->alt_cells = NULL;
  v->alt_rows = v->alt_cols = 0;
  vt_tabs_init (v);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
vt_set_scrollback (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
                   TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  int64_t limit;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "scrollback") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "scrollback");
  if (!v)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[1], &limit, error, "scrollback") != TS_OK)
    return TS_ERR_INVAL;
  if (limit < 0 || limit > 100000)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "scrollback: bad limit");
      return TS_ERR_INVAL;
    }
  /* Trim immediately so the reported length is honest. */
  while (v->sb_len > (size_t) limit)
    {
      free (v->scrollback[0].cells);
      memmove (v->scrollback, v->scrollback + 1,
               (v->sb_len - 1) * sizeof *v->scrollback);
      v->sb_len--;
    }
  v->sb_limit = (size_t) limit;
  ts_std_ret_int (ret, (int64_t) v->sb_len);
  return TS_OK;
}

static TS_Status
vt_feed_fn (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
            TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  const char *text;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "feed") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "feed");
  if (!v || ts_std_str (&argv[1], &text, error, "feed") != TS_OK)
    return TS_ERR_INVAL;
  vt_feed (v, text);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static TS_Status
vt_text (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  ts_sbuf_t b;
  size_t r, i;
  char *out;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "text") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "text");
  if (!v)
    return TS_ERR_INVAL;
  ts_sbuf_init (&b);
  for (r = 0; r < v->rows; r++)
    {
      size_t end = v->cols;
      while (end > 0 && v->cells[r * v->cols + end - 1].ch == ' ')
        end--;
      for (i = 0; i < end; i++)
        {
          uint32_t ch = v->cells[r * v->cols + i].ch;
          char buf[4];
          size_t n = ch ? vt_encode_utf8 (ch, buf) : 0;
          if (n && ts_sbuf_put (&b, buf, n) != 0)
            goto oom;
        }
      if (ts_sbuf_ch (&b, '\n') != 0)
        goto oom;
    }
  out = ts_sbuf_take (&b);
  st = ts_std_ret_str (ret, out, error);
  free (out);            /* ts_std_ret_str copies */
  return st;

oom:
  ts_sbuf_free (&b);
  ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
  return TS_ERR_NOMEM;
}

static TS_Status
vt_cell_get (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
             TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  int64_t r, c;
  char buf[8];
  const vt_cell_t *cell;
  size_t n = 0;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "cell") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "cell");
  if (!v)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[1], &r, error, "cell") != TS_OK
      || ts_std_int (&argv[2], &c, error, "cell") != TS_OK)
    return TS_ERR_INVAL;
  if (r < 0 || c < 0 || (size_t) r >= v->rows || (size_t) c >= v->cols)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "cell: out of range");
      return TS_ERR_INVAL;
    }
  cell = &v->cells[(size_t) r * v->cols + (size_t) c];
  if (cell->ch)
    n = vt_encode_utf8 (cell->ch, buf);
  return ts_std_ret_strn (ret, buf, n, error);
}

static TS_Status
vt_cell_attr (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
              TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  int64_t r, c;
  const vt_cell_t *cell;
  ts_sbuf_t b;
  char *out;
  TS_Status st;
  bool first = true;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, "cell_attr") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "cell_attr");
  if (!v)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[1], &r, error, "cell_attr") != TS_OK
      || ts_std_int (&argv[2], &c, error, "cell_attr") != TS_OK)
    return TS_ERR_INVAL;
  if (r < 0 || c < 0 || (size_t) r >= v->rows || (size_t) c >= v->cols)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "cell_attr: out of range");
      return TS_ERR_INVAL;
    }
  cell = &v->cells[(size_t) r * v->cols + (size_t) c];
  ts_sbuf_init (&b);
#define ATTR_PUT(cond, text)                                    \
  do {                                                          \
    if (cond)                                                   \
      {                                                         \
        if (!first)                                             \
          ts_sbuf_ch (&b, ';');                                 \
        ts_sbuf_str (&b, text);                                 \
        first = false;                                          \
      }                                                         \
  } while (0)
  if (cell->fg != VT_DEFAULT_COLOUR)
    {
      char tmp[16];
      snprintf (tmp, sizeof tmp, "fg=%u", (unsigned) cell->fg);
      ATTR_PUT (1, tmp);
    }
  if (cell->bg != VT_DEFAULT_COLOUR)
    {
      char tmp[16];
      snprintf (tmp, sizeof tmp, "bg=%u", (unsigned) cell->bg);
      ATTR_PUT (1, tmp);
    }
  ATTR_PUT ((cell->attr & VT_BOLD), "bold");
  ATTR_PUT ((cell->attr & VT_DIM), "dim");
  ATTR_PUT ((cell->attr & VT_ITALIC), "italic");
  ATTR_PUT ((cell->attr & VT_UNDERLINE), "underline");
  ATTR_PUT ((cell->attr & VT_DOUBLE_UNDER), "double_underline");
  ATTR_PUT ((cell->attr & VT_BLINK), "blink");
  ATTR_PUT ((cell->attr & VT_REVERSE), "reverse");
  ATTR_PUT ((cell->attr & VT_HIDDEN), "hidden");
  ATTR_PUT ((cell->attr & VT_STRIKE), "strike");
#undef ATTR_PUT
  out = ts_sbuf_take (&b);
  st = ts_std_ret_str (ret, out, error);
  free (out);            /* ts_std_ret_str copies */
  return st;
}

static TS_Status
vt_colour_of (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
              TS_Value *ret, TS_Error *error, bool foreground)
{
  vterm_t *v;
  int64_t r, c;
  const char *what = foreground ? "fg" : "bg";
  const vt_cell_t *cell;
  (void) ud;
  if (ts_std_argc (vm, argc, 3, 3, error, what) != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, what);
  if (!v)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[1], &r, error, what) != TS_OK
      || ts_std_int (&argv[2], &c, error, what) != TS_OK)
    return TS_ERR_INVAL;
  if (r < 0 || c < 0 || (size_t) r >= v->rows || (size_t) c >= v->cols)
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "%s: out of range", what);
      return TS_ERR_INVAL;
    }
  cell = &v->cells[(size_t) r * v->cols + (size_t) c];
  ts_std_ret_int (ret, foreground ? (int64_t) cell->fg
                                  : (int64_t) cell->bg);
  return TS_OK;
}

static TS_Status
vt_fg (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  return vt_colour_of (vm, ud, argv, argc, ret, error, true);
}

static TS_Status
vt_bg (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
       TS_Value *ret, TS_Error *error)
{
  return vt_colour_of (vm, ud, argv, argc, ret, error, false);
}

static TS_Status
vt_cursor (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
           TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  char buf[64];
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "cursor") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "cursor");
  if (!v)
    return TS_ERR_INVAL;
  snprintf (buf, sizeof buf, "%zu,%zu", v->cur_r, v->cur_c);
  return ts_std_ret_str (ret, buf, error);
}

static TS_Status
vt_size (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error, bool rows_wanted)
{
  vterm_t *v;
  const char *what = rows_wanted ? "rows" : "cols";
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, what) != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, what);
  if (!v)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, rows_wanted ? (int64_t) v->rows : (int64_t) v->cols);
  return TS_OK;
}

static TS_Status
vt_rows (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  return vt_size (vm, ud, argv, argc, ret, error, true);
}

static TS_Status
vt_cols (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  return vt_size (vm, ud, argv, argc, ret, error, false);
}

static TS_Status
vt_mode (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
         TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  const char *name;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "mode") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "mode");
  if (!v)
    return TS_ERR_INVAL;
  if (ts_std_str (&argv[1], &name, error, "mode") != TS_OK)
    return TS_ERR_INVAL;
  if (strcmp (name, "wrap") == 0)
    ts_std_ret_bool (ret, v->autowrap);
  else if (strcmp (name, "insert") == 0)
    ts_std_ret_bool (ret, v->insert_mode);
  else if (strcmp (name, "origin") == 0)
    ts_std_ret_bool (ret, v->origin_mode);
  else if (strcmp (name, "app_cursor") == 0)
    ts_std_ret_bool (ret, v->app_cursor);
  else if (strcmp (name, "cursor_visible") == 0)
    ts_std_ret_bool (ret, v->cursor_visible);
  else if (strcmp (name, "reverse_video") == 0)
    ts_std_ret_bool (ret, v->reverse_video);
  else if (strcmp (name, "alt") == 0)
    ts_std_ret_bool (ret, v->alt_screen);
  else if (strcmp (name, "bracketed_paste") == 0)
    ts_std_ret_bool (ret, v->bracketed_paste);
  else if (strcmp (name, "scroll_top") == 0)
    ts_std_ret_int (ret, (int64_t) v->scroll_top);
  else if (strcmp (name, "scroll_bottom") == 0)
    ts_std_ret_int (ret, (int64_t) v->scroll_bot);
  else if (strcmp (name, "graphics") == 0)
    ts_std_ret_bool (ret, v->charset == CHARSET_GRAPHICS);
  else
    {
      ts_error_set (error, TS_ERR_INVAL, 0, 0, "mode: unknown mode '%s'",
                    name);
      return TS_ERR_INVAL;
    }
  return TS_OK;
}

static TS_Status
vt_scrollback_len (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
                   TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "scrollback_lines") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "scrollback_lines");
  if (!v)
    return TS_ERR_INVAL;
  ts_std_ret_int (ret, (int64_t) v->sb_len);
  return TS_OK;
}

/* scrollback_line(v, i): 0 is the oldest retained line. */
static TS_Status
vt_scrollback_line (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
                    TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  int64_t idx;
  const vt_line_t *line;
  ts_sbuf_t b;
  size_t i, end;
  char *out;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, argc, 2, 2, error, "scrollback_line") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "scrollback_line");
  if (!v)
    return TS_ERR_INVAL;
  if (ts_std_int (&argv[1], &idx, error, "scrollback_line") != TS_OK)
    return TS_ERR_INVAL;
  if (idx < 0 || (size_t) idx >= v->sb_len)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  line = &v->scrollback[idx];
  ts_sbuf_init (&b);
  end = line->cols;
  while (end > 0 && line->cells[end - 1].ch == ' ')
    end--;
  for (i = 0; i < end; i++)
    {
      uint32_t ch = line->cells[i].ch;
      char buf[4];
      size_t n = ch ? vt_encode_utf8 (ch, buf) : 0;
      if (n && ts_sbuf_put (&b, buf, n) != 0)
        {
          ts_sbuf_free (&b);
          ts_error_set (error, TS_ERR_NOMEM, 0, 0, "out of memory");
          return TS_ERR_NOMEM;
        }
    }
  out = ts_sbuf_take (&b);
  st = ts_std_ret_str (ret, out, error);
  free (out);            /* ts_std_ret_str copies */
  return st;
}

static TS_Status
vt_title (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "title") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "title");
  if (!v)
    return TS_ERR_INVAL;
  return ts_std_ret_str (ret, v->title, error);
}

/* Drain queued device replies (DSR, DA, colour queries). */
static TS_Status
vt_reply_fn (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
             TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  char *out;
  TS_Status st;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "reply") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "reply");
  if (!v)
    return TS_ERR_INVAL;
  if (v->pending.len == 0)
    {
      ts_std_ret_nil (ret);
      return TS_OK;
    }
  out = ts_sbuf_take (&v->pending);
  ts_sbuf_init (&v->pending);
  st = ts_std_ret_str (ret, out, error);
  free (out);            /* ts_std_ret_str copies */
  return st;
}

static TS_Status
vt_reset (TS_VM *vm, void *ud, const TS_Value *argv, size_t argc,
          TS_Value *ret, TS_Error *error)
{
  vterm_t *v;
  (void) ud;
  if (ts_std_argc (vm, argc, 1, 1, error, "reset") != TS_OK)
    return TS_ERR_INVAL;
  v = vt_unwrap (&argv[0], error, "reset");
  if (!v)
    return TS_ERR_INVAL;
  vt_reset_state (v);
  ts_std_ret_nil (ret);
  return TS_OK;
}

static const TS_FuncDef vterm_funcs[] = {
  { "new", vt_new, NULL },
  { "feed", vt_feed_fn, NULL },
  { "text", vt_text, NULL },
  { "cursor", vt_cursor, NULL },
  { "reset", vt_reset, NULL },
  { "resize", vt_resize, NULL },
  { "rows", vt_rows, NULL },
  { "cols", vt_cols, NULL },
  { "cell", vt_cell_get, NULL },
  { "cell_attr", vt_cell_attr, NULL },
  { "fg", vt_fg, NULL },
  { "bg", vt_bg, NULL },
  { "mode", vt_mode, NULL },
  { "scrollback", vt_set_scrollback, NULL },
  { "scrollback_lines", vt_scrollback_len, NULL },
  { "scrollback_line", vt_scrollback_line, NULL },
  { "title", vt_title, NULL },
  { "reply", vt_reply_fn, NULL },
  { NULL, NULL, NULL }
};

const TS_Module ts_std_vterm_module = { "std.vterm", vterm_funcs };
