#!/bin/sh
# 在電腦上編譯並執行 cup_state.c 的事件偵測單元測試（不需要 ESP-IDF）
#   sh test/host/run.sh
set -e
cd "$(dirname "$0")"
mkdir -p build
cc -std=c11 -Wall -Wextra -Werror -O1 -I stub -I ../../main \
   ../../main/cup_state.c test_cup_event.c -lm -o build/test_cup_event
./build/test_cup_event
