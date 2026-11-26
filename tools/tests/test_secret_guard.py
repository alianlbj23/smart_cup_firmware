"""tools/secret_guard.py：擋敏感檔名、gitleaks 安裝與 git hook 掃描"""
import io
import os
import random
import string
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

import pytest

import secret_guard as sg
from secret_guard import GuardError

TOOLS_PKG_DIR = Path(__file__).resolve().parent.parent
REPO = TOOLS_PKG_DIR.parent
SCRIPT = TOOLS_PKG_DIR / "secret_guard.py"


# ---------------- 敏感檔名 ----------------
@pytest.mark.parametrize("path", [
    ".env", "app/.env", ".env.local", ".env.production",
    "certs/server.pem", "server.key", "keys/client.p12", "cert.pfx", "release.jks", "android.keystore",
    "id_rsa", "home/.ssh/id_ed25519", "id_ecdsa", "id_dsa", "putty.ppk",
    "credentials.json", "gcp/service-account.json", ".netrc", ".pypirc", ".npmrc",
    ".claude/settings.local.json", "secure_boot_signing_key.pem", "flash_encryption_key.bin", "nvs_keys.bin",
    "SERVER.KEY", "Dev.PEM",
    "prod.env", "config/secrets.env", "secrets.json", "secrets.yaml", "secrets.yml", "secrets.toml",
    ".git-credentials", ".htpasswd", "AuthKey_ABC.p8", "vault.kdbx", "terraform.tfstate",
    "sub/.claude/settings.local.json", "certs\\server.key",
])
def test_sensitive_paths_are_blocked(path):
    assert sg.is_blocked_path(path)


@pytest.mark.parametrize("path", [
    ".env.example", ".env.sample", ".env.template", "id_rsa.pub", "id_ed25519.pub",
    "keyboard.c", "docs/keys.md", "main/key_scan.c", "tools/secret_guard.py", "sdkconfig.defaults",
    "README.md", "main/ble_gatt_svr.h", ".github/workflows/build.yml",
])
def test_normal_paths_are_allowed(path):
    assert not sg.is_blocked_path(path)


def test_blocked_paths_filters_list():
    assert sg.blocked_paths(["a.c", ".env", "b/server.key", "id_rsa.pub"]) == [".env", "b/server.key"]


def test_allow_patterns_let_public_certs_through():
    allow = ["main/certs/*_cert.pem"]
    assert not sg.is_blocked_path("main/certs/ca_cert.pem", allow)
    assert sg.is_blocked_path("main/certs/server.key", allow)
    assert sg.is_blocked_path("other/ca_cert.pem", allow)


def test_load_allow_reads_patterns_and_skips_comments(tmp_path: Path):
    (tmp_path / sg.ALLOW_FILE).write_text("# 公開憑證\nmain/certs/*_cert.pem\n\n  docs/*.pem  \n")
    assert sg.load_allow(tmp_path) == ["main/certs/*_cert.pem", "docs/*.pem"]
    assert sg.load_allow(tmp_path / "missing") == []


def test_gitignore_has_every_blocked_pattern():
    """BLOCKED_NAME_PATTERNS 和 .gitignore 要一致，免得之後只改一邊"""
    lines = {ln.strip() for ln in (REPO / ".gitignore").read_text().splitlines()}
    missing = [p for p in sg.BLOCKED_NAME_PATTERNS + sg.BLOCKED_PATH_SUFFIXES if p not in lines]
    assert missing == []


# ---------------- pre-push 的範圍 ----------------
Z = "0" * 40
A, B = "a" * 40, "b" * 40


def test_push_ranges_existing_branch_scans_new_commits_only():
    assert sg.push_ranges(f"refs/heads/main {B} refs/heads/main {A}\n") == [f"{A}..{B}"]


def test_push_ranges_new_branch_scans_commits_not_on_remote():
    assert sg.push_ranges(f"refs/heads/feat {B} refs/heads/feat {Z}\n") == [f"{B} --not --remotes"]


def test_push_ranges_branch_delete_scans_nothing():
    assert sg.push_ranges(f"(delete) {Z} refs/heads/old {A}\n") == []


def test_push_ranges_ignores_blank_lines():
    assert sg.push_ranges("\n  \n") == []


def test_push_ranges_rejects_malformed_line():
    """格式不對就擋（fail closed），不能當作沒東西要掃"""
    with pytest.raises(GuardError):
        sg.push_ranges("garbage line\n")


def test_push_ranges_multiple_refs():
    text = f"refs/heads/main {B} refs/heads/main {A}\nrefs/heads/feat {A} refs/heads/feat {Z}\n"
    assert sg.push_ranges(text) == [f"{A}..{B}", f"{A} --not --remotes"]


