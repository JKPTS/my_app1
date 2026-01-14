// ===== FILE: esp32-2432.ino (tft_client_2432s028) =====
#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;

// UART: ESP32-S3 TX(GPIO21) -> ESP32-2432 RX(GPIO3)
// ใช้ UART1 (Serial1) RX=GPIO3, TX=GPIO1
static const int UART_RX_PIN = 3;
static const int UART_TX_PIN = 1;
static const uint32_t UART_BAUD = 115200;

// -------------------- state --------------------
static String bankName = "BANK";
static String swName[8] = {"SW1","SW2","SW3","SW4","SW5","SW6","SW7","SW8"};
static int bankIndex = 0;

// line buffer
static String lineBuf;

// debounce NVS save
static bool nvsDirty = false;
static uint32_t nvsSaveAtMs = 0;
static const uint32_t NVS_DEBOUNCE_MS = 800;

// -------------------- geometry --------------------
struct Rect { int16_t x, y, w, h; };
static Rect rCard[8];
static Rect rCardLabel[8];
static Rect rCardBar[8];
static Rect rBank;
static Rect rBankPill;
static Rect rBankEqPlate;
static Rect rBankScanPlate;

static bool uiInited = false;

// -------------------- anim --------------------
static uint32_t lastAnimMs = 0;
static uint16_t animPhase = 0; // free-running
static int scanPos = 0;         // 0..(scanW+plateW)
static const int scanW = 26;

static const uint8_t SIN8[64] PROGMEM = {
  128,140,152,165,176,188,198,208,218,226,234,240,245,250,252,254,
  255,254,252,250,245,240,234,226,218,208,198,188,176,165,152,140,
  128,115,103,90,79,67,57,47,37,29,21,15,10,5,3,1,
  0,1,3,5,10,15,21,29,37,47,57,67,79,90,103,115
};

static inline uint8_t sin8(uint8_t p) {
  return pgm_read_byte(&SIN8[p & 63]);
}

// -------------------- color helpers --------------------
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static uint16_t make565_5_6_5(int r5, int g6, int b5) {
  if (r5 < 0) r5 = 0; if (r5 > 31) r5 = 31;
  if (g6 < 0) g6 = 0; if (g6 > 63) g6 = 63;
  if (b5 < 0) b5 = 0; if (b5 > 31) b5 = 31;
  return (uint16_t)((r5 << 11) | (g6 << 5) | (b5));
}

static uint16_t blend565(uint16_t c1, uint16_t c2, uint8_t k /*0..255*/) {
  int r1 = (c1 >> 11) & 31, g1 = (c1 >> 5) & 63, b1 = c1 & 31;
  int r2 = (c2 >> 11) & 31, g2 = (c2 >> 5) & 63, b2 = c2 & 31;
  int r = (r1 * (255 - k) + r2 * k) / 255;
  int g = (g1 * (255 - k) + g2 * k) / 255;
  int b = (b1 * (255 - k) + b2 * k) / 255;
  return make565_5_6_5(r, g, b);
}

static uint16_t brighten565(uint16_t c, int dr5, int dg6, int db5) {
  int r = ((c >> 11) & 31) + dr5;
  int g = ((c >> 5) & 63) + dg6;
  int b = (c & 31) + db5;
  return make565_5_6_5(r, g, b);
}

static uint16_t darken565(uint16_t c, int dr5, int dg6, int db5) {
  return brighten565(c, -dr5, -dg6, -db5);
}

// -------------------- text helpers --------------------
static String toUpperAscii(const String &in) {
  String out = in;
  for (size_t i = 0; i < out.length(); i++) {
    char c = out[i];
    if (c >= 'a' && c <= 'z') out[i] = (char)(c - 'a' + 'A');
  }
  return out;
}

static String normName(const String &in, const char *fallback) {
  String s = in;
  s.trim();
  if (s.length() == 0) return String(fallback);
  return toUpperAscii(s);
}

