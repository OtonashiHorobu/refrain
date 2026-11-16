# ABI-mismatch fixture

One mod built for ABI 99. The CI smoke packs it, installs it into a content
store and resolves that store as the mod catalog; the server must fail with
`version_mismatch`: the ABI gate applies to pinned *and* catalogued mods.