def test_push_ranges_rejects_non_hex_sha():
    with pytest.raises(GuardError):
        sg.push_ranges(f"refs/heads/main {'x' * 40} refs/heads/main {A}\n")


# ---------------- gitleaks 下載 ----------------
@pytest.mark.parametrize("system, machine, asset", [
    ("Linux", "x86_64", "gitleaks_8.30.1_linux_x64.tar.gz"),
    ("Linux", "aarch64", "gitleaks_8.30.1_linux_arm64.tar.gz"),
    ("Darwin", "arm64", "gitleaks_8.30.1_darwin_arm64.tar.gz"),
    ("Darwin", "x86_64", "gitleaks_8.30.1_darwin_x64.tar.gz"),
    ("Windows", "AMD64", "gitleaks_8.30.1_windows_x64.zip"),
])
def test_gitleaks_asset_per_platform(system, machine, asset):
    assert sg.gitleaks_asset(system, machine) == asset
    assert asset in sg.GITLEAKS_SHA256


def test_gitleaks_asset_unknown_platform_raises():
    with pytest.raises(GuardError):
        sg.gitleaks_asset("SunOS", "sparc")


class FakeResp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()


def tar_gz(members: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w:gz") as t:
        for name, data in members.items():
            info = tarfile.TarInfo(name)
            info.size = len(data)
            t.addfile(info, io.BytesIO(data))
    return buf.getvalue()


def zip_data(members: dict[str, bytes]) -> bytes:
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in members.items():
            z.writestr(name, data)
    return buf.getvalue()


def fake_release(monkeypatch, asset: str, data: bytes, sha: str | None = None):
    import hashlib
    monkeypatch.setitem(sg.GITLEAKS_SHA256, asset, sha or hashlib.sha256(data).hexdigest())
    urls = []

    def opener(req, timeout):
        urls.append((req.full_url, timeout))
        return FakeResp(data)
    return opener, urls


def test_install_gitleaks_linux_extracts_only_binary(tmp_path: Path, monkeypatch):
    asset = "gitleaks_8.30.1_linux_x64.tar.gz"
    opener, urls = fake_release(monkeypatch, asset, tar_gz({"LICENSE": b"l", "gitleaks": b"BIN", "../evil": b"x"}))
    path = sg.install_gitleaks(tmp_path / ".tools", opener, "Linux", "x86_64")
    assert path == tmp_path / ".tools" / "gitleaks"
    assert path.read_bytes() == b"BIN"
    assert path.stat().st_mode & 0o111 == 0o111
    assert urls[0][0].endswith(f"/v8.30.1/{asset}") and urls[0][1] > 0
    assert sorted(p.name for p in (tmp_path / ".tools").iterdir()) == ["gitleaks"]


def test_install_gitleaks_windows_zip(tmp_path: Path, monkeypatch):
    asset = "gitleaks_8.30.1_windows_x64.zip"
    opener, _ = fake_release(monkeypatch, asset, zip_data({"gitleaks.exe": b"EXE"}))
    path = sg.install_gitleaks(tmp_path, opener, "Windows", "AMD64")
    assert path.name == "gitleaks.exe" and path.read_bytes() == b"EXE"


def test_install_gitleaks_sha_mismatch_raises_and_writes_nothing(tmp_path: Path, monkeypatch):
    asset = "gitleaks_8.30.1_linux_x64.tar.gz"
    opener, _ = fake_release(monkeypatch, asset, tar_gz({"gitleaks": b"BIN"}), sha="0" * 64)
    with pytest.raises(GuardError, match="SHA256"):
        sg.install_gitleaks(tmp_path / ".tools", opener, "Linux", "x86_64")
    assert not (tmp_path / ".tools" / "gitleaks").exists()


def test_install_gitleaks_archive_without_binary_raises(tmp_path: Path, monkeypatch):
    asset = "gitleaks_8.30.1_linux_x64.tar.gz"
    opener, _ = fake_release(monkeypatch, asset, tar_gz({"README.md": b"r"}))
    with pytest.raises(GuardError):
        sg.install_gitleaks(tmp_path, opener, "Linux", "x86_64")


def test_install_gitleaks_network_error_raises(tmp_path: Path):
    def opener(req, timeout):
        raise TimeoutError("slow")
    with pytest.raises(GuardError):
        sg.install_gitleaks(tmp_path, opener, "Linux", "x86_64")


def test_find_gitleaks_missing_raises_with_setup_hint(tmp_path: Path):
    with pytest.raises(GuardError, match="setup_hooks"):
        sg.find_gitleaks(tmp_path)


# ---------------- 實際跑 hook（需要 .tools/ 裡的 gitleaks；CI 設 REQUIRE_GITLEAKS=1 時沒裝就失敗） ----------------
def _gitleaks_or_skip() -> Path:
    try:
        return sg.find_gitleaks(REPO)
    except GuardError:
        if os.environ.get("REQUIRE_GITLEAKS") == "1":
            raise
        pytest.skip("沒有 .tools/gitleaks（執行 tools/setup_hooks.sh 安裝）")


def fake_token() -> str:
    """執行時才組出來，repo 裡不會有長得像金鑰的字串"""
    return "ghp_" + "".join(random.choices(string.ascii_letters + string.digits, k=36))


@pytest.fixture
def repo(tmp_path: Path) -> Path:
    gl = _gitleaks_or_skip()
    r = tmp_path / "r"
    r.mkdir()
    git(r, "init", "-q", "-b", "main")
    (r / "tools").mkdir()
    (r / "tools" / "secret_guard.py").write_bytes(SCRIPT.read_bytes())
    (r / ".tools").mkdir()
    (r / ".tools" / gl.name).symlink_to(gl)
    (r / "README.md").write_text("hello\n")
    git(r, "add", "README.md")
    git(r, "commit", "-q", "-m", "init")
    return r


def git(r: Path, *args: str) -> str:
    return subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid",
                           "-c", "core.hooksPath=/dev/null", *args],
                          cwd=r, check=True, capture_output=True, text=True, timeout=60).stdout


