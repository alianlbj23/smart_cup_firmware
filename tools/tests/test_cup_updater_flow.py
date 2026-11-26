"""cup_updater：查最新版、整套更新流程（apply_update）"""
import hashlib
import io
import json
import zipfile
from pathlib import Path

import pytest

import cup_updater as up
from cup_updater import Update, UpdateError


class FakeResp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def url_opener(pages: dict[str, bytes], calls: list | None = None):
    def opener(req, timeout):
        if calls is not None:
            calls.append(req.full_url)
        return FakeResp(pages[req.full_url])
    return opener


def zip_bytes(files: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for k, v in files.items():
            z.writestr(k, v)
    return buf.getvalue()


def test_latest_update_queries_github_latest_release():
    rel = {"tag_name": "v9.0.0", "html_url": "https://g/r", "assets": [
        {"name": "SmartCupTool-Linux.zip", "browser_download_url": "https://g/l.zip", "size": 3},
        {"name": "SHA256SUMS.txt", "browser_download_url": "https://g/sums", "size": 3}]}
    calls = []
    u = up.latest_update("0.5.0", "linux", url_opener({up.LATEST_API: json.dumps(rel).encode()}, calls))
    assert calls == [up.LATEST_API]
    assert u is not None and u.version == "9.0.0"


def make_update(zdata: bytes, sums_text: str) -> tuple[Update, dict[str, bytes]]:
    u = Update(version="9.0.0", tag="v9.0.0", url="https://g/l.zip", size=len(zdata),
               sums_url="https://g/sums", page_url="https://g/r")
    return u, {u.url: zdata, u.sums_url: sums_text.encode()}


def test_apply_update_downloads_verifies_and_installs(tmp_path: Path):
    target = tmp_path / "SmartCupTool"
    target.write_bytes(b"old")
    zdata = zip_bytes({"SmartCupTool": b"new"})
    u, pages = make_update(zdata, f"{hashlib.sha256(zdata).hexdigest()}  SmartCupTool-Linux.zip\n")
    logs = []
    assert up.apply_update(u, "linux", target, url_opener(pages), log=logs.append) == target
    assert target.read_bytes() == b"new"
    assert logs   # 有進度紀錄


def test_apply_update_refuses_when_hash_mismatch(tmp_path: Path):
    target = tmp_path / "SmartCupTool"
    target.write_bytes(b"old")
    zdata = zip_bytes({"SmartCupTool": b"new"})
    u, pages = make_update(zdata, f"{'0' * 64}  SmartCupTool-Linux.zip\n")
    with pytest.raises(UpdateError, match="SHA256"):
        up.apply_update(u, "linux", target, url_opener(pages))
    assert target.read_bytes() == b"old"


def test_apply_update_refuses_when_asset_not_in_sums(tmp_path: Path):
    target = tmp_path / "SmartCupTool"
    target.write_bytes(b"old")
    zdata = zip_bytes({"SmartCupTool": b"new"})
    u, pages = make_update(zdata, f"{hashlib.sha256(zdata).hexdigest()}  SmartCupTool-Windows.zip\n")
    with pytest.raises(UpdateError, match="SHA256SUMS"):
        up.apply_update(u, "linux", target, url_opener(pages))
    assert target.read_bytes() == b"old"
