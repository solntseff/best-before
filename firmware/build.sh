#!/usr/bin/env bash
# Собирает все прошивки в firmware/dist/ (готовые .bin для прошивки с адреса 0x0).
#
#   ./build.sh            — все варианты
#   ./build.sh note4c-v   — один вариант (crowpanel | note4c-v | note4c-h)
#
# Нужны: arduino-cli, ядро esp32:esp32 (3.3.x) и библиотеки GxEPD2, Adafruit GFX Library,
# ArduinoJson (версии — в README). Общий код подключается из firmware/libraries.
set -euo pipefail
cd "$(dirname "$0")"

CLI="${ARDUINO_CLI:-arduino-cli}"
FQBN_BASE="esp32:esp32:esp32s3:CDCOnBoot=cdc,PSRAM=opi,FlashSize=16M,PartitionScheme=huge_app"
FQBN_CROWPANEL="esp32:esp32:esp32s3:PSRAM=opi,PartitionScheme=huge_app"
mkdir -p dist build

build() {  # имя, скетч, fqbn, доп. флаги
  local name=$1 sketch=$2 fqbn=$3 flags=${4:-}
  echo "==> $name"
  local args=(compile --fqbn "$fqbn" --libraries libraries --build-path "build/$name" --warnings default)
  [ -n "$flags" ] && args+=(--build-property "compiler.cpp.extra_flags=$flags")
  "$CLI" "${args[@]}" "$sketch"
  # merged.bin дополнен 0xFF до размера flash (16 МБ) — обрезаем хвост: стёртая flash и так 0xFF.
  "$PY" -c 'import sys; d=open(sys.argv[1],"rb").read().rstrip(b"\xff"); d+=b"\xff"*(-len(d)%4096); open(sys.argv[2],"wb").write(d)' \
    "build/$name/$(basename "$sketch").ino.merged.bin" "dist/fridge-tracker-$name.bin"
}
PY=$(for p in python3 python; do "$p" -c "" 2>/dev/null && { echo "$p"; break; }; done)

want=${1:-all}
[[ $want == all || $want == crowpanel ]] && build crowpanel crowpanel-42 "$FQBN_CROWPANEL"
[[ $want == all || $want == note4c-v ]] && build note4c-vertical zectrix-note4c "$FQBN_BASE" "-DORIENTATION=1"
[[ $want == all || $want == note4c-h ]] && build note4c-horizontal zectrix-note4c "$FQBN_BASE" "-DORIENTATION=0"

ls -l dist