def guard(r: Path, *args: str, stdin: str = "") -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(r / "tools" / "secret_guard.py"), *args], cwd=r,
                          input=stdin, capture_output=True, text=True, timeout=120)


def test_pre_commit_passes_clean_change(repo: Path):
    (repo / "main.c").write_text("int main(void) { return 0; }\n")
    git(repo, "add", "main.c")
    res = guard(repo, "pre-commit")
    assert res.returncode == 0, res.stdout + res.stderr


def test_pre_commit_blocks_staged_token_and_redacts_it(repo: Path):
    tok = fake_token()
    (repo / "config.py").write_text(f'GITHUB_TOKEN = "{tok}"\n')
    git(repo, "add", "config.py")
    res = guard(repo, "pre-commit")
    assert res.returncode == 1
    assert "config.py" in res.stdout + res.stderr
    assert tok not in res.stdout + res.stderr


def test_pre_commit_blocks_rename_to_sensitive_name(repo: Path):
    (repo / "notes.txt").write_text("x=1\n")
    git(repo, "add", "notes.txt")
    git(repo, "commit", "-q", "-m", "notes")
    git(repo, "mv", "-f", "notes.txt", ".env")
    res = guard(repo, "pre-commit")
    assert res.returncode == 1
    assert ".env" in res.stderr


def test_pre_commit_allow_file_lets_public_cert_through(repo: Path):
    (repo / sg.ALLOW_FILE).write_text("certs/*_cert.pem\n")
    (repo / "certs").mkdir()
    (repo / "certs" / "ca_cert.pem").write_text("-----BEGIN CERTIFICATE-----\nMIIB\n-----END CERTIFICATE-----\n")
    git(repo, "add", "-f", sg.ALLOW_FILE, "certs/ca_cert.pem")
    res = guard(repo, "pre-commit")
    assert res.returncode == 0, res.stdout + res.stderr


def test_pre_commit_blocks_sensitive_filename(repo: Path):
    (repo / ".env").write_text("NOTHING=1\n")
    git(repo, "add", "-f", ".env")
    res = guard(repo, "pre-commit")
    assert res.returncode == 1
    assert ".env" in res.stdout + res.stderr


def test_pre_push_blocks_commit_with_token(repo: Path):
    base = git(repo, "rev-parse", "HEAD").strip()
    (repo / "config.py").write_text(f'TOKEN = "{fake_token()}"\n')
    git(repo, "add", "config.py")
    git(repo, "commit", "-q", "-m", "oops")
    head = git(repo, "rev-parse", "HEAD").strip()
    res = guard(repo, "pre-push", stdin=f"refs/heads/main {head} refs/heads/main {base}\n")
    assert res.returncode == 1, res.stdout + res.stderr


