// SPDX-License-Identifier: GPL-3.0-or-later
// Fridge Tracker — прошивка для Zectrix NOTE4C
// (ESP32-S3 N16R8, 4.2" e-paper BWRY 400x300, контроллер SSD2683).
// Ориентация экрана выбирается параметром ORIENTATION ниже (вертикальная по умолчанию).
//
// Телефон по BLE присылает список продуктов с датами окончания срока и текущее время.
// Устройство хранит список во flash, само пересчитывает оставшиеся дни, рисует экран
// и спит: просыпается в полночь (пересчитать дни) или по центральной кнопке.
// По центральной кнопке включается BLE на 2 минуты.
// UP + DOWN 5 секунд (в том числе из сна) -> запрос на сброс всех данных.
//
// Протокол — см. docs/protocol.md:
//   запись в характеристику CHAR_UUID, сообщение может приходить кусками;
//   кусок, начинающийся с байта 0x02, сбрасывает буфер; перевод строки в конце — конец сообщения.
//   {"t":<unix сек>,"tz":<минуты от UTC>,"items":[{"n":"Молоко 3.2%","e":"2026-09-26"},...]}
// После обработки значение характеристики = "OK <кол-во>" или "ERR <причина>" (read/notify).
//
// Особенности железа:
//   * 4-цветный экран обновляется только целиком (>10 с). Частичное окно на SSD2683
//     не работает (перерисовывает весь экран в ч/б). Поэтому экран обновляется только
//     при новых данных, смене дня и сбросе; значок BLE по умолчанию выключен.
//   * Часы PCF8563 на плате: время переживает сброс и отключение питания,
//     а внутренний RC-генератор ESP32 во сне уходит — его поправляем по PCF8563.
//   * Защёлка питания GPIO17 удерживается во сне, иначе плата от батареи выключится.
//
// Arduino IDE: Board "ESP32S3 Dev Module", USB CDC On Boot: Enabled, PSRAM: OPI PSRAM,
//              Flash Size: 16MB, Partition Scheme: Huge APP.
// arduino-cli: FQBN esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app
// Общий код (шрифты, BiDi) — в firmware/libraries/FridgeCommon, см. README.

#include <GxEPD2_4C.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <Wire.h>
#include <sys/time.h>
#include "driver/rtc_io.h"
#include <memory>
#include <efont.h>  // firmware/libraries/FridgeCommon
#include <model.h>  // firmware/libraries/FridgeCommon
#include <fonts.h>  // firmware/libraries/FridgeCommon

// ---------- настройки ----------
#define NO_SLEEP 0            // 1 = не засыпать (удобно при разработке по USB)
#define SHOW_BLE_ICON 0       // 1 = значок BLE в шапке (2 полных обновления экрана на каждое нажатие)
const uint32_t AWAKE_MS = 120000;       // сколько ждать телефон после нажатия кнопки
const uint32_t AFTER_SYNC_MS = 15000;   // сколько ещё не спать после приёма данных
const uint32_t RESET_HOLD_MS = 5000;    // удержание UP + DOWN для сброса
const int32_t WAKE_SEC = 30;            // ежедневное пробуждение: 00:00:30 местного времени
const size_t MAX_ITEMS = 60;

// ---------- пины NOTE4C (wiki.zectrix.com/en/hardware/note4c/quick-start) ----------
#define PWR_HOLD 17           // защёлка питания платы
#define EPD_PWR 6
#define EPD_BUSY 8
#define EPD_RST 9
#define EPD_DC 10
#define EPD_CS 11
#define EPD_SCK 12
#define EPD_MOSI 13
#define LED_PIN 3
#define I2C_SDA 47
#define I2C_SCL 48
#define PCF8563_ADDR 0x51
#define BTN_OK 0              // центральная кнопка (BOOT) — будит и включает BLE
#define BTN_UP 39             // не умеет будить из глубокого сна (не RTC GPIO)
#define BTN_DOWN 18
const uint8_t BUTTONS[] = {BTN_OK, BTN_UP, BTN_DOWN};

// Отличается от CrowPanel ("Fridge-Tracker"), чтобы не путать два устройства.
#define BLE_NAME "Fridge-Tracker-Color"
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"

// Драйвер панели напрямую (без GxEPD2_4C) и свой буфер кадра: так проще поворачивать
// картинку и снимать скриншот по Serial.
GxEPD2_420c_GDEY0420F51 epd(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY);

