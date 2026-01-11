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

// state
static String bankName = "BANK";
static String swName[8] = {"SW1","SW2","SW3","SW4","SW5","SW6","SW7","SW8"};
static int bankIndex = 0;

// line buffer
static String lineBuf;

// debounce NVS save
static bool nvsDirty = false;
static uint32_t nvsSaveAtMs = 0;
static const uint32_t NVS_DEBOUNCE_MS = 800;

// -------------------- helpers --------------------
static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

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

// fit text to width with "..."
static String fitText(const String &txt, int maxPx, int font)
{
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

// centered label with safe font fallback (4 -> 2)
static void drawCenteredLabel(int x, int y, int w, int h, const String &label, uint16_t fg, uint16_t bg)
{
  const int pad = 10;
  const int maxPx = w - pad * 2;

  int font = 4;
  if (tft.textWidth(label, font) > maxPx) font = 2;

  String shown = fitText(label, maxPx, font);

  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(font);
  tft.setTextColor(fg, bg);
  tft.drawString(shown, x + w / 2, y + h / 2);
}

// minimal card: fill + thin border + accent underline
static void drawSwitchCard(int x, int y, int w, int h, uint16_t fill, uint16_t border, uint16_t accent, const String &label)
{
  tft.fillRoundRect(x, y, w, h, 12, fill);
  tft.drawRoundRect(x, y, w, h, 12, border);

  // subtle inner border
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 11, rgb565(45, 52, 64));

  // accent underline (minimal color)
  const int lineH = 5;
  tft.fillRoundRect(x + 10, y + h - 10, w - 20, lineH, 3, accent);

  drawCenteredLabel(x, y, w, h - 2, label, TFT_WHITE, fill);
}

// bank panel: main highlight + accent border + inner accent line
static void drawBankPanel(int x, int y, int w, int h, uint16_t fill, uint16_t border, uint16_t accent, const String &label)
{
  tft.fillRoundRect(x, y, w, h, 16, fill);
  tft.drawRoundRect(x, y, w, h, 16, border);

  // accent border line (outer glow style but still minimal)
  tft.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 15, accent);

  // inner accent line (thin)
  tft.drawFastHLine(x + 18, y + 12, w - 36, accent);
  tft.drawFastHLine(x + 18, y + h - 13, w - 36, accent);

  // BANK text: 4 -> 2
  const int pad = 18;
  const int maxPx = w - pad * 2;
  int font = 4;
  if (tft.textWidth(label, font) > maxPx) font = 2;

  String shown = fitText(label, maxPx, font);

  // slight text shadow (1px) ให้ดูชัดแต่ไม่รก
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(font);
  tft.setTextColor(rgb565(0, 0, 0), fill);
  tft.drawString(shown, x + w / 2 + 1, y + h / 2 + 1);

  tft.setTextColor(TFT_WHITE, fill);
  tft.drawString(shown, x + w / 2, y + h / 2);
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

// -------------------- UI --------------------
static void drawUI()
{
  const int W = tft.width();
  const int H = tft.height();

  // theme colors (balanced minimal + color)
  const uint16_t BG        = rgb565(12, 15, 20);   // soft dark
  const uint16_t CARD_BG   = rgb565(22, 27, 35);   // card
  const uint16_t CARD_BRD  = rgb565(38, 46, 60);   // border

  const uint16_t ACC_CYAN  = rgb565(70, 220, 255); // top row + bank accent
  const uint16_t ACC_AMBER = rgb565(255, 185, 90); // bottom row

  const uint16_t BANK_BG   = rgb565(16, 22, 30);
  const uint16_t BANK_BRD  = rgb565(45, 55, 70);

  tft.fillScreen(BG);

  // layout (balanced)
  const int margin = 14;
  const int gap = 10;

  const int cardW = (W - margin * 2 - gap * 3) / 4;
  const int cardH = 58;

  const int topY = 18;
  const int botY = H - 18 - cardH;

  // middle bank area fits naturally between rows
  const int midTop = topY + cardH;
  const int midBot = botY;
  const int midH = midBot - midTop;

  int bankH = 78;
  int bankW = W - margin * 2;
  if (midH > 0) {
    int maxH = midH - 18;
    if (maxH < 60) maxH = 60;
    if (bankH > maxH) bankH = maxH;
  }
  const int bankX = margin;
  const int bankY = midTop + (midH - bankH) / 2;

  // normalize display text
  String b = normName(bankName, "BANK");
  String s[8];
  for (int i = 0; i < 8; i++) s[i] = normName(swName[i], "");

  // top 4
  for (int i = 0; i < 4; i++) {
    int x = margin + i * (cardW + gap);
    drawSwitchCard(x, topY, cardW, cardH, CARD_BG, CARD_BRD, ACC_CYAN, s[i]);
  }

  // bank panel (accent cyan)
  drawBankPanel(bankX, bankY, bankW, bankH, BANK_BG, BANK_BRD, ACC_CYAN, b);

  // bottom 4
  for (int i = 0; i < 4; i++) {
    int x = margin + i * (cardW + gap);
    drawSwitchCard(x, botY, cardW, cardH, CARD_BG, CARD_BRD, ACC_AMBER, s[i + 4]);
  }
}

// -------------------- protocol --------------------
// from S3: "@U,<bank>,<bankname>,<sw1>,...,<sw8>\r\n"
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

  bankIndex = parts[1].toInt();  // เก็บไว้ แม้ไม่แสดงเลข
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

  Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  tft.init();
  tft.setRotation(1); // landscape

  loadFromNVS();

  // normalize uppercase for display
  bankName = normName(bankName, "BANK");
  for (int i = 0; i < 8; i++) swName[i] = normName(swName[i], "");

  drawUI();

  Serial.println("tft client ready ✅");
  Serial.printf("uart1 rx=gpio%d tx=gpio%d baud=%lu\n", UART_RX_PIN, UART_TX_PIN, (unsigned long)UART_BAUD);
}

void loop()
{
  while (Serial1.available()) {
    char ch = (char)Serial1.read();
    if (ch == '\r') continue;

    if (ch == '\n') {
      String line = lineBuf;
      lineBuf = "";
      line.trim();
      if (line.length() == 0) continue;

      if (parseUpdateLine(line)) {
        bankName = normName(bankName, "BANK");
        for (int i = 0; i < 8; i++) swName[i] = normName(swName[i], "");

        // ACK
        Serial1.print("@A,OK\n");
        Serial1.flush();

        drawUI();

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
