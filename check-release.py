#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""check-release -- build and check a crt-bridge client release archive.

Two subcommands, stdlib-only (no third-party dependency, no network access):

    check-release.py archive --platform {windows,linux,macos} --version vX.Y.Z \\
        --build-dir DIR --docs-dir DIR --out-dir DIR [--objdump PATH]
        Builds one archive from an explicit six-file allowlist: the core
        binary and its `.info` (from --build-dir), plus LICENSE,
        NOTICE.txt, README.md and INSTALL.md (from --docs-dir). Nothing
        else goes in. Writes
        `OUT-DIR/crt-bridge-client-VERSION-<platform-suffix><ext>`
        (windows-x64.zip, linux-x64.tar.gz, macos-universal.zip), then runs
        `check` on the archive it just wrote and returns that result.

    check-release.py check --platform {windows,linux,macos} --version vX.Y.Z \\
        --archive FILE [--objdump PATH]
        Verifies a built archive: file name, exact member list (read in
        memory only -- never extracted to disk, so a hostile archive
        cannot write outside the output directory), binary architecture,
        the 25 `retro_*` libretro v1 exports, Windows import set (requires
        --objdump), the `.info`/binary version, and text members for
        leaked private addresses, machine-specific paths or the retired
        core name.

Exit codes: 0 (PASS), 1 (one or more "FAIL <rule> <detail>" lines printed),
2 (usage or environment error).