static String fitText(const String &txt, int maxPx, int font) {
  String s = txt;
  if (s.length() == 0) return s;
  if (tft.textWidth(s, font) <= maxPx) return s;
  const String dots = "...";
  const int dotsW = tft.textWidth(dots, font);
  while (s.length() > 0 && (tft.textWidth(s, font) + dotsW) > maxPx) {
    s.remove(s.length() - 1);
  }
  if (s.length() == 0) return dots;
  return s + dots;
}

static void computeFitted(const String &label, int maxPx, int preferFont, int fallbackFont,
                          String &outTxt, int &outFont) {
  outFont = preferFont;
  outTxt = label;
  if (tft.textWidth(outTxt, outFont) > maxPx) outFont = fallbackFont;
  outTxt = fitText(outTxt, maxPx, outFont);
}

static void drawTextOutlineCentered(const Rect &r, const String &label,
                                    uint16_t fg, uint16_t bg,
                                    int preferFont, int fallbackFont,
                                    int xOffset = 0, int yOffset = 0) {
  const int pad = 10;
  const int maxPx = r.w - pad * 2;

  String shown;
  int font = preferFont;
  computeFitted(label, maxPx, preferFont, fallbackFont, shown, font);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(font);

  // outline
  tft.setTextColor(rgb565(0,0,0), bg);
  tft.drawString(shown, r.x + r.w/2 - 1 + xOffset, r.y + r.h/2 + yOffset);
  tft.drawString(shown, r.x + r.w/2 + 1 + xOffset, r.y + r.h/2 + yOffset);
  tft.drawString(shown, r.x + r.w/2 + xOffset,     r.y + r.h/2 - 1 + yOffset);
  tft.drawString(shown, r.x + r.w/2 + xOffset,     r.y + r.h/2 + 1 + yOffset);

  // soft shadow
  tft.setTextColor(darken565(fg, 6, 10, 6), bg);
  tft.drawString(shown, r.x + r.w/2 + 1 + xOffset, r.y + r.h/2 + 2 + yOffset);

  // main
  tft.setTextColor(fg, bg);
  tft.drawString(shown, r.x + r.w/2 + xOffset,     r.y + r.h/2 + yOffset);
}

// -------------------- theme (จัดเต็ม + คมชัด) --------------------
static const uint16_t BG_TOP  = rgb565(8, 10, 20);
static const uint16_t BG_BOT  = rgb565(5, 22, 30);
static const uint16_t BG_LINE = rgb565(18, 30, 40);
static const uint16_t BG_DOT  = rgb565(20, 26, 36);

static const uint16_t GLASS_1 = rgb565(14, 18, 30);
static const uint16_t GLASS_2 = rgb565(26, 34, 52);

static const uint16_t PILL_BG = rgb565(10, 12, 20);
static const uint16_t PILL_IN = rgb565(22, 26, 36);

static const uint16_t TEXT_FG = TFT_WHITE;

static const uint16_t ACC_CARD[8] = {
  rgb565(0, 220, 255),   // CYAN
  rgb565(90, 130, 255),  // BLUE
  rgb565(210, 90, 255),  // VIOLET
  rgb565(120, 255, 190), // MINT
  rgb565(255, 175, 45),  // AMBER
  rgb565(255, 85, 140),  // PINK
  rgb565(255, 240, 85),  // YELLOW
  rgb565(80, 255, 120)   // GREEN
};

static uint16_t bankAccentFor(int idx) {
  switch ((idx < 0 ? -idx : idx) % 6) {
    case 0: return rgb565(0, 220, 255);
    case 1: return rgb565(210, 90, 255);
    case 2: return rgb565(120, 255, 190);
    case 3: return rgb565(255, 175, 45);
    case 4: return rgb565(255, 85, 140);
    default:return rgb565(90, 130, 255);
  }
}

// -------------------- NVS --------------------
static void saveToNVS() {
  prefs.begin("disp", false);
  prefs.putInt("bank_i", bankIndex);
  prefs.putString("bank_n", bankName);
  for (int i = 0; i < 8; i++) {
    String key = "sw" + String(i + 1);
    prefs.putString(key.c_str(), swName[i]);
  }
  prefs.end();
}

