#include <Arduino.h>
#include <Preferences.h>
#include <TFT_eSPI.h>
#include <math.h>

TFT_eSPI tft = TFT_eSPI();
TFT_eSprite cardSpr = TFT_eSprite(&tft);
TFT_eSprite bankSpr = TFT_eSprite(&tft);
Preferences prefs;

// -------------------- UART --------------------
static const int UART_RX_PIN = 3;
static const int UART_TX_PIN = 1;
static const uint32_t UART_BAUD = 115200;
#define UART_PORT Serial1

// -------------------- state --------------------
static String bankName = "BANK";
static String swName[8] = {"SW1","SW2","SW3","SW4","SW5","SW6","SW7","SW8"};
static int bankIndex = 0; // เก็บไว้แต่ไม่โชว์
static String lineBuf;

// debounce NVS save
static bool nvsDirty = false;
static uint32_t nvsSaveAtMs = 0;
static const uint32_t NVS_DEBOUNCE_MS = 800;

// -------------------- layout --------------------
struct Rect { int x, y, w, h; };
static Rect cardTop[4];
static Rect cardBot[4];
static Rect bankRect;
static bool layoutReady = false;

// -------------------- cache --------------------
static String prevBank = "";
static String prevSw[8];

// -------------------- UI geometry --------------------
static const int R_SWITCH = 20; // โค้ง
static const int R_BANK   = 26; // โค้ง

// -------------------- typewriter animation (BANK only) --------------------
static bool bankTyping = false;
static String bankTarget = "BANK";     // full (fitted) text to type
static int bankTypedLen = 0;
static uint32_t bankNextStepMs = 0;
static const uint32_t BANK_TYPE_MS = 55; // ความเร็วพิมพ์ทีละตัว

// -------------------- color helpers --------------------
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static uint16_t blend565(uint16_t a, uint16_t b, uint8_t t) // 0..255
{
  uint8_t ar = (a >> 11) & 0x1F; ar = (ar * 255) / 31;
  uint8_t ag = (a >> 5)  & 0x3F; ag = (ag * 255) / 63;
  uint8_t ab = (a)       & 0x1F; ab = (ab * 255) / 31;

  uint8_t br = (b >> 11) & 0x1F; br = (br * 255) / 31;
  uint8_t bg = (b >> 5)  & 0x3F; bg = (bg * 255) / 63;
  uint8_t bb = (b)       & 0x1F; bb = (bb * 255) / 31;

  uint8_t cr = (uint8_t)((ar * (255 - t) + br * t) / 255);
  uint8_t cg = (uint8_t)((ag * (255 - t) + bg * t) / 255);
  uint8_t cb = (uint8_t)((ab * (255 - t) + bb * t) / 255);

  return rgb565(cr, cg, cb);
}

static void vgradTFT(int x, int y, int w, int h, uint16_t topC, uint16_t botC)
{
  if (w <= 0 || h <= 0) return;
  int denom = (h > 1) ? (h - 1) : 1;
  for (int i = 0; i < h; i++) {
    uint8_t t = (uint8_t)((i * 255) / denom);
    tft.drawFastHLine(x, y + i, w, blend565(topC, botC, t));
  }
}

// gradient “แบบมนโค้ง” ลง sprite (มุมโปร่งใสไว้ push แบบ transparent)
static void vgradRoundSPR(TFT_eSprite &g, int w, int h, int radius, uint16_t topC, uint16_t botC)
{
  if (w <= 0 || h <= 0) return;
  if (radius < 0) radius = 0;
  if (radius * 2 > w) radius = w / 2;
  if (radius * 2 > h) radius = h / 2;

  int denom = (h > 1) ? (h - 1) : 1;

  for (int y = 0; y < h; y++) {
    uint8_t t = (uint8_t)((y * 255) / denom);
    uint16_t c = blend565(topC, botC, t);

    int dx = 0;
    if (radius > 0) {
      if (y < radius) {
        int yy = (radius - 1) - y;
        float fx = sqrtf((float)radius * (float)radius - (float)yy * (float)yy);
        dx = radius - (int)(fx + 0.5f);
      } else if (y >= (h - radius)) {
        int yy = y - (h - radius);
        float fx = sqrtf((float)radius * (float)radius - (float)yy * (float)yy);
        dx = radius - (int)(fx + 0.5f);
      }
    }

    int x = dx;
    int ww = w - 2 * dx;
    if (ww > 0) g.drawFastHLine(x, y, ww, c);
  }
}

