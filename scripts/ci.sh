#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# CI / local verification for Refrain (Linux x86_64, GCC 16, Conan 2.31).
# All vendor dependencies must already be in the Conan cache (CI restores
# ~/.conan2 from actions/cache); this script never uses FetchContent or
# network downloads outside of Conan.
#
#   scripts/ci.sh                          # configure + build + test + smoke
#   scripts/ci.sh --skip-configure         # reuse existing build/Release
#   scripts/ci.sh --skip-build             # test/smoke only
#   scripts/ci.sh --skip-format            # skip the clang-format gate
#   scripts/ci.sh --offline                # pass -nr to conan install
#
# The vendored lockfile is mandatory: a missing/stale conan/locks/vendor.lock
# fails the run instead of silently building against unlocked deps.
# Exit code 0 = all pass. Output is grouped so CI logs are readable.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROFILE="${REFRAIN_CONAN_PROFILE:-gcc-release}"
CONAN_HOME_DIR="${CONAN_HOME:-$HOME/.conan2}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
LOCKFILE="conan/locks/vendor.lock"
SKIP_CONFIGURE=""
SKIP_BUILD=""
OFFLINE=""
RUN_FORMAT_CHECK=1

for arg in "$@"; do
    case "$arg" in
        --skip-configure)    SKIP_CONFIGURE=1 ;;
        --skip-build)        SKIP_BUILD=1 ;;
        --offline)           OFFLINE=1 ;;
        --skip-format)       RUN_FORMAT_CHECK="" ;;
        -h|--help)           sed -n '2,23p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

cd "$ROOT"

say "[1/12] vendor policy guard (tools/vendor_check.py, G1-G7)"
python3 tools/vendor_check.py
python3 tools/check_action_pins.py
python3 tools/check_licenses.py
python3 tools/check_protocol_boundary.py
test -s NOTICE.md || { echo "NOTICE.md is missing or empty" >&2; exit 1; }

say "[2/12] platform abstraction guard (tools/platform_check.py)"
python3 tools/platform_check.py

say "[3/11] lockfile-checked conan install (profile: $PROFILE)"
if [[ ! -f "$CONAN_HOME_DIR/profiles/$PROFILE" ]]; then
    echo "  profile missing, running: scripts/bootstrap.sh --profile-only"
    bash "$ROOT/scripts/bootstrap.sh" --profile-only
fi
# The lockfile is mandatory, not best-effort. `conan install --lockfile`
# fails closed when a pinned revision no longer exists, so the install itself
# is the staleness gate; re-resolving the graph with `conan lock create` and
# diffing it against the committed lock depends on the local cache contents and
# reports false staleness.
[[ -f "$LOCKFILE" ]] || die "$LOCKFILE is missing; vendor dependencies must be pinned (conan lock create . -pr $PROFILE --lockfile-out=$LOCKFILE)"
conan install . -pr "$PROFILE" --build=missing --lockfile="$LOCKFILE" \
    $([ -n "$OFFLINE" ] && echo -nr)

say "[4/11] cmake configure + build"
if [[ -n "$SKIP_CONFIGURE" ]]; then
    echo "  configure skipped (--skip-configure)"
else
    cmake --preset conan-release
fi
if [[ -n "$SKIP_BUILD" ]]; then
    echo "  build skipped (--skip-build)"
else
    cmake --build --preset conan-release -j "$JOBS"
fi

# Locate the build outputs (conan cmake_layout -> build/Release/{bin,generators}).
BIN_DIR="$(find "$ROOT/build" -maxdepth 3 -type d -path '*/bin' 2>/dev/null | head -1)"
[[ -n "$BIN_DIR" ]] || die "build/bin not found (run the build first)"
echo "  binaries: $BIN_DIR"
[[ -x "$BIN_DIR/refrain-tests" ]] || die "refrain-tests not found in $BIN_DIR"
[[ -x "$BIN_DIR/refrain-tools" ]] || die "refrain-tools not found in $BIN_DIR"

# Shared third-party libraries (spdlog/fmt/...) live in the Conan cache.
RUN_ENV="$(find "$ROOT/build" -maxdepth 4 -name conanrun.sh 2>/dev/null | head -1)"
if [[ -n "$RUN_ENV" ]]; then
    # shellcheck disable=SC1090
    set +u
    source "$RUN_ENV"
    set -u
fi

say "[5/11] unit tests (ctest + binary)"
ctest --test-dir "$(dirname "$BIN_DIR")" --output-on-failure \
    || die "ctest unit tests failed"
"$BIN_DIR/refrain-tests" || die "refrain-tests failed"

say "[6/11] schema codegen drift check"
python3 scripts/check_format_versions.py || die "format-version guard failed"
"$BIN_DIR/refrain-tools" schema gen --check \
    || die "schema codegen drift detected (run schema gen)"

say "[7/11] generated C ABI headers compile as C11"
ABI_CHECK="$(mktemp --suffix=.c)"
EXTRACT_DIR="$(mktemp -d)"
CRASH_DIR="$(mktemp -d)"
trap 'rm -f "$ABI_CHECK"; rm -rf "$EXTRACT_DIR" "$CRASH_DIR"' EXIT
cat > "$ABI_CHECK" <<'ABI_EOF'
#include <refrain/contracts.h>
int main(void) { return 0; }
ABI_EOF
cc -std=c11 -Wall -Wextra -Wpedantic -Werror -Iinclude -Isdk/generated/c \
   -fsyntax-only "$ABI_CHECK" || die "generated C ABI headers do not compile"

say "[8/11] tools smoke test (gamepack/mods/builtin images + wasm)"
"$BIN_DIR/refrain-tools" version
"$BIN_DIR/refrain-tools" gamepack check -f "$ROOT/gamepack/gamepack.toml"
"$BIN_DIR/refrain-tools" gamepack check \
    -f "$ROOT/gamepack/presets/minimal-flat.toml"
"$BIN_DIR/refrain-tools" gamepack dump -f "$ROOT/gamepack/gamepack.toml" \
    >/dev/null
# Mod manifest tree + deterministic builtin RFSI image.
"$BIN_DIR/refrain-tools" mods check --mods "$ROOT/mods" \
    --gamepack "$ROOT/gamepack/gamepack.toml" >"$EXTRACT_DIR/mods-check.txt"
grep -q 'refrain.content.vanilla@1.0.0' "$EXTRACT_DIR/mods-check.txt" \
    || die "mods check: content mod manifest missing"
grep -q 'refrain.data.vanilla@1.0.0' "$EXTRACT_DIR/mods-check.txt" \
    || die "mods check: data mod manifest missing"
"$BIN_DIR/refrain-tools" builtin image --mods "$ROOT/mods" \
    -o "$EXTRACT_DIR/builtin-a.image" >"$EXTRACT_DIR/builtin-image.txt"
"$BIN_DIR/refrain-tools" builtin image --mods "$ROOT/mods" \
    -o "$EXTRACT_DIR/builtin-b.image" >/dev/null
grep -Eq '^builtin image: 12 file\(s\), [0-9]+ byte\(s\), BLAKE3 [0-9a-f]{64} -> ' \
    "$EXTRACT_DIR/builtin-image.txt" \
    || die "builtin image: unexpected summary line"
cmp "$EXTRACT_DIR/builtin-a.image" "$EXTRACT_DIR/builtin-b.image" \
    || die "builtin image is not deterministic"
# Mod host: build an image of two synthetic data-only mods and load them
# through a test gamepack (end-to-end smoke; CI-safe content).
"$BIN_DIR/refrain-tools" builtin image --mods "$ROOT/test/fixtures/mods" \
    -o "$EXTRACT_DIR/fixture-builtin.image" >/dev/null
"$BIN_DIR/refrain-tools" mods load \
    -g "$ROOT/test/fixtures/gamepack/data-only.toml" \
    --image "$EXTRACT_DIR/fixture-builtin.image" \
    >"$EXTRACT_DIR/mods-load.txt"
grep -q 'mod host: 2 mod(s) loaded' "$EXTRACT_DIR/mods-load.txt" \
    || die "mods load: expected two fixture mods to load"
grep -q 'refrain.test.data@1.0.0 (abi 1, data-only' \
    "$EXTRACT_DIR/mods-load.txt" \
    || die "mods load: data mod line missing"
# Server auto_scan + mods.lock runtime wiring (directory mods
# only). States covered: fresh write, verified reuse, manifest version drift
# (fail, then --update-lock), and a `mods list --check-lock` re-verification of
# the full builtin+user lock.
SERVER_LOCK="$EXTRACT_DIR/mods.lock"
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --mods "$ROOT/test/fixtures/instance" --auto-scan \
    --mods-lock "$SERVER_LOCK" --update-lock >"$EXTRACT_DIR/auto-scan-1.txt" 2>&1
grep -q 'mods.lock: written' "$EXTRACT_DIR/auto-scan-1.txt" \
    || die "auto_scan: lock was not written"