static void loadFromNVS() {
  prefs.begin("disp", true);
  bankIndex = prefs.getInt("bank_i", 0);
  bankName  = prefs.getString("bank_n", "BANK");
  for (int i = 0; i < 8; i++) {
    String key = "sw" + String(i + 1);
    swName[i] = prefs.getString(key.c_str(), "SW" + String(i + 1));
  }
  prefs.end();
}

// -------------------- drawing primitives --------------------
static void drawVGradient(int x, int y, int w, int h, uint16_t cTop, uint16_t cBot) {
  if (h <= 0 || w <= 0) return;
  for (int i = 0; i < h; i++) {
    uint8_t k = (uint8_t)((i * 255) / (h - 1 <= 0 ? 1 : (h - 1)));
    tft.drawFastHLine(x, y + i, w, blend565(cTop, cBot, k));
  }
}

static void drawGlowRoundRect(int x, int y, int w, int h, int r, uint16_t color) {
  tft.drawRoundRect(x, y, w, h, r, brighten565(color, 3, 6, 3));
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, color);
  tft.drawRoundRect(x + 2, y + 2, w - 4, h - 4, r - 2, darken565(color, 2, 4, 2));
}

static void drawCornerBrackets(const Rect &r, uint16_t c) {
  // small sci-fi brackets (static)
  int x0 = r.x + 6, y0 = r.y + 6;
  int x1 = r.x + r.w - 7, y1 = r.y + r.h - 7;

  // TL
  tft.drawFastHLine(x0, y0, 14, c);
  tft.drawFastVLine(x0, y0, 14, c);
  // TR
  tft.drawFastHLine(x1 - 13, y0, 14, c);
  tft.drawFastVLine(x1, y0, 14, c);
  // BL
  tft.drawFastHLine(x0, y1, 14, c);
  tft.drawFastVLine(x0, y1 - 13, 14, c);
  // BR
  tft.drawFastHLine(x1 - 13, y1, 14, c);
  tft.drawFastVLine(x1, y1 - 13, 14, c);
}

// -------------------- layout --------------------
static void computeLayout() {
  const int W = tft.width();
  const int H = tft.height();

  const int margin = 12;
  const int gap = 10;

  const int cardW = (W - margin * 2 - gap * 3) / 4;
  const int cardH = 72;

  const int topY = 12;
  const int botY = H - 12 - cardH;

  for (int i = 0; i < 4; i++) {
    int x = margin + i * (cardW + gap);
    rCard[i] = { (int16_t)x, (int16_t)topY, (int16_t)cardW, (int16_t)cardH };
  }
  for (int i = 0; i < 4; i++) {
    int x = margin + i * (cardW + gap);
    rCard[i + 4] = { (int16_t)x, (int16_t)botY, (int16_t)cardW, (int16_t)cardH };
  }

  // card label plate + anim bar
  for (int i = 0; i < 8; i++) {
    const Rect &c = rCard[i];
    rCardLabel[i] = { (int16_t)(c.x + 8), (int16_t)(c.y + 20), (int16_t)(c.w - 16), (int16_t)30 };
    rCardBar[i]   = { (int16_t)(c.x + 10), (int16_t)(c.y + c.h - 14), (int16_t)(c.w - 20), (int16_t)8 };
  }

  const int midTop = topY + cardH;
  const int midBot = botY;
  const int midH = midBot - midTop;

  int bankH = 98;
  int bankW = W - margin * 2;
  if (midH > 0) {
    int maxH = midH - 10;
    if (maxH < 78) maxH = 78;
    if (bankH > maxH) bankH = maxH;
  }

  int bankX = margin;
  int bankY = midTop + (midH - bankH) / 2;
  rBank = { (int16_t)bankX, (int16_t)bankY, (int16_t)bankW, (int16_t)bankH };

  // BANK center pill
  rBankPill = { (int16_t)(rBank.x + 22), (int16_t)(rBank.y + (rBank.h/2) - 18),
                (int16_t)(rBank.w - 44), (int16_t)36 };

  // equalizer plate (top-right)
  rBankEqPlate = { (int16_t)(rBank.x + rBank.w - 88), (int16_t)(rBank.y + 12),
                   (int16_t)70, (int16_t)22 };

  // scan plate (bottom)
  rBankScanPlate = { (int16_t)(rBank.x + 18), (int16_t)(rBank.y + rBank.h - 22),
                     (int16_t)(rBank.w - 36), (int16_t)10 };
}

