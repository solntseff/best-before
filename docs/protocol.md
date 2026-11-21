# BLE protocol

The web app and every firmware variant speak the same protocol, so one site works with all devices.

| | |
|---|---|
| Device name | `Fridge-Tracker` (CrowPanel), `Fridge-Tracker-Color` (Zectrix NOTE4C) |
| Service UUID | `4fafc201-1fb5-459e-8fcc-c5c9c331914b` |
| Characteristic UUID | `beb5483e-36e1-4688-b7f5-ea07361b26a8` (write, read, notify) |

The site looks for any device whose name starts with `Fridge-Tracker` or that advertises the service UUID.

## Message

One message is a single JSON object followed by `\n`:

```json
{"t":1763712000,"tz":120,"items":[{"n":"Молоко 3.2%","e":"2026-11-22"},{"n":"חלב תנובה 3%","e":"2026-11-27"}]}
```

| Field | Meaning |
|---|---|
| `t` | Current Unix time, seconds (UTC). The device sets its clock from it. |
| `tz` | Local offset from UTC in minutes (e.g. `120` for UTC+2). Used to know when local midnight is. |
| `items` | Full list of products. Each message replaces the previous list. |
| `items[].n` | Product name, UTF-8. Latin, Cyrillic and Hebrew are rendered; Hebrew is laid out right-to-left. |
| `items[].e` | Expiry date, `YYYY-MM-DD`, local. |

The device keeps at most 60 items. Days left are computed on the device, so the screen stays correct after the phone is gone.

## Chunking

BLE writes are limited by MTU, so the site splits the UTF-8 bytes of `"\x02" + json + "\n"` into chunks of 180 bytes and writes them one by one with `writeValueWithResponse`.

- A chunk that starts with `0x02` resets the receive buffer.
- The message is complete when the buffer ends with `\n`.
- Maximum message size is 16 KB.

## Reply

After a complete message the characteristic value becomes:

- `OK <count>` — list accepted and saved, `<count>` is the number of items;
- `ERR <reason>` — JSON could not be parsed (`<reason>` comes from ArduinoJson) or `items` is missing.

The site reads the characteristic after the last chunk. The reply is also sent as a notification.

## Serial (USB) debug commands

Both firmwares accept the same message over USB serial (115200 baud), which is handy without a phone:

- `J <json>` — load a message as if it came over BLE;
- `S` — dump the current screen image (used by `firmware/tools/dev.py` for CrowPanel and `firmware/tools/shot4c.py` for NOTE4C).
