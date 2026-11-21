# SPDX-License-Identifier: GPL-3.0-or-later
"""Утилита разработки для CrowPanel (1-битный экран): отправка тестовых данных по USB и снятие кадра экрана.

  python dev.py send sample.json [out.png]   — загрузить данные (как по BLE) и сохранить кадр
  python dev.py shot [out.png]               — сохранить текущий кадр
  python dev.py listen [сек]                 — просто читать Serial
"""
import json
import os
import sys
import time
from datetime import datetime, timedelta

import serial
from PIL import Image

PORT = os.environ.get("FRIDGE_PORT", "COM4")  # порт: set FRIDGE_PORT=COM5


def open_port(reset=False):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = PORT, 115200, 0.2
    s.dtr = False  # не дёргать reset при открытии
    s.rts = False
    s.open()
    if reset:
        s.rts = True; time.sleep(0.1); s.rts = False
    return s


def read_until_shot(s, timeout=40):
    raw, t = b"", time.time()
    while time.time() - t < timeout:
        raw += s.read(8192)
        if raw.rstrip().endswith(b"SHOT_END"):
            break
    buf = raw.decode("utf-8", "replace")
    head, _, rest = buf.rpartition("SHOT_BEGIN")
    print(head.strip())
    if not rest:
        print("кадр не получен")
        return None
    dims, _, data = rest.partition("\n")
    w, h = map(int, dims.split())
    hexstr = "".join(data.split("SHOT_END")[0].split())
    raw = bytes.fromhex(hexstr)
    img = Image.frombytes("1", (w, h), bytes(b ^ 0xFF for b in raw))  # 1 = краска -> чёрный
    return img


def wait_for(s, marker, timeout):
    raw, t = b"", time.time()
    while time.time() - t < timeout and marker not in raw:
        raw += s.read(4096)
    print(raw.decode("utf-8", "replace").strip())


def save(img, out):
    if img:
        img = img.convert("L").resize((img.width * 2, img.height * 2), Image.NEAREST)
        img.save(out)
        print("->", out)


def sample_payload(path):
    """В sample.json даты можно задавать как "+3" (дней от сегодня)."""
    data = json.load(open(path, encoding="utf-8"))
    today = datetime.now().date()
    for it in data["items"]:
        e = str(it["e"])
        if e.startswith(("+", "-")):
            it["e"] = (today + timedelta(days=int(e))).isoformat()
    data["t"] = int(time.time())
    data["tz"] = int(datetime.now().astimezone().utcoffset().total_seconds() // 60)
    return json.dumps(data, ensure_ascii=False, separators=(",", ":"))


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "listen"
    s = open_port()
    if cmd == "send":
        payload = sample_payload(sys.argv[2])
        s.close(); s = open_port(reset=True)  # устройство может спать — будим через Reset
        wait_for(s, b"refresh", 20)
        s.write(("J " + payload + "\n").encode("utf-8"))
        save(read_until_shot(s), sys.argv[3] if len(sys.argv) > 3 else "shot.png")
    elif cmd == "shot":
        s.write(b"S\n")
        save(read_until_shot(s), sys.argv[2] if len(sys.argv) > 2 else "shot.png")
    elif cmd == "reset":
        s.close(); s = open_port(reset=True)
        wait_for(s, b"refresh", 20)
        s.write(b"S\n")
        save(read_until_shot(s), sys.argv[2] if len(sys.argv) > 2 else "shot.png")
    else:
        end = time.time() + float(sys.argv[2] if len(sys.argv) > 2 else 10)
        while time.time() < end:
            sys.stdout.write(s.read(4096).decode("utf-8", "replace"))


if __name__ == "__main__":
    main()
