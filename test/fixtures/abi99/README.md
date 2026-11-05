# ABI-mismatch fixture

One auto-scanned directory mod built for ABI 99. `refrain-server --auto-scan
--mods test/fixtures/abi99 --gamepack gamepack/gamepack.toml` must fail with
`version_mismatch`: the ABI gate applies to pinned *and* auto-scanned mods.
