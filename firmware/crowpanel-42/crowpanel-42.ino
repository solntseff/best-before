// SPDX-License-Identifier: GPL-3.0-or-later
// Fridge-Tracker для Elecrow CrowPanel ESP32-S3 4.2" E-paper (v1.2, UC8276C).
//
// Телефон по BLE присылает список продуктов с датами окончания срока и текущее время.
// Устройство хранит список во flash, само пересчитывает оставшиеся дни, рисует экран
// и спит: просыпается в полночь (пересчитать дни) или по любой кнопке.
// По кнопке включается BLE на 2 минуты, в шапке виден значок Bluetooth.
// EXIT + MENU 5 секунд -> сброс всех данных (для передачи устройства другому человеку).
//
// Протокол (характеристика CHAR_UUID, запись): JSON, может приходить кусками.
//   Кусок, начинающийся с \x02, сбрасывает буфер; '\n' в конце означает конец сообщения.
//   {"t":<unix сек>,"tz":<минуты от UTC>,"items":[{"n":"Молоко 3.2%","e":"2026-09-26"},...]}
// После обработки значение характеристики = "OK <кол-во>" или "ERR <причина>" (read/notify).

#include <GxEPD2_BW.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <sys/time.h>
#include "driver/rtc_io.h"
#include "esp_rom_crc.h"
#include <memory>
#include <efont.h>  // firmware/libraries/FridgeCommon
#include <model.h>  // firmware/libraries/FridgeCommon
#include <fonts.h>  // firmware/libraries/FridgeCommon

// ---------- настройки ----------
#define NO_SLEEP 0            // 1 = не засыпать (удобно при разработке по USB)
const uint32_t AWAKE_MS = 120000;       // сколько ждать телефон после нажатия кнопки
const uint32_t AFTER_SYNC_MS = 15000;   // сколько ещё не спать после приёма данных
const uint32_t RESET_HOLD_MS = 5000;    // удержание EXIT + MENU для сброса
const int32_t WAKE_SEC = 30;            // ежедневное пробуждение: 00:00:30 местного времени
const size_t MAX_ITEMS = 60;

// ---------- пины ----------
#define EPD_PWR 7
#define EPD_SCK 12
#define EPD_MOSI 11
#define EPD_CS 45
#define EPD_DC 46
#define EPD_RST 47
#define EPD_BUSY 48
#define BTN_EXIT 1
#define BTN_MENU 2
#define BTN_DOWN 4
#define BTN_OK 5
#define BTN_UP 6
const uint8_t BUTTONS[] = {BTN_EXIT, BTN_MENU, BTN_DOWN, BTN_OK, BTN_UP};

#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a8"

