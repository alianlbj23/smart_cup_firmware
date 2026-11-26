"""cup_updater：下載、驗證、安裝、重新啟動（Task 2）"""
import hashlib
import io
import os
import stat
import sys
import urllib.error
import zipfile
from pathlib import Path

import pytest

import cup_updater as up
from cup_updater import UpdateError


# ---------------- 假網路 ----------------
class FakeResp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def opener_returning(data: bytes, calls: list | None = None):
    def opener(req, timeout):
        if calls is not None:
            calls.append((req.full_url, timeout, req.get_header("User-agent")))
        return FakeResp(data)
    return opener


def opener_raising(exc):
    def opener(req, timeout):
        raise exc
    return opener


# ---------------- fetch_json ----------------
def test_fetch_json_parses_body_and_passes_timeout():
    calls = []
    assert up.fetch_json("https://api/x", opener_returning(b'{"tag_name": "v1.0.0"}', calls), 10) == {"tag_name": "v1.0.0"}
    assert calls[0][0] == "https://api/x" and calls[0][1] == 10 and calls[0][2]


def test_fetch_json_404_means_no_release_yet():
    err = urllib.error.HTTPError("https://api/x", 404, "Not Found", {}, None)
    with pytest.raises(UpdateError, match="Release"):
        up.fetch_json("https://api/x", opener_raising(err), 10)


def test_fetch_json_network_error_raises_update_error():
    with pytest.raises(UpdateError):
        up.fetch_json("https://api/x", opener_raising(urllib.error.URLError("down")), 10)


def test_fetch_json_invalid_json_raises_update_error():
    with pytest.raises(UpdateError):
        up.fetch_json("https://api/x", opener_returning(b"<html>"), 10)


# ---------------- download ----------------
def test_download_writes_file(tmp_path: Path):
    dest = tmp_path / "a.zip"
    assert up.download("https://x/a.zip", dest, opener_returning(b"abc" * 1000), 60, 10_000, expected_size=3000) == dest
    assert dest.read_bytes() == b"abc" * 1000


def test_download_over_limit_raises_and_removes_partial(tmp_path: Path):
    dest = tmp_path / "a.zip"
    with pytest.raises(UpdateError):
        up.download("https://x/a.zip", dest, opener_returning(b"x" * 5000), 60, 4000)
    assert not dest.exists()


def test_download_size_mismatch_raises(tmp_path: Path):
    dest = tmp_path / "a.zip"
    with pytest.raises(UpdateError):
        up.download("https://x/a.zip", dest, opener_returning(b"x" * 10), 60, 4000, expected_size=11)
    assert not dest.exists()


def test_download_network_error_raises_update_error(tmp_path: Path):
    with pytest.raises(UpdateError):
        up.download("https://x/a.zip", tmp_path / "a.zip", opener_raising(TimeoutError("slow")), 60, 4000)


# ---------------- sha256 ----------------
def test_verify_sha256_ok_and_mismatch(tmp_path: Path):
    f = tmp_path / "f"
    f.write_bytes(b"hello")
    good = hashlib.sha256(b"hello").hexdigest()
    up.verify_sha256(f, good.upper())
    with pytest.raises(UpdateError, match="SHA256"):
        up.verify_sha256(f, "0" * 64)


# ---------------- install_target / check_writable ----------------
def test_install_target_macos_is_app_bundle():
    exe = Path("/Applications/SmartCupTool.app/Contents/MacOS/SmartCupTool")
    assert up.install_target(exe, "darwin") == Path("/Applications/SmartCupTool.app")


def test_install_target_macos_not_in_bundle_raises():
    with pytest.raises(UpdateError):
        up.install_target(Path("/usr/local/bin/SmartCupTool"), "darwin")


def test_install_target_macos_translocated_asks_to_move_app():
    exe = Path("/private/var/folders/x/AppTranslocation/ABC/d/SmartCupTool.app/Contents/MacOS/SmartCupTool")
    with pytest.raises(UpdateError, match="應用程式"):
        up.install_target(exe, "darwin")


@pytest.mark.parametrize("platform", ["win32", "linux"])
def test_install_target_other_platforms_is_exe(platform):
    assert up.install_target(Path("/opt/x/SmartCupTool"), platform) == Path("/opt/x/SmartCupTool")


def test_check_writable_ok(tmp_path: Path):
    up.check_writable(tmp_path)
    assert list(tmp_path.iterdir()) == []