// ---------- ориентация ----------
// 0 — горизонтально (400x300, родная для панели);
// 1 — вертикально, поворот на 90° по часовой; 3 — вертикально, против часовой.
// Если вертикальная картинка вверх ногами относительно того, как висит устройство, — 1 <-> 3.
// Можно задать и при сборке: --build-property "compiler.cpp.extra_flags=-DORIENTATION=0"
#ifndef ORIENTATION
#define ORIENTATION 1
#endif
const int NW = 400, NH = 300;     // родной размер панели
#if ORIENTATION == 0
const int W = 400, H = 300;       // логический размер — под него раскладка
#else
const int W = 300, H = 400;
#endif
const int HEADER_H = 28;
const uint16_t INK = GxEPD_BLACK, PAPER = GxEPD_WHITE;
const uint16_t RED = GxEPD_RED, YELLOW = GxEPD_YELLOW;

// Кадр в формате контроллера: 2 бита на точку (0 чёрный, 1 белый, 2 жёлтый, 3 красный).
class Canvas4C : public Adafruit_GFX {
 public:
  uint8_t buf[NW / 4 * NH];
  Canvas4C() : Adafruit_GFX(W, H) {}
  static uint8_t code(uint16_t c) {
    return c == GxEPD_BLACK ? 0 : c == GxEPD_RED ? 3 : c == GxEPD_YELLOW ? 2 : 1;
  }
  // Логические (x, y) -> родные координаты панели.
  static void toNative(int x, int y, int& nx, int& ny) {
#if ORIENTATION == 0
    nx = x; ny = y;
#elif ORIENTATION == 1
    nx = NW - 1 - y; ny = x;
#else
    nx = y; ny = NH - 1 - x;
#endif
  }
  void drawPixel(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || x >= W || y < 0 || y >= H) return;
    int nx, ny;
    toNative(x, y, nx, ny);
    uint32_t i = nx / 4 + (uint32_t)ny * (NW / 4);
    uint8_t sh = 6 - 2 * (nx % 4);
    buf[i] = (buf[i] & ~(3 << sh)) | (code(color) << sh);
  }
  void fillScreen(uint16_t color) override {
    uint8_t c = code(color);
    memset(buf, c | c << 2 | c << 4 | c << 6, sizeof buf);
  }
  uint8_t get(int x, int y) const {  // логические координаты
    int nx, ny;
    toNative(x, y, nx, ny);
    return (buf[nx / 4 + ny * (NW / 4)] >> (6 - 2 * (nx % 4))) & 3;
  }
};
Canvas4C canvas;
#define G canvas

Preferences prefs;

// ---------- данные ----------
std::vector<Item> items;
int32_t tzMin = 0;           // смещение локального времени от UTC, минуты
// RTC-память переживает глубокий сон, но не Reset/отключение питания.
RTC_DATA_ATTR int32_t lastRenderedDay = -1;
RTC_DATA_ATTR bool clockSynced = false;   // время получено от телефона (или из PCF8563)

// ---------- время ----------
int32_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int32_t)doe - 719468;
}

bool clockValid() { return time(nullptr) > 1700000000; }
time_t localNow() { return time(nullptr) + (time_t)tzMin * 60; }
int32_t todayDay() { return (int32_t)(localNow() / 86400); }

int32_t parseDate(const char* s) {  // "YYYY-MM-DD"
  int y, m, d;
  if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3) return INT32_MIN;
  return daysFromCivil(y, m, d);
}

void setClock(time_t t) {
  struct timeval tv = {t, 0};
  settimeofday(&tv, nullptr);
}

// ---------- часы PCF8563 (хранят UTC) ----------
uint8_t bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool rtcRead(time_t& out) {
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(0x02);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(PCF8563_ADDR, 7) != 7) return false;
  uint8_t r[7];
  for (int i = 0; i < 7; i++) r[i] = Wire.read();
  if (r[0] & 0x80) return false;  // VL: питание часов пропадало, время недостоверно
  int sec = bcd2bin(r[0] & 0x7F), min = bcd2bin(r[1] & 0x7F), hour = bcd2bin(r[2] & 0x3F);
  int day = bcd2bin(r[3] & 0x3F), mon = bcd2bin(r[5] & 0x1F), year = 2000 + bcd2bin(r[6]);
  if (mon < 1 || mon > 12 || day < 1 || day > 31) return false;
  out = (time_t)daysFromCivil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
  return out > 1700000000;
}

void rtcWrite(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(0x00);
  Wire.write(0x00);  // control 1: часы идут
  Wire.write(0x00);  // control 2: прерывания выключены
  Wire.write(bin2bcd(tm.tm_sec));  // VL = 0
  Wire.write(bin2bcd(tm.tm_min));
  Wire.write(bin2bcd(tm.tm_hour));
  Wire.write(bin2bcd(tm.tm_mday));
  Wire.write(tm.tm_wday);
  Wire.write(bin2bcd(tm.tm_mon + 1));
  Wire.write(bin2bcd(tm.tm_year % 100));
  bool ok = Wire.endTransmission() == 0;
  Wire.beginTransmission(PCF8563_ADDR);
  Wire.write(0x0D);
  Wire.write(0x00);  // CLKOUT выключен — меньше потребление
  Wire.endTransmission();
  Serial.printf("PCF8563: запись времени %s\n", ok ? "OK" : "ОШИБКА");
}

