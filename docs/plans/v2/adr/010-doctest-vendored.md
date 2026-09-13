# ADR-010: doctest, vendored

**Status:** Proposed

## Context
No C++ test framework is installed on the dev machine (`catch`, `gtest`,
`doctest` all absent). CI runs in a container where we can install anything,
but developers should be able to run tests without extra packages. GLib's
`g_test` works but is C-flavoured and awkward for value-semantic C++ models.

## Decision
Vendor the doctest single header (MIT) under `subprojects/packagefiles/`
with a meson wrap so `dependency('doctest')` resolves offline. One test
executable per layer. Property-style tests use a small in-repo random
generator seeded from the environment (`USTUDIO_TEST_SEED`) for
reproducibility; no additional framework.

## Consequences
- Zero new system packages for tests; one ~7k-line header in the repo.
- Upgrades are a file replacement plus a changelog line.
- If the team prefers Catch2/GTest later, tests are written with plain
  `CHECK`-style macros and port mechanically.
