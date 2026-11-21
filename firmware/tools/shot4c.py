# SPDX-License-Identifier: GPL-3.0-or-later
"""Снимок экрана NOTE4C по USB (без обновления панели) и загрузка тестовых данных.

  python shot4c.py COM5 shot [out.png]            — снять текущий кадр
  python shot4c.py COM5 send sample.json [out.png] — загрузить данные (как по BLE) и снять кадр

Прошивка должна не спать: включите её кнопкой (BLE-окно 2 минуты) или соберите с NO_SLEEP 1.
В sample.json даты можно задавать как "+3" (дней от сегодня).
"""
import json
import sys
import time
from datetime import datetime, timedelta

import serial
from PIL import Image

PALETTE = {"0": (0, 0, 0), "1": (255, 255, 255), "2": (200, 30, 30), "3": (240, 200, 0)}


def payload(path):
    data = json.load(open(path, encoding="utf-8"))
    today = datetime.now().date()
    for it in data["items"]:
        e = str(it["e"])
        if e.startswith(("+", "-")):
            it["e"] = (today + timedelta(days=int(e))).isoformat()
    data["t"] = int(time.time())
    data["tz"] = int(datetime.now().astimezone().utcoffset().total_seconds() // 60)
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


def read_shot(s, timeout=30):
    raw, t = b"", time.time()
    while time.time() - t < timeout and b"SHOT4C_END" not in raw:
        raw += s.read(65536)
    text = raw.decode("utf-8", "replace")
    head, _, rest = text.partition("SHOT4C_BEGIN")
    print(head.strip())
    if not rest:
        print("кадр не получен")
        return None
    lines = rest.split("SHOT4C_END")[0].strip().splitlines()
    w, h = map(int, lines[0].split())
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y, row in enumerate(lines[1:h + 1]):
        for x, ch in enumerate(row.strip()[:w]):
            px[x, y] = PALETTE.get(ch, (255, 0, 255))
    return img.resize((w * 2, h * 2), Image.NEAREST)


def main():
    port, cmd = sys.argv[1], sys.argv[2]
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 0.2
    s.dtr = False  # не дёргать reset при открытии порта
    s.rts = False
    try:
        s.open()
    except serial.SerialException as e:
        print(f"Не удалось открыть {port}: {e}")
        print("- Закройте Serial Monitor в Arduino IDE (и другие программы, держащие порт).")
        print("- Устройство могло уснуть: нажмите центральную кнопку и запустите скрипт в течение 2 минут.")
        sys.exit(1)
    if cmd == "send":
        s.write(("J " + payload(sys.argv[3]) + "\n").encode("utf-8"))
        time.sleep(1)
        out = sys.argv[4] if len(sys.argv) > 4 else "shot.png"
    else:
        out = sys.argv[3] if len(sys.argv) > 3 else "shot.png"
    s.reset_input_buffer()
    s.write(b"S\n")
    img = read_shot(s)
    if img:
        img.save(out)
        print("->", out)


if __name__ == "__main__":
    main()
