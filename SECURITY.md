# Security policy

## Supported versions

Refrain has no published release yet. Security fixes are applied to the
development branch; older snapshots are not maintained.

## Reporting a vulnerability

Do not open a public issue for a security problem. Use the repository's private
vulnerability reporting channel (GitHub security advisories) and include:

- the affected component and version or commit;
- a minimal reproduction, or a description precise enough to reproduce it;
- the impact you believe it has;
- any suggested fix or mitigation.

Reports are acknowledged as soon as a maintainer reads them, normally within a
few days. A fix and an advisory are prepared before public disclosure. Credit
is given unless you ask otherwise.

## Scope

The following areas are treated as security relevant:

- memory safety in parsers for untrusted input: NBT, JSON, Anvil regions, RWF
  records, zip and tar containers, datapacks and mod packages;
- path handling for archives, world directories and content stores, including
  symlink and path traversal defenses;
- signature verification for mod packages and trusted key handling;
- resource exhaustion through unbounded allocations, recursion or decompression;
- crash reporting paths that could expose local data.

Minecraft itself is out of scope. Report game vulnerabilities to Mojang.