// -------------------- string helpers --------------------
static String toUpperAscii(const String &in)
{
  String out = in;
  for (size_t i = 0; i < out.length(); i++) {
    char c = out[i];
    if (c >= 'a' && c <= 'z') out[i] = (char)(c - 'a' + 'A');
  }
  return out;
}

static String normName(const String &in, const char *fallback)
{
  String s = in;
  s.trim();
  if (s.length() == 0) return String(fallback);
  return toUpperAscii(s);
}

// -------------------- text helpers --------------------
static String fitText(TFT_eSprite &g, const String &txt, int maxPx, int font)
{
  String s = txt;
  if (s.length() == 0) return s;
  if (g.textWidth(s, font) <= maxPx) return s;

  const String dots = "...";
  const int dotsW = g.textWidth(dots, font);
  while (s.length() > 0 && (g.textWidth(s, font) + dotsW) > maxPx) {
    s.remove(s.length() - 1);
  }
  if (s.length() == 0) return dots;
  return s + dots;
}

// switch: font 2 คงที่ (center)
static void drawCenteredFixedFont2(TFT_eSprite &g, int x, int y, int w, int h,
                                   const String &label, uint16_t fg, uint16_t bg)
{
  const int font = 2;
  const int pad = 12;
  const int maxPx = w - pad * 2;

  String shown = fitText(g, label, maxPx, font);

  g.setTextDatum(MC_DATUM);
  g.setTextFont(font);

  g.setTextColor(rgb565(0,0,0), bg);
  g.drawString(shown, x + w/2 + 1, y + h/2 + 1);

  g.setTextColor(fg, bg);
  g.drawString(shown, x + w/2, y + h/2);
}

// BANK: font 4 center (typewriter)
static void drawBankCenteredTypewriterFont4(TFT_eSprite &g, int w, int h,
                                            const String &label, uint16_t fg, uint16_t bg)
{
  const int font = 4;
  const int padX = 22;
  const int maxPx = w - padX * 2;

  String shown = fitText(g, label, maxPx, font);

  g.setTextDatum(MC_DATUM);
  g.setTextFont(font);

  g.setTextColor(rgb565(0,0,0), bg);
  g.drawString(shown, w/2 + 2, h/2 + 2);

  g.setTextColor(fg, bg);
  g.drawString(shown, w/2, h/2);
}

// -------------------- layout compute --------------------
static void computeLayout()
{
  const int W = tft.width();
  const int H = tft.height();

  const int margin = 12;
  const int gap = 10;

  const int cardW = (W - margin * 2 - gap * 3) / 4;
  const int cardH = 62;

  const int topY = 14;
  const int botY = H - 14 - cardH;

  const int midTop = topY + cardH;
  const int midBot = botY;
  const int midH = midBot - midTop;

  int bankH = 96;
  int bankW = W - margin * 2;
  if (midH > 0) {
    int maxH = midH - 16;
    if (maxH < 72) maxH = 72;
    if (bankH > maxH) bankH = maxH;
  }

  const int bankX = margin;
  const int bankY = midTop + (midH - bankH) / 2;

  for (int i = 0; i < 4; i++) {
    int x = margin + i * (cardW + gap);
    cardTop[i] = {x, topY, cardW, cardH};
    cardBot[i] = {x, botY, cardW, cardH};
  }
  bankRect = {bankX, bankY, bankW, bankH};
  layoutReady = true;
}

// -------------------- THEME --------------------
static uint16_t BG_TOP, BG_BOT;
static uint16_t TXT_MAIN;
static uint16_t SHADOW_1, SHADOW_2;