GxEPD2_BW<GxEPD2_420_SE0420NQ04, GxEPD2_420_SE0420NQ04::HEIGHT> display(
  GxEPD2_420_SE0420NQ04(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

const int W = 300, H = 400;       // портретная ориентация
GFXcanvas1 canvas(W, H);          // 1 = краска, 0 = бумага
const uint16_t INK = 1, PAPER = 0;

Preferences prefs;

// ---------- данные ----------
std::vector<Item> items;
int32_t tzMin = 0;           // смещение локального времени от UTC, минуты
// RTC-память переживает глубокий сон, но не Reset/отключение питания.
RTC_DATA_ATTR int32_t lastRenderedDay = -1;
RTC_DATA_ATTR bool clockSynced = false;   // время получено от телефона после последнего сброса
RTC_DATA_ATTR uint32_t savedFrameCrc = 0;

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

// ---------- хранение ----------
bool loadJson(const char* json, size_t len, String& err, bool applyClock) {
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, json, len);
  if (e) { err = e.c_str(); return false; }
  if (applyClock && doc["t"].is<long long>()) {
    struct timeval tv = {(time_t)doc["t"].as<long long>(), 0};
    settimeofday(&tv, nullptr);
    clockSynced = true;
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
  if (!clockValid()) {  // после сброса питания RTC пуст — берём последнее известное время
    int64_t t = prefs.getLong64("lastT", 0);
    if (t > 1700000000) { struct timeval tv = {(time_t)t, 0}; settimeofday(&tv, nullptr); }
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

void dither(int x, int y, int w, int h) {  // светло-серый фон (1 точка из 4)
  for (int yy = y; yy < y + h; yy += 2)
    for (int xx = x + ((yy / 2) & 1); xx < x + w; xx += 2)
      if (((xx + yy) & 3) == 0) canvas.drawPixel(xx, yy, INK);
}

void thickRoundRect(int x, int y, int w, int h, int r, int t) {
  for (int i = 0; i < t; i++) canvas.drawRoundRect(x + i, y + i, w - 2 * i, h - 2 * i, r - i, INK);
}

void drawSync(int x, int y) {  // круговая стрелка 12x12
  canvas.drawCircle(x + 6, y + 6, 5, INK);
  canvas.drawCircle(x + 6, y + 6, 4, INK);
  canvas.fillRect(x + 7, y, 6, 6, PAPER);
  canvas.fillTriangle(x + 6, y - 2, x + 11, y + 2, x + 6, y + 6, INK);
}

void drawBle(int x, int y) {  // значок Bluetooth 9x14
  canvas.drawLine(x + 4, y, x + 4, y + 13, INK);
  canvas.drawLine(x + 4, y, x + 8, y + 3, INK);
  canvas.drawLine(x + 8, y + 3, x, y + 10, INK);
  canvas.drawLine(x + 4, y + 13, x + 8, y + 10, INK);
  canvas.drawLine(x + 8, y + 10, x, y + 3, INK);
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
    efDrawRight(canvas, right, 18, F_SMALL, buf, INK);
  } else {
    // После Reset часы восстановлены по последней синхронизации — дням верить нельзя.
    drawSync(MARGIN + 2, 6);
    efDraw(canvas, MARGIN + 19, 18, F_SMALL, "НУЖНА СИНХРОНИЗАЦИЯ", INK, right - MARGIN - 23);
    canvas.drawFastHLine(MARGIN, 25, W - 2 * MARGIN, INK);
    return;
  }
  efDraw(canvas, MARGIN + 2, 18, F_SMALL, "МОЙ ХОЛОДИЛЬНИК", INK);
  canvas.drawFastHLine(MARGIN, 25, W - 2 * MARGIN, INK);
}

void drawSectionHeader(int x, int y, int w, const Section& s) {
  if (s.urgent) {
    canvas.fillRoundRect(x, y, w, HDR_H + 8, 8, INK);
    canvas.fillRect(x, y + HDR_H, w, 8, PAPER);  // срезать нижние скругления
    canvas.fillCircle(x + 13, y + HDR_H / 2 + 1, 5, PAPER);
    efDraw(canvas, x + 24, y + 23, F_HEAD, s.title, PAPER);
  } else {
    dither(x + 3, y + 3, w - 6, HDR_H - 3);
    canvas.fillCircle(x + 13, y + HDR_H / 2 + 1, 5, INK);
    efDraw(canvas, x + 24, y + 23, F_HEAD, s.title, INK);
    canvas.drawFastHLine(x, y + HDR_H, w, INK);
  }
}

void drawUrgentRow(int x, int y, int w, const Item& it, int32_t days) {
  // «!» рисуется геометрией: у глифа шрифта ножка сужается, и на 1-битном экране
  // это даёт ступеньку, похожую на флажок цифры «1».
  canvas.fillRoundRect(x + 11, y + 9, 6, 20, 2, INK);
  canvas.fillCircle(x + 13, y + 35, 3, INK);
  canvas.fillRect(x + 11, y + 34, 6, 2, INK);
  const int colX = x + 28, colW = 46;
  char num[8];
  snprintf(num, sizeof num, "%d", abs((int)days));
  const char* unit = days == 0 ? "сегодня" : ruDays(days);
  uint16_t fg = INK;
  if (days < 0) {  // просрочено: инверсная плашка
    canvas.fillRoundRect(colX, y + 4, colW, 38, 6, INK);
    fg = PAPER;
  }
  efDraw(canvas, colX + (colW - efWidth(F_NUM, num)) / 2, y + 26, F_NUM, num, fg);
  efDraw(canvas, colX + (colW - efWidth(F_UNIT, unit)) / 2, y + 39, F_UNIT, unit, fg);

  int nx = colX + colW + 6;
  int nameY = days < 0 ? y + 25 : y + 30;
  nx += efDraw(canvas, nx, nameY, F_ITEM, "- ", INK);
  int nameW = efDraw(canvas, nx, nameY, F_ITEM, it.name.c_str(), INK, x + w - 8 - nx);
  if (days < 0) {
    canvas.drawFastHLine(nx, nameY - 6, nameW, INK);  // зачёркнуто
    efDraw(canvas, nx, y + 41, F_UNIT, "просрочено", INK);
  }
}

void drawNormalRow(int x, int y, int w, const Item& it, int32_t days) {
  const int base = y + 22;
  canvas.drawCircle(x + 13, y + ROW_NORMAL / 2, 5, INK);
  char num[8];
  snprintf(num, sizeof num, "%d", (int)days);
  int nx = x + 24;
  nx += efDraw(canvas, nx, base + 1, F_NUM, num, INK) + 3;
  nx += efDraw(canvas, nx, base, F_UNIT, ruDays(days), INK);
  nx += efDraw(canvas, nx, base, F_ITEM, " - ", INK);
  efDraw(canvas, nx, base, F_ITEM, it.name.c_str(), INK, x + w - 8 - nx);
}

void drawEmpty() {
  const char* l1 = "Холодильник пуст";
  const char* l2 = "Нажмите любую кнопку";
  const char* l3 = "и отправьте список с телефона";
  efDraw(canvas, (W - efWidth(F_HEAD, l1)) / 2, 190, F_HEAD, l1, INK);
  efDraw(canvas, (W - efWidth(F_SMALL, l2)) / 2, 222, F_SMALL, l2, INK);
  efDraw(canvas, (W - efWidth(F_SMALL, l3)) / 2, 242, F_SMALL, l3, INK);
}

void renderScreen(bool bleActive) {
  canvas.fillScreen(PAPER);
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

  // Распределение места: сначала все строки, затем по одной убираем строку
  // у самой длинной несрочной секции (при равенстве — у более поздней).
  const int x = MARGIN, w = W - 2 * MARGIN;
  int y = 32;
  const int bottom = H - MARGIN;
  int fits[3], nsec = 0;
  for (int i = 0; i < 3; i++) { fits[i] = secs[i].count; if (secs[i].count) nsec++; }
  auto boxHeight = [&](int i) {
    int rowH = secs[i].urgent ? ROW_URGENT : ROW_NORMAL;
    return HDR_H + fits[i] * rowH + (fits[i] < secs[i].count ? MORE_H : 0) + 4;
  };
  auto total = [&]() {
    int t = (nsec - 1) * GAP;
    for (int i = 0; i < 3; i++) if (secs[i].count) t += boxHeight(i);
    return t;
  };
  while (total() > bottom - y) {
    int pick = -1;
    for (int i = 1; i < 3; i++)
      if (fits[i] > 1 && (pick < 0 || fits[i] >= fits[pick])) pick = i;
    if (pick < 0 && fits[0] > 1) pick = 0;
    if (pick < 0) for (int i = 2; i >= 0; i--) if (fits[i] > 0) { pick = i; break; }
    if (pick < 0) break;
    fits[pick]--;
  }

  for (int si = 0; si < 3; si++) {
    const Section& s = secs[si];
    if (!s.count) continue;
    const int rowH = s.urgent ? ROW_URGENT : ROW_NORMAL;
    int fit = fits[si];
    bool more = fit < s.count;
    int boxH = boxHeight(si);

    canvas.fillRect(x, y, w, boxH, PAPER);
    drawSectionHeader(x, y, w, s);
    int ry = y + HDR_H + 2;
    for (int r = 0; r < fit; r++) {
      const Item& it = items[s.from + r];
      int32_t d = it.expDay - today;
      if (s.urgent) drawUrgentRow(x, ry, w, it, d); else drawNormalRow(x, ry, w, it, d);
      ry += rowH;
      if (r < fit - 1 || more)
        for (int xx = x + 10; xx < x + w - 10; xx += 3) canvas.drawPixel(xx, ry - 1, INK);  // пунктир
    }
    if (more) {
      char buf[32];
      snprintf(buf, sizeof buf, "+ ещё %d", s.count - fit);
      efDraw(canvas, x + 24, ry + 16, F_SMALL, buf, INK);
    }
    thickRoundRect(x, y, w, boxH, 8, 2);
    y += boxH + GAP;
  }
}

// ---------- экран ----------
// Кадры храним в «родной» ориентации контроллера (400x300, 1 = белый).
// shownFrame — то, что сейчас физически на экране; при засыпании он сохраняется в LittleFS
// (в NVS 15 КБ не помещаются),
// чтобы после пробуждения сделать быстрое (частичное) обновление без мусора.
const int NW = 400, NH = 300, NBYTES = NW / 8 * NH;
static uint8_t shownFrame[NBYTES], nextFrame[NBYTES];
bool shownValid = false;
bool displayReady = false;

void displayInit() {
  if (displayReady) return;
  gpio_hold_dis((gpio_num_t)EPD_PWR);
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, HIGH);
  delay(10);
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(0, !shownValid, 10, false);
  displayReady = true;
}

void canvasToNative(uint8_t* out) {  // портрет 300x400 -> родные 400x300 (как setRotation(1))
  memset(out, 0xFF, NBYTES);
  const uint8_t* b = canvas.getBuffer();
  const int bw = (W + 7) / 8;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++)
      if (b[y * bw + (x >> 3)] & (0x80 >> (x & 7))) {
        int nx = NW - 1 - y, ny = x;
        out[ny * (NW / 8) + (nx >> 3)] &= ~(0x80 >> (nx & 7));
      }
}

void dumpShot() {
  const uint8_t* b = canvas.getBuffer();
  Serial.printf("\nSHOT_BEGIN %d %d\n", W, H);
  for (int i = 0; i < (W + 7) / 8 * H; i++) {
    Serial.printf("%02X", b[i]);
    if (i % 64 == 63) Serial.println();
  }
  Serial.println("\nSHOT_END");
}

void pushCanvas(bool fast) {
  displayInit();
  canvasToNative(nextFrame);
  if (!shownValid) fast = false;
  uint32_t t = millis();
  display.epd2.writeImageToPrevious(fast ? shownFrame : nextFrame, 0, 0, NW, NH);
  display.epd2.writeImage(nextFrame, 0, 0, NW, NH);
  display.epd2.refresh(fast);
  memcpy(shownFrame, nextFrame, NBYTES);
  shownValid = true;
  Serial.printf("refresh %s %lu ms\n", fast ? "fast" : "full", millis() - t);
}

void showScreen(bool bleActive, bool fast) {
  renderScreen(bleActive);
  pushCanvas(fast);
  lastRenderedDay = todayDay();
}

bool fsMount() {
  static bool mounted = false;
  if (!mounted) mounted = LittleFS.begin(true);  // true = отформатировать при первом запуске
  return mounted;
}

void loadFrame() {
  shownValid = false;
  if (!fsMount()) return;
  File f = LittleFS.open("/frame.bin", "r");
  if (f && f.size() == NBYTES) shownValid = f.read(shownFrame, NBYTES) == NBYTES;
  if (f) f.close();
  Serial.printf("кадр из flash: %s\n", shownValid ? "есть" : "нет");
}

void saveFrame() {  // пишем во flash, только если картинка изменилась
  if (!shownValid) return;
  uint32_t crc = esp_rom_crc32_le(0, shownFrame, NBYTES);
  if (crc == savedFrameCrc) return;
  if (!fsMount()) return;
  File f = LittleFS.open("/frame.bin", "w");
  if (!f) return;
  bool ok = f.write(shownFrame, NBYTES) == NBYTES;
  f.close();
  if (ok) savedFrameCrc = crc;
  Serial.printf("кадр сохранён: %s\n", ok ? "да" : "ОШИБКА");
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
  BLEDevice::init("Fridge-Tracker");
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
  saveFrame();
  if (displayReady) display.hibernate();
  pinMode(EPD_PWR, OUTPUT);
  digitalWrite(EPD_PWR, LOW);
  gpio_hold_en((gpio_num_t)EPD_PWR);
  gpio_deep_sleep_hold_en();

  uint64_t mask = 0;
  for (uint8_t p : BUTTONS) {
    mask |= 1ULL << p;
    rtc_gpio_pullup_en((gpio_num_t)p);
    rtc_gpio_pulldown_dis((gpio_num_t)p);
  }
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);  // подтяжки кнопок во сне
  esp_sleep_enable_ext1_wakeup(mask, ESP_EXT1_WAKEUP_ANY_LOW);

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
  canvas.fillScreen(PAPER);
  drawHeader(true);
  const int x = MARGIN, w = W - 2 * MARGIN, y = 110, h = 180;
  thickRoundRect(x, y, w, h, 8, 2);
  auto center = [&](int yy, const EFont& f, const char* t) { efDraw(canvas, (W - efWidth(f, t)) / 2, yy, f, t, INK); };
  center(y + 40, F_HEAD, "Сбросить все данные?");
  center(y + 70, F_SMALL, "Список продуктов и время");
  center(y + 90, F_SMALL, "будут удалены.");
  canvas.drawFastHLine(x + 20, y + 108, w - 40, INK);
  center(y + 135, F_SMALL, "Нажмите джойстик — сбросить");
  center(y + 158, F_SMALL, "Другая кнопка — отмена");
}