// -------------------- background (static, ไม่กระพริบ) --------------------
static void drawBackgroundStatic() {
  const int W = tft.width();
  const int H = tft.height();

  drawVGradient(0, 0, W, H, BG_TOP, BG_BOT);

  // neon frame lines
  tft.drawFastHLine(0, 0, W, rgb565(0, 140, 180));
  tft.drawFastHLine(0, H - 1, W, rgb565(180, 90, 0));

  // diagonal tech lines
  for (int i = -H; i < W; i += 40) {
    tft.drawLine(i, H - 1, i + H, 0, BG_LINE);
  }

  // dot grid
  for (int y = 10; y < H; y += 22) {
    for (int x = 10; x < W; x += 22) {
      tft.drawPixel(x, y, BG_DOT);
    }
  }

  // corner accents
  tft.drawFastHLine(0, 12, 26, rgb565(0, 220, 255));
  tft.drawFastVLine(12, 0, 26, rgb565(0, 220, 255));

  tft.drawFastHLine(W - 26, H - 13, 26, rgb565(255, 85, 140));
  tft.drawFastVLine(W - 13, H - 26, 26, rgb565(255, 85, 140));
}

// -------------------- CARD (static + anim) --------------------
static void drawCardStatic(int idx) {
  const Rect &c = rCard[idx];
  const Rect &lp = rCardLabel[idx];
  const Rect &bar = rCardBar[idx];
  uint16_t acc = ACC_CARD[idx];

  // shadow
  tft.fillRoundRect(c.x + 3, c.y + 5, c.w, c.h, 16, rgb565(0,0,0));

  // glass gradient
  uint16_t top = blend565(GLASS_1, acc, 40);
  uint16_t bot = blend565(GLASS_2, acc, 16);
  drawVGradient(c.x, c.y, c.w, c.h, top, bot);

  // border glow
  drawGlowRoundRect(c.x, c.y, c.w, c.h, 16, acc);

  // bracket decor
  drawCornerBrackets(c, darken565(acc, 2, 4, 2));

  // top-left notch triangle
  tft.fillTriangle(c.x + 8, c.y + 8, c.x + 28, c.y + 8, c.x + 8, c.y + 28, darken565(acc, 3, 6, 3));

  // label plate (solid => clear text clean, ไม่ค้าง)
  tft.fillRoundRect(lp.x, lp.y, lp.w, lp.h, 10, PILL_BG);
  tft.drawRoundRect(lp.x, lp.y, lp.w, lp.h, 10, darken565(acc, 2, 4, 2));
  tft.drawRoundRect(lp.x + 1, lp.y + 1, lp.w - 2, lp.h - 2, 9, PILL_IN);

  // text on plate
  Rect tr = lp;
  drawTextOutlineCentered(tr, swName[idx], TEXT_FG, PILL_BG, 4, 2, 0, 0);

  // anim bar base (will be redrawn in anim tick too)
  tft.fillRoundRect(bar.x, bar.y, bar.w, bar.h, 4, darken565(acc, 6, 12, 6));
  tft.drawRoundRect(bar.x, bar.y, bar.w, bar.h, 4, darken565(acc, 2, 4, 2));

  // LED dot (fixed position, overwritten each frame => no ghost)
  int ledX = c.x + c.w - 14;
  int ledY = c.y + 14;
  tft.fillCircle(ledX, ledY, 3, darken565(acc, 2, 4, 2));
}

