# Synthetic datapack fixture

Used by `refrain-tools data check` in CI and by the datapack parser tests.
All ids/content are ours (`refrain_test:*`) — no Mojang assets are copied into
this repository. Keep the fixture small and deterministic; changing a file
changes the parser counts asserted in CI.