void factoryResetFlow() {
  Serial.println("Запрос сброса");
  renderResetPrompt();
  pushCanvas(true);
  waitButtonsReleased();
  int choice = 0;  // 1 = сбросить, -1 = отмена
  for (uint32_t t0 = millis(); millis() - t0 < 20000 && !choice;) {
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
    struct timeval tv = {0, 0};
    settimeofday(&tv, nullptr);
    lastRenderedDay = -1;
    savedFrameCrc = 0;
    Serial.println("Данные сброшены");
  }
  showScreen(true, false);
}

// ---------- serial-команды для отладки ----------
// J <json>  — загрузить данные как по BLE (после отрисовки выгрузит кадр);  S — выгрузить кадр
String serialLine;
bool shotRequested = false;
void pollSerial(bool& dirty) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n') { serialLine += c; continue; }
    serialLine.trim();
    if (serialLine.startsWith("J ")) { if (handleMessage(serialLine.substring(2))) dirty = shotRequested = true; }
    else if (serialLine == "S") dumpShot();
    serialLine = "";
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  Serial.setRxBufferSize(16384);
  for (uint8_t p : BUTTONS) { rtc_gpio_deinit((gpio_num_t)p); pinMode(p, INPUT_PULLUP); }
  prefs.begin("fridge", false);
  restoreState();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  Serial.printf("\nFridge-Tracker: пробуждение=%d, продуктов=%d, часы=%s\n",
                cause, items.size(), clockSynced ? "синхр." : (clockValid() ? "примерно" : "нет"));

  if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    // Полуночное пробуждение: полное обновление (заодно чистит следы быстрых обновлений).
    if (todayDay() != lastRenderedDay) showScreen(false, false);
    goToSleep();
  }
  if (cause == ESP_SLEEP_WAKEUP_EXT1) loadFrame();  // для быстрого обновления со значком
}

void loop() {
  static bool interactive = false;
  static uint32_t awakeUntil = 0;
  static uint32_t comboSince = 0;
  static bool dirty = false;

  if (!interactive) {
    // Кнопка или включение питания: показать значок BLE и ждать телефон.
    interactive = true;
    bleStart();
    showScreen(true, true);  // после Reset кадра нет -> будет полное обновление
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
    showScreen(true, false);
    if (shotRequested) { shotRequested = false; dumpShot(); }
    awakeUntil = millis() + AFTER_SYNC_MS;
  }

  bool combo = digitalRead(BTN_EXIT) == LOW && digitalRead(BTN_MENU) == LOW;
  if (!combo) comboSince = 0;
  else if (!comboSince) comboSince = millis();
  else if (millis() - comboSince > RESET_HOLD_MS) {
    comboSince = 0;
    factoryResetFlow();
    awakeUntil = millis() + AWAKE_MS;
  }
  if (anyButtonDown()) awakeUntil = max(awakeUntil, millis() + 30000);

  if (!NO_SLEEP && (int32_t)(millis() - awakeUntil) > 0 && !bleConnected) {
    showScreen(false, true);  // убрать значок BLE
    goToSleep();
  }
  delay(20);
}