// ---------- хранение ----------
bool loadJson(const char* json, size_t len, String& err, bool applyClock) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json, len);
  if (e) { err = e.c_str(); return false; }
  if (!doc["items"].is<JsonArray>()) { err = "no items"; return false; }
  if (applyClock && doc["t"].is<long long>()) {
    setClock((time_t)doc["t"].as<long long>());
    clockSynced = true;
    rtcWrite(time(nullptr));
  }
  if (applyClock && doc["tz"].is<int>()) tzMin = doc["tz"];
  items.clear();
  for (JsonObject o : doc["items"].as<JsonArray>()) {
    if (items.size() >= MAX_ITEMS) break;
    int32_t d = parseDate(o["e"] | "");
    if (d == INT32_MIN) continue;
    items.push_back({String((const char*)(o["n"] | "?")), d});
  }
  std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.expDay < b.expDay; });
  return true;
}

void saveState(const String& json) {
  prefs.putBytes("json", json.c_str(), json.length());
  prefs.putInt("tz", tzMin);
  prefs.putLong64("lastT", (int64_t)time(nullptr));
}

void restoreState() {
  tzMin = prefs.getInt("tz", 0);
  // PCF8563 точнее внутреннего генератора во сне — берём время из него, если оно достоверно.
  time_t t;
  if (rtcRead(t)) {
    setClock(t);
    clockSynced = true;
  } else if (!clockValid()) {  // часов нет — последнее известное время, дням верить нельзя
    int64_t last = prefs.getLong64("lastT", 0);
    if (last > 1700000000) setClock((time_t)last);
  }
  size_t len = prefs.getBytesLength("json");
  if (!len) return;
  std::unique_ptr<char[]> buf(new char[len + 1]);
  prefs.getBytes("json", buf.get(), len);
  buf[len] = 0;
  String err;
  loadJson(buf.get(), len, err, false);  // время в сохранённом JSON устарело
}

// ---------- рисование ----------
const char* ruDays(int n) {
  n = abs(n) % 100;
  int n1 = n % 10;
  if (n > 10 && n < 20) return "дней";
  if (n1 == 1) return "день";
  if (n1 >= 2 && n1 <= 4) return "дня";
  return "дней";
}

void thickRoundRect(int x, int y, int w, int h, int r, int t) {
  for (int i = 0; i < t; i++) G.drawRoundRect(x + i, y + i, w - 2 * i, h - 2 * i, r - i, INK);
}

void drawSync(int x, int y) {  // круговая стрелка 12x12
  G.drawCircle(x + 6, y + 6, 5, INK);
  G.drawCircle(x + 6, y + 6, 4, INK);
  G.fillRect(x + 7, y, 6, 6, PAPER);
  G.fillTriangle(x + 6, y - 2, x + 11, y + 2, x + 6, y + 6, INK);
}

void drawBle(int x, int y) {  // значок Bluetooth 9x14
  G.drawLine(x + 4, y, x + 4, y + 13, INK);
  G.drawLine(x + 4, y, x + 8, y + 3, INK);
  G.drawLine(x + 8, y + 3, x, y + 10, INK);
  G.drawLine(x + 4, y + 13, x + 8, y + 10, INK);
  G.drawLine(x + 8, y + 10, x, y + 3, INK);
}

const int MARGIN = 6;
const int HDR_H = 30;         // заголовок секции
const int ROW_URGENT = 46;
const int ROW_NORMAL = 30;
const int MORE_H = 22;
const int GAP = 7;

void drawHeader(bool bleActive) {
  static const char* WD[] = {"ВОСКРЕСЕНЬЕ", "ПОНЕДЕЛЬНИК", "ВТОРНИК", "СРЕДА", "ЧЕТВЕРГ", "ПЯТНИЦА", "СУББОТА"};
  static const char* MON[] = {"ЯНВ", "ФЕВ", "МАР", "АПР", "МАЙ", "ИЮН", "ИЮЛ", "АВГ", "СЕН", "ОКТ", "НОЯ", "ДЕК"};
  int right = W - MARGIN - 2;
  if (bleActive) { drawBle(right - 9, 5); right -= 16; }
  if (clockValid() && clockSynced) {
    char buf[40];
    time_t t = localNow();
    struct tm tm; gmtime_r(&t, &tm);
    snprintf(buf, sizeof buf, "%s, %d %s", WD[tm.tm_wday], tm.tm_mday, MON[tm.tm_mon]);
    efDrawRight(G, right, 18, F_SMALL, buf, INK);
  } else {
    drawSync(MARGIN + 2, 6);
    efDraw(G, MARGIN + 19, 18, F_SMALL, "НУЖНА СИНХРОНИЗАЦИЯ", RED, right - MARGIN - 23);
    G.drawFastHLine(MARGIN, 25, W - 2 * MARGIN, INK);
    return;
  }
  efDraw(G, MARGIN + 2, 18, F_SMALL, "МОЙ ХОЛОДИЛЬНИК", INK);
  G.drawFastHLine(MARGIN, 25, W - 2 * MARGIN, INK);
}