static void drawCardAnim(int idx, uint8_t phase8) {
  const Rect &c = rCard[idx];
  const Rect &bar = rCardBar[idx];
  uint16_t acc = ACC_CARD[idx];

  // bar shimmer
  // clear bar (solid)
  uint16_t base = darken565(acc, 7, 14, 7);
  tft.fillRoundRect(bar.x, bar.y, bar.w, bar.h, 4, base);

  // moving highlight
  int travel = bar.w + 18;
  int pos = (int)((phase8 * 2 + idx * 19) % travel) - 18;
  int hw = 22;

  int x0 = bar.x + pos;
  int x1 = x0 + hw;
  if (x1 < bar.x) x1 = bar.x;
  if (x0 > bar.x + bar.w) x0 = bar.x + bar.w;

  if (x0 < bar.x) x0 = bar.x;
  if (x1 > bar.x + bar.w) x1 = bar.x + bar.w;

  if (x1 > x0) {
    uint16_t hi = brighten565(acc, 4, 8, 4);
    tft.fillRoundRect(x0, bar.y + 1, x1 - x0, bar.h - 2, 3, hi);
  }

  // moving dot on bar
  int dotX = bar.x + (int)((phase8 * 3 + idx * 31) % bar.w);
  tft.fillCircle(dotX, bar.y + bar.h/2, 2, brighten565(acc, 5, 10, 5));

  // LED pulse (fixed position)
  int ledX = c.x + c.w - 14;
  int ledY = c.y + 14;
  uint8_t p = sin8(phase8 + idx * 7);
  uint16_t led = blend565(darken565(acc, 3, 6, 3), brighten565(acc, 6, 12, 6), (uint8_t)(p));
  tft.fillCircle(ledX, ledY, 3, led);
  tft.drawCircle(ledX, ledY, 4, darken565(acc, 3, 6, 3));
}

// -------------------- BANK (static + anim) --------------------
static void drawBankStatic(bool withText /*true at init/update*/) {
  uint16_t acc = bankAccentFor(bankIndex);

  // shadow
  tft.fillRoundRect(rBank.x + 4, rBank.y + 6, rBank.w, rBank.h, 18, rgb565(0,0,0));

  // glass gradient
  uint16_t top = blend565(GLASS_1, acc, 52);
  uint16_t bot = blend565(GLASS_2, acc, 20);
  drawVGradient(rBank.x, rBank.y, rBank.w, rBank.h, top, bot);

  // base border (anim will overwrite glow/pulse)
  drawGlowRoundRect(rBank.x, rBank.y, rBank.w, rBank.h, 18, acc);
  tft.drawRoundRect(rBank.x + 6, rBank.y + 6, rBank.w - 12, rBank.h - 12, 14, darken565(acc, 2, 4, 2));

  // rails
  tft.fillRoundRect(rBank.x + 18, rBank.y + 14, rBank.w - 36, 6, 3, darken565(acc, 6, 12, 6));
  tft.fillRoundRect(rBank.x + 20, rBank.y + 15, rBank.w - 40, 4, 2, brighten565(acc, 3, 6, 3));

  tft.fillRoundRect(rBank.x + 18, rBank.y + rBank.h - 20, rBank.w - 36, 6, 3, darken565(acc, 6, 12, 6));
  tft.fillRoundRect(rBank.x + 20, rBank.y + rBank.h - 19, rBank.w - 40, 4, 2, brighten565(acc, 3, 6, 3));

  // corner brackets
  drawCornerBrackets(rBank, darken565(acc, 2, 4, 2));

  // EQ plate (static)
  tft.fillRoundRect(rBankEqPlate.x, rBankEqPlate.y, rBankEqPlate.w, rBankEqPlate.h, 8, PILL_BG);
  tft.drawRoundRect(rBankEqPlate.x, rBankEqPlate.y, rBankEqPlate.w, rBankEqPlate.h, 8, darken565(acc, 2, 4, 2));

  // scan plate (static)
  tft.fillRoundRect(rBankScanPlate.x, rBankScanPlate.y, rBankScanPlate.w, rBankScanPlate.h, 5, PILL_BG);
  tft.drawRoundRect(rBankScanPlate.x, rBankScanPlate.y, rBankScanPlate.w, rBankScanPlate.h, 5, darken565(acc, 2, 4, 2));

  // center pill (static)
  tft.fillRoundRect(rBankPill.x, rBankPill.y, rBankPill.w, rBankPill.h, 12, PILL_BG);
  tft.drawRoundRect(rBankPill.x, rBankPill.y, rBankPill.w, rBankPill.h, 12, darken565(acc, 2, 4, 2));
  tft.drawRoundRect(rBankPill.x + 1, rBankPill.y + 1, rBankPill.w - 2, rBankPill.h - 2, 11, PILL_IN);

  if (withText) {
    Rect tr = rBankPill;
    drawTextOutlineCentered(tr, bankName, TEXT_FG, PILL_BG, 4, 2, 0, 0);
  }

  // reset scan position to keep clean
  scanPos = 0;
}