@pytest.mark.skipif(sys.platform == "win32" or os.geteuid() == 0, reason="需要 POSIX 權限、非 root")
def test_check_writable_read_only_dir_raises(tmp_path: Path):
    ro = tmp_path / "ro"
    ro.mkdir()
    ro.chmod(0o555)
    try:
        with pytest.raises(UpdateError, match="手動"):
            up.check_writable(ro)
    finally:
        ro.chmod(0o755)


# ---------------- install ----------------
def make_zip(path: Path, files: dict[str, bytes]) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        for name, data in files.items():
            z.writestr(name, data)
    return path


def test_install_linux_replaces_file_and_sets_exec(tmp_path: Path):
    target = tmp_path / "app" / "SmartCupTool"
    target.parent.mkdir()
    target.write_bytes(b"old")
    z = make_zip(tmp_path / "u.zip", {"SmartCupTool": b"new"})
    assert up.install(z, "linux", target) == target
    assert target.read_bytes() == b"new"
    assert target.stat().st_mode & stat.S_IXUSR
    assert sorted(p.name for p in target.parent.iterdir()) == ["SmartCupTool"]   # 暫存資料夾清掉了


def test_install_windows_keeps_old_copy(tmp_path: Path):
    target = tmp_path / "SmartCupTool.exe"
    target.write_bytes(b"old")
    z = make_zip(tmp_path / "u.zip", {"SmartCupTool.exe": b"new"})
    up.install(z, "win32", target)
    assert target.read_bytes() == b"new"
    assert (tmp_path / "SmartCupTool.exe.old").read_bytes() == b"old"


def test_install_missing_member_raises_and_keeps_old(tmp_path: Path):
    target = tmp_path / "SmartCupTool.exe"
    target.write_bytes(b"old")
    z = make_zip(tmp_path / "u.zip", {"other.exe": b"new"})
    with pytest.raises(UpdateError):
        up.install(z, "win32", target)
    assert target.read_bytes() == b"old"


def test_install_zip_slip_member_is_not_extracted(tmp_path: Path):
    app = tmp_path / "app"
    app.mkdir()
    target = app / "SmartCupTool"
    target.write_bytes(b"old")
    z = make_zip(tmp_path / "u.zip", {"../SmartCupTool": b"evil"})
    with pytest.raises(UpdateError):
        up.install(z, "linux", target)
    assert target.read_bytes() == b"old"
    assert not (tmp_path / "SmartCupTool").exists()


def test_install_corrupt_zip_raises(tmp_path: Path):
    target = tmp_path / "SmartCupTool"
    target.write_bytes(b"old")
    bad = tmp_path / "u.zip"
    bad.write_bytes(b"not a zip")
    with pytest.raises(UpdateError):
        up.install(bad, "linux", target)
    assert target.read_bytes() == b"old"


def test_install_windows_restores_old_when_move_fails(tmp_path: Path, monkeypatch):
    target = tmp_path / "SmartCupTool.exe"
    target.write_bytes(b"old")
    z = make_zip(tmp_path / "u.zip", {"SmartCupTool.exe": b"new"})
    real_replace = os.replace
    calls = []

    def flaky_replace(src, dst):
        calls.append((Path(src).name, Path(dst).name))
        if len(calls) == 2:            # 第 1 次：原檔 → .old；第 2 次：新檔 → 原位置（失敗）
            raise PermissionError("locked")
        return real_replace(src, dst)

    monkeypatch.setattr(up.os, "replace", flaky_replace)
    with pytest.raises(UpdateError):
        up.install(z, "win32", target)
    assert target.read_bytes() == b"old"
    assert not (tmp_path / "SmartCupTool.exe.old").exists()


def fake_ditto(app_files: dict[str, bytes], calls: list):
    """假 ditto：在目的資料夾建出 SmartCupTool.app"""
    def run(cmd, check, timeout):
        calls.append(cmd)
        dest = Path(cmd[-1])
        for rel, data in app_files.items():
            p = dest / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
    return run


def test_install_macos_swaps_app_bundle_with_ditto(tmp_path: Path):
    target = tmp_path / "SmartCupTool.app"
    (target / "Contents" / "MacOS").mkdir(parents=True)
    (target / "Contents" / "MacOS" / "SmartCupTool").write_bytes(b"old")
    z = make_zip(tmp_path / "u.zip", {"x": b"unused"})
    calls = []
    run = fake_ditto({"SmartCupTool.app/Contents/MacOS/SmartCupTool": b"new"}, calls)
    assert up.install(z, "darwin", target, run=run) == target
    assert calls[0][:4] == ["ditto", "-x", "-k", str(z)]
    assert (target / "Contents" / "MacOS" / "SmartCupTool").read_bytes() == b"new"
    up.cleanup_old(target)
    assert sorted(p.name for p in tmp_path.iterdir()) == ["SmartCupTool.app", "u.zip"]


