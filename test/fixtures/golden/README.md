# Golden baselines

This directory holds the frozen contract baselines for mod composition. The
outputs and bytes below must not change silently: any drift appears here as a
reviewable diff.

| File | Frozen object | Consumers |
|---|---|---|
| `mods-lock.canonical.txt` | `mod::mod_lock::canonical_text()` | `mods.lock` fingerprint, world identity |
| `mods-lock.fingerprint.txt` | `mod::mod_lock::fingerprint()` (BLAKE3 lowercase hex) | same as above |
| `mods-lock.txt` | `mod::format_mod_lock()` (`path` normalised to `<root>`) | `refrain-tools mods list`, server startup log |
| `startup-plan.txt` | `mod::composition::format()` (manifest-level preview) | `gamepack check` / `dump`, `--dump-gamepack`, CI grep |
| `cli-contract.txt` | CLI output stream and string contract | `tools/cli_contract_check.py`, `scripts/ci.sh`, GitHub workflow |

## Provenance

`gamepack/gamepack.toml` (10 builtin mods, `auto_scan=false`) plus the
repository root `mods/` (origin=builtin) plus `test/fixtures/instance`
(origin=user, `auto_scan=true`, matching the CI `refrain-server --auto-scan`
smoke) produce the 11-mod `mods.lock`.

## Updating

```bash
REFRAIN_UPDATE_GOLDEN=1 build/Release/bin/refrain-tests
# review the diff under test/fixtures/golden/ before committing
```

Update only for an intentional contract change (for example, stronger
determinism or a format change). Fix the code when a refactor causes drift, not
the golden files.

## Verification

```bash
build/Release/bin/refrain-tests                       # golden tests (suite mod/pipeline-golden)
python3 tools/cli_contract_check.py \
    --root . --bin-dir build/Release/bin              # CLI stream/string contract
```

The tools smoke step in `scripts/ci.sh` runs both.
