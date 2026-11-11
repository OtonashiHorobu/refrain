# Third-party notices

Refrain is distributed under the MIT License (see `LICENSE`). This file
summarizes the third-party components that Refrain links against or ships.
The exact license texts are distributed with each Conan package under its
`licenses/` directory and accompany every binary distribution.

## Minecraft-derived data

The game-derived tables that Refrain ships — compiled block-state and registry
tables, biome parameters, block default states, the packet catalog and the
protocol registry — live under `content/vanilla/`. That directory is not
covered by the MIT license; the material is Copyright (c) Mojang Studios /
Microsoft Corporation and is included solely for interoperability with a
locally owned copy of the game. See `content/vanilla/NOTICE.md` for the full
terms.

Vanilla Minecraft datapacks are not distributed in this repository; the
datapack and its manifest are generated locally from the user's own
`server.jar` with `scripts/fetch-content.sh`. No Mojang code or assets ship in
source files or in the libraries below.

## Runtime and content libraries

| Component | Version | License | Used for |
|---|---|---|---|
| spdlog | 1.17.0 | MIT | logging sinks and formatting |
| fmt | 12.2.0 | MIT | project-wide string formatting |
| CLI11 | 2.7.2 | BSD-3-Clause | command-line parsing |
| glaze | 9.1.0 | MIT | TOML DOM for gamepack and mod manifests |
| xxHash | 0.8.4 | BSD-2-Clause | schema and generated-file drift hashes |
| Asio | 1.38.2 | BSL-1.0 | asynchronous I/O and network runtime |
| liburing | 2.15 | MIT | Linux io_uring backend (optional) |
| mimalloc | 3.6.0 | MIT | allocator backend (optional) |
| simdjson | 5.1.0 | Apache-2.0 | JSON parsing and serialization |
| zlib (zlib-ng drop-in) | 1.3.1 | Zlib | deflate for zip and region data |
| zstd | 1.5.7 | BSD-3-Clause | RWF and content compression |
| LZ4 | 1.10.0 | BSD-2-Clause | LZ4-compressed Anvil region import and export |
| BLAKE3 | 1.8.7 | CC0-1.0 OR Apache-2.0 OR Apache-2.0 WITH LLVM-exception | content addressing |
| libsodium | 1.0.22 | ISC | Ed25519 package signatures |
| libzip | 1.12.1 | BSD-3-Clause | jar and zip extraction |
| libarchive | 3.8.11 | BSD-2-Clause | package tar read side |
| crc32c | 1.1.2 | BSD-3-Clause | container and record checksums |
| Crashpad | 2026.10.26 | Apache-2.0 | crash reporting (optional) |
| wasmtime | 50.0.1 | Apache-2.0 WITH LLVM-exception | WebAssembly runtime (optional) |
| Steam Audio | 4.8.1 | Apache-2.0 | spatial audio (optional) |

## Test and development libraries

| Component | Version | License | Used for |
|---|---|---|---|
| Boost.Ext.UT | 2.3.1 | BSL-1.0 | unit tests |
| Google Benchmark | 1.10.0 | Apache-2.0 | benchmarks |
| refl-cpp | 0.12.4 | MIT | annotation-based reflection |
| magic_enum | 0.9.8 | MIT | enum names |
| EnTT | 4.0.0 | MIT | entity and metadata support |

## Notes

- Minecraft code and assets are not part of this repository or its
  distributions. The derived tables under `content/vanilla/` are the only
  game-derived material that ships; local game files are used only as an
  interoperability oracle.
- Release archives carry the license text of every runtime dependency under
  `licenses/`. MinGW archives also carry the GCC runtime license and exception
  because libstdc++ and libgcc are linked statically; MSVC archives carry a note
  about the app-local Visual C++ runtime DLLs.
- Some Conan build requirements (CMake, Ninja, pkg-config, autoconf, libtool)
  are build-time tools and are not shipped.
- When a dependency changes its license, update this table together with
  `conan/locks/vendor.lock` and the corresponding in-repo recipe.
