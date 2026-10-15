# Windows DLL artifact builds

`.github/workflows/libass-dll.yml` retains MSVC x64, classic `C:\vcpkg`,
`x64-windows-static`, a shared Mangetsu library and `-Db_vscrt=mt`. The dependency
request remains FreeType, FriBidi, HarfBuzz and pkgconf, including default
features and transitive packages. Both Debug and Release dependency variants
are still built on a cold cache.

## Persistent binary packages

The supported `actions/cache/restore@v5` and `actions/cache/save@v5` actions
persist only `$RUNNER_TEMP\vcpkg-binary-cache`. This is
`VCPKG_DEFAULT_BINARY_CACHE`, with `VCPKG_BINARY_SOURCES=clear;default,readwrite`.
Only vcpkg's ABI-addressed ZIP packages live there; no source checkout,
buildtrees, downloads, installed tree or Mangetsu build directory is cached.

The configuration key includes Windows/runner architecture, target triplet,
MSVC version and compiler hash, SDK version, vcpkg checkout revision and
executable hash, target/host triplet contents, and the dependency request
(including any explicitly requested features). It does not include Mangetsu
source files or the Mangetsu commit. Registry port versions, default features
and helper recipes are covered by the vcpkg revision. If custom overlay
triplets are introduced later, extend the key helper to fingerprint those files.

Actions caches are immutable. Each saved key ends in a fingerprint of the
sorted archive names and sizes. Restore first searches the current configuration
prefix, then older configurations in the same Windows/architecture/triplet
family. **vcpkg still checks each package's ABI**, including dependency ABIs,
compiler, triplet, recipe, features and relevant tools; a prefix match never
forces an incompatible package to be used. New ABIs build normally and create
new archives. A snapshot is saved immediately after successful installation
only when its inventory changed. Unchanged warm builds upload no new snapshot.
Concurrent identical uploads converge on the same content-derived key.

This gives renderer-only changes stable reuse, while dependency/baseline,
compiler/SDK and triplet changes select a new configuration. Older compatible
packages can still seed it. Changes in additional ABI inputs not represented
in the outer Actions key can refresh the archive inventory without overwriting
an existing snapshot. Cache eviction, branch scoping, runner image updates and
ABI changes can still cause cold or partial rebuilds. Old snapshots are subject
to normal GitHub cache quota/eviction; no cache-deletion permission is needed.

## Diagnostics and artifact checks

The job summary reports actual Actions step durations, including Python/MSVC
and tool setup, archive restore/save, dependency installation, Meson configure,
compilation, installation/packaging and artifact upload. It separately reports
Actions' matched cache key and vcpkg's own restored/built package counts.
Compatible Meson and Ninja installations are reused after version probes.
NASM is installed only when absent; pip itself is not unconditionally upgraded.

`libass-dll` keeps the canonical DLL, `ass.dll`, `libassmod.dll`, `assmod.dll`,
`libass.dll`, `mangetsu.dll`, the same-build PDB and `libassmod-build.txt`.
Every alias is SHA-256 checked against the canonical DLL. A standard-library
Python check compares the DLL's CodeView GUID/age/PDB name with the PDB info
stream, and records that identity in the build metadata.

## Follow-up opportunities

A release-only triplet (`VCPKG_BUILD_TYPE=release`) can reduce cold dependency
build work, but is intentionally not introduced here. The current Meson default
is `debugoptimized`, and custom-triplet CRT/library selection and host-tool
requirements need separate validation before changing the dependency contract.
Path filters and push-only concurrency cancellation could reduce redundant jobs;
triggers remain unchanged here so ordinary pushes and manual runs keep producing
artifacts. Never cancel release/manual builds through a broad concurrency rule.

References: [vcpkg binary caching](https://learn.microsoft.com/en-us/vcpkg/reference/binarycaching)
and [Actions restore/save](https://github.com/actions/cache#using-a-combination-of-restore-and-save-actions).