This file ships in the public client repository and runs in public CI. It
never sees, and does not need, the private forbidden-terms list this
project's own scripts/guard-fork-publish.py checks against -- the regexes
below (private IPv4 ranges, MAC addresses, machine-specific home paths) are
duplicated from that private script on purpose, so this file stays
self-contained outside the private repository.
"""
from __future__ import annotations

import argparse
import re
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Platform tables
# ---------------------------------------------------------------------------

VERSION_RE = re.compile(r"^v(\d+\.\d+\.\d+)$")

PLATFORM_SUFFIX = {"windows": "windows-x64", "linux": "linux-x64", "macos": "macos-universal"}
BINARY_EXT = {"windows": "dll", "linux": "so", "macos": "dylib"}
ARCHIVE_EXT = {"windows": ".zip", "linux": ".tar.gz", "macos": ".zip"}

# The 25 `retro_*` entry points of the libretro v1 API (RETRO_API declarations
# in client/include/libretro.h), measured with
# `grep -o 'RETRO_API [^(]*retro_[a-z_]*' client/include/libretro.h`.
RETRO_EXPORTS = (
    "retro_api_version",
    "retro_cheat_reset",
    "retro_cheat_set",
    "retro_deinit",
    "retro_get_memory_data",
    "retro_get_memory_size",
    "retro_get_region",
    "retro_get_system_av_info",
    "retro_get_system_info",
    "retro_init",
    "retro_load_game",
    "retro_load_game_special",
    "retro_reset",
    "retro_run",
    "retro_serialize",
    "retro_serialize_size",
    "retro_set_audio_sample",
    "retro_set_audio_sample_batch",
    "retro_set_controller_port_device",
    "retro_set_environment",
    "retro_set_input_poll",
    "retro_set_input_state",
    "retro_set_video_refresh",
    "retro_unload_game",
    "retro_unserialize",
)

WINDOWS_SYSTEM_DLLS = {
    "kernel32.dll",
    "msvcrt.dll",
    "ws2_32.dll",
    "winmm.dll",
    "advapi32.dll",
    "user32.dll",
}

DOC_NAMES = ("LICENSE", "NOTICE.txt", "README.md", "INSTALL.md")

# ---------------------------------------------------------------------------
# Leak scan -- duplicated from scripts/guard-fork-publish.py (see module
# docstring for why this file cannot import it).
# ---------------------------------------------------------------------------

_OCTET = r"(?:25[0-5]|2[0-4][0-9]|1[0-9][0-9]|[1-9]?[0-9])"
PRIVATE_IPV4_RE = re.compile(
    (
        r"\b(?:"
        r"10\.{o}\.{o}\.{o}"
        r"|172\.(?:1[6-9]|2[0-9]|3[01])\.{o}\.{o}"
        r"|192\.168\.{o}\.{o}"
        r")\b"
    ).format(o=_OCTET)
)
MAC_RE = re.compile(r"\b(?:[0-9A-Fa-f]{2}[:-]){5}[0-9A-Fa-f]{2}\b")
_MAC_DOC_PREFIX = "00:00:5e:00:53"  # RFC 7042 documentation range, not a finding.
MACHINE_PATH_RE = re.compile(
    r"(?i)\b[a-z]:\\users\\|/home/[^/\s]+|/Users/[^/\s]+|~[A-Za-z0-9_.-]+[\\/]"
)
RETIRED_NAME_RE = re.compile(r"groovy_libretro|Groovy Client")


def _is_doc_mac(matched: str) -> bool:
    return matched.replace("-", ":").lower().startswith(_MAC_DOC_PREFIX)


def _scan_leak(text: str) -> list[str]:
    """Return human-readable leak descriptions found in `text`."""
    findings: list[str] = []
    for m in PRIVATE_IPV4_RE.finditer(text):
        findings.append(f"private IPv4 address {m.group(0)!r}")
    for m in MAC_RE.finditer(text):
        if _is_doc_mac(m.group(0)):
            continue
        findings.append(f"MAC address {m.group(0)!r}")
    for m in MACHINE_PATH_RE.finditer(text):
        findings.append(f"machine-specific path {m.group(0)!r}")
    for m in RETIRED_NAME_RE.finditer(text):
        findings.append(f"retired core name {m.group(0)!r}")
    return findings


# ---------------------------------------------------------------------------
# Architecture rule -- pure Python, no external tool.
# ---------------------------------------------------------------------------


def _check_pe(data: bytes) -> Optional[str]:
    if len(data) < 0x40 or data[0:2] != b"MZ":
        return "not a PE file (missing 'MZ' signature)"
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    if len(data) < e_lfanew + 26:
        return "PE header truncated"
    if data[e_lfanew : e_lfanew + 4] != b"PE\x00\x00":
        return "missing 'PE\\0\\0' signature"
    machine = struct.unpack_from("<H", data, e_lfanew + 4)[0]
    if machine != 0x8664:
        return f"Machine 0x{machine:04x}, expected 0x8664 (x86-64)"
    magic = struct.unpack_from("<H", data, e_lfanew + 24)[0]
    if magic != 0x20B:
        return f"optional header Magic 0x{magic:04x}, expected 0x020b (PE32+)"
    return None


def _check_elf(data: bytes) -> Optional[str]:
    if len(data) < 20 or data[0:4] != b"\x7fELF":
        return "not an ELF file (missing magic)"
    ei_class = data[4]
    if ei_class != 2:
        return f"ELF class {ei_class}, expected 2 (ELFCLASS64)"
    e_type = struct.unpack_from("<H", data, 16)[0]
    e_machine = struct.unpack_from("<H", data, 18)[0]
    if e_machine != 62:
        return f"e_machine {e_machine}, expected 62 (EM_X86_64)"
    if e_type != 3:
        return f"e_type {e_type}, expected 3 (ET_DYN)"
    return None


def _check_macho_universal(data: bytes) -> Optional[str]:
    if len(data) < 8 or struct.unpack_from(">I", data, 0)[0] != 0xCAFEBABE:
        return "missing fat (universal) magic 0xcafebabe"
    nfat = struct.unpack_from(">I", data, 4)[0]
    if nfat != 2:
        return f"{nfat} slice(s) in the fat header, expected exactly 2"
    types = []
    for i in range(nfat):
        off = 8 + i * 20
        if len(data) < off + 4:
            return "fat header truncated"
        types.append(struct.unpack_from(">I", data, off)[0])
    expected = {0x0100000C, 0x01000007}
    if set(types) != expected:
        got = ", ".join(hex(t) for t in types)
        return f"fat slices [{got}], expected exactly arm64 (0x100000c) and x86_64 (0x1000007)"
    return None


_ARCH_CHECK = {"windows": _check_pe, "linux": _check_elf, "macos": _check_macho_universal}


def _rule_arch(data: bytes, platform: str) -> list[str]:
    detail = _ARCH_CHECK[platform](data)
    return [detail] if detail else []


# ---------------------------------------------------------------------------
# Exports / imports rules
# ---------------------------------------------------------------------------


class ObjdumpError(RuntimeError):
    """objdump could not be run, or exited non-zero. Distinct from an empty
    (but successful) parse -- a silent, empty result used to be read
    as "inconclusive, not a finding", which let a genuine objdump failure
    (bad path conversion under MSYS2, a truncated binary, an unrecognized
    format) pass every check that depends on it."""


def _run_objdump(binary_path: Path, objdump: str) -> str:
    try:
        result = subprocess.run(
            [objdump, "-p", str(binary_path)], capture_output=True, text=True, timeout=60
        )
    except OSError as exc:
        raise ObjdumpError(f"cannot run {objdump!r}: {exc}") from exc
    if result.returncode != 0:
        detail = result.stderr.strip() or "(no stderr)"
        raise ObjdumpError(f"{objdump!r} exited {result.returncode}: {detail}")
    return result.stdout


def _pe_export_names(binary_path: Path, objdump: str) -> set[str]:
    stdout = _run_objdump(binary_path, objdump)
    names: set[str] = set()
    # The "[Ordinal/Name Pointer] Table" rows of "The Export Tables
    # (interpreted .edata section contents)", as GNU binutils' objdump -p
    # actually prints them for a PE32+ image:
    #   "\t[   0] +base[   1]  0000 retro_api_version"
    # -- ordinal index, "+base[ordinal]", a hex hint, then the name (always
    # the last field). Measured against this project's own MinGW toolchain
    # (the previous, simpler pattern -- one bracketed index then a
    # single trailing token -- never matched this real four-field form; it
    # only ever matched the "Export Address Table" rows just above, which
    # end in two words ("Export RVA") and so never matched either, leaving
    # objdump_names permanently empty and this whole cross-check silently
    # inert since it was added).
    entry_re = re.compile(r"^\s*\[\s*\d+\]\s+\+base\[\s*\d+\]\s+[0-9a-fA-F]+\s+(\S+)\s*$")
    for line in stdout.splitlines():
        m = entry_re.match(line)
        if m:
            names.add(m.group(1))
    return names


def _dll_import_names(binary_path: Path, objdump: str) -> list[str]:
    stdout = _run_objdump(binary_path, objdump)
    names: list[str] = []
    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith("DLL Name:"):
            names.append(line.split(":", 1)[1].strip())
    return names


def _rule_exports(data: bytes, platform: str, objdump_names: Optional[set]) -> list[str]:
    findings: list[str] = []
    prefix = b"_" if platform == "macos" else b""
    missing = [n for n in RETRO_EXPORTS if (prefix + n.encode("ascii") + b"\x00") not in data]
    if missing:
        findings.append("missing symbol(s) in binary bytes: " + ", ".join(missing))
    # Additional, Windows-only confirmation via the PE export table. `None`
    # means objdump failed or was not run (already reported by the caller);
    # an empty-but-successful parse is itself reported by the caller too --
    # by the time objdump_names reaches here it is either `None`
    # or non-empty, so this block only adds the by-name comparison.
    if objdump_names:
        missing2 = [n for n in RETRO_EXPORTS if n not in objdump_names]
        if missing2:
            findings.append(
                "missing symbol(s) in objdump export table: " + ", ".join(missing2)
            )
    return findings


def _rule_imports(platform: str, objdump: Optional[str], binary_path: Optional[Path]) -> list[str]:
    if platform != "windows":
        return []
    if not objdump:
        return ["--objdump is required to check Windows imports"]
    if binary_path is None:
        return ["binary unavailable, cannot check imports"]
    try:
        names = _dll_import_names(binary_path, objdump)
    except ObjdumpError as exc:
        return [f"objdump failed while reading imports: {exc}"]
    if not names:
        # Every MinGW-built DLL imports at least kernel32.dll. An
        # empty result means the import table could not actually be read,
        # not that the DLL imports nothing -- a finding, not a PASS.
        return ["no DLL imports found -- objdump's import table appears empty or unreadable"]
    bad = sorted({n for n in names if n.lower() not in WINDOWS_SYSTEM_DLLS})
    if bad:
        return ["non-system import(s): " + ", ".join(bad)]
    return []


# ---------------------------------------------------------------------------
# Version rule
# ---------------------------------------------------------------------------


def _info_field(info_text: str, key: str) -> Optional[str]:
    m = re.search(rf'^{re.escape(key)}\s*=\s*"([^"]*)"\s*$', info_text, re.MULTILINE)
    return m.group(1) if m else None


def _rule_version(info_text: str, binary_data: bytes, version_number: str) -> list[str]:
    findings: list[str] = []
    display_version = _info_field(info_text, "display_version")
    if display_version != version_number:
        findings.append(f".info display_version = {display_version!r}, expected {version_number!r}")
    corename = _info_field(info_text, "corename")
    if corename != "crt-bridge client":
        findings.append(f".info corename = {corename!r}, expected 'crt-bridge client'")
    supports_no_game = _info_field(info_text, "supports_no_game")
    if supports_no_game != "true":
        findings.append(f".info supports_no_game = {supports_no_game!r}, expected 'true'")
    if (version_number.encode("ascii") + b"\x00") not in binary_data:
        findings.append(f"binary does not contain version bytes {version_number!r}")
    return findings


# ---------------------------------------------------------------------------
# Text rule
# ---------------------------------------------------------------------------


def _rule_text(member_name: str, text: str) -> list[str]:
    return [f"{member_name}: {leak}" for leak in _scan_leak(text)]


# ---------------------------------------------------------------------------
# Member-name safety (zip slip / path traversal)
# ---------------------------------------------------------------------------


def _is_dangerous_member_name(name: str) -> bool:
    if name.startswith("/") or name.startswith("\\"):
        return True
    if re.match(r"^[A-Za-z]:", name):
        return True
    return ".." in name.replace("\\", "/").split("/")


def _read_archive(archive_path: Path, platform: str) -> list[tuple]:
    """Return a (name, kind, data) list for every member -- kind is "dir",
    "file" or "other" (symlink/special file); data is the member's bytes for
    "file", else None. Reads every member into memory; never writes anything
    to disk (an archive read this way cannot escape into the filesystem
    through a crafted member path)."""
    entries: list[tuple] = []
    if platform == "linux":
        with tarfile.open(archive_path, "r:gz") as tf:
            for member in tf.getmembers():
                name = member.name.replace("\\", "/")
                if member.isdir():
                    entries.append((name, "dir", None))
                elif member.isfile():
                    fh = tf.extractfile(member)
                    entries.append((name, "file", fh.read() if fh else b""))
                else:
                    entries.append((name, "other", None))
    else:
        with zipfile.ZipFile(archive_path) as zf:
            for info in zf.infolist():
                name = info.filename.replace("\\", "/")
                if name.endswith("/"):
                    entries.append((name, "dir", None))
                    continue
                # A zip member whose external_attr encodes S_IFLNK is a
                # symlink, not a regular file -- the tar branch above already
                # refuses one via TarInfo.isfile(); this makes the zip branch
                # match it. No archive produced here is ever extracted (this
                # tool always reads members into memory), but the tool is
                # documented as replayable by hand against any archive.
                unix_mode = (info.external_attr >> 16) & 0xFFFF
                if unix_mode and stat.S_ISLNK(unix_mode):
                    entries.append((name, "other", None))
                else:
                    entries.append((name, "file", zf.read(info)))
    return entries


def _rule_members(entries: list[tuple], expected: set) -> tuple[list[str], dict]:
    findings: list[str] = []
    contents: dict = {}
    seen: set = set()
    for name, kind, data in entries:
        if _is_dangerous_member_name(name):
            findings.append(f"dangerous member path {name!r}")
            continue
        if kind == "dir":
            continue
        if kind == "other":
            findings.append(f"non-regular member {name!r} (symlink or special file)")
            continue
        if name not in expected:
            findings.append(f"unexpected member {name!r}")
            continue
        seen.add(name)
        contents[name] = data
    for name in sorted(expected - seen):
        findings.append(f"missing member {name!r}")
    return findings, contents


def _rule_name(archive_path: Path, platform: str, version_tag: str) -> list[str]:
    expected = f"crt-bridge-client-{version_tag}-{PLATFORM_SUFFIX[platform]}{ARCHIVE_EXT[platform]}"
    if archive_path.name != expected:
        return [f"file name is {archive_path.name!r}, expected {expected!r}"]
    return []


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------


def cmd_check(args: argparse.Namespace) -> int:
    m = VERSION_RE.match(args.version)
    if not m:
        print(f"ERROR: --version {args.version!r} does not match vX.Y.Z", file=sys.stderr)
        return 2
    version_number = m.group(1)
    platform = args.platform
    archive_path = Path(args.archive)
    if not archive_path.is_file():
        print(f"ERROR: --archive not found: {archive_path}", file=sys.stderr)
        return 2

    findings: list[tuple[str, str]] = []
    findings.extend(("name", d) for d in _rule_name(archive_path, platform, args.version))

    root_name = f"crt-bridge-client-{args.version}-{PLATFORM_SUFFIX[platform]}"
    binary_rel = f"{root_name}/crt_bridge_libretro.{BINARY_EXT[platform]}"
    info_rel = f"{root_name}/crt_bridge_libretro.info"
    text_rels = [f"{root_name}/{name}" for name in DOC_NAMES]
    expected = {binary_rel, info_rel, *text_rels}

    try:
        entries = _read_archive(archive_path, platform)
    except (zipfile.BadZipFile, tarfile.TarError, OSError) as exc:
        print(f"FAIL members {archive_path}: cannot open archive ({exc})")
        print("FAIL (1)")
        return 1

    member_findings, contents = _rule_members(entries, expected)
    findings.extend(("members", d) for d in member_findings)

    binary_data = contents.get(binary_rel)
    info_data = contents.get(info_rel)

    if binary_data is not None:
        findings.extend(("arch", d) for d in _rule_arch(binary_data, platform))
        if platform == "windows" and args.objdump:
            with tempfile.TemporaryDirectory(prefix="check-release-") as tmp:
                binary_tmp = Path(tmp) / f"crt_bridge_libretro.{BINARY_EXT[platform]}"
                binary_tmp.write_bytes(binary_data)
                try:
                    objdump_names = _pe_export_names(binary_tmp, args.objdump)
                except ObjdumpError as exc:
                    findings.append(("exports", f"objdump failed while reading exports: {exc}"))
                    objdump_names = None
                else:
                    if not objdump_names:
                        # An empty parse means the export table itself
                        # could not be read -- a finding now, not a silent PASS.
                        findings.append(
                            ("exports", "objdump's export table is empty or unreadable")
                        )
                findings.extend(
                    ("exports", d) for d in _rule_exports(binary_data, platform, objdump_names)
                )
                findings.extend(("imports", d) for d in _rule_imports(platform, args.objdump, binary_tmp))
        else:
            findings.extend(("exports", d) for d in _rule_exports(binary_data, platform, None))
            findings.extend(("imports", d) for d in _rule_imports(platform, args.objdump, None))
    else:
        findings.append(("arch", f"{binary_rel}: missing, cannot check architecture"))
        findings.append(("exports", f"{binary_rel}: missing, cannot check exports"))
        if platform == "windows":
            findings.append(("imports", f"{binary_rel}: missing, cannot check imports"))

    if binary_data is not None and info_data is not None:
        info_text = info_data.decode("utf-8", errors="replace")
        findings.extend(("version", d) for d in _rule_version(info_text, binary_data, version_number))
    elif info_data is None:
        findings.append(("version", f"{info_rel}: missing, cannot check version"))

    for rel in [info_rel, *text_rels]:
        data = contents.get(rel)
        if data is None:
            continue  # already reported by the members rule
        text = data.decode("utf-8", errors="replace")
        findings.extend(("text", d) for d in _rule_text(rel, text))

    for rule, detail in findings:
        print(f"FAIL {rule} {detail}")
    if findings:
        print(f"FAIL ({len(findings)})")
        return 1
    print("PASS")
    return 0


# ---------------------------------------------------------------------------
# archive
# ---------------------------------------------------------------------------


def cmd_archive(args: argparse.Namespace) -> int:
    m = VERSION_RE.match(args.version)
    if not m:
        print(f"ERROR: --version {args.version!r} does not match vX.Y.Z", file=sys.stderr)
        return 2
    platform = args.platform
    build_dir = Path(args.build_dir)
    docs_dir = Path(args.docs_dir)
    out_dir = Path(args.out_dir)

    binary_name = f"crt_bridge_libretro.{BINARY_EXT[platform]}"
    binary_src = build_dir / binary_name
    info_src = build_dir / "crt_bridge_libretro.info"
    doc_srcs = [docs_dir / name for name in DOC_NAMES]

    for src in [binary_src, info_src, *doc_srcs]:
        if not src.is_file():
            print(f"ERROR: required input not found: {src}", file=sys.stderr)
            return 2

    root_name = f"crt-bridge-client-{args.version}-{PLATFORM_SUFFIX[platform]}"
    out_path = out_dir / f"{root_name}{ARCHIVE_EXT[platform]}"
    if out_path.exists():
        print(f"ERROR: output already exists: {out_path}", file=sys.stderr)
        return 2
    out_dir.mkdir(parents=True, exist_ok=True)

    members = [(binary_src, f"{root_name}/{binary_name}", True), (info_src, f"{root_name}/crt_bridge_libretro.info", False)]
    for name, src in zip(DOC_NAMES, doc_srcs):
        members.append((src, f"{root_name}/{name}", False))
    members.sort(key=lambda t: t[1])

    if platform in ("windows", "macos"):
        with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as zf:
            for src, arcname, executable in members:
                info = zipfile.ZipInfo(arcname, date_time=(1980, 1, 1, 0, 0, 0))
                info.compress_type = zipfile.ZIP_DEFLATED
                perm = 0o755 if executable else 0o644
                info.external_attr = (0o100000 | perm) << 16
                zf.writestr(info, src.read_bytes())
    else:
        executable_names = {arcname for _, arcname, executable in members if executable}

        def _filter(tarinfo: tarfile.TarInfo) -> tarfile.TarInfo:
            tarinfo.uid = 0
            tarinfo.gid = 0
            tarinfo.uname = "root"
            tarinfo.gname = "root"
            tarinfo.mtime = 0
            tarinfo.mode = 0o755 if tarinfo.name in executable_names else 0o644
            return tarinfo

        with tarfile.open(out_path, "w:gz") as tf:
            for src, arcname, _executable in members:
                tf.add(src, arcname=arcname, filter=_filter)

    print(f"archived: {out_path}")

    check_args = argparse.Namespace(
        platform=platform, version=args.version, archive=str(out_path), objdump=args.objdump
    )
    return cmd_check(check_args)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main(argv: Optional[list] = None) -> int:
    parser = argparse.ArgumentParser(prog="check-release.py", description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_archive = sub.add_parser("archive")
    p_archive.add_argument("--platform", required=True, choices=["windows", "linux", "macos"])
    p_archive.add_argument("--version", required=True)
    p_archive.add_argument("--build-dir", required=True)
    p_archive.add_argument("--docs-dir", required=True)
    p_archive.add_argument("--out-dir", required=True)
    p_archive.add_argument("--objdump")

    p_check = sub.add_parser("check")
    p_check.add_argument("--platform", required=True, choices=["windows", "linux", "macos"])
    p_check.add_argument("--version", required=True)
    p_check.add_argument("--archive", required=True)
    p_check.add_argument("--objdump")

    args = parser.parse_args(argv)
    if args.command == "archive":
        return cmd_archive(args)
    if args.command == "check":
        return cmd_check(args)
    return 2  # unreachable -- argparse subparsers are required


if __name__ == "__main__":
    raise SystemExit(main())