def test_pre_push_new_branch_scans_unpushed_commits(repo: Path):
    tok = fake_token()
    git(repo, "checkout", "-q", "-b", "feat")
    (repo / "config.py").write_text(f'TOKEN = "{tok}"\n')
    git(repo, "add", "config.py")
    git(repo, "commit", "-q", "-m", "oops")
    head = git(repo, "rev-parse", "HEAD").strip()
    res = guard(repo, "pre-push", "origin", "url", stdin=f"refs/heads/feat {head} refs/heads/feat {Z}\n")
    assert res.returncode == 1, res.stdout + res.stderr
    assert tok not in res.stdout + res.stderr


def test_pre_push_new_branch_clean_passes(repo: Path):
    git(repo, "checkout", "-q", "-b", "feat")
    (repo / "a.txt").write_text("fine\n")
    git(repo, "add", "a.txt")
    git(repo, "commit", "-q", "-m", "ok")
    head = git(repo, "rev-parse", "HEAD").strip()
    res = guard(repo, "pre-push", stdin=f"refs/heads/feat {head} refs/heads/feat {Z}\n")
    assert res.returncode == 0, res.stdout + res.stderr


def test_pre_push_finds_token_added_in_merge_commit(repo: Path):
    """解衝突時才寫進 merge commit 的金鑰也要抓到"""
    base = git(repo, "rev-parse", "HEAD").strip()
    (repo / "c.py").write_text("A = 1\n")
    git(repo, "add", "c.py")
    git(repo, "commit", "-q", "-m", "main side")
    git(repo, "checkout", "-q", "-b", "side", base)
    (repo / "c.py").write_text("A = 2\n")
    git(repo, "add", "c.py")
    git(repo, "commit", "-q", "-m", "side")
    git(repo, "checkout", "-q", "main")
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "merge", "-q", "side"],
                   cwd=repo, capture_output=True, timeout=60)   # 會衝突
    (repo / "c.py").write_text(f'A = 3\nTOKEN = "{fake_token()}"\n')
    git(repo, "add", "c.py")
    git(repo, "commit", "-q", "--no-edit")
    head = git(repo, "rev-parse", "HEAD").strip()
    res = guard(repo, "pre-push", stdin=f"refs/heads/main {head} refs/heads/main {base}\n")
    assert res.returncode == 1, res.stdout + res.stderr


def test_pre_push_passes_clean_commits(repo: Path):
    base = git(repo, "rev-parse", "HEAD").strip()
    (repo / "a.txt").write_text("fine\n")
    git(repo, "add", "a.txt")
    git(repo, "commit", "-q", "-m", "ok")
    head = git(repo, "rev-parse", "HEAD").strip()
    res = guard(repo, "pre-push", stdin=f"refs/heads/main {head} refs/heads/main {base}\n")
    assert res.returncode == 0, res.stdout + res.stderr


def test_ci_scan_finds_token_in_history_even_if_deleted(repo: Path):
    (repo / "config.py").write_text(f'TOKEN = "{fake_token()}"\n')
    git(repo, "add", "config.py")
    git(repo, "commit", "-q", "-m", "oops")
    git(repo, "rm", "-q", "config.py")
    git(repo, "commit", "-q", "-m", "remove")
    res = guard(repo, "scan")
    assert res.returncode == 1, res.stdout + res.stderr


def test_ci_scan_redacts_token(repo: Path):
    tok = fake_token()
    (repo / "config.py").write_text(f'TOKEN = "{tok}"\n')
    git(repo, "add", "config.py")
    git(repo, "commit", "-q", "-m", "oops")
    res = guard(repo, "scan")
    assert res.returncode == 1
    assert tok not in res.stdout + res.stderr


def test_ci_scan_finds_file_renamed_to_sensitive_name(repo: Path):
    (repo / "notes.txt").write_text("x=1\n")
    git(repo, "add", "notes.txt")
    git(repo, "commit", "-q", "-m", "notes")
    git(repo, "mv", "-f", "notes.txt", ".env")
    git(repo, "commit", "-q", "-m", "rename")
    git(repo, "rm", "-q", ".env")
    git(repo, "commit", "-q", "-m", "remove")
    res = guard(repo, "scan")
    assert res.returncode == 1
    assert ".env" in res.stderr


def test_ci_scan_finds_sensitive_file_ever_committed(repo: Path):
    (repo / "server.key").write_text("not really a key\n")
    git(repo, "add", "-f", "server.key")
    git(repo, "commit", "-q", "-m", "oops")
    git(repo, "rm", "-q", "server.key")
    git(repo, "commit", "-q", "-m", "remove")
    res = guard(repo, "scan")
    assert res.returncode == 1
    assert "server.key" in res.stdout + res.stderr


def test_ci_scan_clean_repo_passes(repo: Path):
    res = guard(repo, "scan")
    assert res.returncode == 0, res.stdout + res.stderr