// Срочное — красная плашка, ближайшие дни — жёлтая, «позже» — без заливки.
void drawSectionHeader(int x, int y, int w, const Section& s, int idx) {
  if (idx == 0) {
    G.fillRoundRect(x, y, w, HDR_H + 8, 8, RED);
    G.fillRect(x, y + HDR_H, w, 8, PAPER);  // срезать нижние скругления
    G.fillCircle(x + 13, y + HDR_H / 2 + 1, 5, PAPER);
    efDraw(G, x + 24, y + 23, F_HEAD, s.title, PAPER);
  } else if (idx == 1) {
    G.fillRoundRect(x, y, w, HDR_H + 8, 8, YELLOW);
    G.fillRect(x, y + HDR_H, w, 8, PAPER);
    G.fillCircle(x + 13, y + HDR_H / 2 + 1, 5, INK);
    efDraw(G, x + 24, y + 23, F_HEAD, s.title, INK);
    G.drawFastHLine(x, y + HDR_H, w, INK);
  } else {
    G.fillCircle(x + 13, y + HDR_H / 2 + 1, 5, INK);
    efDraw(G, x + 24, y + 23, F_HEAD, s.title, INK);
    G.drawFastHLine(x, y + HDR_H, w, INK);
  }
}

void drawUrgentRow(int x, int y, int w, const Item& it, int32_t days) {
  G.fillRoundRect(x + 11, y + 9, 6, 20, 2, RED);  // «!»
  G.fillCircle(x + 13, y + 35, 3, RED);
  G.fillRect(x + 11, y + 34, 6, 2, RED);
  const int colX = x + 28, colW = 46;
  char num[8];
  snprintf(num, sizeof num, "%d", abs((int)days));
  const char* unit = days == 0 ? "сегодня" : ruDays(days);
  uint16_t fg = INK;
  if (days < 0) {  // просрочено: красная плашка
    G.fillRoundRect(colX, y + 4, colW, 38, 6, RED);
    fg = PAPER;
  }
  efDraw(G, colX + (colW - efWidth(F_NUM, num)) / 2, y + 26, F_NUM, num, fg);
  efDraw(G, colX + (colW - efWidth(F_UNIT, unit)) / 2, y + 39, F_UNIT, unit, fg);

  int nx = colX + colW + 6;
  int nameY = days < 0 ? y + 25 : y + 30;
  nx += efDraw(G, nx, nameY, F_ITEM, "- ", INK);
  int nameW = efDraw(G, nx, nameY, F_ITEM, it.name.c_str(), INK, x + w - 8 - nx);
  if (days < 0) {
    G.drawFastHLine(nx, nameY - 6, nameW, INK);  // зачёркнуто
    efDraw(G, nx, y + 41, F_UNIT, "просрочено", RED);
  }
}

// Компактная срочная строка (высота как у обычной): красная точка, срок красным.
void drawUrgentCompact(int x, int y, int w, const Item& it, int32_t days) {
  const int base = y + 22;
  G.fillCircle(x + 13, y + ROW_NORMAL / 2, 5, RED);
  int nx = x + 24;
  if (days < 0) {
    nx += efDraw(G, nx, base, F_UNIT, "просрочено", RED);
  } else if (days == 0) {
    nx += efDraw(G, nx, base, F_UNIT, "сегодня", RED);
  } else {
    char num[8];
    snprintf(num, sizeof num, "%d", (int)days);
    nx += efDraw(G, nx, base + 1, F_NUM, num, RED) + 3;
    nx += efDraw(G, nx, base, F_UNIT, ruDays(days), RED);
  }
  nx += efDraw(G, nx, base, F_ITEM, " - ", INK);
  int nameW = efDraw(G, nx, base, F_ITEM, it.name.c_str(), INK, x + w - 8 - nx);
  if (days < 0) G.drawFastHLine(nx, base - 6, nameW, INK);  // зачёркнуто
}

