#!/usr/bin/env bash
# 敏感資料檢查的 git hook：安裝、掃描、移除（互動選單）
#   bash tools/setup_hooks.sh
# 安裝後每次 git commit／git push 前會用 tools/secret_guard.py 檢查（敏感檔名＋gitleaks）
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
PY=$(command -v python3 || command -v python) || { echo "需要 python3" >&2; exit 1; }

install_hooks() {
  # core.hooksPath 會讓 .git/hooks 裡原本的 hook 失效，先提醒
  existing=$(find "$(git rev-parse --git-dir)/hooks" -maxdepth 1 -type f ! -name '*.sample' 2>/dev/null || true)
  if [ -n "$existing" ]; then
    echo "注意：下面這些 .git/hooks 裡的 hook 啟用 .githooks 後就不會再執行（要保留的話搬進 .githooks/）："
    echo "$existing"
  fi
  "$PY" tools/secret_guard.py install
  git config core.hooksPath .githooks
  echo "已啟用 .githooks（pre-commit、pre-push）。"
}

remove_hooks() {
  if [ "$(git config --get core.hooksPath || true)" = ".githooks" ]; then
    git config --unset core.hooksPath
    echo "已停用 .githooks（.tools/gitleaks 留著，要刪自己刪）。"
  else
    echo "目前沒有啟用 .githooks。"
  fi
}

echo "目前 core.hooksPath：$(git config --get core.hooksPath || echo '（未設定）')"
PS3="請選擇（輸入數字）："
select _ in "安裝／更新 hook（下載 gitleaks、啟用 .githooks）" "掃描整個 repo（含 git 歷史）" "移除 hook" "離開"; do
  case "$REPLY" in
    1) install_hooks; break ;;
    2) "$PY" tools/secret_guard.py scan || true; break ;;   # 結果已印出，選單本身不算失敗
    3) remove_hooks; break ;;
    4) break ;;
    *) echo "請輸入 1～4" ;;
  esac
done