static uint16_t ACC_SW[8];
static uint16_t ACC_BANK_1, ACC_BANK_2;

static uint16_t TRANSP;

// -------------------- background --------------------
static void drawStaticBackground()
{
  vgradTFT(0, 0, tft.width(), tft.height(), BG_TOP, BG_BOT);

  for (int y = 6; y < tft.height(); y += 18) {
    int x0 = (y * 11) % 19;
    for (int x = x0; x < tft.width(); x += 29) {
      int k = (x + y) % 10;
      uint16_t c = BG_TOP;
      if (k == 0) c = blend565(ACC_SW[0], BG_TOP, 120);
      else if (k == 1) c = blend565(ACC_SW[1], BG_TOP, 120);
      else if (k == 2) c = blend565(ACC_SW[2], BG_TOP, 120);
      else if (k == 3) c = blend565(ACC_SW[3], BG_TOP, 120);
      else if (k == 4) c = blend565(ACC_SW[4], BG_TOP, 120);
      else if (k == 5) c = blend565(ACC_SW[5], BG_TOP, 120);
      else if (k == 6) c = blend565(ACC_SW[6], BG_TOP, 120);
      else if (k == 7) c = blend565(ACC_SW[7], BG_TOP, 120);
      else if (k == 8) c = blend565(ACC_BANK_1, BG_TOP, 120);
      else c = blend565(ACC_BANK_2, BG_TOP, 120);
      tft.drawPixel(x, y, c);
    }
  }

  tft.fillCircle(18, 18, 20, blend565(ACC_BANK_1, BG_TOP, 170));
  tft.fillCircle(tft.width() - 18, 18, 18, blend565(ACC_SW[0], BG_TOP, 170));
  tft.fillCircle(tft.width() - 18, tft.height() - 18, 18, blend565(ACC_SW[4], BG_BOT, 170));
  tft.fillCircle(18, tft.height() - 18, 18, blend565(ACC_SW[6], BG_BOT, 170));
}

static void drawShadowOnTFT(int x, int y, int w, int h, int radius)
{
  tft.fillRoundRect(x + 6, y + 7, w, h, radius, SHADOW_2);
  tft.fillRoundRect(x + 3, y + 4, w, h, radius, SHADOW_1);
}

// -------------------- THICK VIVID BORDER (5 layers) --------------------
static void drawThickVividBorderSPR(TFT_eSprite &g, int w, int h, int radius, uint16_t accent)
{
  uint16_t white = rgb565(255,255,255);
  uint16_t black = rgb565(0,0,0);

  // 5 ชั้น: สด -> ไฮไลต์ -> เข้ม -> ไฮไลต์ -> เข้ม
  uint16_t c0 = accent;
  uint16_t c1 = blend565(white, accent, 150);
  uint16_t c2 = blend565(black, accent, 185);
  uint16_t c3 = blend565(white, accent, 190);
  uint16_t c4 = blend565(black, accent, 205);

  g.drawRoundRect(0, 0, w, h, radius, c0);
  g.drawRoundRect(1, 1, w - 2, h - 2, radius - 1, c1);
  g.drawRoundRect(2, 2, w - 4, h - 4, radius - 2, c2);
  g.drawRoundRect(3, 3, w - 6, h - 6, radius - 3, c3);
  g.drawRoundRect(4, 4, w - 8, h - 8, radius - 4, c4);
}

static uint16_t bankAccentMix()
{
  return blend565(ACC_BANK_1, ACC_BANK_2, 128);
}