void drawNormalRow(int x, int y, int w, const Item& it, int32_t days) {
  const int base = y + 22;
  G.drawCircle(x + 13, y + ROW_NORMAL / 2, 5, INK);
  char num[8];
  snprintf(num, sizeof num, "%d", (int)days);
  int nx = x + 24;
  nx += efDraw(G, nx, base + 1, F_NUM, num, INK) + 3;
  nx += efDraw(G, nx, base, F_UNIT, ruDays(days), INK);
  nx += efDraw(G, nx, base, F_ITEM, " - ", INK);
  efDraw(G, nx, base, F_ITEM, it.name.c_str(), INK, x + w - 8 - nx);
}

void drawEmpty() {
  const char* l1 = "Холодильник пуст";
  const char* l2 = "Нажмите центральную кнопку";
  const char* l3 = "и отправьте список с телефона";
  efDraw(G, (W - efWidth(F_HEAD, l1)) / 2, H / 2 - 10, F_HEAD, l1, INK);
  efDraw(G, (W - efWidth(F_SMALL, l2)) / 2, H / 2 + 22, F_SMALL, l2, INK);
  efDraw(G, (W - efWidth(F_SMALL, l3)) / 2, H / 2 + 42, F_SMALL, l3, INK);
}

void renderScreen(bool bleActive) {
  G.fillScreen(PAPER);
  drawHeader(bleActive);
  if (items.empty()) { drawEmpty(); return; }

  int32_t today = todayDay();
  Section secs[3] = {{"СЕГОДНЯ / ЗАВТРА", true, 0, 0}, {"В БЛИЖАЙШИЕ ДНИ", false, 0, 0}, {"ПОЗЖЕ", false, 0, 0}};
  for (size_t i = 0; i < items.size(); i++) {
    int32_t d = items[i].expDay - today;
    int k = d <= 1 ? 0 : (d <= 7 ? 1 : 2);
    if (!secs[k].count) secs[k].from = i;
    secs[k].count++;
  }

  // Распределение места.
  // Красная зона показывается целиком: смотреть список больше негде.
  // Её вид выбирается по количеству: крупные строки -> компактные -> компактные в 2 колонки.
  // Остальным секциям достаётся оставшееся место (строки убираются у самой длинной,
  // в крайнем случае остаётся только заголовок с «+ ещё N»).
  const int x = MARGIN, w = W - 2 * MARGIN;
  int y = 32;
  const int avail = H - MARGIN - y;
  int fits[3] = {secs[0].count, secs[1].count, secs[2].count};
  bool shown[3] = {secs[0].count > 0, secs[1].count > 0, secs[2].count > 0};

  // Вид красной зоны: 0 = крупно, 1 = компактно, 2 = компактно в 2 колонки.
  int urgentMode = 0;
  auto urgentRows = [&]() { return urgentMode == 2 ? (fits[0] + 1) / 2 : fits[0]; };
  auto boxHeight = [&](int i) {
    if (i == 0) {
      int rowH = urgentMode == 0 ? ROW_URGENT : ROW_NORMAL;
      return HDR_H + urgentRows() * rowH + (fits[0] < secs[0].count ? MORE_H : 0) + 4;
    }
    return HDR_H + fits[i] * ROW_NORMAL + (fits[i] < secs[i].count ? MORE_H : 0) + 4;
  };
  auto total = [&]() {
    int t = 0, n = 0;
    for (int i = 0; i < 3; i++) if (shown[i]) { t += boxHeight(i); n++; }
    return t + (n > 1 ? (n - 1) * GAP : 0);
  };
  // Минимум для остальных секций: заголовок + «+ ещё N».
  auto minOthers = [&]() {
    int t = 0;
    for (int i = 1; i < 3; i++) if (secs[i].count) t += HDR_H + MORE_H + 4 + GAP;
    return t;
  };
  if (secs[0].count) {
    for (urgentMode = 0; urgentMode < 2; urgentMode++)
      if (boxHeight(0) + minOthers() <= avail) break;
    // Совсем много срочного: красная зона забирает всё место, остальное скрываем.
    if (boxHeight(0) + minOthers() > avail) {
      shown[1] = shown[2] = false;
      while (boxHeight(0) > avail && fits[0] > 0) fits[0]--;
    }
  }
  while (total() > avail) {
    int pick = -1;
    for (int i = 1; i < 3; i++)
      if (shown[i] && fits[i] > 0 && (pick < 0 || fits[i] >= fits[pick])) pick = i;
    if (pick >= 0) { fits[pick]--; continue; }
    // Даже заголовки не помещаются — скрываем секции с конца.
    if (shown[2]) shown[2] = false; else if (shown[1]) shown[1] = false; else break;
  }

  for (int si = 0; si < 3; si++) {
    const Section& s = secs[si];
    if (!shown[si]) continue;
    int fit = fits[si];
    bool more = fit < s.count;
    int boxH = boxHeight(si);

    G.fillRect(x, y, w, boxH, PAPER);
    drawSectionHeader(x, y, w, s, si);
    int ry = y + HDR_H + 2;
    if (si == 0 && urgentMode == 2) {
      // Две колонки, заполняются сверху вниз: сначала левая, потом правая.
      const int rows = urgentRows(), cw = w / 2;
      for (int r = 0; r < fit; r++) {
        int col = r / rows, row = r % rows;
        const Item& it = items[s.from + r];
        drawUrgentCompact(x + col * cw, ry + row * ROW_NORMAL, cw, it, it.expDay - today);
      }
      G.drawFastVLine(x + cw, ry + 2, rows * ROW_NORMAL - 4, INK);
      ry += rows * ROW_NORMAL;
    } else {
      const int rowH = (si == 0 && urgentMode == 0) ? ROW_URGENT : ROW_NORMAL;
      for (int r = 0; r < fit; r++) {
        const Item& it = items[s.from + r];
        int32_t d = it.expDay - today;
        if (si == 0) {
          if (urgentMode == 0) drawUrgentRow(x, ry, w, it, d); else drawUrgentCompact(x, ry, w, it, d);
        } else {
          drawNormalRow(x, ry, w, it, d);
        }
        ry += rowH;
        if (r < fit - 1 || more)
          for (int xx = x + 10; xx < x + w - 10; xx += 3) G.drawPixel(xx, ry - 1, INK);  // пунктир
      }
    }
    if (more) {
      char buf[32];
      snprintf(buf, sizeof buf, "+ ещё %d", s.count - fit);
      efDraw(G, x + 24, ry + 16, F_SMALL, buf, INK);
    }
    thickRoundRect(x, y, w, boxH, 8, 2);
    y += boxH + GAP;
  }
}

