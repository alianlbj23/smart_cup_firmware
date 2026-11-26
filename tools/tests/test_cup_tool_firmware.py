"""cup_tool：韌體燒錄檔的說明檔（Release 不能用中文檔名，GitHub 會改成 default.txt）"""
from pathlib import Path

import cup_tool

NOTE = "韌體 v0.5.1（commit cb44bdd）ESP-IDF v6.1、esp32c6\n\nA. ...\n"


def test_flash_note_name_is_ascii():
    assert cup_tool.FLASH_NOTE.isascii()


def test_release_download_wants_ascii_note():
    assert cup_tool.FLASH_NOTE in cup_tool.RELEASE_FIRMWARE_FILES
    assert all(name.isascii() for name in cup_tool.RELEASE_FIRMWARE_FILES)


def test_firmware_version_reads_ascii_note(tmp_path: Path):
    (tmp_path / cup_tool.FLASH_NOTE).write_text(NOTE, encoding="utf-8")
    assert cup_tool.firmware_version(tmp_path) == "v0.5.1"


def test_firmware_version_still_reads_old_chinese_note(tmp_path: Path):
    """舊的 Actions zip（保留 90 天）裡還是 燒錄說明.txt"""
    (tmp_path / "燒錄說明.txt").write_text(NOTE, encoding="utf-8")
    assert cup_tool.firmware_version(tmp_path) == "v0.5.1"


def test_firmware_version_without_note_is_local_build(tmp_path: Path):
    assert cup_tool.firmware_version(tmp_path) == "本機建置"
