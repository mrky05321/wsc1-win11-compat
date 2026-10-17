"""Explicit allowlists for the four-file runtime ZIP and portable source ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
import zipfile

HERE = Path(__file__).resolve().parent
VERSION = "1.0.0"
BACKEND_ARCHIVE_SHA256 = "3894368ebce69f348d08e189c2f0818c49aa3b18ad01e06348743bf6a29af97a"
BACKEND_DLL_SHA256 = "ec42e51cbb4408518d6348706557b18ef50a87485c8bfa1839c895123fa3295f"
SOURCE_FILES = (
    ".gitignore", "CMakeLists.txt", "build.ps1", "package.py", "dxwrapper.ini",
    "README.md", "LICENSE", "THIRD_PARTY_NOTICES.txt",
    "src/proxy.cpp", "src/runtime_fixes.inl", "src/ddraw.def",
    "tools/extract_cd_audio.py",
)
RUNTIME_NAMES = ("ddraw.dll", "dxwrapper.dll", "dxwrapper.ini", "README.txt")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def write_zip(path, members):
    # Atomic publication: do not leave a partly written release asset.
    with tempfile.NamedTemporaryFile(dir=path.parent, suffix=".tmp", delete=False) as f:
        temporary = Path(f.name)
    try:
        with zipfile.ZipFile(temporary, "w", zipfile.ZIP_DEFLATED) as z:
            for name, data in members.items():
                info = zipfile.ZipInfo(name, (2026, 10, 16, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = 0o100644 << 16
                z.writestr(info, data)
        temporary.replace(path)
    finally:
        if temporary.exists():
            temporary.unlink()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--backend-archive", type=Path, default=HERE / ".cache/dxwrapper.zip")
    parser.add_argument("--proxy", type=Path, default=HERE / "build/Release/ddraw.dll")
    args = parser.parse_args()
    if not args.backend_archive.is_file() or not args.proxy.is_file():
        raise SystemExit("Provide the pinned dxwrapper.zip and a built x86 ddraw.dll")
    if digest(args.backend_archive.read_bytes()) != BACKEND_ARCHIVE_SHA256:
        raise SystemExit("dxwrapper archive SHA-256 mismatch")
    with zipfile.ZipFile(args.backend_archive) as archive:
        backend = archive.read("dxwrapper.dll")
        upstream_notice = archive.read("License.txt")
    if digest(backend) != BACKEND_DLL_SHA256:
        raise SystemExit("dxwrapper DLL SHA-256 mismatch")
    if (HERE / "THIRD_PARTY_NOTICES.txt").read_bytes() != upstream_notice:
        raise SystemExit("The complete upstream license notice must be preserved")
    proxy = args.proxy.read_bytes()
    if proxy[:2] != b"MZ":
        raise SystemExit("Proxy is not a Windows PE file")
    pe = int.from_bytes(proxy[0x3C:0x40], "little")
    if proxy[pe:pe+6] != b"PE\x00\x00\x4c\x01":
        raise SystemExit("Proxy must be an x86 PE file")
    readme = (HERE / "README.md").read_text(encoding="utf-8")
    readme += "\n\n---\nOriginal compatibility code license\n\n" + (HERE / "LICENSE").read_text(encoding="utf-8")
    readme += "\n\n---\nUnmodified dxwrapper: complete upstream notices\n\n" + upstream_notice.decode("utf-8-sig")
    members = {
        "ddraw.dll": proxy,
        "dxwrapper.dll": backend,
        "dxwrapper.ini": (HERE / "dxwrapper.ini").read_bytes(),
        "README.txt": readme.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8-sig"),
    }
    if tuple(members) != RUNTIME_NAMES:
        raise SystemExit("Unexpected runtime file list")
    artifacts = HERE / "artifacts"
    artifacts.mkdir(exist_ok=True)
    runtime_zip = artifacts / ("wsc1-win11-compat-v" + VERSION + ".zip")
    source_zip = artifacts / ("wsc1-win11-compat-source-v" + VERSION + ".zip")
    source = {name: (HERE / name).read_bytes() for name in SOURCE_FILES}
    write_zip(runtime_zip, members)
    write_zip(source_zip, source)
    (artifacts / "SHA256SUMS.txt").write_text("".join(
        digest(p.read_bytes()) + "  " + p.name + "\n" for p in (runtime_zip, source_zip)), encoding="ascii")
    audit = {
        "package_version": VERSION, "proxy_version": "1.0.0",
        "runtime_file_count": len(members), "install_file_count": 3,
        "runtime_files": {n: digest(b) for n, b in members.items()},
        "source_files": list(SOURCE_FILES),
        "backend_archive_sha256": BACKEND_ARCHIVE_SHA256,
        "backend_notice_unmodified": True,
        "game_files_and_music_included": False,
        "runtime_sha256": digest(runtime_zip.read_bytes()),
        "source_sha256": digest(source_zip.read_bytes()),
        "game_launched": False, "tests_run": False,
    }
    (artifacts / "package-audit.json").write_text(json.dumps(audit, indent=2), encoding="utf-8")
    print("Created " + str(runtime_zip) + " (4 files; copy 3 into the game folder)")
    print("Created " + str(source_zip))


if __name__ == "__main__":
    main()