// ---------- экран ----------
bool displayReady = false;

void displayInit() {
  if (displayReady) return;
  gpio_hold_dis((gpio_num_t)EPD_PWR);
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  delay(20);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  epd.selectSPI(SPI, SPISettings(4000000, MSBFIRST, SPI_MODE0));
  epd.init(115200, true, 2, false);
  displayReady = true;
}

// Полное обновление всего экрана (>10 с).
void pushFull() {
  displayInit();
  uint32_t t = millis();
  epd.writeNative(canvas.buf, nullptr, 0, 0, NW, NH);
  epd.refresh(false);
  epd.powerOff();
  Serial.printf("refresh full %lu ms\n", millis() - t);
}

// Обновление шапки. Частичное окно (epd.refresh(x,y,w,h)) на SSD2683 НЕ работает:
// контроллер обновляет весь экран, причём в чёрно-белом режиме (жёлтое -> чёрное,
// красное пропадает). Поэтому — только полное обновление.
void pushHeader() {
  pushFull();
}

void showScreen(bool bleActive) {
  renderScreen(bleActive);
  pushFull();
  lastRenderedDay = todayDay();
}

// Только значок BLE в шапке. Остальной кадр рисуется заново тем же кодом —
// он совпадает с тем, что уже на экране (день тот же, данные те же).
void showHeader(bool bleActive) {
  if (todayDay() != lastRenderedDay) { showScreen(bleActive); return; }
  renderScreen(bleActive);
  pushHeader();
}

// Снимок экрана в Serial без обновления панели: по символу на пиксель
// (0 чёрный, 1 белый, 2 красный, 3 жёлтый). См. tools/shot4c.py.
void dumpShot() {
  renderScreen(false);
  static const char CH[4] = {'0', '1', '3', '2'};  // код контроллера -> символ снимка
  Serial.printf("\nSHOT4C_BEGIN %d %d\n", W, H);
  char line[W + 1];
  for (int y = 0; y < H; y++) {
    for (int x = 0; x < W; x++) line[x] = CH[canvas.get(x, y)];
    line[W] = 0;
    Serial.println(line);
  }
  Serial.println("SHOT4C_END");
}

// ---------- BLE ----------
BLECharacteristic* bleChar = nullptr;
String rxBuf;
volatile bool rxReady = false;
volatile bool bleConnected = false;
portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;

class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override { bleConnected = true; Serial.println("BLE: подключен"); }
  void onDisconnect(BLEServer* s) override {
    bleConnected = false;
    Serial.println("BLE: отключен");
    s->getAdvertising()->start();
  }
};

class CharCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    if (!v.length()) return;
    portENTER_CRITICAL(&rxMux);
    if (v[0] == '\x02') { rxBuf = ""; v.remove(0, 1); }
    if (rxBuf.length() + v.length() < 16384) rxBuf += v;
    if (rxBuf.endsWith("\n")) rxReady = true;
    portEXIT_CRITICAL(&rxMux);
  }
};

