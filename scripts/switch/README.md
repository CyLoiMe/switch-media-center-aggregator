# Switch portlibs — PKGBUILDs used by the `switch-portlibs` workflow

**These do not currently reproduce what `.github/workflows/switch.yaml` installs.**

`switch.yaml` installs ten packages by exact versioned filename from this
repository's `switch-portlibs` release — for example
`switch-ffmpeg-7.1.5-5-any.pkg.tar.zst`. Those assets are byte-identical copies of
GMCA's `ci-deps` (sha256 checked), not builds of this directory. This directory is an
older snapshot of the same PKGBUILDs:

| package | here (`pkgver-pkgrel`) | installed by `switch.yaml` | match |
|---|---|---|---|
| ffmpeg  | 7.1.4-5  | 7.1.5-5  | no |
| mbedtls | 3.6.5-1  | 3.6.7-1  | no |
| dav1d   | 1.5.3-1  | 1.5.4-1  | no |
| curl    | 8.16.0-3 | 8.16.0-3 | yes |
| libmpv  | 0.36.0-5 | 0.36.0-5 | yes |

So running the `switch-portlibs` workflow today produces package names the build does
not ask for (the `dkp-pacman -U` step would 404). Bumping the versions here is not a
`pkgver` edit: the three devkitPro port patches are version-locked, and
`git apply --check` rejects `ffmpeg-7.1.4.patch` (593 KB), `mbedtls-3.6.5.patch` and
`dav1d.patch` against the newer sources.

**Nothing is broken.** The release assets and the `devkita64:20260219` container image
are both immutable, so CI is reproducible. The gap only matters when a portlib needs a
security fix or a new feature, when the devkitPro base image changes ABI, or when the
release assets disappear.

Before touching anything here, read the "portlibs" note in `docs/backlog.md`
(working notes, not in this repository).