// -------------------- SWITCH CARD (static) --------------------
static void drawSwitchCardStatic(const Rect &r, uint16_t accent, const String &label)
{
  drawShadowOnTFT(r.x, r.y, r.w, r.h, R_SWITCH);

  cardSpr.fillSprite(TRANSP);

  // ✅ พื้นหลังการ์ด “ดำ”
  uint16_t topC = rgb565(0, 0, 0);
  uint16_t botC = rgb565(0, 0, 0);

  vgradRoundSPR(cardSpr, r.w, r.h, R_SWITCH, topC, botC);

  // ✅ ขอบหนาขึ้น + สดๆ
  drawThickVividBorderSPR(cardSpr, r.w, r.h, R_SWITCH, accent);

  // text
  drawCenteredFixedFont2(cardSpr, 0, 0, r.w, r.h, label, TXT_MAIN, botC);

  cardSpr.pushSprite(r.x, r.y, TRANSP);
}

// -------------------- BANK PANEL (static/typing) --------------------
static void drawBankPanel(const Rect &r, const String &textToShow)
{
  uint16_t accent = bankAccentMix();

  drawShadowOnTFT(r.x, r.y, r.w, r.h, R_BANK);

  bankSpr.fillSprite(TRANSP);

  // ✅ พื้นหลัง bank “ดำ”
  uint16_t topC = rgb565(0, 0, 0);
  uint16_t botC = rgb565(0, 0, 0);

  vgradRoundSPR(bankSpr, r.w, r.h, R_BANK, topC, botC);

  // ✅ ขอบหนาขึ้น + สดๆ
  drawThickVividBorderSPR(bankSpr, r.w, r.h, R_BANK, accent);

  // ✅ ตัวหนังสือกลาง + typewriter
  drawBankCenteredTypewriterFont4(bankSpr, r.w, r.h, textToShow, TXT_MAIN, botC);

  bankSpr.pushSprite(r.x, r.y, TRANSP);
}

// -------------------- BANK TYPEWRITER CONTROL --------------------
static void startBankTyping(const String &fullBankName)
{
  // fit ก่อน แล้วค่อย type จะไม่สั่น
  const int font = 4;
  const int padX = 22;
  const int maxPx = bankRect.w - padX * 2;

  bankSpr.setTextFont(font);
  bankTarget = fitText(bankSpr, fullBankName, maxPx, font);

  bankTypedLen = 0;
  bankTyping = true;
  bankNextStepMs = millis();

  drawBankPanel(bankRect, String("_"));
}

// -------------------- redraw logic --------------------
static void redrawStaticIfChanged(bool startTypingIfBankChanged)
{
  if (!layoutReady) computeLayout();

  String b = normName(bankName, "BANK");
  String s[8];
  for (int i = 0; i < 8; i++) s[i] = normName(swName[i], "");

  // switches
  for (int i = 0; i < 4; i++) {
    if (prevSw[i] != s[i]) {
      prevSw[i] = s[i];
      drawSwitchCardStatic(cardTop[i], ACC_SW[i], s[i]);
    }
  }
  for (int i = 0; i < 4; i++) {
    int idx = i + 4;
    if (prevSw[idx] != s[idx]) {
      prevSw[idx] = s[idx];
      drawSwitchCardStatic(cardBot[i], ACC_SW[idx], s[idx]);
    }
  }

  // bank
  if (prevBank != b) {
    prevBank = b;
    if (startTypingIfBankChanged) startBankTyping(b);
    else drawBankPanel(bankRect, b);
  }
}

static void drawAllUI()
{
  prevBank = "";
  for (int i = 0; i < 8; i++) prevSw[i] = "";

  redrawStaticIfChanged(false);
  startBankTyping(normName(bankName, "BANK"));
}

// -------------------- NVS --------------------
static void saveToNVS()
{
  prefs.begin("disp", false);
  prefs.putInt("bank_i", bankIndex);
  prefs.putString("bank_n", bankName);
  for (int i = 0; i < 8; i++) {
    String key = "sw" + String(i + 1);
    prefs.putString(key.c_str(), swName[i]);
  }
  prefs.end();
}

static void loadFromNVS()
{
  prefs.begin("disp", true);
  bankIndex = prefs.getInt("bank_i", 0);
  bankName  = prefs.getString("bank_n", "BANK");
  for (int i = 0; i < 8; i++) {
    String key = "sw" + String(i + 1);
    swName[i] = prefs.getString(key.c_str(), "SW" + String(i + 1));
  }
  prefs.end();
}

