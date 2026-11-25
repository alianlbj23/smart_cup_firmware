#!/bin/sh
# 在電腦上編譯並執行 cup_state.c 的事件偵測單元測試（不需要 ESP-IDF）
#   sh test/host/run.sh
set -e
cd "$(dirname "$0")"
mkdir -p build
cc -std=c11 -Wall -Wextra -Werror -O1 -I stub -I ../../main \
   ../../main/cup_state.c test_cup_event.c -lm -o build/test_cup_event
./build/test_cup_event

# 再編一次：關掉「直立不動時水位下降也算喝水」（EVT_STATIC_DROP_ENABLE=0），確認開關有效、原本的測試照樣通過
echo
echo "=== EVT_STATIC_DROP_ENABLE=0 ==="
cc -std=c11 -Wall -Wextra -Werror -O1 -I stub -I ../../main \
   -DEVT_STATIC_DROP_ENABLE=0 -DTEST_STATIC_DISABLED \
   ../../main/cup_state.c test_cup_event.c -lm -o build/test_cup_event_static_off
./build/test_cup_event_static_off