static void drawBankAnim(uint8_t phase8) {
  uint16_t acc = bankAccentFor(bankIndex);

  // border pulse (overwrite only borders, not full panel)
  uint8_t p = sin8(phase8);
  uint16_t pulse = blend565(darken565(acc, 2, 4, 2), brighten565(acc, 6, 12, 6), (uint8_t)(p));
  tft.drawRoundRect(rBank.x, rBank.y, rBank.w, rBank.h, 18, pulse);
  tft.drawRoundRect(rBank.x + 1, rBank.y + 1, rBank.w - 2, rBank.h - 2, 17, darken565(pulse, 1, 2, 1));
  tft.drawRoundRect(rBank.x + 6, rBank.y + 6, rBank.w - 12, rBank.h - 12, 14, darken565(pulse, 3, 6, 3));

  // EQ bars (clear inside plate, redraw bars)
  tft.fillRoundRect(rBankEqPlate.x + 2, rBankEqPlate.y + 2, rBankEqPlate.w - 4, rBankEqPlate.h - 4, 7, PILL_BG);
  int bars = 6;
  int bw = (rBankEqPlate.w - 16) / bars;
  int baseY = rBankEqPlate.y + rBankEqPlate.h - 5;
  for (int i = 0; i < bars; i++) {
    uint8_t sp = sin8(phase8 + i * 9);
    int h = 4 + (sp * 12) / 255; // 4..16
    int x = rBankEqPlate.x + 8 + i * bw;
    uint16_t bc = blend565(darken565(acc, 2, 4, 2), brighten565(acc, 6, 12, 6), sp);
    tft.fillRoundRect(x, baseY - h, bw - 2, h, 2, bc);
  }

  // scan plate (clear + draw moving scanner)
  tft.fillRoundRect(rBankScanPlate.x + 2, rBankScanPlate.y + 2, rBankScanPlate.w - 4, rBankScanPlate.h - 4, 4, PILL_BG);

  // update scan position
  scanPos += 4;
  int travel = rBankScanPlate.w + scanW;
  if (scanPos >= travel) scanPos = 0;

  int sx = rBankScanPlate.x + scanPos - scanW;
  int sy = rBankScanPlate.y + 2;
  int sh = rBankScanPlate.h - 4;

  int x0 = sx;
  int x1 = sx + scanW;
  if (x1 < rBankScanPlate.x + 2 || x0 > rBankScanPlate.x + rBankScanPlate.w - 2) {
    // out
  } else {
    if (x0 < rBankScanPlate.x + 2) x0 = rBankScanPlate.x + 2;
    if (x1 > rBankScanPlate.x + rBankScanPlate.w - 2) x1 = rBankScanPlate.x + rBankScanPlate.w - 2;

    // bright core + soft edges
    int w = x1 - x0;
    if (w > 0) {
      uint16_t edge = darken565(acc, 2, 4, 2);
      uint16_t core = brighten565(acc, 6, 12, 6);
      tft.fillRect(x0, sy, w, sh, edge);
      int coreW = (w >= 10) ? (w - 8) : w;
      if (coreW > 0) tft.fillRect(x0 + (w - coreW)/2, sy + 1, coreW, sh - 2, core);
    }
  }

  // small orbit dot around pill (fixed path, overwrite only dot)
  // draw on top of pill border area (dot overwrites previous)
  int cx = rBankPill.x + rBankPill.w/2;
  int cy = rBankPill.y + rBankPill.h/2;
  // 0..63 => map to rectangle perimeter-ish
  uint8_t q = phase8 & 63;
  int ox, oy;
  if (q < 16) { ox = rBankPill.x + (q * rBankPill.w) / 16; oy = rBankPill.y - 2; }
  else if (q < 32) { ox = rBankPill.x + rBankPill.w + 2; oy = rBankPill.y + ((q - 16) * rBankPill.h) / 16; }
  else if (q < 48) { ox = rBankPill.x + rBankPill.w - ((q - 32) * rBankPill.w) / 16; oy = rBankPill.y + rBankPill.h + 2; }
  else { ox = rBankPill.x - 2; oy = rBankPill.y + rBankPill.h - ((q - 48) * rBankPill.h) / 16; }
  tft.fillCircle(ox, oy, 2, brighten565(acc, 6, 12, 6));
}