// -------------------- protocol --------------------
static bool parseUpdateLine(const String &line)
{
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

  bankIndex = parts[1].toInt();
  bankName  = parts[2];
  bankName.trim();

  for (int i = 0; i < 8; i++) {
    int pi = 3 + i;
    if (pi < count) {
      String s = parts[pi];
      s.trim();
      if (s.length() > 0) swName[i] = s;
    }
    if (swName[i].length() == 0) swName[i] = "SW" + String(i + 1);
  }

  return true;
}

void setup()
{
  Serial.begin(115200);
  delay(50);

  UART_PORT.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
  UART_PORT.setRxBufferSize(1024);
  UART_PORT.flush();

  tft.init();
  tft.setRotation(1); // landscape

  TRANSP = rgb565(255, 0, 255);

  BG_TOP = rgb565(8, 10, 28);
  BG_BOT = rgb565(38, 8, 52);

  TXT_MAIN = rgb565(252, 252, 255);

  SHADOW_1 = rgb565(0, 0, 0);
  SHADOW_2 = rgb565(0, 0, 0);

  // สดๆ
  ACC_SW[0] = rgb565(0,   255, 255);
  ACC_SW[1] = rgb565(0,   255, 120);
  ACC_SW[2] = rgb565(255, 0,   220);
  ACC_SW[3] = rgb565(255, 230, 0);
  ACC_SW[4] = rgb565(255, 120, 0);
  ACC_SW[5] = rgb565(160, 60,  255);
  ACC_SW[6] = rgb565(0,   255, 200);
  ACC_SW[7] = rgb565(255, 40,  60);

  ACC_BANK_1 = rgb565(210, 0,   255);
  ACC_BANK_2 = rgb565(0,   210, 255);

  computeLayout();

  cardSpr.setColorDepth(16);
  bankSpr.setColorDepth(16);
  cardSpr.createSprite(cardTop[0].w, cardTop[0].h);
  bankSpr.createSprite(bankRect.w, bankRect.h);

  drawStaticBackground();

  loadFromNVS();

  bankName = normName(bankName, "BANK");
  for (int i = 0; i < 8; i++) swName[i] = normName(swName[i], "");

  drawAllUI();
}

void loop()
{
  // ---------- UART handling ----------
  while (UART_PORT.available()) {
    char ch = (char)UART_PORT.read();
    if (ch == '\r') continue;

    if (ch == '\n') {
      String line = lineBuf;
      lineBuf = "";
      line.trim();
      if (line.length() == 0) continue;

      if (parseUpdateLine(line)) {
        bankName = normName(bankName, "BANK");
        for (int i = 0; i < 8; i++) swName[i] = normName(swName[i], "");

        UART_PORT.print("@A,OK\n");
        UART_PORT.flush();

        // bank เปลี่ยน -> เริ่ม typewriter
        redrawStaticIfChanged(true);

        nvsDirty = true;
        nvsSaveAtMs = millis() + NVS_DEBOUNCE_MS;
      }
    } else {
      if (lineBuf.length() < 300) lineBuf += ch;
      else lineBuf = "";
    }
  }

  // ---------- BANK typewriter tick ----------
  if (bankTyping) {
    uint32_t now = millis();
    if ((int32_t)(now - bankNextStepMs) >= 0) {
      bankNextStepMs = now + BANK_TYPE_MS;

      bankTypedLen++;
      int L = (int)bankTarget.length();

      if (bankTypedLen >= L) {
        bankTyping = false;
        drawBankPanel(bankRect, bankTarget);
      } else {
        String partial = bankTarget.substring(0, bankTypedLen);
        partial += "_";
        drawBankPanel(bankRect, partial);
      }
    }
  }

  // ---------- NVS debounce ----------
  if (nvsDirty && (int32_t)(millis() - nvsSaveAtMs) >= 0) {
    saveToNVS();
    nvsDirty = false;
  }

  yield();
}
