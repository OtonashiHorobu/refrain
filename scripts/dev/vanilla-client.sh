#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
#
# Launch the vanilla Minecraft Java 26.3 client for Refrain end-to-end tests.
#
# The client runtime is provisioned outside the repository (client jar,
# libraries, assets, logging config).  This script never downloads anything:
# use the provisioner that produced the runtime, then point MC_CLIENT_HOME at
# it.  All game files stay outside git.
#
#   scripts/dev/vanilla-client.sh            # launch against 127.0.0.1:25565
#   scripts/dev/vanilla-client.sh --check    # validate the runtime, no GUI
#   MC_SERVER=127.0.0.1:25566 scripts/dev/vanilla-client.sh
#   MC_RUN_DIR=/tmp/refrain-client scripts/dev/vanilla-client.sh
#
# Environment:
#   MC_CLIENT_HOME  runtime root (required)
#   MC_RUN_DIR      game directory (default: $MC_CLIENT_HOME/run)
#   MC_SERVER       quick-play address (default: 127.0.0.1:25565; empty = menu)
#   MC_NATIVES_DIR  native scratch space (default: $MC_CLIENT_HOME/natives)
#   MC_JAVA         java executable (default: $JAVA_HOME/bin/java, else java)
#   MC_USERNAME     offline username (default: Dev)

set -euo pipefail

CLIENT_HOME="${MC_CLIENT_HOME:?set MC_CLIENT_HOME to the 26.3 client runtime}"
RUN_DIR="${MC_RUN_DIR:-$CLIENT_HOME/run}"
NATIVES_DIR="${MC_NATIVES_DIR:-$CLIENT_HOME/natives}"
SERVER="${MC_SERVER-127.0.0.1:25565}"
USERNAME="${MC_USERNAME:-Dev}"

# Resolve relative overrides once: the launch changes directory to the game
# directory, and every runtime path handed to Java must stay valid.
CLIENT_HOME="$(realpath -m "$CLIENT_HOME")"
RUN_DIR="$(realpath -m "$RUN_DIR")"
NATIVES_DIR="$(realpath -m "$NATIVES_DIR")"
VERSION_JSON="$(realpath -m "${MC_VERSION_JSON:-$CLIENT_HOME/26.3.json}")"

if [[ -n "${MC_JAVA:-}" ]]; then
    JAVA_BIN="$MC_JAVA"
elif [[ -n "${JAVA_HOME:-}" ]]; then
    JAVA_BIN="$JAVA_HOME/bin/java"
else
    JAVA_BIN="java"
fi

CHECK_ONLY=0
for arg in "$@"; do
    case "$arg" in
        --check) CHECK_ONLY=1 ;;
        -h|--help) sed -n '6,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

die() { printf '\033[1;31mvanilla-client: %s\033[0m\n' "$*" >&2; exit 1; }
say() { printf '\033[1;36m%s\033[0m\n' "$*"; }

[[ -f "$VERSION_JSON" ]] || die "missing version json: $VERSION_JSON (provision the client runtime first)"
[[ -d "$CLIENT_HOME" ]] || die "missing client runtime: $CLIENT_HOME"

ASSET_INDEX="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["assetIndex"]["id"])' "$VERSION_JSON")"
[[ -n "$ASSET_INDEX" ]] || die "version json does not name an asset index"

# Build the classpath from the version manifest, keeping only libraries whose
# rules allow linux/<arch>.  Native jars stay on the classpath: LWJGL extracts
# them into the directory named by org.lwjgl.system.SharedLibraryExtractPath.
classpath() {
    python3 - "$VERSION_JSON" "$CLIENT_HOME" <<'PY'
import json
import os
import platform
import sys

version_json, client_home = sys.argv[1], sys.argv[2]
with open(version_json, encoding="utf-8") as handle:
    manifest = json.load(handle)

machine = platform.machine()
arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}.get(
    machine, machine
)


