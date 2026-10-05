#pragma once
#include <cstdint>
#include <string>

namespace text_input {
// Keep interactive commands bounded, including paste/type macros.
inline bool append(std::string &text, uint32_t ch) {
  if (ch < 32 || ch == 127 || ch > 0x10ffff || (ch >= 0xd800 && ch <= 0xdfff)) return false;
  const std::size_t bytes = ch < 0x80 ? 1 : ch < 0x800 ? 2 : ch < 0x10000 ? 3 : 4;
  if (text.size() + bytes > 16384) return false;
  if (bytes == 1) text += static_cast<char>(ch);
  else {
    if (bytes == 2) text += static_cast<char>(0xc0 | (ch >> 6));
    else if (bytes == 3) text += static_cast<char>(0xe0 | (ch >> 12));
    else text += static_cast<char>(0xf0 | (ch >> 18));
    for (int shift = static_cast<int>(bytes - 2) * 6; shift >= 0; shift -= 6)
      text += static_cast<char>(0x80 | ((ch >> shift) & 63));
  }
  return true;
}
inline void erase_last(std::string &text) {
  if (text.empty()) return;
  auto offset = text.size() - 1;
  while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) --offset;
  text.resize(offset);
}
}