void bleStart() {
  BLEDevice::init(BLE_NAME);
  BLEDevice::setMTU(517);
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());
  BLEService* svc = server->createService(SERVICE_UUID);
  bleChar = svc->createCharacteristic(CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  bleChar->addDescriptor(new BLE2902());
  bleChar->setCallbacks(new CharCb());
  bleChar->setValue("READY");
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  adv->start();
  digitalWrite(LED_PIN, HIGH);
  Serial.println("BLE: реклама запущена");
}

void bleReply(const String& s) {
  if (!bleChar) return;
  bleChar->setValue(s.c_str());
  if (bleConnected) bleChar->notify();
}

// Возвращает true, если данные приняты.
bool handleMessage(String msg) {
  msg.trim();
  String err;
  if (!loadJson(msg.c_str(), msg.length(), err, true)) {
    Serial.println("JSON ошибка: " + err);
    bleReply("ERR " + err);
    return false;
  }
  saveState(msg);
  Serial.printf("Принято %d продуктов\n", items.size());
  bleReply("OK " + String(items.size()));
  return true;
}

// ---------- сон ----------
void goToSleep() {
  if (displayReady) epd.hibernate();
  digitalWrite(LED_PIN, LOW);

  // Питание экрана выключено, защёлка питания платы включена — и так во всё время сна.
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, LOW);
  gpio_hold_en((gpio_num_t)EPD_PWR);
  digitalWrite(PWR_HOLD, HIGH);
  gpio_hold_en((gpio_num_t)PWR_HOLD);
  gpio_deep_sleep_hold_en();

  // Будят центральная кнопка (GPIO0, BLE) и DOWN (GPIO18, только для сброса UP+DOWN).
  // GPIO39 (UP) будить не умеет — не RTC GPIO.
  for (uint8_t p : {BTN_OK, BTN_DOWN}) {
    rtc_gpio_pullup_en((gpio_num_t)p);
    rtc_gpio_pulldown_dis((gpio_num_t)p);
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // подтяжки кнопок во сне
  esp_sleep_enable_ext1_wakeup((1ULL << BTN_OK) | (1ULL << BTN_DOWN), ESP_EXT1_WAKEUP_ANY_LOW);

  uint64_t secs = 24ULL * 3600;
  if (clockValid()) {
    // Если часы ушли и мы проснулись до полуночи, дата не сменится —
    // тогда просто доспим до 00:00:30.
    int32_t sod = (int32_t)(localNow() % 86400);
    int32_t d = WAKE_SEC - sod;
    if (d < 120) d += 86400;
    secs = d;
  }
  esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  Serial.printf("Сон на %llu с\n", secs);
  Serial.flush();
  esp_deep_sleep_start();
}

bool anyButtonDown() {
  for (uint8_t p : BUTTONS) if (digitalRead(p) == LOW) return true;
  return false;
}

void waitButtonsReleased() {
  while (anyButtonDown()) delay(20);
  delay(50);
}

// ---------- сброс данных ----------
void renderResetPrompt() {
  G.fillScreen(PAPER);
  drawHeader(false);
  const int x = W > H ? MARGIN + 30 : MARGIN, w = W - 2 * x, h = 200, y = (H - h) / 2;
  thickRoundRect(x, y, w, h, 8, 2);
  auto center = [&](int yy, const EFont& f, const char* t, uint16_t c) { efDraw(G, (W - efWidth(f, t)) / 2, yy, f, t, c); };
  center(y + 40, F_HEAD, "Сбросить все данные?", RED);
  center(y + 70, F_SMALL, "Список продуктов и время", INK);
  center(y + 90, F_SMALL, "будут удалены.", INK);
  G.drawFastHLine(x + 20, y + 112, w - 40, INK);
  center(y + 145, F_SMALL, "Центральная кнопка — сбросить", INK);
  center(y + 170, F_SMALL, "Другая кнопка — отмена", INK);
}

void factoryResetFlow(bool bleActive) {
  Serial.println("Запрос сброса");
  renderResetPrompt();
  pushFull();
  waitButtonsReleased();
  int choice = 0;  // 1 = сбросить, -1 = отмена
  for (uint32_t t0 = millis(); millis() - t0 < 30000 && !choice;) {
    if (digitalRead(BTN_OK) == LOW) choice = 1;
    else if (anyButtonDown()) choice = -1;
    delay(20);
  }
  waitButtonsReleased();
  if (choice == 1) {
    prefs.clear();
    items.clear();
    tzMin = 0;
    clockSynced = false;
    setClock(0);
    lastRenderedDay = -1;
    Serial.println("Данные сброшены");
  }
  showScreen(bleActive);  // сброс или возврат к прежнему экрану
}

