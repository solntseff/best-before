// SPDX-License-Identifier: GPL-3.0-or-later
// Минимальный рендер 1-битных шрифтов (см. tools/gen_fonts.py) поверх Adafruit_GFX.
// Поддержка UTF-8 и упрощённого BiDi: иврит пишется справа налево,
// числа и латиница/кириллица внутри него сохраняют порядок.
#pragma once
#include <Adafruit_GFX.h>
#include <vector>
#include <algorithm>

struct EGlyph {
  uint16_t cp;
  uint16_t off;
  uint8_t w, h;
  int8_t xo, yo;  // смещение битмапа от точки пера на базовой линии
  uint8_t adv;
};

struct EFont {
  const uint8_t* bits;
  const EGlyph* glyphs;
  uint16_t count;
  uint8_t ascent, descent;
};

typedef std::vector<uint32_t> U32;

inline U32 utf8Decode(const char* s) {
  U32 out;
  const uint8_t* p = (const uint8_t*)s;
  while (*p) {
    uint32_t c = *p++;
    int extra = 0;
    if (c >= 0xF0) { c &= 0x07; extra = 3; }
    else if (c >= 0xE0) { c &= 0x0F; extra = 2; }
    else if (c >= 0xC0) { c &= 0x1F; extra = 1; }
    while (extra-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 0x3F);
    out.push_back(c);
  }
  return out;
}

