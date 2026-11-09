#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
#
# SPDX-FileCopyrightText: Copyright (c) 2026 Otonashi Horobu
"""Capture the clientbound stream from a locally owned vanilla server.

This is an opt-in developer oracle: no server jar, assets or vanilla output is
shipped in the repository. Pass --accept-eula only when the local server jar is
yours and you accept Mojang's EULA.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time

PROPERTIES = {
    "allow-nether": "false",
    "difficulty": "normal",
    "enable-status": "true",
    "enforce-secure-profile": "false",
    "gamemode": "creative",
    "level-name": "oracle",
    "level-seed": "1234",
    "max-players": "20",
    "motd": "Refrain oracle",
    "online-mode": "false",
    "server-ip": "127.0.0.1",
    "server-port": "{port}",
    "simulation-distance": "3",
    "spawn-protection": "0",
    "sync-chunk-writes": "false",
    "view-distance": "3",
    "white-list": "false",
    "enforce-whitelist": "false",
}


def wait_for_done(process: subprocess.Popen[str], timeout: float) -> None:
    lines: queue.Queue[str | None] = queue.Queue()

    def pump() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            lines.put(line)
        lines.put(None)

    threading.Thread(target=pump, daemon=True).start()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            line = lines.get(timeout=1.0)
        except queue.Empty:
            if process.poll() is not None:
                break
            continue
        if line is None:
            break
        print(line, end="", flush=True)
        if "Done (" in line:
            return
    raise RuntimeError("vanilla server did not reach the Done state")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-jar", type=pathlib.Path, required=True)
    parser.add_argument(
        "--libraries",
        type=pathlib.Path,
        default=None,
        help="directory with the vanilla server library jars",
    )
    parser.add_argument("--accept-eula", action="store_true")
    parser.add_argument("--port", type=int, default=25566)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument(
        "--world",
        type=pathlib.Path,
        default=None,
        help="copy an existing Anvil world into the oracle server directory",
    )
    parser.add_argument("--java", default="java")
    parser.add_argument("--tools", type=pathlib.Path, default=None)
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    if not args.accept_eula:
        print("oracle: pass --accept-eula after reviewing Mojang's EULA", file=sys.stderr)
        return 2
    if not args.server_jar.is_file():
        print(f"oracle: server jar not found: {args.server_jar}", file=sys.stderr)
        return 2
    tools = args.tools
    if tools is None:
        tools = pathlib.Path(__file__).resolve().parents[3] / "build/Release/bin/refrain-tools"
    if not tools.is_file():
        print(f"oracle: refrain-tools not found: {tools}", file=sys.stderr)
        return 2

    work = pathlib.Path(tempfile.mkdtemp(prefix="refrain-oracle-"))
    try:
        if args.world is not None:
            if not args.world.is_dir():
                print(f"oracle: world not found: {args.world}", file=sys.stderr)
                return 2
            shutil.copytree(args.world, work / "oracle")
        (work / "eula.txt").write_text("eula=true\n", encoding="utf-8")
        properties = "\n".join(
            f"{key}={value.format(port=args.port)}"
            for key, value in PROPERTIES.items()
        )
        (work / "server.properties").write_text(properties + "\n", encoding="utf-8")
        command = [args.java]
        if args.libraries is not None:
            classpath = [str(args.server_jar)]
            classpath.extend(
                str(path) for path in sorted(args.libraries.rglob("*.jar"))
            )
            command.extend(
                ["-cp", os.pathsep.join(classpath), "net.minecraft.server.Main"]
            )
        else:
            command.extend(["-jar", str(args.server_jar)])
        command.append("--nogui")
        process = subprocess.Popen(
            command,
            cwd=work,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_for_done(process, args.timeout)
            args.out.parent.mkdir(parents=True, exist_ok=True)
            capture = subprocess.run(
                [
                    str(tools),
                    "protocol",
                    "capture",
                    "--host",
                    f"127.0.0.1:{args.port}",
                    "--out",
                    str(args.out),
                    "--username",
                    "OracleBot",
                    "--wait-chunks",
                    "1",
                    "--linger-ms",
                    "500",
                ],
                check=False,
            )
            if capture.returncode != 0:
                print("oracle: capture failed", file=sys.stderr)
                return 1
            print(f"oracle: vanilla capture -> {args.out}")
            return 0
        finally:
            if process.poll() is None:
                assert process.stdin is not None
                process.stdin.write("stop\n")
                process.stdin.flush()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"oracle: {error}", file=sys.stderr)
        raise SystemExit(2) from error