grep -q 'refrain.test.extra@1.0.0' "$EXTRACT_DIR/auto-scan-1.txt" \
    || die "auto_scan: fixture user mod missing from the lock"
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --mods "$ROOT/test/fixtures/instance" --auto-scan \
    --mods-lock "$SERVER_LOCK" >"$EXTRACT_DIR/auto-scan-2.txt" 2>&1
grep -q 'mods.lock: verified' "$EXTRACT_DIR/auto-scan-2.txt" \
    || die "auto_scan: existing lock was not verified"
# Manifest version drift: the on-disk manifest changed while the lock still
# records the old version. Without --update-lock the server must fail with a
# version-drift diagnostic; --update-lock then rewrites the lock and
# `mods list --check-lock` must re-verify the full builtin+user composition.
cp -R "$ROOT/test/fixtures/instance" "$EXTRACT_DIR/instance-drift"
INSTANCE_DRIFT_MANIFEST="$EXTRACT_DIR/instance-drift/mods/refrain.test.extra/manifest.toml"
python3 - "$INSTANCE_DRIFT_MANIFEST" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8").read()
assert 'version = "1.0.0"' in text
open(path, "w", encoding="utf-8").write(
    text.replace('version = "1.0.0"', 'version = "1.0.1"', 1))
PY
if "$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --mods "$EXTRACT_DIR/instance-drift" --auto-scan \
    --mods-lock "$SERVER_LOCK" >"$EXTRACT_DIR/auto-scan-drift.txt" 2>&1; then
    die "auto_scan: drifted manifest version was accepted without --update-lock"
fi
grep -q 'version drift' "$EXTRACT_DIR/auto-scan-drift.txt" \
    || die "auto_scan: manifest drift failure does not mention version drift"
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --mods "$EXTRACT_DIR/instance-drift" --auto-scan \
    --mods-lock "$SERVER_LOCK" --update-lock \
    >"$EXTRACT_DIR/auto-scan-update.txt" 2>&1
grep -q 'mods.lock: updated' "$EXTRACT_DIR/auto-scan-update.txt" \
    || die "auto_scan: --update-lock did not rewrite the drifted lock"
"$BIN_DIR/refrain-tools" mods list --mods "$EXTRACT_DIR/instance-drift" \
    --gamepack "$ROOT/gamepack/gamepack.toml" \
    --image "$BIN_DIR/../generated/builtin.image" \
    --check-lock "$SERVER_LOCK" >"$EXTRACT_DIR/mods-list-lock.txt"
grep -q 'lock: .* verified' "$EXTRACT_DIR/mods-list-lock.txt" \
    || die "mods list --check-lock: server lock did not verify"
# `mods list` without a lock: the candidate table for an instance root.
"$BIN_DIR/refrain-tools" mods list --mods "$ROOT/test/fixtures/instance" \
    >"$EXTRACT_DIR/mods-list.txt"
grep -q 'mods: 1 candidate(s)' "$EXTRACT_DIR/mods-list.txt" \
    || die "mods list: expected one candidate in the instance fixture"
grep -q 'refrain.test.extra@1.0.0' "$EXTRACT_DIR/mods-list.txt" \
    || die "mods list: instance fixture candidate missing"
# Negatives: the unified catalog must catch a case-only collision and an
# auto-scanned mod built for a foreign ABI. Both guard the single-scanner
# design.
if "$BIN_DIR/refrain-tools" mods check     --mods "$ROOT/test/fixtures/case-collision/mods"     >"$EXTRACT_DIR/case-collision.txt" 2>&1; then
    die "mods check accepted a case-only id collision"
fi
grep -q 'already_exists' "$EXTRACT_DIR/case-collision.txt" \
    || die "case-collision diagnostic is missing"
if "$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --auto-scan --mods "$ROOT/test/fixtures/abi99" \
    --mods-lock "$EXTRACT_DIR/abi99.lock" \
    >"$EXTRACT_DIR/abi99.txt" 2>&1; then
    die "server accepted an auto-scanned mod with a foreign ABI"
fi
grep -q 'declares abi 99' "$EXTRACT_DIR/abi99.txt" \
    || die "ABI 99 diagnostic is missing"
# Only one scanner/resolver pipeline may exist.
if grep -REn 'scan_mod_manifests|collect_mod_tree|resolve_gamepack' \
    "$ROOT/src" "$ROOT/include" >"$EXTRACT_DIR/duplicate-pipeline.txt"; then
    die "duplicate mod pipeline implementation found (see duplicate-pipeline.txt)"
fi

# Explicit mod inputs with auto_scan disabled are a usage error (exit 2),
# never a silent no-op.
set +e
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --mods "$ROOT/test/fixtures/instance" \
    >"$EXTRACT_DIR/auto-scan-off.txt" 2>&1
AUTO_SCAN_OFF_STATUS=$?
set -e
[[ "$AUTO_SCAN_OFF_STATUS" -eq 2 ]] \
    || die "server accepted --mods with auto_scan disabled (exit $AUTO_SCAN_OFF_STATUS)"
grep -q 'require auto_scan' "$EXTRACT_DIR/auto-scan-off.txt" \
    || die "server auto_scan-off usage error is missing"
# An explicit --mods root without mods/ is an io_error, not an empty scan.
mkdir -p "$EXTRACT_DIR/root-without-mods"
if "$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --mods "$EXTRACT_DIR/root-without-mods" --auto-scan \
    --mods-lock "$EXTRACT_DIR/no-mods.lock" \
    >"$EXTRACT_DIR/no-mods-root.txt" 2>&1; then
    die "server accepted a --mods root without a mods/ directory"
fi
grep -q "has no 'mods' directory" "$EXTRACT_DIR/no-mods-root.txt" \
    || die "server root-without-mods error is missing"
# Drift without --update-lock must fail fast (no silent composition
# change). Editing a locked version changes the canonical fingerprint too, so
# the server must refuse to start.
python3 - "$SERVER_LOCK" <<'PY'
import sys
p = sys.argv[1]
text = open(p, encoding="utf-8").read()
open(p, "w", encoding="utf-8").write(text.replace('version = "1.0.0"', 'version = "9.9.9"', 1))
PY
if "$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --mods "$ROOT/test/fixtures/instance" --auto-scan \
    --mods-lock "$SERVER_LOCK" >/dev/null 2>&1; then
    die "auto_scan: drifted mods.lock was accepted without --update-lock"
fi
# Signed .rfmod: deterministic pack, signature verification,
# tamper rejection and CAS install with dedupe (synthetic fixture only).
RFMOD_DIR="$EXTRACT_DIR/rfmod"
mkdir -p "$RFMOD_DIR"
SEED=000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f
"$BIN_DIR/refrain-tools" mods pack "$ROOT/test/fixtures/mod-rfmod" \
    -o "$RFMOD_DIR/a.rfmod" --seed-hex "$SEED" --key-id ci-test \
    >"$RFMOD_DIR/pack.txt"
"$BIN_DIR/refrain-tools" mods pack "$ROOT/test/fixtures/mod-rfmod" \
    -o "$RFMOD_DIR/b.rfmod" --seed-hex "$SEED" --key-id ci-test >/dev/null
cmp "$RFMOD_DIR/a.rfmod" "$RFMOD_DIR/b.rfmod" || die "rfmod pack is not deterministic"
grep -q "packed refrain.test.rfmod@1.0.0: 4 file(s)" "$RFMOD_DIR/pack.txt" \
    || die "rfmod pack: unexpected member count"
grep -q '^signature: ed25519 key_id=ci-test' "$RFMOD_DIR/pack.txt" \
    || die "rfmod pack: signed output missing"
RF_PUB="$(sed -n 's/^public key: //p' "$RFMOD_DIR/pack.txt")"
[[ -n "$RF_PUB" ]] || die "rfmod pack: signing public key missing"
"$BIN_DIR/refrain-tools" mods verify "$RFMOD_DIR/a.rfmod" \
    --key-hex "$RF_PUB" --key-id ci-test >"$RFMOD_DIR/verify.txt"
grep -q '^verify: OK' "$RFMOD_DIR/verify.txt" || die "rfmod verify failed"
grep -q '^trusted: yes' "$RFMOD_DIR/verify.txt" || die "rfmod verify: not trusted"
# Wrong public key (different seed, same key_id) must be rejected.
SEED2=fffefdfcfbfaf9f8f7f6f5f4f3f2f1f0efeeedecebeae9e8e7e6e5e4e3e2e1e0
"$BIN_DIR/refrain-tools" mods pack "$ROOT/test/fixtures/mod-rfmod" \
    -o "$RFMOD_DIR/other.rfmod" --seed-hex "$SEED2" --key-id ci-test >"$RFMOD_DIR/other.txt"
RF_PUB2="$(sed -n 's/^public key: //p' "$RFMOD_DIR/other.txt")"
if "$BIN_DIR/refrain-tools" mods verify "$RFMOD_DIR/a.rfmod" \
    --key-hex "$RF_PUB2" --key-id ci-test >/dev/null 2>&1; then
    die "rfmod verify accepted a wrong public key"