def allowed(library):
    rules = library.get("rules")
    if not rules:
        return True
    result = False
    for rule in rules:
        matched = True
        os_rule = rule.get("os")
        if os_rule:
            if os_rule.get("name") and os_rule["name"] != "linux":
                matched = False
            if os_rule.get("arch") and os_rule["arch"] != arch:
                matched = False
        features = rule.get("features")
        if features and any(features.values()):
            matched = False
        if matched:
            result = rule.get("action") == "allow"
    return result


entries = []
for library in manifest["libraries"]:
    artifact = library["downloads"].get("artifact")
    if artifact is None:
        continue
    path = os.path.join(client_home, "libraries", artifact["path"])
    if not os.path.isfile(path):
        sys.stderr.write(f"missing library: {path}\n")
        sys.exit(1)
    if allowed(library):
        entries.append(path)

client_jar = os.path.join(client_home, "client.jar")
if not os.path.isfile(client_jar):
    sys.stderr.write(f"missing client jar: {client_jar}\n")
    sys.exit(1)
entries.append(client_jar)
print(os.pathsep.join(entries))
PY
}

CP="$(classpath)" || die "classpath construction failed"

if [[ "$CHECK_ONLY" -eq 1 ]]; then
    say "Minecraft 26.3 runtime check"
    python3 - "$VERSION_JSON" "$CLIENT_HOME" <<'PY'
import hashlib
import json
import os
import platform
import sys

version_json, client_home = sys.argv[1], sys.argv[2]
failures = []
counts = {"libraries": 0, "classpath": 0, "assets": 0}
asset_bytes = 0


def digest(path):
    value = hashlib.sha1()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            value.update(chunk)
    return value.hexdigest()


def check(path, size, sha1, label):
    if not os.path.isfile(path):
        failures.append(f"{label}: missing {path}")
        return False
    actual_size = os.path.getsize(path)
    if actual_size != size:
        failures.append(f"{label}: size {actual_size} != {size}: {path}")
        return False
    actual_sha1 = digest(path)
    if actual_sha1 != sha1:
        failures.append(f"{label}: sha1 {actual_sha1} != {sha1}: {path}")
        return False
    return True


with open(version_json, encoding="utf-8") as handle:
    version = json.load(handle)

if version.get("id") != "26.3":
    failures.append(f"version json: unexpected id {version.get('id')!r}")

client = version["downloads"]["client"]
check(os.path.join(client_home, "client.jar"), client["size"], client["sha1"], "client.jar")

machine = platform.machine()
arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}.get(
    machine, machine
)


def allowed(library):
    rules = library.get("rules")
    if not rules:
        return True
    result = False
    for rule in rules:
        matched = True
        os_rule = rule.get("os")
        if os_rule:
            if os_rule.get("name") and os_rule["name"] != "linux":
                matched = False
            if os_rule.get("arch") and os_rule["arch"] != arch:
                matched = False
        features = rule.get("features")
        if features and any(features.values()):
            matched = False
        if matched:
            result = rule.get("action") == "allow"
    return result


for library in version["libraries"]:
    artifact = library["downloads"].get("artifact")
    if artifact is None:
        continue
    counts["libraries"] += 1
    check(
        os.path.join(client_home, "libraries", artifact["path"]),
        artifact["size"],
        artifact["sha1"],
        f"library {library['name']}",
    )
    if allowed(library):
        counts["classpath"] += 1

index = version["assetIndex"]
index_path = os.path.join(client_home, "assets", "indexes", f"{index['id']}.json")
if check(index_path, index["size"], index["sha1"], f"asset index {index['id']}"):
    with open(index_path, encoding="utf-8") as handle:
        objects = json.load(handle)["objects"]
    seen = set()
    for entry in objects.values():
        sha1 = entry["hash"]
        if sha1 in seen:
            continue
        seen.add(sha1)
        counts["assets"] += 1
        asset_bytes += entry["size"]
        check(
            os.path.join(client_home, "assets", "objects", sha1[:2], sha1),
            entry["size"],
            sha1,
            "asset",
        )

logging_client = version.get("logging", {}).get("client", {})
logging_file = logging_client.get("file")
if logging_file:
    check(
        os.path.join(client_home, "logging", os.path.basename(logging_file["id"])),
        logging_file["size"],
        logging_file["sha1"],
        "logging config",
    )

