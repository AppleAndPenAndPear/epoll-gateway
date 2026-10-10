# Supply-Chain Gate

A release must not ship a known-vulnerable dependency. `./ci.sh supply-chain`
runs three gates over the source tree and the linked binary and fails the build
on a real defect. It is implemented in [`scripts/supply_chain.sh`](../scripts/supply_chain.sh).

This gate is a **release prerequisite**: it runs in the `test` job of
[`.github/workflows/ci.yml`](../.github/workflows/ci.yml) after build/unit/
integration, and a failing gate fails the job.

## The three gates

| Gate | Tool | What it does | Fails the build on |
| --- | --- | --- | --- |
| **SAST** | `cppcheck` | Static analysis of `src/` and `include/` (C++17) | `error` severity only (definite bugs) |
| **SBOM** | built in (`ldd` + `dpkg-query`) | CycloneDX 1.5 inventory of the project plus every runtime shared library of `build/server` | a malformed/unwritable SBOM |
| **SCA** | `osv-scanner` | Known-vulnerability scan of the SBOM against the OSV database | any reported vulnerability |

### Why SAST only fails on `error` severity

`cppcheck` categorises findings by severity. The gate treats only `error`
findings as blocking, because those are definite bugs (null dereference, syntax
errors, out-of-bounds). `warning` / `performance` / `portability` findings are
printed and counted but do not fail the build — on an existing codebase a
severity-everything gate is immediately blocked by style noise and gets
disabled, which is worse than a narrow gate that stays on.

### Why an SBOM of the *linked libraries* specifically

The interesting supply-chain surface of this project is its dynamic
dependencies, not its own source tree. The SBOM therefore enumerates every
shared library `build/server` resolves through `ldd`, taking the version from
the owning Debian package (`dpkg-query -S` / `-W`) and falling back to the
soname filename when there is no package owner. It is generated **fully
offline** and is written to `build/sbom.cdx.json`; `python3 -m json.tool`
validates it before the gate can pass.

## Required vs optional

| Tool | Gate | If missing |
| --- | --- | --- |
| `cppcheck` | SAST | gate **SKIPPED** with the install command |
| `python3` | SBOM | gate **FAILS** (needed to emit/validate JSON) |
| `osv-scanner` | SCA | gate **SKIPPED** with the install hint |
| network | SCA only | `osv-scanner` cannot query OSV for updated advisories |

A missing tool is always reported as **SKIPPED**, never as a silent PASS:
"we did not look" must not read as "nothing was found".

## Running it

```bash
./ci.sh build              # once, so the SBOM can see the linked libraries
./ci.sh supply-chain       # all three gates
```

Or directly:

```bash
./scripts/supply_chain.sh              # all three gates
./scripts/supply_chain.sh --sbom-only  # SBOM only (works with no scanner installed)
```

The SBOM gate runs even when `build/server` is absent: it then reports this
clearly and emits an SBOM containing only the project component.

### Installing the optional tools

```bash
sudo apt-get install -y cppcheck
# osv-scanner: single static binary from
# https://github.com/google/osv-scanner/releases
```

Install `osv-scanner` as the Python/Go distributions do (or drop the release
binary on `PATH`); CI downloads the `linux_amd64` asset into `~/.local/bin`.

## Scope and current limits

- SCA is **only as fresh as the OSV database it queries** — it needs network
  access. Locally, a run without network and without `osv-scanner` will report
  SCA as SKIPPED.
- The SBOM describes the **build host's** library set. The packaged tarball
  changes which libraries are bundled vs. taken from the target distribution;
  cross-distribution runtime resolution is verified separately by
  [`scripts/verify_artifact.sh`](../scripts/verify_artifact.sh).
- SAST coverage is limited to what `cppcheck` detects; it is a floor, not a
  proof of absence.
