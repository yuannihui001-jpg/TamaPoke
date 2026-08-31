#pragma once

#include <Arduino_GFX_Library.h>
#include "cn_font.h"

// Arduino_GFX's built-in font is ASCII-only. This canvas keeps that renderer
// for every existing language and intercepts UTF-8 CJK code points for ZH.
class CnCanvas : public Arduino_Canvas {
 public:
  using Arduino_Canvas::Arduino_Canvas;

  size_t write(uint8_t c) override {
    if (utf8Remaining) {
      if ((c & 0xC0) != 0x80) {
        utf8Remaining = 0;
        utf8Codepoint = 0;
        return Arduino_Canvas::write(c);
      }
      utf8Codepoint = (utf8Codepoint << 6) | (c & 0x3F);
      if (--utf8Remaining) return 1;
      drawCodepoint(utf8Codepoint);
      utf8Codepoint = 0;
      return 1;
    }

    if (c < 0x80) return Arduino_Canvas::write(c);
    if ((c & 0xE0) == 0xC0) {
      utf8Codepoint = c & 0x1F;
      utf8Remaining = 1;
      return 1;
    }
    if ((c & 0xF0) == 0xE0) {
      utf8Codepoint = c & 0x0F;
      utf8Remaining = 2;
      return 1;
    }
    if ((c & 0xF8) == 0xF0) {
      utf8Codepoint = c & 0x07;
      utf8Remaining = 3;
      return 1;
    }
    return 1;
  }

 private:
  uint32_t utf8Codepoint = 0;
  uint8_t utf8Remaining = 0;

  void drawCodepoint(uint32_t codepoint) {
    const CnGlyph *glyph = nullptr;
    for (size_t i = 0; i < CN_GLYPH_COUNT; ++i) {
      if (pgm_read_word(&CN_GLYPHS[i].codepoint) == codepoint) {
        glyph = &CN_GLYPHS[i];
        break;
      }
    }

    // Unknown Unicode characters remain visible as a familiar fallback.
    if (!glyph) {
      drawChar(cursor_x, cursor_y, '?', textcolor, textbgcolor);
      cursor_x += CN_GLYPH_WIDTH;
      return;
    }

    // Chinese uses a native 25x25 cell. Keep it at 1x regardless of the
    // surrounding ASCII text size so the requested dimensions remain stable.
    const uint8_t cnScaleX = 1;
    const uint8_t cnScaleY = 1;
    const int16_t cellW = (int16_t)cnScaleX * CN_GLYPH_WIDTH;
    const int16_t cellH = (int16_t)cnScaleY * CN_GLYPH_HEIGHT;
    if (wrap && cursor_x + cellW - 1 > _max_text_x) {
      cursor_x = _min_text_x;
      cursor_y += cellH;
    }
    if (cursor_x > _max_text_x || cursor_y > _max_text_y ||
        cursor_x + cellW - 1 < _min_text_x || cursor_y + cellH - 1 < _min_text_y) {
      cursor_x += cellW;
      return;
    }

    startWrite();
    for (uint8_t row = 0; row < CN_GLYPH_HEIGHT; ++row) {
      const uint32_t bits = pgm_read_dword(&glyph->rows[row]);
      for (uint8_t col = 0; col < CN_GLYPH_WIDTH; ++col) {
        if (!(bits & (1UL << (24 - col)))) continue;
        for (uint8_t dy = 0; dy < cnScaleY; ++dy) {
          for (uint8_t dx = 0; dx < cnScaleX; ++dx) {
            int16_t x = cursor_x + col * cnScaleX + dx;
            int16_t y = cursor_y + row * cnScaleY + dy;
            if (x >= _min_text_x && x <= _max_text_x &&
                y >= _min_text_y && y <= _max_text_y)
              writePixelPreclipped(x, y, textcolor);
          }
        }
      }
    }
    endWrite();
    cursor_x += cellW;
  }
};