def test_install_macos_ditto_failure_raises_and_keeps_old(tmp_path: Path):
    import subprocess
    target = tmp_path / "SmartCupTool.app"
    target.mkdir()
    (target / "marker").write_text("old")

    def run(cmd, check, timeout):
        raise subprocess.CalledProcessError(1, cmd)

    with pytest.raises(UpdateError):
        up.install(make_zip(tmp_path / "u.zip", {"x": b""}), "darwin", target, run=run)
    assert (target / "marker").read_text() == "old"


def test_install_macos_zip_without_app_raises(tmp_path: Path):
    target = tmp_path / "SmartCupTool.app"
    target.mkdir()
    calls = []
    with pytest.raises(UpdateError):
        up.install(make_zip(tmp_path / "u.zip", {"x": b""}), "darwin", target,
                   run=fake_ditto({"Other.app/x": b""}, calls))
    assert target.is_dir()


# ---------------- cleanup / restart ----------------
def test_cleanup_old_removes_file_or_dir_and_ignores_missing(tmp_path: Path):
    target = tmp_path / "SmartCupTool.exe"
    (tmp_path / "SmartCupTool.exe.old").write_bytes(b"x")
    up.cleanup_old(target)
    assert not (tmp_path / "SmartCupTool.exe.old").exists()
    app = tmp_path / "SmartCupTool.app"
    (tmp_path / "SmartCupTool.app.old" / "Contents").mkdir(parents=True)
    up.cleanup_old(app)
    assert not (tmp_path / "SmartCupTool.app.old").exists()
    up.cleanup_old(target)   # 沒有 .old 也不出錯


def test_restart_command_per_platform():
    assert up.restart_command("darwin", Path("/A/SmartCupTool.app")) == ["open", "-n", "/A/SmartCupTool.app"]
    assert up.restart_command("win32", Path("C:/x/SmartCupTool.exe")) == [str(Path("C:/x/SmartCupTool.exe"))]
    assert up.restart_command("linux", Path("/opt/SmartCupTool")) == ["/opt/SmartCupTool"]


def test_restart_env_resets_pyinstaller_and_keeps_others():
    env = up.restart_env({"PATH": "/bin"})
    assert env == {"PATH": "/bin", "PYINSTALLER_RESET_ENVIRONMENT": "1"}


# ---------------- review 補的 ----------------
def test_fetch_json_incomplete_read_raises_update_error():
    import http.client
    with pytest.raises(UpdateError):
        up.fetch_json("https://api/x", opener_raising(http.client.IncompleteRead(b"x", 10)), 10)


def test_download_incomplete_read_raises_and_removes_partial(tmp_path: Path):
    import http.client

    class Broken(FakeResp):
        def read(self, n=-1):
            raise http.client.IncompleteRead(b"", 10)

    dest = tmp_path / "a.zip"
    with pytest.raises(UpdateError):
        up.download("https://x/a.zip", dest, lambda req, timeout: Broken(b""), 60, 4000)
    assert not dest.exists()


def test_download_does_not_send_github_api_accept_header(tmp_path: Path):
    seen = []

    def opener(req, timeout):
        seen.append(req.get_header("Accept"))
        return FakeResp(b"x")

    up.download("https://x/a.zip", tmp_path / "a", opener, 60, 10)
    assert seen == [None]


def test_install_macos_leaves_old_bundle_until_cleanup(tmp_path: Path):
    target = tmp_path / "SmartCupTool.app"
    target.mkdir()
    (target / "marker").write_text("old")
    up.install(make_zip(tmp_path / "u.zip", {"x": b""}), "darwin", target,
               run=fake_ditto({"SmartCupTool.app/marker": b"new"}, []))
    assert (target / "marker").read_text() == "new"
    assert (tmp_path / "SmartCupTool.app.old" / "marker").read_text() == "old"


def test_install_windows_first_rename_fails_keeps_old(tmp_path: Path, monkeypatch):
    target = tmp_path / "SmartCupTool.exe"
    target.write_bytes(b"old")
    z = make_zip(tmp_path / "u.zip", {"SmartCupTool.exe": b"new"})

    def deny(src, dst):
        raise PermissionError("in use")

    monkeypatch.setattr(up.os, "replace", deny)
    with pytest.raises(UpdateError):
        up.install(z, "win32", target)
    assert target.read_bytes() == b"old"
    assert sorted(p.name for p in tmp_path.iterdir()) == ["SmartCupTool.exe", "u.zip"]
