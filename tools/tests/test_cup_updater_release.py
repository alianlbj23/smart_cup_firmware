"""cup_updater：版號解析、Release JSON 解析（Task 1）"""
from pathlib import Path

import pytest

import cup_updater as up
from cup_updater import Update, UpdateError

HEADER = """
#define CUP_FW_MAJOR 0
#define CUP_FW_MINOR 5
#define CUP_FW_PATCH 0
"""


def release(tag="v0.6.0", assets=None):
    if assets is None:
        assets = [
            {"name": "SmartCupTool-Linux.zip", "browser_download_url": "https://x/linux.zip", "size": 1234},
            {"name": "SmartCupTool-Windows.zip", "browser_download_url": "https://x/win.zip", "size": 99},
            {"name": "SHA256SUMS.txt", "browser_download_url": "https://x/sums", "size": 10},
        ]
    return {"tag_name": tag, "html_url": "https://github.com/r/releases/tag/" + tag, "assets": assets}


# ---------------- 版號 ----------------
@pytest.mark.parametrize("text, want", [("0.5.0", (0, 5, 0)), ("v1.12.3", (1, 12, 3)), (" v2.0.1\n", (2, 0, 1))])
def test_parse_version_accepts_semver_with_optional_v(text, want):
    assert up.parse_version(text) == want


@pytest.mark.parametrize("text", ["0.5", "abc", "", "1.2.3.4", "v1.x.0", "1.2.-3"])
def test_parse_version_rejects_malformed(text):
    with pytest.raises(ValueError):
        up.parse_version(text)


def test_read_header_version_reads_three_defines():
    assert up.read_header_version(HEADER) == "0.5.0"


def test_read_header_version_missing_define_raises():
    with pytest.raises(ValueError):
        up.read_header_version("#define CUP_FW_MAJOR 0\n#define CUP_FW_MINOR 5\n")


def test_current_version_prefers_bundled_version_file(tmp_path: Path):
    (tmp_path / "VERSION").write_text("0.7.1\n")
    assert up.current_version(tmp_path, tmp_path / "no-repo") == "0.7.1"


def test_current_version_from_repo_header_when_not_bundled(tmp_path: Path):
    hdr = tmp_path / "main" / "ble_gatt_svr.h"
    hdr.parent.mkdir()
    hdr.write_text(HEADER)
    assert up.current_version(None, tmp_path) == "0.5.0"


def test_current_version_is_dev_when_nothing_readable(tmp_path: Path):
    (tmp_path / "VERSION").write_text("garbage")
    assert up.current_version(tmp_path, tmp_path) == "dev"
    assert up.current_version(None, tmp_path / "missing") == "dev"


# ---------------- 平台資產 ----------------
@pytest.mark.parametrize("platform, name", [
    ("darwin", "SmartCupTool-macOS.zip"),
    ("win32", "SmartCupTool-Windows.zip"),
    ("linux", "SmartCupTool-Linux.zip"),
])
def test_asset_name_per_platform(platform, name):
    assert up.asset_name(platform) == name


def test_asset_name_unknown_platform_raises():
    with pytest.raises(UpdateError):
        up.asset_name("sunos5")


# ---------------- pick_update ----------------
def test_pick_update_returns_update_when_newer():
    u = up.pick_update(release(), "0.5.0", "linux")
    assert u == Update(version="0.6.0", tag="v0.6.0", url="https://x/linux.zip", size=1234,
                       sums_url="https://x/sums", page_url="https://github.com/r/releases/tag/v0.6.0")


@pytest.mark.parametrize("current", ["0.6.0", "0.7.0", "1.0.0"])
def test_pick_update_none_when_same_or_older(current):
    assert up.pick_update(release(), current, "linux") is None


def test_pick_update_none_when_running_dev_build():
    assert up.pick_update(release(), "dev", "linux") is None


def test_pick_update_none_when_platform_asset_missing():
    assert up.pick_update(release(), "0.5.0", "darwin") is None


def test_pick_update_raises_without_sha256sums():
    assets = [{"name": "SmartCupTool-Linux.zip", "browser_download_url": "https://x/l", "size": 1}]
    with pytest.raises(UpdateError, match="SHA256SUMS"):
        up.pick_update(release(assets=assets), "0.5.0", "linux")


@pytest.mark.parametrize("bad", [
    [],                                   # 不是 dict
    {"assets": []},                       # 沒有 tag_name
    {"tag_name": "v0.6.0", "assets": "x"},  # assets 不是 list
    {"tag_name": "nightly", "assets": []},  # tag 不是版號
    release(assets=[{"name": "SmartCupTool-Linux.zip", "browser_download_url": "https://x/l", "size": "big"},
                    {"name": "SHA256SUMS.txt", "browser_download_url": "https://x/s", "size": 1}]),
    release(assets=[{"name": "SmartCupTool-Linux.zip", "browser_download_url": 5, "size": 1},
                    {"name": "SHA256SUMS.txt", "browser_download_url": "https://x/s", "size": 1}]),
    release(assets=["not-a-dict"]),
])
def test_pick_update_rejects_malformed_release(bad):
    with pytest.raises(UpdateError):
        up.pick_update(bad, "0.5.0", "linux")


def test_pick_update_rejects_non_https_url():
    assets = [{"name": "SmartCupTool-Linux.zip", "browser_download_url": "http://x/l", "size": 1},
              {"name": "SHA256SUMS.txt", "browser_download_url": "https://x/s", "size": 1}]
    with pytest.raises(UpdateError):
        up.pick_update(release(assets=assets), "0.5.0", "linux")


# ---------------- SHA256SUMS ----------------
def test_parse_sums_reads_lines_and_binary_marker():
    a, b = "a" * 64, "B" * 64
    text = f"{a}  SmartCupTool-Linux.zip\n\n{b} *SmartCupTool-Windows.zip\n"
    assert up.parse_sums(text) == {"SmartCupTool-Linux.zip": a, "SmartCupTool-Windows.zip": b.lower()}


def test_parse_sums_ignores_malformed_lines():
    assert up.parse_sums("not a hash line\nzz  file\n") == {}