// ---------- serial-команды для отладки ----------
// J <json>  — загрузить данные как по BLE;  S — снимок экрана в Serial (без обновления панели)
String serialLine;
void pollSerial(bool& dirty) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') { serialLine += c; continue; }
    serialLine.trim();
    if (serialLine.startsWith("J ")) { if (handleMessage(serialLine.substring(2))) dirty = true; }
    else if (serialLine == "S") dumpShot();
    serialLine = "";
  }
}

// ---------- main ----------
void setup() {
  // Первым делом удерживаем питание платы (важно при работе от батареи).
  pinMode(PWR_HOLD, OUTPUT);
  digitalWrite(PWR_HOLD, HIGH);
  gpio_hold_dis((gpio_num_t)PWR_HOLD);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  Serial.begin(115200);
  Serial.setRxBufferSize(16384);
  rtc_gpio_deinit((gpio_num_t)BTN_OK);
  rtc_gpio_deinit((gpio_num_t)BTN_DOWN);
  for (uint8_t p : BUTTONS) pinMode(p, INPUT_PULLUP);
  Wire.begin(I2C_SDA, I2C_SCL);
  prefs.begin("fridge", false);
  restoreState();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("\nFridge-Tracker NOTE4C: пробуждение=%d, продуктов=%d, часы=%s\n",
                cause, items.size(), clockSynced ? "синхр." : (clockValid() ? "примерно" : "нет"));

  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    // Полуночное пробуждение: перерисовать, только если сменился день.
    if (todayDay() != lastRenderedDay) showScreen(false);
    goToSleep();
  }

  // Разбудила боковая кнопка DOWN (а не центральная): ждём UP + DOWN 5 секунд для сброса.
  // BLE не включаем; отпустили раньше — снова спать.
  if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t pins = esp_sleep_get_ext1_wakeup_status();
    if ((pins & (1ULL << BTN_DOWN)) && !(pins & (1ULL << BTN_OK))) {
      Serial.println("Разбудила DOWN: ожидание UP + DOWN для сброса");
      uint32_t since = 0;
      for (uint32_t t0 = millis(); millis() - t0 < RESET_HOLD_MS + 3000;) {
        bool combo = digitalRead(BTN_UP) == LOW && digitalRead(BTN_DOWN) == LOW;
        if (!combo) since = 0;
        else if (!since) since = millis();
        else if (millis() - since > RESET_HOLD_MS) { factoryResetFlow(false); break; }
        if (!anyButtonDown() && millis() - t0 > 1500) break;  // кнопки отпущены
        delay(20);
      }
      goToSleep();
    }
  }
}

void loop() {
  static bool interactive = false;
  static uint32_t awakeUntil = 0;
  static uint32_t comboSince = 0;
  static bool dirty = false;

  if (!interactive) {
    interactive = true;
    bleStart();
    // После включения/Reset (не после сна) экран рисуем всегда — состояние могло смениться.
    // После нажатия кнопки — только если нужен значок BLE или сменился день.
    bool afterSleep = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1;
    if (!afterSleep) showScreen(SHOW_BLE_ICON);
    else if (SHOW_BLE_ICON) showHeader(true);           // только полоса шапки
    else if (todayDay() != lastRenderedDay) showScreen(false);
    awakeUntil = millis() + AWAKE_MS;
  }

  bool rx = false;
  String msg;
  portENTER_CRITICAL(&rxMux);
  if (rxReady) { msg = rxBuf; rxBuf = ""; rxReady = false; rx = true; }
  portEXIT_CRITICAL(&rxMux);
  if (rx && handleMessage(msg)) dirty = true;
  pollSerial(dirty);

  if (dirty) {
    dirty = false;
    showScreen(SHOW_BLE_ICON);
    awakeUntil = millis() + AFTER_SYNC_MS;
  }

  bool combo = digitalRead(BTN_UP) == LOW && digitalRead(BTN_DOWN) == LOW;
  if (!combo) comboSince = 0;
  else if (!comboSince) comboSince = millis();
  else if (millis() - comboSince > RESET_HOLD_MS) {
    comboSince = 0;
    factoryResetFlow(SHOW_BLE_ICON);
    awakeUntil = millis() + AWAKE_MS;
  }
  if (anyButtonDown()) awakeUntil = max(awakeUntil, millis() + 30000);

  if (!NO_SLEEP && (int32_t)(millis() - awakeUntil) > 0 && !bleConnected) {
    if (SHOW_BLE_ICON) showHeader(false);  // убрать значок BLE (только полоса шапки)
    goToSleep();
  }
  delay(20);
}