print(f"  version json   ok (id {version.get('id')})")
print(f"  client jar     ok ({client['size']} bytes)")
print(f"  libraries      {counts['libraries']} verified, {counts['classpath']} on the linux classpath")
print(f"  asset index    {index['id']} ok")
print(f"  assets         {counts['assets']} objects verified ({asset_bytes} bytes)")
if logging_file:
    print("  logging config ok")

if failures:
    print(f"  FAIL: {len(failures)} problem(s)")
    for failure in failures[:20]:
        print(f"    {failure}")
    sys.exit(1)
print("  runtime check: PASS")
PY
    status=$?
    [[ $status -eq 0 ]] || exit $status

    JAVA_MAJOR="$("$JAVA_BIN" -version 2>&1 | sed -n 's/.*version "\([0-9][0-9]*\).*/\1/p' | head -1)"
    JAVA_VERSION="$("$JAVA_BIN" -version 2>&1 | head -1)"
    [[ -n "$JAVA_MAJOR" ]] || die "cannot determine Java version from $JAVA_BIN"
    [[ "$JAVA_MAJOR" -ge 25 ]] || die "Minecraft 26.3 needs Java 25 or newer, found: $JAVA_VERSION"
    say "  java           $JAVA_VERSION"
    say "PASS"
    exit 0
fi

# --- launch ---------------------------------------------------------------
[[ -f "$CLIENT_HOME/assets/indexes/$ASSET_INDEX.json" ]] || die "missing asset index: $CLIENT_HOME/assets/indexes/$ASSET_INDEX.json"
[[ -f "$CLIENT_HOME/logging/client-1.21.2.xml" ]] || die "missing log4j config: $CLIENT_HOME/logging/client-1.21.2.xml"

mkdir -p "$RUN_DIR" "$NATIVES_DIR/java" "$NATIVES_DIR/jna" "$NATIVES_DIR/lwjgl" "$NATIVES_DIR/netty"

command -v "$JAVA_BIN" >/dev/null 2>&1 || die "java executable not found: $JAVA_BIN"
JAVA_MAJOR="$("$JAVA_BIN" -version 2>&1 | sed -n 's/.*version "\([0-9][0-9]*\).*/\1/p' | head -1)"
[[ -n "$JAVA_MAJOR" ]] || die "cannot determine Java version from $JAVA_BIN"
[[ "$JAVA_MAJOR" -ge 25 ]] || die "Minecraft 26.3 needs Java 25 or newer, found $("$JAVA_BIN" -version 2>&1 | head -1)"

ARGS=(
    -XX:StackShadowPages=32
    --enable-native-access=ALL-UNNAMED
    --add-exports java.base/jdk.internal.misc=ALL-UNNAMED
    "-Djava.library.path=$NATIVES_DIR/java"
    "-Djna.tmpdir=$NATIVES_DIR/jna"
    "-Dorg.lwjgl.system.SharedLibraryExtractPath=$NATIVES_DIR/lwjgl"
    "-Dio.netty.native.workdir=$NATIVES_DIR/netty"
    -Dminecraft.launcher.brand=refrain-dev
    -Dminecraft.launcher.version=dev
    "-Dlog4j.configurationFile=$CLIENT_HOME/logging/client-1.21.2.xml"
    -cp "$CP"
    net.minecraft.client.main.Main
    --username "$USERNAME"
    --version 26.3
    --gameDir "$RUN_DIR"
    --assetsDir "$CLIENT_HOME/assets"
    --assetIndex "$ASSET_INDEX"
    --uuid 00000000-0000-0000-0000-000000000000
    --accessToken 0
    --clientId ""
    --xuid ""
    --versionType release
)

if [[ -n "$SERVER" ]]; then
    ARGS+=(--quickPlayMultiplayer "$SERVER")
fi

# log4j's file appender uses a path relative to the working directory.  Run
# from the game directory so logs and crash reports stay outside the repo.
cd "$RUN_DIR"

say "launching Minecraft 26.3 (user $USERNAME, server ${SERVER:-none}, run dir $RUN_DIR)"
exec "$JAVA_BIN" "${ARGS[@]}"
