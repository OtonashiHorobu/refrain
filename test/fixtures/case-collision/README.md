# Case-collision fixture

Synthetic mod tree with two ids that only differ by case (`Alpha` vs `alpha`).
`refrain-tools mods check --mods test/fixtures/case-collision/mods` must fail with
`already_exists`: the single scanner groups by ASCII-folded id, so portable
(case-insensitive) file systems cannot represent the tree unambiguously.

No Mojang content: the manifests are hand-written text.
