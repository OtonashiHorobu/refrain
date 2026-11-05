#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Platform abstraction guard (CI).

The rule: OS-specific APIs may only be
called from the platform layer (`src/platform/**`, `include/refrain/platform/**`).
Everything else -- kernel, services, mods, tools, tests -- goes through the
`refrain::platform` abstractions. This keeps the engine portable, testable and
future-proof (a different OS backend / sandboxed loader can be swapped in).

Usage:
    python3 tools/platform_check.py [--verbose]

Exit code 0 = clean, 1 = violations found.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

ALLOWED_PREFIXES = (
    Path("src/platform"),
    Path("include/refrain/platform"),
)

# White-box tests of the platform layer itself: verifying crash/re-raise
# semantics requires the real OS primitives (fork/waitpid/setrlimit) to observe
# how a *child process* died, which cannot be done through the abstraction
# under test. Keep this list minimal; every addition needs a rationale here.
ALLOWED_WHITE_BOX_TESTS = (
    # Signals: fork + waitpid + rlimit to observe how the child died.
    Path("test/platform/test_process.cpp"),
    # File permissions: POSIX mode bits, compiled out on Windows (`#if
    # !defined(_WIN32)`) because the ACL model differs.
    Path("test/platform/test_file.cpp"),
    # Non-regular files: a FIFO (and device nodes) can only be created with
    # `mkfifo` and `<sys/stat.h>`, which is what the `read_file` rejection test
    # needs to prove the check happens before `open` (a real open on a FIFO
    # blocks). POSIX-only; compiled out on Windows (`#if !defined(_WIN32)`).
    Path("test/core/test_io.cpp"),
    # Console sink direction: proving that diagnostics go to stderr and never
    # to the command's stdout data channel is only observable at the stdio
    # descriptor level; the test dup2's fd 1/2 onto temp files and restores
    # them. A production "redirect stdio" abstraction would exist solely for
    # this test. POSIX-only (`#if !defined(_WIN32)`).
    Path("test/core/test_log.cpp"),
)

SOURCE_DIRS = ("include", "src", "test", "bench", "tools", "sdk", "schema",
               "gamepack")
SOURCE_SUFFIXES = {".hpp", ".h", ".cpp", ".cc", ".cxx", ".inl"}

# Bare tokens that indicate a direct OS API use (win32/loader/signals). They
# are matched after stripping comments, so documentation mentions are allowed.
FORBIDDEN_TOKENS = (
    "dlopen(",
    "dlsym(",
    "dlclose(",
    "sigaction(",
    "sigemptyset(",
    "sigprocmask(",
    "sigaltstack(",
    "pthread_create(",
    "SetUnhandledExceptionFilter(",
    "AddVectoredExceptionHandler(",
    "MoveFileExW(",
    "CreateFileW(",
    "CreateFileA(",
    "FlushFileBuffers(",
    "CloseHandle(",
    "LoadLibraryA(",
    "LoadLibraryW(",
    "GetProcAddress(",
    "FreeLibrary(",
    "GetLastError(",
)

# Global-scope OS calls: `::open(` etc. but not `fs::remove(` / `std::...::write(`.
FORBIDDEN_GLOBAL_CALLS = re.compile(
    r"(?<![A-Za-z0-9_])::(open|openat|write|read|close|fsync|fdatasync|rename|"
    r"remove|unlink|raise|kill|_exit|mmap|munmap|fork|waitpid|getpid|_getpid|"
    r"fcntl|ioctl|syscall|socket|poll|epoll_wait|epoll_create|epoll_ctl|"
    r"pthread_join|stat|lstat|mkdir|chmod|readlink|symlink|dup|dup2|pipe)\b"
)

# Bare OS calls without the `::` qualifier (`open(...)`). Without this rule a
# hand-written `extern "C"` declaration plus `open(...)` slips through the
# global-call check. `read`/`write`/`close`/`rename`/`remove` are deliberately
# excluded: they are also ordinary project API/member names
# (zip_reader::read, log::write, ...) and the token/global rules still catch
# their OS use; the remaining tokens have no plausible C++/project meaning.
FORBIDDEN_BARE_CALLS = re.compile(
    r"(?<![A-Za-z0-9_.>:])(open|openat|fsync|fdatasync|fcntl|ioctl|syscall|"
    r"socket|poll|epoll_wait|epoll_create|epoll_ctl|pthread_join|"
    r"pthread_create|unlink|mmap|munmap|fork|waitpid|getpid|dup|dup2|pipe|"
    r"readlink|symlink|chmod|mkdir|stat|lstat|raise|kill|_exit)\s*\("
)

# Declaration prefixes that make a matching name a *declaration* rather than a
# call (`auto open(...)`, `void write(...)`): these are project APIs named like
# OS calls, not OS calls.
_DECLARATION_PREFIXES = ("auto", "void", "bool", "int", "char", "const",
                         "static", "inline", "constexpr", "explicit",
                         "virtual", "friend", "->")