// bank name transition (no full-screen redraw, no ghost)
static void animateBankNameChange() {
  uint16_t acc = bankAccentFor(bankIndex);

  // flash pill border + slide text (only inside pill)
  const int frames = 9;
  for (int f = 0; f < frames; f++) {
    int off = (26 * (frames - 1 - f)) / (frames - 1); // slide from right
    uint8_t k = (uint8_t)(180 - (f * 110) / (frames - 1));
    uint16_t flash = blend565(acc, brighten565(acc, 8, 16, 8), k);

    // clear pill
    tft.fillRoundRect(rBankPill.x, rBankPill.y, rBankPill.w, rBankPill.h, 12, PILL_BG);
    tft.drawRoundRect(rBankPill.x, rBankPill.y, rBankPill.w, rBankPill.h, 12, flash);
    tft.drawRoundRect(rBankPill.x + 1, rBankPill.y + 1, rBankPill.w - 2, rBankPill.h - 2, 11, PILL_IN);

    Rect tr = rBankPill;
    drawTextOutlineCentered(tr, bankName, TEXT_FG, PILL_BG, 4, 2, off, 0);

    delay(10);
  }

  // final crisp
  tft.fillRoundRect(rBankPill.x, rBankPill.y, rBankPill.w, rBankPill.h, 12, PILL_BG);
  tft.drawRoundRect(rBankPill.x, rBankPill.y, rBankPill.w, rBankPill.h, 12, darken565(acc, 2, 4, 2));
  tft.drawRoundRect(rBankPill.x + 1, rBankPill.y + 1, rBankPill.w - 2, rBankPill.h - 2, 11, PILL_IN);
  Rect tr = rBankPill;
  drawTextOutlineCentered(tr, bankName, TEXT_FG, PILL_BG, 4, 2, 0, 0);
}

// -------------------- UI init/update --------------------
static void uiInitAndDrawAll() {
  computeLayout();
  drawBackgroundStatic();

  // draw static panels
  drawBankStatic(true);
  for (int i = 0; i < 8; i++) {
    drawCardStatic(i);
  }

  uiInited = true;
}

static void uiApplyUpdate(bool bankChanged, uint8_t swMaskChanged) {
  if (!uiInited) uiInitAndDrawAll();

  tft.startWrite();

  if (bankChanged) {
    // redraw bank base with new accent (inside bank rect only)
    drawBankStatic(false);     // base
    animateBankNameChange();   // animated text (pill only)
  }

  for (int i = 0; i < 8; i++) {
    if (swMaskChanged & (1 << i)) {
      // redraw card fully (inside card rect only)
      drawCardStatic(i);
    }
  }

  tft.endWrite();
}