inline const EGlyph* efGlyph(const EFont& f, uint32_t cp) {
  int lo = 0, hi = f.count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint16_t c = f.glyphs[mid].cp;
    if (c == cp) return &f.glyphs[mid];
    if (c < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

inline const EGlyph* efGlyphOrFallback(const EFont& f, uint32_t cp) {
  const EGlyph* g = efGlyph(f, cp);
  if (!g) {
    if (cp == 0x401) g = efGlyph(f, 0x415);        // Ё -> Е
    else if (cp == 0x451) g = efGlyph(f, 0x435);   // ё -> е
    else g = efGlyph(f, '?');
  }
  return g;
}

// ---------- BiDi (упрощённый UAX #9: правила W7, N1/N2, L2) ----------
enum BidiClass : uint8_t { B_L, B_R, B_EN, B_N };

inline BidiClass bidiClass(uint32_t c) {
  if (c >= 0x590 && c <= 0x5FF) return B_R;
  if (c >= '0' && c <= '9') return B_EN;
  if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c >= 0x400) return B_L;
  return B_N;
}

inline uint32_t bidiMirror(uint32_t c) {
  switch (c) {
    case '(': return ')'; case ')': return '(';
    case '[': return ']'; case ']': return '[';
    case '<': return '>'; case '>': return '<';
    case 0xAB: return 0xBB; case 0xBB: return 0xAB;
    default: return c;
  }
}

inline bool hasRtl(const U32& s) {
  for (uint32_t c : s) if (bidiClass(c) == B_R) return true;
  return false;
}

// Логический порядок -> визуальный (слева направо).
inline U32 bidiVisual(const U32& s) {
  if (!hasRtl(s)) return s;
  const size_t n = s.size();
  std::vector<BidiClass> cls(n);
  for (size_t i = 0; i < n; i++) cls[i] = bidiClass(s[i]);

  BidiClass base = B_L;
  for (BidiClass k : cls) if (k == B_L || k == B_R) { base = k; break; }

  // Знаки при числах (3%, 1.5, 10₪) — часть числа.
  for (size_t i = 0; i < n; i++) {
    uint32_t c = s[i];
    bool sign = c == '%' || c == '.' || c == ',' || c == ':' || c == '/' || c == 0xB0 || c == 0x20AA || c == 0x20AC;
    bool nearDigit = (i > 0 && bidiClass(s[i - 1]) == B_EN) || (i + 1 < n && bidiClass(s[i + 1]) == B_EN);
    if (sign && nearDigit) cls[i] = B_EN;
  }
  // W7: число после латиницы/кириллицы ведёт себя как латиница.
  BidiClass lastStrong = base;
  for (size_t i = 0; i < n; i++) {
    if (cls[i] == B_L || cls[i] == B_R) lastStrong = cls[i];
    else if (cls[i] == B_EN && lastStrong == B_L) cls[i] = B_L;
  }
  // N1/N2: нейтральные между одинаковыми направлениями (число считается RTL) берут его,
  // иначе — направление абзаца.
  auto dir = [](BidiClass k) { return k == B_L ? B_L : B_R; };
  for (size_t i = 0; i < n;) {
    if (cls[i] != B_N) { i++; continue; }
    size_t j = i;
    while (j < n && cls[j] == B_N) j++;
    BidiClass before = i > 0 ? dir(cls[i - 1]) : base;
    BidiClass after = j < n ? dir(cls[j]) : base;
    BidiClass r = (before == after) ? before : base;
    for (size_t k = i; k < j; k++) cls[k] = r;
    i = j;
  }
  // Уровни встраивания.
  std::vector<uint8_t> lvl(n);
  uint8_t maxLvl = 0;
  for (size_t i = 0; i < n; i++) {
    if (base == B_L) lvl[i] = cls[i] == B_L ? 0 : (cls[i] == B_R ? 1 : 2);
    else lvl[i] = cls[i] == B_R ? 1 : 2;
    maxLvl = std::max(maxLvl, lvl[i]);
  }
  U32 out(s);
  for (size_t i = 0; i < n; i++) if (lvl[i] & 1) out[i] = bidiMirror(out[i]);
  // L2: от старшего уровня к младшему нечётному разворачиваем участки с уровнем >= L.
  for (int L = maxLvl; L >= 1; L--) {
    for (size_t i = 0; i < n;) {
      if (lvl[i] < L) { i++; continue; }
      size_t j = i;
      while (j < n && lvl[j] >= L) j++;
      std::reverse(out.begin() + i, out.begin() + j);
      std::reverse(lvl.begin() + i, lvl.begin() + j);
      i = j;
    }
  }
  return out;
}

// ---------- измерение и рисование ----------
inline int efWidth(const EFont& f, const U32& s) {
  int w = 0;
  for (uint32_t c : s) { const EGlyph* g = efGlyphOrFallback(f, c); if (g) w += g->adv; }
  return w;
}
inline int efWidth(const EFont& f, const char* s) { return efWidth(f, utf8Decode(s)); }

// Обрезает строку (в логическом порядке) до maxW пикселей, добавляя «…».
inline U32 efFit(const EFont& f, U32 s, int maxW) {
  if (efWidth(f, s) <= maxW) return s;
  int ell = efWidth(f, U32{0x2026});
  while (!s.empty() && efWidth(f, s) + ell > maxW) s.pop_back();
  while (!s.empty() && s.back() == ' ') s.pop_back();
  s.push_back(0x2026);
  return s;
}

// Рисует уже визуально упорядоченную строку. y — базовая линия. Возвращает ширину.
inline int efDrawVisual(Adafruit_GFX& gfx, int x, int y, const EFont& f, const U32& vis, uint16_t color) {
  int x0 = x;
  for (uint32_t c : vis) {
    const EGlyph* g = efGlyphOrFallback(f, c);
    if (!g) continue;
    const uint8_t* b = f.bits + g->off;
    uint32_t bit = 0;
    for (int yy = 0; yy < g->h; yy++)
      for (int xx = 0; xx < g->w; xx++, bit++)
        if (b[bit >> 3] & (0x80 >> (bit & 7))) gfx.drawPixel(x + g->xo + xx, y + g->yo + yy, color);
    x += g->adv;
  }
  return x - x0;
}

// Рисует строку UTF-8 (логический порядок) с BiDi и опциональной обрезкой.
inline int efDraw(Adafruit_GFX& gfx, int x, int y, const EFont& f, const char* s, uint16_t color, int maxW = 0) {
  U32 u = utf8Decode(s);
  if (maxW > 0) u = efFit(f, u, maxW);
  return efDrawVisual(gfx, x, y, f, bidiVisual(u), color);
}

inline int efDrawRight(Adafruit_GFX& gfx, int xRight, int y, const EFont& f, const char* s, uint16_t color) {
  U32 v = bidiVisual(utf8Decode(s));
  return efDrawVisual(gfx, xRight - efWidth(f, v), y, f, v, color);
}