def is_declaration(code_line: str, match_start: int) -> bool:
    prefix = code_line[:match_start].rstrip()
    return any(prefix.endswith(word) for word in _DECLARATION_PREFIXES)

# OS-specific preprocessor branches are only allowed inside the platform
# layer (`src/platform/**`, `include/refrain/platform/**`). Kernel/services/
# tools/tests must go through the abstractions instead of `#if defined(_WIN32)`.
FORBIDDEN_WIN32_BRANCH = re.compile(
    r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*\b_WIN32\b"
)

FORBIDDEN_INCLUDES = (
    "<windows.h>",
    "<process.h>",
    "<fcntl.h>",
    "<unistd.h>",
    "<signal.h>",
    "<dlfcn.h>",
    "<sys/",
    "<pthread.h>",
)


def strip_comments(text: str) -> str:
    """Remove // and /* */ comments while preserving newlines/line numbers."""
    out: list[str] = []
    index = 0
    length = len(text)
    in_block = False
    in_string = False
    while index < length:
        character = text[index]
        if in_block:
            if character == "*" and index + 1 < length and text[index + 1] == "/":
                out.extend("\n" if c == "\n" else " " for c in text[index:index + 2])
                index += 2
                in_block = False
            else:
                out.append("\n" if character == "\n" else " ")
                index += 1
            continue
        if not in_string and character == "/" and index + 1 < length:
            if text[index + 1] == "/":
                while index < length and text[index] != "\n":
                    out.append(" ")
                    index += 1
                continue
            if text[index + 1] == "*":
                in_block = True
                out.extend((" ", " "))
                index += 2
                continue
        if character == '"' and (index == 0 or text[index - 1] != "\\"):
            in_string = not in_string
        out.append(character)
        index += 1
    return "".join(out)


def is_allowed(relative: Path) -> bool:
    if relative in ALLOWED_WHITE_BOX_TESTS:
        return True
    return any(relative == prefix or prefix in relative.parents
               for prefix in ALLOWED_PREFIXES)


def iter_sources():
    for directory in SOURCE_DIRS:
        base = ROOT / directory
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                yield path


def check(verbose: bool) -> int:
    violations: list[str] = []
    checked = 0

    for path in iter_sources():
        relative = path.relative_to(ROOT)
        if is_allowed(relative):
            continue
        checked += 1
        text = path.read_text(encoding="utf-8", errors="replace")
        # Strip comments (but not includes: they are checked separately) so
        # documentation comments can mention OS APIs.
        raw_lines = text.splitlines()
        code_lines = strip_comments(text).splitlines()
        for line_number, raw_line in enumerate(raw_lines, start=1):
            code_line = code_lines[line_number - 1] if line_number <= len(
                code_lines) else ""
            if raw_line.lstrip().startswith("#include"):
                for include in FORBIDDEN_INCLUDES:
                    if include in raw_line:
                        violations.append(
                            f"{relative}:{line_number}: include {include}")
            for token in FORBIDDEN_TOKENS:
                if token in code_line:
                    violations.append(
                        f"{relative}:{line_number}: OS API token '{token}'")
            for match in FORBIDDEN_GLOBAL_CALLS.finditer(code_line):
                violations.append(
                    f"{relative}:{line_number}: global OS call "
                    f"'{match.group(0)}'")
            for match in FORBIDDEN_BARE_CALLS.finditer(code_line):
                if is_declaration(code_line, match.start()):
                    continue
                violations.append(
                    f"{relative}:{line_number}: bare OS call "
                    f"'{match.group(0)}' (qualify it as std::/fs:: or move it "
                    f"into src/platform/)")
            if FORBIDDEN_WIN32_BRANCH.match(code_line):
                violations.append(
                    f"{relative}:{line_number}: _WIN32 preprocessor branch "
                    f"outside the platform layer")

    # Sanity: the platform layer itself must exist and stay scoped.
    expected = (
        ROOT / "include/refrain/platform/dynamic_library.hpp",
        ROOT / "include/refrain/platform/file.hpp",
        ROOT / "include/refrain/platform/mmap.hpp",
        ROOT / "include/refrain/platform/process.hpp",
    )
    for path in expected:
        if not path.is_file():
            violations.append(
                f"{path.relative_to(ROOT)}: missing platform abstraction header")

    if violations:
        print("platform_check: VIOLATIONS "
              f"({len(violations)} found outside the platform layer)")
        for violation in violations:
            print(f"  {violation}")
        print("\nMove the OS API usage into src/platform/{posix,windows}/ and "
              "expose it through include/refrain/platform/.")
        return 1

    print(f"platform_check: OK ({checked} non-platform translation units "
          f"checked, {len(ALLOWED_PREFIXES)} platform scopes)")
    if verbose:
        print("  scanned dirs:", ", ".join(SOURCE_DIRS))
        print("  allowed:", ", ".join(str(prefix) for prefix in ALLOWED_PREFIXES))
    return 0


def main(argv: list[str]) -> int:
    verbose = "--verbose" in argv[1:]
    return check(verbose)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