static void uiAnimTick() {
  if (!uiInited) return;

  uint32_t now = millis();
  if ((uint32_t)(now - lastAnimMs) < 33) return; // ~30 FPS
  lastAnimMs = now;

  animPhase++;
  uint8_t ph = (uint8_t)(animPhase & 63);

  tft.startWrite();

  // bank motion
  drawBankAnim(ph);

  // cards motion (bars + LED)
  for (int i = 0; i < 8; i++) {
    drawCardAnim(i, (uint8_t)(ph + i * 3));
  }

  tft.endWrite();
}

// -------------------- protocol --------------------
// from S3: "@U,<bank>,<bankname>,<sw1>,...,<sw8>\r\n"
static bool parseUpdateLine(const String &line, int &outBankIndex, String &outBankName, String outSw[8]) {
  if (!line.startsWith("@U,")) return false;

  String parts[11];
  int count = 0;
  int start = 0;

  while (count < 11) {
    int idx = line.indexOf(',', start);
    if (idx < 0) {
      parts[count++] = line.substring(start);
      break;
    }
    parts[count++] = line.substring(start, idx);
    start = idx + 1;
  }

  if (count < 3) return false;

  outBankIndex = parts[1].toInt();
  outBankName = parts[2];
  outBankName.trim();

  for (int i = 0; i < 8; i++) {
    int pi = 3 + i;
    if (pi < count) {
      outSw[i] = parts[pi];
      outSw[i].trim();
    } else {
      outSw[i] = "";
    }
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(50);

  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  tft.init();
  tft.setRotation(1); // landscape

  loadFromNVS();

  bankName = normName(bankName, "BANK");
  for (int i = 0; i < 8; i++) {
    String fb = "SW" + String(i + 1);
    swName[i] = normName(swName[i], fb.c_str());
  }

  uiInitAndDrawAll();

  Serial.println("tft client ready ✅");
  Serial.printf("uart1 rx=gpio%d tx=gpio%d baud=%lu\n", UART_RX_PIN, UART_TX_PIN, (unsigned long)UART_BAUD);
}

void loop() {
  // animation always runs (no full-screen redraw)
  uiAnimTick();

  while (Serial1.available()) {
    char ch = (char)Serial1.read();
    if (ch == '\r') continue;

    if (ch == '\n') {
      String line = lineBuf;
      lineBuf = "";
      line.trim();
      if (line.length() == 0) continue;

      int newIdx = bankIndex;
      String newBank = bankName;
      String newSw[8];
      for (int i = 0; i < 8; i++) newSw[i] = swName[i];

      if (parseUpdateLine(line, newIdx, newBank, newSw)) {
        newBank = normName(newBank, "BANK");
        for (int i = 0; i < 8; i++) {
          String fb = "SW" + String(i + 1);
          newSw[i] = normName(newSw[i], fb.c_str());
        }

        bool bankChanged = (newIdx != bankIndex) || (newBank != bankName);
        uint8_t swMaskChanged = 0;
        for (int i = 0; i < 8; i++) {
          if (newSw[i] != swName[i]) swMaskChanged |= (1 << i);
        }

        bankIndex = newIdx;
        bankName = newBank;
        for (int i = 0; i < 8; i++) swName[i] = newSw[i];

        // ACK
        Serial1.print("@A,OK\n");
        Serial1.flush();

        // apply UI update (only inside affected regions)
        uiApplyUpdate(bankChanged, swMaskChanged);

        // save debounce
        nvsDirty = true;
        nvsSaveAtMs = millis() + NVS_DEBOUNCE_MS;

        Serial.println("update received ✅");
      }
    } else {
      if (lineBuf.length() < 300) lineBuf += ch;
    }
  }

  if (nvsDirty && (int32_t)(millis() - nvsSaveAtMs) >= 0) {
    saveToNVS();
    nvsDirty = false;
    Serial.println("saved to NVS ✅");
  }
}