fi
# Tampered archive must be rejected.
python3 - "$RFMOD_DIR/a.rfmod" "$RFMOD_DIR/tampered.rfmod" <<'PY'
import sys
data = bytearray(open(sys.argv[1], "rb").read())
data[len(data) // 2] ^= 0xFF
open(sys.argv[2], "wb").write(data)
PY
if "$BIN_DIR/refrain-tools" mods verify "$RFMOD_DIR/tampered.rfmod" \
    --key-hex "$RF_PUB" --key-id ci-test >/dev/null 2>&1; then
    die "rfmod verify accepted a tampered archive"
fi
# Unsigned package: accepted by default, rejected under --require-signature.
"$BIN_DIR/refrain-tools" mods pack "$ROOT/test/fixtures/mod-rfmod" \
    -o "$RFMOD_DIR/unsigned.rfmod" >"$RFMOD_DIR/pack-unsigned.txt"
grep -q '^signature: unsigned' "$RFMOD_DIR/pack-unsigned.txt" \
    || die "rfmod pack: unsigned output missing"
"$BIN_DIR/refrain-tools" mods verify "$RFMOD_DIR/unsigned.rfmod" \
    >"$RFMOD_DIR/verify-unsigned.txt"
grep -q '^trusted: no' "$RFMOD_DIR/verify-unsigned.txt" \
    || die "rfmod verify: unsigned package must not be trusted"
if "$BIN_DIR/refrain-tools" mods verify "$RFMOD_DIR/unsigned.rfmod" \
    --require-signature >/dev/null 2>&1; then
    die "rfmod verify accepted an unsigned package under --require-signature"
fi
# CAS install: trusted signature, blobs land in the store, reinstall dedupes.
"$BIN_DIR/refrain-tools" mods install "$RFMOD_DIR/a.rfmod" \
    --store "$RFMOD_DIR/store" --key-hex "$RF_PUB" --key-id ci-test \
    >"$RFMOD_DIR/install1.txt"
grep -q '^install: OK' "$RFMOD_DIR/install1.txt" || die "rfmod install failed"
grep -q '^trusted: yes' "$RFMOD_DIR/install1.txt" || die "rfmod install: not trusted"
grep -q '^cook: ' "$RFMOD_DIR/install1.txt" \
    || die "rfmod install: install-time cook artifact missing"
COOK_FILE="$(find "$RFMOD_DIR/store/content/cook" -maxdepth 1 -name '*.rfs' \
    -print -quit 2>/dev/null)"
[[ -n "$COOK_FILE" ]] || die "rfmod install: cook RFS file missing"
"$BIN_DIR/refrain-tools" data check --pack "$COOK_FILE" >/dev/null \
    || die "rfmod install: cooked RFS is not a readable datapack"
"$BIN_DIR/refrain-tools" mods install "$RFMOD_DIR/a.rfmod" \
    --store "$RFMOD_DIR/store" --key-hex "$RF_PUB" --key-id ci-test \
    >"$RFMOD_DIR/install2.txt"
grep -q 'new blobs: 0' "$RFMOD_DIR/install2.txt" \
    || die "rfmod install: second install did not dedupe"
[[ -d "$RFMOD_DIR/store/content/blobs" ]] || die "rfmod install: CAS root missing"
# One install = one segment batch. Small members live in
# content/segments/*.rfseg and are indexed by content/blobs.idx; only packed
# members (>64 KiB, none in this fixture) keep a standalone CAS file.
RFMOD_STORE="$RFMOD_DIR/store"
[[ -d "$RFMOD_STORE/content/segments" ]] \
    || die "rfmod install: no content segments directory"
RF_SEGMENT_COUNT="$(find "$RFMOD_STORE/content/segments" -maxdepth 1 -type f \
    -name '*.rfseg' 2>/dev/null | wc -l)"
[[ "$RF_SEGMENT_COUNT" -ge 1 ]] \
    || die "rfmod install: no content segment was published"
[[ -f "$RFMOD_STORE/content/blobs.idx" ]] \
    || die "rfmod install: blobs.idx was not published"
# Per-mod install receipt: deterministic, listable.
grep -q '^receipt: ' "$RFMOD_DIR/install1.txt" || die "rfmod install: receipt line missing"
RF_RECEIPT="$(sed -n 's/^receipt: //p' "$RFMOD_DIR/install1.txt")"
[[ -f "$RF_RECEIPT" ]] || die "rfmod install: receipt file missing"
# Every small receipt member must be addressed through a segment, and the
# published table must expose the members to the content CLI.
python3 - "$RFMOD_STORE" "$RF_RECEIPT" <<'PY'
import json
import os
import sys

store, receipt_path = sys.argv[1], sys.argv[2]
with open(receipt_path, encoding="utf-8") as handle:
    receipt = json.load(handle)
files = receipt["files"]
if not files:
    sys.exit("rfmod install: receipt lists no files")
small = [entry for entry in files if entry["size"] <= 64 * 1024]
if not small:
    sys.exit("rfmod install: fixture has no small member")
for entry in small:
    digest = entry["blake3"]
    path = os.path.join(store, "content", "blobs", digest[:2], digest)
    if os.path.exists(path):
        sys.exit(f"rfmod install: small member stored as a standalone blob: {path}")
PY
"$BIN_DIR/refrain-tools" content dump --store "$RFMOD_STORE" \
    >"$RFMOD_DIR/install-dump.txt"
grep -q "content: 4 blob(s)" "$RFMOD_DIR/install-dump.txt" \
    || die "rfmod install: blobs.idx does not index the installed members"
cp "$RF_RECEIPT" "$RFMOD_DIR/receipt-1.json"
RF_SEGMENT_BYTES="$(find "$RFMOD_STORE/content/segments" -maxdepth 1 -type f \
    -name '*.rfseg' -printf '%s\n' | awk '{total += $1} END {print total + 0}')"
"$BIN_DIR/refrain-tools" mods install "$RFMOD_DIR/a.rfmod" \
    --store "$RFMOD_DIR/store" --key-hex "$RF_PUB" --key-id ci-test \
    >"$RFMOD_DIR/install3.txt"
cmp "$RF_RECEIPT" "$RFMOD_DIR/receipt-1.json" || die "rfmod install: receipt is not deterministic"
RF_SEGMENT_COUNT_AFTER="$(find "$RFMOD_STORE/content/segments" -maxdepth 1 \
    -type f -name '*.rfseg' 2>/dev/null | wc -l)"
[[ "$RF_SEGMENT_COUNT_AFTER" -eq "$RF_SEGMENT_COUNT" ]] \
    || die "rfmod install: reinstall appended a content segment"
RF_SEGMENT_BYTES_AFTER="$(find "$RFMOD_STORE/content/segments" -maxdepth 1 \
    -type f -name '*.rfseg' -printf '%s\n' | awk '{total += $1} END {print total + 0}')"
[[ "$RF_SEGMENT_BYTES_AFTER" -eq "$RF_SEGMENT_BYTES" ]] \
    || die "rfmod install: reinstall appended segment bytes"
"$BIN_DIR/refrain-tools" mods installed --store "$RFMOD_DIR/store" \
    >"$RFMOD_DIR/installed.txt"
grep -q 'refrain.test.rfmod@1.0.0' "$RFMOD_DIR/installed.txt" || die "mods installed: receipt missing"
grep -q 'signed trusted' "$RFMOD_DIR/installed.txt" || die "mods installed: trust state missing"
# A corrupt receipt must fail `mods installed` (strict audit surface) and the
# error must name the offending file; restore the good receipt afterwards.
cp "$RF_RECEIPT" "$RFMOD_DIR/receipt-backup.json"
printf '{' >"$RF_RECEIPT"
if "$BIN_DIR/refrain-tools" mods installed --store "$RFMOD_DIR/store" \
    >"$RFMOD_DIR/installed-bad.txt" 2>&1; then
    die "mods installed: corrupt receipt was accepted"
fi
grep -Fq "$RF_RECEIPT" "$RFMOD_DIR/installed-bad.txt" \
    || die "mods installed: corrupt receipt error does not name the receipt path"
cp "$RFMOD_DIR/receipt-backup.json" "$RF_RECEIPT"
# Server startup content bootstrap (scan inbox -> install -> publish
# index -> GC). A fresh store + inbox with the signed CI package must be
# installed and indexed by one `--dump-gamepack` run; a second run is
# byte-idempotent; an applying GC keeps the receipt/index roots.
STARTUP_DIR="$EXTRACT_DIR/startup-content"
STARTUP_STORE="$STARTUP_DIR/store"
STARTUP_INBOX="$STARTUP_DIR/inbox"
mkdir -p "$STARTUP_INBOX"
cp "$RFMOD_DIR/a.rfmod" "$STARTUP_INBOX/a.rfmod"
STARTUP_SERVER_ARGS=(--gamepack "$ROOT/gamepack/gamepack.toml"
    --content-store "$STARTUP_STORE" --content-inbox "$STARTUP_INBOX"
    --content-composition test-gp --auto-install --content-require-signature
    --content-trusted-key "ci-test:$RF_PUB" --dump-gamepack)
"$BIN_DIR/refrain-server" "${STARTUP_SERVER_ARGS[@]}" \
    >"$STARTUP_DIR/server1.txt" 2>&1
grep -q 'startup-content: install: installed refrain.test.rfmod@1.0.0' \
    "$STARTUP_DIR/server1.txt" \
    || die "startup content: package was not installed"
grep -q 'startup-content: index: published .*test-gp.idx' \
    "$STARTUP_DIR/server1.txt" \
    || die "startup content: index was not published"
grep -q 'startup-content: lock: acquired' "$STARTUP_DIR/server1.txt" \
    || die "startup content: store lock was not acquired"
[[ -f "$STARTUP_STORE/mods/refrain.test.rfmod/1.0.0.json" ]] \
    || die "startup content: receipt missing"
[[ -f "$STARTUP_STORE/content/blobs.idx" ]] \
    || die "startup content: blobs.idx missing"
[[ -f "$STARTUP_STORE/content/resolutions/test-gp.idx" ]] \
    || die "startup content: resolution index missing"
"$BIN_DIR/refrain-tools" content where --store "$STARTUP_STORE" \
    --composition test-gp data/refrain_test/recipe/a.json \
    >"$STARTUP_DIR/where.txt"
grep -q 'source=refrain.test.rfmod priority=1000' "$STARTUP_DIR/where.txt" \
    || die "startup content: resolution winner is wrong"
[[ "$(find "$STARTUP_STORE/content/segments" -maxdepth 1 -type f \
    -name '*.rfseg' | wc -l)" -ge 1 ]] \
    || die "startup content: no content segment was published"
"$BIN_DIR/refrain-tools" content verify --store "$STARTUP_STORE" --blobs \
    >"$STARTUP_DIR/verify1.txt" \
    || die "startup content: content verify failed after the first run"
cp "$STARTUP_STORE/mods/refrain.test.rfmod/1.0.0.json" \
    "$STARTUP_DIR/receipt1.json"
STARTUP_SEGMENT_SHA="$(cd "$STARTUP_STORE/content/segments" && sha256sum *.rfseg)"
# Idempotency: offer the same archive again. The receipt matches its archive
# digest, so it is skipped and neither the segment nor the receipt changes.
cp "$RFMOD_DIR/a.rfmod" "$STARTUP_INBOX/a.rfmod"
"$BIN_DIR/refrain-server" "${STARTUP_SERVER_ARGS[@]}" \
    >"$STARTUP_DIR/server2.txt" 2>&1
grep -q 'startup-content: install: refrain.test.rfmod@1.0.0 already installed' \
    "$STARTUP_DIR/server2.txt" \
    || die "startup content: second run did not skip the installed package"
cmp "$STARTUP_STORE/mods/refrain.test.rfmod/1.0.0.json" \
    "$STARTUP_DIR/receipt1.json" \
    || die "startup content: receipt changed on the second run"
[[ "$(cd "$STARTUP_STORE/content/segments" && sha256sum *.rfseg)" == \
    "$STARTUP_SEGMENT_SHA" ]] \
    || die "startup content: segments changed on the second run"
# Applying GC: the receipt and the published index are roots, so verify still
# passes and the installed blobs stay in the store.
"$BIN_DIR/refrain-server" "${STARTUP_SERVER_ARGS[@]}" --content-gc=apply \
    >"$STARTUP_DIR/server3.txt" 2>&1
grep -q 'startup-content: gc: applied' "$STARTUP_DIR/server3.txt" \
    || die "startup content: applying GC did not run"
[[ -f "$STARTUP_STORE/mods/refrain.test.rfmod/1.0.0.json" ]] \
    || die "startup content: GC removed the receipt root"
[[ -f "$STARTUP_STORE/content/resolutions/test-gp.idx" ]] \
    || die "startup content: GC removed the resolution index"
"$BIN_DIR/refrain-tools" content verify --store "$STARTUP_STORE" --blobs \
    >"$STARTUP_DIR/verify2.txt" \
    || die "startup content: content verify failed after the applying GC"
echo "  startup content: inbox -> install -> index -> GC OK (idempotent)"
# Runtime content mount. The published `test-gp` composition is the
# lowest-priority layer and an external `--datapack` directory is layered on
# top; the summary proves both contributed (2 layers, 5 files, 2 recipes: the
# fixture recipe from the content pack + ci_runtime.json from the directory).
# A fresh inbox keeps the startup bootstrap from picking up the older package.
RUNTIME_DIR="$STARTUP_DIR/runtime-content"
RUNTIME_INBOX="$RUNTIME_DIR/inbox"
RUNTIME_PACK="$RUNTIME_DIR/pack"
mkdir -p "$RUNTIME_INBOX" "$RUNTIME_PACK/data/refrain_test/recipe"
cat >"$RUNTIME_PACK/pack.mcmeta" <<'JSON'
{
  "pack": {
    "pack_format": 121,
    "description": "Refrain synthetic runtime datapack (no Mojang content)"
  }
}
JSON
cat >"$RUNTIME_PACK/data/refrain_test/recipe/ci_runtime.json" <<'JSON'
{
  "type": "minecraft:crafting_shaped",
  "pattern": ["MM", "MM"],
  "key": {
    "M": "#refrain_test:metal"
  },
  "result": {
    "id": "refrain_test:ci_runtime_block"
  }
}
JSON
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --content-store "$STARTUP_STORE" --content-inbox "$RUNTIME_INBOX" \
    --content-composition test-gp --datapack "$RUNTIME_PACK" \
    >"$RUNTIME_DIR/server.txt" 2>&1 \
    || die "runtime content: server failed to mount the content layers"
grep -q 'runtime-content: layers=2 files=5' \
    "$RUNTIME_DIR/server.txt" \
    || die "runtime content: content composition + datapack did not merge"
grep -q 'errors=0 warnings=0' "$RUNTIME_DIR/server.txt" \
    || die "runtime content: merged datapack set did not validate"
# Malformed JSON in a --datapack directory is fail-closed: non-zero exit and a
# diagnostic naming the offending file.
RUNTIME_BAD_PACK="$RUNTIME_DIR/badpack"
mkdir -p "$RUNTIME_BAD_PACK/data/refrain_test/recipe"
printf '{"type":"minecraft:crafting_shaped"' \
    >"$RUNTIME_BAD_PACK/data/refrain_test/recipe/bad.json"
if "$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --datapack "$RUNTIME_BAD_PACK" >"$RUNTIME_DIR/server-bad.txt" 2>&1; then
    die "runtime content: malformed datapack JSON was accepted"
fi
grep -q 'data/refrain_test/recipe/bad.json' "$RUNTIME_DIR/server-bad.txt" \
    || die "runtime content: malformed datapack diagnostic is missing"
echo "  runtime content: composition low + --datapack high mount/load/validate OK"
# Datapack content: parse + validate the synthetic fixture (tags/recipe/
# loot/advancement/worldgen/function), then a negative malformed-JSON case.
"$BIN_DIR/refrain-tools" data check --pack "$ROOT/test/fixtures/datapack" \
    >"$EXTRACT_DIR/data-check.txt"
grep -q 'data: 1 layer(s), 7 data file(s), 9 total file(s), 7 content entries' \
    "$EXTRACT_DIR/data-check.txt" \
    || die "data check: unexpected datapack fixture counts"
grep -q 'validate: OK' "$EXTRACT_DIR/data-check.txt" \
    || die "data check: fixture validation failed"
# Content closure: build/freeze one content registry per kind and check the
# frozen fingerprint shape plus the honest tags-skipped line (thin CLI over the
# already unit-tested registry/datapack_set).
"$BIN_DIR/refrain-tools" data registry --pack "$ROOT/test/fixtures/datapack" \
    >"$EXTRACT_DIR/data-registry.txt" \
    || die "data registry: fixture load/freeze failed"
grep -Eq '^registry recipe: [0-9]+ entry\(s\) fingerprint=[0-9a-f]{64}$' \
    "$EXTRACT_DIR/data-registry.txt" \
    || die "data registry: recipe registry/fingerprint line is missing"
grep -Fqx 'registry tags: skipped (merged tag registry; see data check)' \
    "$EXTRACT_DIR/data-registry.txt" \
    || die "data registry: tags-skipped line is missing"
grep -Eq '^data registry: [0-9]+ kind\(s\), [0-9]+ entry\(s\)$' \
    "$EXTRACT_DIR/data-registry.txt" \
    || die "data registry: summary line is missing"
mkdir -p "$EXTRACT_DIR/badpack/data/refrain_test/recipe"
printf '{"type":"minecraft:crafting_shaped"' \
    >"$EXTRACT_DIR/badpack/data/refrain_test/recipe/broken.json"
if "$BIN_DIR/refrain-tools" data check --pack "$EXTRACT_DIR/badpack" \
    >/dev/null 2>&1; then
    die "data check: malformed JSON was accepted"
fi
# Content store TOC indexes (RFBT/RFRX) + `content` CLI. The fixture has
# four files but only three blobs (a.json == dup.json), big.bin is packed (>64
# KiB). The index format contains neither the composition id nor a timestamp,
# so the same input in a second store must produce byte-identical bytes; the
# blob-table BLAKE3 prefix is why two stores are compared instead of two
# compositions in one store.
CONTENT_STORE="$EXTRACT_DIR/content-store"
CONTENT_STORE2="$EXTRACT_DIR/content-store-2"
"$BIN_DIR/refrain-tools" content build --root "$ROOT/test/fixtures/content-store" \
    --store "$CONTENT_STORE" --source ci:fixture --priority 10 --composition fixture \
    >"$EXTRACT_DIR/content-build.txt"
grep -q 'content build: 4 file(s), 3 blob(s)' "$EXTRACT_DIR/content-build.txt" \
    || die "content build: unexpected counts"
# Small raw blobs are sealed into content segments; only the packed blob
# (big.bin) keeps a standalone file under content/blobs/<hh>/<digest>.
SEGMENT_DIR="$CONTENT_STORE/content/segments"
SEGMENT_COUNT="$(find "$SEGMENT_DIR" -maxdepth 1 -type f -name '*.rfseg' 2>/dev/null | wc -l)"
[[ "$SEGMENT_COUNT" -ge 1 ]] \
    || die "content build: no content segment was published"
BLOB_FILE_COUNT="$(find "$CONTENT_STORE/content/blobs" -type f 2>/dev/null | wc -l)"
[[ "$BLOB_FILE_COUNT" -eq 1 ]] \
    || die "content build: expected only the packed blob as a file (found $BLOB_FILE_COUNT)"
grep -q 'segments: 1 segment(s), 2 blob(s)' "$EXTRACT_DIR/content-build.txt" \
    || die "content build: unexpected segment counts"
"$BIN_DIR/refrain-tools" content build --root "$ROOT/test/fixtures/content-store" \
    --store "$CONTENT_STORE2" --source ci:fixture --priority 10 --composition fixture2 \
    >/dev/null
cmp "$CONTENT_STORE/content/resolutions/fixture.idx" \
    "$CONTENT_STORE2/content/resolutions/fixture2.idx" \
    || die "content build: resolution index is not deterministic"
"$BIN_DIR/refrain-tools" content dump --store "$CONTENT_STORE" >"$EXTRACT_DIR/content-dump.txt"
grep -q 'content: 3 blob(s)' "$EXTRACT_DIR/content-dump.txt" \
    || die "content dump: blob count mismatch"
"$BIN_DIR/refrain-tools" content where --store "$CONTENT_STORE" --composition fixture \
    data/refrain_test/a.json >"$EXTRACT_DIR/content-where.txt"
grep -q 'ci:fixture' "$EXTRACT_DIR/content-where.txt" || die "content where: source missing"
# Dump reports the physical segment location for in-segment blobs, and the
# small blob's digest must not exist as a standalone CAS file.
grep -q 'segment=' "$EXTRACT_DIR/content-dump.txt" \
    || die "content dump: segment location missing"
SMALL_BLOB="$(sed -n 's/^winner: .* blob=\([0-9a-f]\{64\}\).*/\1/p' "$EXTRACT_DIR/content-where.txt")"
[[ -n "$SMALL_BLOB" ]] || die "content where: winner digest missing"
[[ ! -f "$CONTENT_STORE/content/blobs/${SMALL_BLOB:0:2}/$SMALL_BLOB" ]] \
    || die "content build: a small blob was stored as a standalone file"
# Adding a second composition rewrites blobs.idx, so every existing RFRX is
# re-encoded against the new table (the BLAKE3 prefix binding must stay valid).
"$BIN_DIR/refrain-tools" content build --root "$ROOT/test/fixtures/content-store" \
    --store "$CONTENT_STORE" --source ci:fixture --priority 10 --composition fixture2 \
    >/dev/null
"$BIN_DIR/refrain-tools" content verify --store "$CONTENT_STORE" --blobs \
    >"$EXTRACT_DIR/content-verify.txt" || die "content verify failed"
grep -q 'segment .* OK' "$EXTRACT_DIR/content-verify.txt" \
    || die "content verify: no segment was verified"
# A single corrupted byte inside a segment must fail `verify --blobs`; the
# pristine store stays untouched for the later GC/compaction steps.
TAMPER_STORE="$EXTRACT_DIR/content-tamper-store"
cp -R "$CONTENT_STORE" "$TAMPER_STORE"
TAMPER_SEGMENT="$(find "$TAMPER_STORE/content/segments" -maxdepth 1 -type f \
    -name '*.rfseg' -print -quit)"
[[ -n "$TAMPER_SEGMENT" ]] || die "content verify: no segment to corrupt"
python3 - "$TAMPER_SEGMENT" <<'PY'
import sys

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
if len(data) <= 80:
    raise SystemExit("segment has no body to corrupt")
data[-1] ^= 0xFF
open(path, "wb").write(data)
PY
if "$BIN_DIR/refrain-tools" content verify --store "$TAMPER_STORE" --blobs \
    >"$EXTRACT_DIR/content-verify-tampered.txt" 2>&1; then
    die "content verify: corrupted segment was accepted"
fi
"$BIN_DIR/refrain-tools" content gc --store "$CONTENT_STORE" --dry-run \
    --budget-ms 0 >"$EXTRACT_DIR/content-gc.txt"
grep -q 'unreferenced' "$EXTRACT_DIR/content-gc.txt" || die "content gc: no output"
grep -q 'orphan segment' "$EXTRACT_DIR/content-gc.txt" \
    || die "content gc: segment statistics missing"
"$BIN_DIR/refrain-tools" content gc --store "$CONTENT_STORE" --dry-run --compact \
    >"$EXTRACT_DIR/content-gc-compact-dry.txt"
grep -q 'segment(s) to compact' "$EXTRACT_DIR/content-gc-compact-dry.txt" \
    || die "content gc: compaction plan missing"
# Ingest refuses symbolic links instead of following them outside --root.
mkdir -p "$EXTRACT_DIR/content-symlink"
ln -s "$ROOT/test/fixtures/content-store/data/refrain_test/a.json" \
    "$EXTRACT_DIR/content-symlink/a.json"
if "$BIN_DIR/refrain-tools" content build --root "$EXTRACT_DIR/content-symlink" \
    --store "$EXTRACT_DIR/content-symlink-store" --source ci:symlink \
    --composition symlink >"$EXTRACT_DIR/content-symlink.txt" 2>&1; then
    die "content build: symbolic link was accepted"
fi
grep -q 'symbolic link' "$EXTRACT_DIR/content-symlink.txt" \
    || die "content build: symlink rejection does not name the cause"
# GC --apply must fail closed when an index-referenced blob is missing: dropping
# the record would shift every later blob_index and invalidate all RFRX files.
GC_STORE="$EXTRACT_DIR/content-gc-store"
cp -R "$CONTENT_STORE" "$GC_STORE"
GC_VICTIM="$(find "$GC_STORE/content/blobs" -type f -print -quit)"
[[ -n "$GC_VICTIM" ]] || die "content gc: no blob file to remove"
rm -f "$GC_VICTIM"
cp "$GC_STORE/content/blobs.idx" "$EXTRACT_DIR/content-gc-table-before.idx"
if "$BIN_DIR/refrain-tools" content gc --store "$GC_STORE" --apply \
    >"$EXTRACT_DIR/content-gc-missing.txt" 2>&1; then
    die "content gc: missing referenced blob was accepted"
fi
grep -q 'referenced blob(s) are missing' "$EXTRACT_DIR/content-gc-missing.txt" \
    || die "content gc: missing-blob error is missing"
cmp "$EXTRACT_DIR/content-gc-table-before.idx" "$GC_STORE/content/blobs.idx" \
    || die "content gc: blobs.idx changed on a failed apply"
# A corrupt old RFRX aborts the rebuild before blobs.idx is republished.
HALF_STORE="$EXTRACT_DIR/content-half-store"
cp -R "$CONTENT_STORE" "$HALF_STORE"
python3 - "$HALF_STORE/content/resolutions/fixture2.idx" <<'PY'
import sys

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
data[100] ^= 0xFF
open(path, "wb").write(data)
PY
cp "$HALF_STORE/content/blobs.idx" "$EXTRACT_DIR/content-half-table-before.idx"
if "$BIN_DIR/refrain-tools" content build --root "$ROOT/test/fixtures/content-store" \
    --store "$HALF_STORE" --source ci:fixture --priority 10 \
    --composition fixture3 >"$EXTRACT_DIR/content-half.txt" 2>&1; then
    die "content build: corrupt old resolution index was accepted"
fi
cmp "$EXTRACT_DIR/content-half-table-before.idx" "$HALF_STORE/content/blobs.idx" \
    || die "content build: blobs.idx changed before index rebuilds succeeded"
[[ ! -f "$HALF_STORE/content/resolutions/fixture3.idx" ]] \
    || die "content build: index written after a failed rebuild"
# `gc --apply --compact` is valid on a clean store (nothing is compactable
# and it must still exit 0 and leave a verifiable store behind).
PLAIN_COMPACT_STORE="$EXTRACT_DIR/content-plain-compact-store"
cp -R "$CONTENT_STORE" "$PLAIN_COMPACT_STORE"
"$BIN_DIR/refrain-tools" content gc --store "$PLAIN_COMPACT_STORE" --apply --compact \
    >"$EXTRACT_DIR/content-gc-plain-compact.txt" \
    || die "content gc: --apply --compact failed on a clean store"
"$BIN_DIR/refrain-tools" content verify --store "$PLAIN_COMPACT_STORE" --blobs \
    >"$EXTRACT_DIR/content-gc-plain-compact-verify.txt" \
    || die "content verify failed after gc --compact"
# Compaction with real dead bytes: build two compositions, then drop one. GC must
# delete the unreferenced packed blob and rewrite the segment holding the
# surviving raw blob (dead blob + padding are > 50% of its body).
COMPACT_STORE="$EXTRACT_DIR/content-compact-store"
COMPACT_SRC="$EXTRACT_DIR/content-compact-src"
mkdir -p "$COMPACT_SRC"
cp "$ROOT/test/fixtures/content-store/assets/refrain_test/lang/en_us.json" \
    "$COMPACT_SRC/keep.json"
"$BIN_DIR/refrain-tools" content build --root "$ROOT/test/fixtures/content-store" \
    --store "$COMPACT_STORE" --source ci:fixture --priority 10 \
    --composition keep-all >/dev/null
"$BIN_DIR/refrain-tools" content build --root "$COMPACT_SRC" \
    --store "$COMPACT_STORE" --source ci:keep --priority 10 --composition keep-one \
    >/dev/null
rm -f "$COMPACT_STORE/content/resolutions/keep-all.idx"
"$BIN_DIR/refrain-tools" content gc --store "$COMPACT_STORE" --apply --compact \
    >"$EXTRACT_DIR/content-gc-compact.txt" \
    || die "content gc: --apply --compact failed"
grep -q 'gc: compacted 1 segment(s)' "$EXTRACT_DIR/content-gc-compact.txt" \
    || die "content gc: segment was not compacted"
grep -q 'gc: deleted 1 segment(s)' "$EXTRACT_DIR/content-gc-compact.txt" \
    || die "content gc: old segment was not deleted"
[[ "$(find "$COMPACT_STORE/content/segments" -maxdepth 1 -type f -name '*.rfseg' | wc -l)" -eq 1 ]] \
    || die "content gc: unexpected segment count after compaction"
"$BIN_DIR/refrain-tools" content verify --store "$COMPACT_STORE" --blobs \
    >"$EXTRACT_DIR/content-gc-compact-verify.txt" \
    || die "content verify failed after compaction"
# Install receipts are content GC roots. A store with only an installed mod
# has no RFRX at all, but `gc --apply` must keep its blobs/segment because the
# receipt records every file digest.
GC_RECEIPT_STORE="$EXTRACT_DIR/gc-receipt-store"
"$BIN_DIR/refrain-tools" mods install "$RFMOD_DIR/a.rfmod" \
    --store "$GC_RECEIPT_STORE" --key-hex "$RF_PUB" --key-id ci-test \
    >"$EXTRACT_DIR/gc-receipt-install.txt" \
    || die "content gc: receipt-rooted install failed"
GC_RECEIPT_SEGMENTS="$(find "$GC_RECEIPT_STORE/content/segments" -maxdepth 1 \
    -type f -name '*.rfseg' | wc -l)"
[[ "$GC_RECEIPT_SEGMENTS" -ge 1 ]] \
    || die "content gc: receipt-rooted install published no segment"
"$BIN_DIR/refrain-tools" content gc --store "$GC_RECEIPT_STORE" --apply \
    >"$EXTRACT_DIR/gc-receipt-apply.txt" \
    || die "content gc: --apply with only receipt roots failed"
grep -q 'gc: roots: 0 index reference(s), 4 receipt file(s)' \
    "$EXTRACT_DIR/gc-receipt-apply.txt" \
    || die "content gc: receipt roots were not counted"
grep -q 'gc: blobs.idx: 4 blob(s)' "$EXTRACT_DIR/gc-receipt-apply.txt" \
    || die "content gc: receipt-rooted records were dropped"
grep -q 'gc: deleted 0 blob(s), 0 byte(s)' "$EXTRACT_DIR/gc-receipt-apply.txt" \
    || die "content gc: receipt-rooted blobs were deleted"
grep -q 'gc: deleted 0 segment(s), 0 byte(s)' "$EXTRACT_DIR/gc-receipt-apply.txt" \
    || die "content gc: receipt-rooted segment was deleted"
[[ "$(find "$GC_RECEIPT_STORE/content/segments" -maxdepth 1 -type f \
    -name '*.rfseg' | wc -l)" -eq "$GC_RECEIPT_SEGMENTS" ]] \
    || die "content gc: receipt-rooted segment disappeared"
"$BIN_DIR/refrain-tools" content verify --store "$GC_RECEIPT_STORE" --blobs \
    >"$EXTRACT_DIR/gc-receipt-verify.txt" \
    || die "content verify failed after receipt-rooted gc"
# Content build keeps every previous table record, including refcount=0
# records whose only root is an install receipt. The RFRX rebuild must still
# leave a verifiable store.
GC_BUILD_STORE="$EXTRACT_DIR/gc-build-store"
"$BIN_DIR/refrain-tools" mods install "$RFMOD_DIR/a.rfmod" \
    --store "$GC_BUILD_STORE" --key-hex "$RF_PUB" --key-id ci-test \
    >/dev/null || die "content build: install before build failed"
"$BIN_DIR/refrain-tools" content build --root "$ROOT/test/fixtures/content-store" \
    --store "$GC_BUILD_STORE" --source ci:keep --priority 10 \
    --composition extra >"$EXTRACT_DIR/gc-build.txt" \
    || die "content build: build after install failed"
"$BIN_DIR/refrain-tools" content dump --store "$GC_BUILD_STORE" \
    >"$EXTRACT_DIR/gc-build-dump.txt"
grep -q 'content: 7 blob(s)' "$EXTRACT_DIR/gc-build-dump.txt" \
    || die "content build: previous (install-only) records were dropped"
[[ "$(grep -c 'refcount=0' "$EXTRACT_DIR/gc-build-dump.txt")" -eq 4 ]] \
    || die "content build: refcount=0 records were not retained"
"$BIN_DIR/refrain-tools" content verify --store "$GC_BUILD_STORE" --blobs \
    >"$EXTRACT_DIR/gc-build-verify.txt" \
    || die "content verify failed after build retained install records"
# Once the receipt is removed and another root (the RFRX) exists, the
# install-only records/blobs/segment are unreferenced and are collected.
GC_DROP_STORE="$EXTRACT_DIR/gc-drop-store"
"$BIN_DIR/refrain-tools" content build --root "$ROOT/test/fixtures/content-store" \
    --store "$GC_DROP_STORE" --source ci:keep --priority 10 \
    --composition keep >/dev/null \
    || die "content gc: seed build for receipt-drop failed"
"$BIN_DIR/refrain-tools" mods install "$RFMOD_DIR/a.rfmod" \
    --store "$GC_DROP_STORE" --key-hex "$RF_PUB" --key-id ci-test \
    >/dev/null || die "content gc: install before receipt-drop failed"
GC_DROP_SEGMENTS="$(find "$GC_DROP_STORE/content/segments" -maxdepth 1 \
    -type f -name '*.rfseg' | wc -l)"
rm -f "$GC_DROP_STORE/mods/refrain.test.rfmod/1.0.0.json"
"$BIN_DIR/refrain-tools" content gc --store "$GC_DROP_STORE" --apply \
    >"$EXTRACT_DIR/gc-drop-apply.txt" \
    || die "content gc: --apply after receipt removal failed"
grep -q 'gc: roots: 4 index reference(s), 0 receipt file(s)' \
    "$EXTRACT_DIR/gc-drop-apply.txt" \
    || die "content gc: index roots were not counted after receipt removal"
grep -q 'gc: blobs.idx: 3 blob(s)' "$EXTRACT_DIR/gc-drop-apply.txt" \
    || die "content gc: install-only records were not removed"
grep -q 'gc: deleted 1 segment(s)' "$EXTRACT_DIR/gc-drop-apply.txt" \
    || die "content gc: install-only segment was not removed"
[[ "$(find "$GC_DROP_STORE/content/segments" -maxdepth 1 -type f \
    -name '*.rfseg' | wc -l)" -eq $((GC_DROP_SEGMENTS - 1)) ]] \
    || die "content gc: unexpected segment count after receipt removal"
"$BIN_DIR/refrain-tools" content dump --store "$GC_DROP_STORE" \
    >"$EXTRACT_DIR/gc-drop-dump.txt"
grep -q 'content: 3 blob(s)' "$EXTRACT_DIR/gc-drop-dump.txt" \
    || die "content gc: receipt-dropped blobs are still in blobs.idx"
"$BIN_DIR/refrain-tools" content verify --store "$GC_DROP_STORE" --blobs \
    >"$EXTRACT_DIR/gc-drop-verify.txt" \
    || die "content verify failed after receipt removal"
# A corrupt/foreign receipt fails `gc --apply` closed before any table or
# blob is touched (fail-closed, no deletion).
GC_BAD_STORE="$EXTRACT_DIR/gc-bad-receipt-store"
"$BIN_DIR/refrain-tools" mods install "$RFMOD_DIR/a.rfmod" \
    --store "$GC_BAD_STORE" --key-hex "$RF_PUB" --key-id ci-test \
    >/dev/null || die "content gc: bad-receipt install failed"
GC_BAD_RECEIPT="$GC_BAD_STORE/mods/refrain.test.rfmod/1.0.0.json"
cp "$GC_BAD_STORE/content/blobs.idx" "$EXTRACT_DIR/gc-bad-table-before.idx"
GC_BAD_SEGMENTS="$(find "$GC_BAD_STORE/content/segments" -maxdepth 1 \
    -type f -name '*.rfseg' | wc -l)"
printf '{' >"$GC_BAD_RECEIPT"
if "$BIN_DIR/refrain-tools" content gc --store "$GC_BAD_STORE" --apply \
    >"$EXTRACT_DIR/gc-bad-json.txt" 2>&1; then
    die "content gc: corrupt receipt was accepted"
fi
grep -Fq "$GC_BAD_RECEIPT" "$EXTRACT_DIR/gc-bad-json.txt" \
    || die "content gc: corrupt-receipt error does not name the receipt"
printf '{"schema":"refrain.mod-receipt/2"}' >"$GC_BAD_RECEIPT"
if "$BIN_DIR/refrain-tools" content gc --store "$GC_BAD_STORE" --apply \
    >"$EXTRACT_DIR/gc-bad-schema.txt" 2>&1; then
    die "content gc: foreign receipt schema was accepted"
fi
grep -q 'unsupported receipt schema' "$EXTRACT_DIR/gc-bad-schema.txt" \
    || die "content gc: foreign-schema error is missing"
cmp "$EXTRACT_DIR/gc-bad-table-before.idx" \
    "$GC_BAD_STORE/content/blobs.idx" \
    || die "content gc: blobs.idx changed on a rejected receipt"
[[ "$(find "$GC_BAD_STORE/content/segments" -maxdepth 1 -type f \
    -name '*.rfseg' | wc -l)" -eq "$GC_BAD_SEGMENTS" ]] \
    || die "content gc: segment deleted on a rejected receipt"
echo "  content: 4 file(s) -> 3 blob(s) (2 in segments), deterministic RFRX, where/verify/gc+compact OK"
"$BIN_DIR/refrain-tools" wasm smoke
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/presets/minimal-flat.toml" \
    --dump-gamepack | grep -q 'worldgen -> refrain.worldgen.flat@1.0.0' \
    || die "server did not resolve the flat-worldgen preset"
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --dump-gamepack 2>&1 | grep -q 'builtin image: files=12' \
    || die "server did not load the embedded built-in image"
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    2>&1 | grep -q 'mod host: 11 mod(s) loaded' \
    || die "server did not load the gamepack mods through the mod host"
# Console diagnostics are stderr; stdout carries only the command's data.
# CI/scripts grep these streams directly (`dump > out.txt`), so a console log
# line on stdout would corrupt machine-readable output.
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --dump-gamepack >"$EXTRACT_DIR/stream-out.txt" 2>"$EXTRACT_DIR/stream-err.txt"
grep -q 'worldgen -> ' "$EXTRACT_DIR/stream-out.txt" \
    || die "stream split: dump plan missing from stdout"
if grep -Eq '\[[^]]*/(info|warn|error|debug|critical)\]' \
    "$EXTRACT_DIR/stream-out.txt"; then
    die "stream split: console log leaked into stdout"
fi
grep -Eq '\[[^]]*/info\]' "$EXTRACT_DIR/stream-err.txt" \
    || die "stream split: console log is not on stderr"
# Frozen CLI/output contract (stream + strings), see
# test/fixtures/golden/cli-contract.txt.
python3 "$ROOT/tools/cli_contract_check.py" --root "$ROOT" \
    --bin-dir "$BIN_DIR" \
    || die "CLI/output contract drifted (golden contract)"
# Data provider: built-in mod code activated through REFR_MOD_STATIC.
"$BIN_DIR/refrain-tools" mods load \
    -g "$ROOT/gamepack/presets/data-only.toml" \
    --image "$BIN_DIR/../generated/builtin.image" \
    >"$EXTRACT_DIR/mods-load-real.txt"
grep -q 'refrain.data.vanilla@1.0.0 (abi 1, code, provides refrain:data.packs' \
    "$EXTRACT_DIR/mods-load-real.txt" \
    || die "data.vanilla provider was not activated by the mod host"
echo "  provider swap: default=RWF/vanilla, preset=memory/flat"
echo "  mods: 2 manifest(s), builtin.image deterministic, embedded image OK"

# World storage: synthetic Anvil -> RWF -> Anvil round-trip. The fixture is
# written and validated by an independent Python implementation
# (tools/make_test_world.py), so a bug in the C++ codec cannot mask itself.
WORLD_ROOT="$EXTRACT_DIR/world"
rm -rf "$WORLD_ROOT"
mkdir -p "$WORLD_ROOT"
python3 "$ROOT/tools/make_test_world.py" selftest >"$WORLD_ROOT/selftest.txt" \
    || die "world fixture selftest failed"
python3 "$ROOT/tools/make_test_world.py" gen --out "$WORLD_ROOT/anvil" --seed 1234 \
    || die "world fixture generation failed"
"$BIN_DIR/refrain-tools" world import --from "$WORLD_ROOT/anvil" \
    --to "$WORLD_ROOT/rwf" >"$WORLD_ROOT/import.txt" \
    || die "world import failed"
grep -q 'world import: 4 chunks' "$WORLD_ROOT/import.txt" \
    || die "world import summary is missing"
"$BIN_DIR/refrain-tools" world fsck "$WORLD_ROOT/rwf" >"$WORLD_ROOT/fsck.txt" \
    || die "world fsck reported corruption for a freshly imported world"
grep -q 'corrupt=0' "$WORLD_ROOT/fsck.txt" || die "world fsck summary is missing"
"$BIN_DIR/refrain-tools" world info "$WORLD_ROOT/rwf" --json \
    >"$WORLD_ROOT/info.json" || die "world info failed"
grep -q '"chunks":4' "$WORLD_ROOT/info.json" || die "world info chunk count is wrong"
"$BIN_DIR/refrain-tools" world info "$WORLD_ROOT/rwf" --strict \
    >"$WORLD_ROOT/info-strict.txt" || die "world info --strict rejected a valid world"
"$BIN_DIR/refrain-tools" world export --from "$WORLD_ROOT/rwf" \
    --to "$WORLD_ROOT/exported" >"$WORLD_ROOT/export.txt" \
    || die "world export failed"
python3 "$ROOT/tools/make_test_world.py" verify "$WORLD_ROOT/exported" --json \
    >"$WORLD_ROOT/verify.json" \
    || die "exported world failed independent Anvil validation"
"$BIN_DIR/refrain-tools" world diff "$WORLD_ROOT/anvil" "$WORLD_ROOT/exported" \
    >"$WORLD_ROOT/diff.txt" || die "world diff is not at 100% after round-trip"
grep -q 'match=100.00%' "$WORLD_ROOT/diff.txt" || die "world diff summary is missing"
"$BIN_DIR/refrain-tools" worldgen harness --candidate "$WORLD_ROOT/rwf" \
    --oracle "$WORLD_ROOT/anvil" --report "$WORLD_ROOT/report.json" \
    >"$WORLD_ROOT/harness.txt" || die "worldgen harness is not at 100%"
grep -q '"chunks":4' "$WORLD_ROOT/report.json" || die "harness report is missing"
# Palette GC rebuilds the world into a fresh directory; semantics stay identical.
"$BIN_DIR/refrain-tools" world palette-gc --from "$WORLD_ROOT/rwf" \
    --to "$WORLD_ROOT/gc" >"$WORLD_ROOT/gc.txt" \
    || die "world palette-gc failed"
"$BIN_DIR/refrain-tools" world diff "$WORLD_ROOT/rwf" "$WORLD_ROOT/gc" \
    >"$WORLD_ROOT/gc-diff.txt" || die "world palette-gc changed chunk semantics"
grep -q 'match=100.00%' "$WORLD_ROOT/gc-diff.txt" \
    || die "world palette-gc diff is not 100%"
# Default per-dimension zstd training: 128 chunks exceed the 64-chunk/256 KiB
# sample threshold, so the dictionary file must exist afterwards.
python3 "$ROOT/tools/make_test_world.py" gen --out "$WORLD_ROOT/dict-anvil" \
    --seed 7 --chunks 128 >/dev/null \
    || die "dictionary fixture generation failed"
"$BIN_DIR/refrain-tools" world import --from "$WORLD_ROOT/dict-anvil" \
    --to "$WORLD_ROOT/dict-rwf" >/dev/null \
    || die "dictionary import failed"
test -f "$WORLD_ROOT/dict-rwf/dimensions/minecraft/overworld/dictionaries/chunk.zdict" \
    || die "auto per-dimension dictionary was not trained"
"$BIN_DIR/refrain-tools" world fsck "$WORLD_ROOT/dict-rwf" >/dev/null \
    || die "fsck failed for a world with a trained dictionary"
# Corruption must fail closed: a flipped chunk byte makes fsck exit non-zero.
cp -R "$WORLD_ROOT/rwf" "$WORLD_ROOT/damaged"
WORLD_REGION="$WORLD_ROOT/damaged/dimensions/minecraft/overworld/regions/0.0.rfw"
python3 - "$WORLD_REGION" <<'PY'
import sys
path = sys.argv[1]
data = bytearray(open(path, "rb").read())
data[77824 + 40] ^= 0x5A  # first record payload (RWF data area offset)
open(path, "wb").write(data)
PY
if "$BIN_DIR/refrain-tools" world fsck "$WORLD_ROOT/damaged" \
    >"$WORLD_ROOT/damaged.txt" 2>&1; then
    die "world fsck accepted a corrupted region"
fi
"$BIN_DIR/refrain-tools" world fsck --repair "$WORLD_ROOT/damaged" \
    >"$WORLD_ROOT/repaired.txt" 2>&1 \
    || die "world fsck --repair failed"
grep -q 'repaired=yes' "$WORLD_ROOT/repaired.txt" \
    || die "world fsck --repair did not report a repair"
"$BIN_DIR/refrain-tools" world fsck "$WORLD_ROOT/damaged" \
    >"$WORLD_ROOT/after-repair.txt" \
    || die "world fsck still reports corruption after --repair"
# Headless runtime smoke: scripted block edit + deterministic tick hash.
# Two runs over the same world must produce the same hash at the same tick.
RUNTIME_SCRIPT="$WORLD_ROOT/runtime-script.txt"
cat >"$RUNTIME_SCRIPT" <<'EOF'
0 place 0 64 0 minecraft:dirt
5 hash
10 stop
EOF
for run in 1 2; do
    "$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
        --world "$WORLD_ROOT/rwf" --script "$RUNTIME_SCRIPT" \
        --hash-every 5 --ticks 20 >"$WORLD_ROOT/runtime-$run.log" 2>&1 \
        || die "runtime smoke run $run failed"
    grep -q 'replay: tick=5 hash=' "$WORLD_ROOT/runtime-$run.log" \
        || die "runtime smoke run $run did not emit its replay hash"
done
sed -n 's/.*replay: tick=5 hash=//p' "$WORLD_ROOT/runtime-1.log" \
    >"$WORLD_ROOT/runtime-hash-1.txt"
sed -n 's/.*replay: tick=5 hash=//p' "$WORLD_ROOT/runtime-2.log" \
    >"$WORLD_ROOT/runtime-hash-2.txt"
cmp -s "$WORLD_ROOT/runtime-hash-1.txt" "$WORLD_ROOT/runtime-hash-2.txt" \
    || die "runtime replay hash is not deterministic across runs"
# Scheduler-driven periodic maintenance is observable from the
# console; autosave must fire on its grid (tick 2) with a dirty chunk.
SCHEDULE_SCRIPT="$WORLD_ROOT/schedule-script.txt"
cat >"$SCHEDULE_SCRIPT" <<'EOF'
0 schedules
3 schedules
4 stop
EOF
"$BIN_DIR/refrain-server" --gamepack "$ROOT/gamepack/gamepack.toml" \
    --world "$WORLD_ROOT/rwf" --script "$SCHEDULE_SCRIPT" \
    --autosave-interval-ticks 2 --ticks 10 \
    >"$WORLD_ROOT/schedule.log" 2>&1 \
    || die "scheduler smoke run failed"
grep -q 'schedule: runtime.autosave phase=post_tick interval=2 .*runs=0' \
    "$WORLD_ROOT/schedule.log" \
    || die "scheduler smoke did not list the autosave task before it ran"
grep -q 'schedule: runtime.autosave .*runs=1' "$WORLD_ROOT/schedule.log" \
    || die "scheduler smoke did not run autosave on its tick grid"
grep -q 'schedule: runtime.autosave .*failures=0' "$WORLD_ROOT/schedule.log" \
    || die "scheduler smoke recorded a spurious autosave failure"
echo "  runtime: scripted edit + deterministic replay hash OK"
echo "  scheduler: autosave grid + console stats OK"

echo "  world: synthetic Anvil -> RWF -> Anvil, diff=100%, independent verify OK"

say "[9/11] extract smoke test (synthetic jar; no Mojang assets in CI)"
python3 - "$EXTRACT_DIR" <<'PY'
import os
import sys
import zipfile

root = sys.argv[1]
path = os.path.join(root, "synthetic.jar")
with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
    archive.writestr("data/minecraft/recipe/stone.json",
                     '{"type":"minecraft:crafting_shaped"}')
    archive.writestr("data/refrain/tags/block/air.json", "[]")
    archive.writestr("assets/minecraft/texts/en_us.txt", "hello")
    archive.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
print(path)
PY
"$BIN_DIR/refrain-tools" extract --jar "$EXTRACT_DIR/synthetic.jar" \
    --out "$EXTRACT_DIR/cooked" --filter all >/dev/null
grep -q '"schema":"refrain.data-manifest/1"' \
    "$EXTRACT_DIR/cooked/manifest.json" \
    || die "extract: manifest.json missing the expected schema field"
"$BIN_DIR/refrain-tools" extract --jar "$EXTRACT_DIR/synthetic.jar" \
    --out "$EXTRACT_DIR/cooked" --filter data --extract >/dev/null
[[ -f "$EXTRACT_DIR/cooked/data/minecraft/recipe/stone.json" ]] \
    || die "extract: data/minecraft/recipe/stone.json was not written"
echo "  manifest + data extraction OK"

say "[10/11] Crashpad minidump smoke"
HANDLER="$(find "$CONAN_HOME_DIR" -type f -name crashpad_handler \
    -perm -u+x -print -quit 2>/dev/null || true)"
[[ -n "$HANDLER" ]] || die "crashpad_handler not found under $CONAN_HOME_DIR \
(run: python3 tools/vendor_export.py --build)"
set +e
"$BIN_DIR/refrain-tools" crash smoke -o "$CRASH_DIR/db" \
    --handler "$HANDLER" >/dev/null 2>&1
CRASH_STATUS=$?
set -e
[[ "$CRASH_STATUS" -ne 0 ]] || die "crash smoke: the process did not crash"
DUMP=""
for _ in $(seq 1 50); do
    DUMP="$(find "$CRASH_DIR/db" -type f -name '*.dmp' -print -quit 2>/dev/null || true)"
    [[ -n "$DUMP" ]] && break
    sleep 0.2
done
[[ -n "$DUMP" ]] || die "crash smoke: Crashpad produced no minidump"
head -c 4 "$DUMP" | grep -q 'MDMP' \
    || die "crash smoke: '$DUMP' is not a minidump"
echo "  minidump: $(basename "$DUMP") ($(stat -c%s "$DUMP") bytes)"

if [[ -n "$RUN_FORMAT_CHECK" ]]; then
    say "[11/11] clang-format check (first-party sources)"
    command -v clang-format >/dev/null \
        || die "clang-format not found (pip install 'clang-format>=23' or apt install clang-format-23)"
    mapfile -t FORMAT_FILES < <(
        # --others: include new untracked sources in local runs; in CI the
        # checkout has them tracked already.
        git ls-files --cached --others --exclude-standard '*.cpp' '*.hpp' '*.h' \
            | grep -v '^sdk/generated/' || true
    )
    [[ ${#FORMAT_FILES[@]} -gt 0 ]] || die "no C/C++ sources found for the format check"
    if ! clang-format --dry-run --Werror "${FORMAT_FILES[@]}"; then
        die "clang-format violations (run clang-format -i on the files above)"
    fi
    echo "  ${#FORMAT_FILES[@]} files formatted"
else
    say "[11/11] clang-format check skipped (--skip-format)"
fi

say "CI OK (11 steps)"
