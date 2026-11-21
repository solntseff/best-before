// SPDX-License-Identifier: GPL-3.0-or-later
// Типы данных вынесены в заголовок: Arduino генерирует прототипы функций
// выше определений структур в .ino.
#pragma once
#include <Arduino.h>

struct Item {
  String name;
  int32_t expDay;   // дни от 1970-01-01 (локальная дата)
};

struct Section {
  const char* title;
  bool urgent;
  int from, count;  // диапазон в items
};
