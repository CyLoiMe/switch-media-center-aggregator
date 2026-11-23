#!/usr/bin/env bash
#
# Reports how far the Switch portlibs this build installs have drifted from
#
#   (a) GMCA's scripts/switch — the fork those binaries were built from, i.e. the
#       only place where someone else has already rebased the version-locked
#       devkitPro patches; and
#   (b) the libraries' own latest upstream release — how far behind the ecosystem
#       we actually are, which is where security fixes land.
#
# Read-only: no build, no pacman, no packages installed. Writes a markdown report
# and sets the `drift` output for the workflow. Exits 0 even when it finds drift:
# "we are behind" is information, not a failure.
#
# Local use: bash scripts/check-portlib-drift.sh  (needs curl; GITHUB_TOKEN is
# optional but raises the GitHub API rate limit a lot). No jq: the few fields are
# read with grep so the report also works on a bare runner.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SWITCH_YML="$REPO_ROOT/.github/workflows/switch.yaml"
REPORT="$REPO_ROOT/portlib-drift.md"
GMCA_RAW="https://raw.githubusercontent.com/thcolin/gamepad-media-center-aggregator/dev/scripts/switch"
GH_API="https://api.github.com"
AUTH=()
[ -n "${GITHUB_TOKEN:-}" ] && AUTH=(-H "Authorization: Bearer $GITHUB_TOKEN")

# ---- what the build installs --------------------------------------------------
# The `for pkg in …` list in switch.yaml is the single source of truth. It wraps
# across lines and mixes a shell expansion, so `${{ … }}` is dropped BEFORE
# tokenising (it carries spaces and would shred `libmpv${{ env.SHUFFIX }}-0.36.0-5`).
pinned_list() {
    awk '/for pkg in /{f=1} f{print} f && /; do/{exit}' "$SWITCH_YML" |
        sed 's/\${{[^}]*}}//g' | tr -d '\\' | tr ' ' '\n' | grep -E '^[a-z0-9]+-[0-9]' || true
}

pinned_ver() {  # $1 = package name (mbedtls, ffmpeg, dav1d, curl, libmpv, …)
    pinned_list | sed -n "s/^$1-\([0-9][0-9.]*\)-[0-9]*\$/\1/p" | head -1
}

# ---- where the versions are elsewhere -----------------------------------------
GMCA_DEPS_URL="$GH_API/repos/thcolin/gamepad-media-center-aggregator/releases/tags/ci-deps"

# The binaries this project actually installs came out of GMCA's `ci-deps` release,
# not out of their current branch — a release is a snapshot, the branch moved on.
# One request, reused by every row.
DEPS_JSON="$(curl -fsSL "${AUTH[@]}" "$GMCA_DEPS_URL" 2>/dev/null || true)"

deps_ver() {  # $1 = asset name prefix (switch-ffmpeg, switch-libmpv-deko3d, …)
    printf '%s' "$DEPS_JSON" |
        grep -oE '"name"[[:space:]]*:[[:space:]]*"[^"]*"' |
        sed -E 's/.*"([^"]*)"$/\1/' |
        sed -n "s/^$1-\([0-9][0-9.]*\)-[0-9]*-any\.pkg\.tar\.zst\$/\1/p" | head -1
}

grep_ver() {  # $1 = raw PKGBUILD url -> pkgver or ""
    curl -fsSL "$1" 2>/dev/null | sed -n 's/^pkgver=//p' | head -1
}

json_str() {  # $1 = field name, stdin = json -> first value or ""
    grep -oE "\"$1\"[[:space:]]*:[[:space:]]*\"[^\"]*\"" | head -1 |
        sed -E 's/.*:[[:space:]]*"([^"]*)"/\1/' || true
}

latest_from_tags() {  # $1 = tags api url, $2 = regex the tag must match
    curl -fsSL "${AUTH[@]}" "$1" 2>/dev/null |
        grep -oE '"name"[[:space:]]*:[[:space:]]*"[^"]*"' |
        sed -E 's/.*"([^"]*)"$/\1/' | grep -oE "$2" || true
}

ver_cmp() {  # -1 = $1 older, 0 = equal, 1 = $1 newer
    if [ "$1" = "$2" ]; then
        echo 0
    elif [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$1" ]; then
        echo 1
    else
        echo -1
    fi
}

# ---- report -------------------------------------------------------------------
rows=()
drift=false

report_row() {  # $1 name, $2 installed, $3 GMCA ci-deps, $4 GMCA dev, $5 upstream latest
    local have="$2" deps="$3" gmc="$4" up="$5" note
    if [ -z "$up" ]; then
        note="upstream unknown"
    else
        case "$(ver_cmp "$have" "$up")" in
            0) note="current" ;;
            1) note="installed is ahead of upstream" ;;
            *)
                note="**behind** (upstream $up)"
                drift=true
                ;;
        esac
    fi
    # somebody already built something newer — either their release or their tree
    if [ -n "$deps" ] && [ "$(ver_cmp "$deps" "$have")" = "1" ]; then
        note="$note; a newer GMCA build exists ($deps)"
        drift=true
    fi
    if [ -n "$gmc" ] && [ "$(ver_cmp "$gmc" "$have")" = "1" ]; then
        note="$note; GMCA tree is newer ($gmc)"
        drift=true
    fi
    rows+=("| $1 | $have | ${deps:-?} | ${gmc:-?} | ${up:-?} | $note |")
}

for lib in ffmpeg mbedtls dav1d curl; do
    have=$(pinned_ver "$lib")
    deps=$(deps_ver "switch-$lib")
    gmc=$(grep_ver "$GMCA_RAW/$lib/PKGBUILD")
    up=""
    case "$lib" in
        ffmpeg)
            # tags are n7.1.5 / n9.1 / n9.2-dev; -dev is a branch, not a release
            up=$(latest_from_tags "$GH_API/repos/FFmpeg/FFmpeg/tags?per_page=40" '^n[0-9]+(\.[0-9]+)*$' |
                sed 's/^n//' | sort -V | tail -1)
            ;;
        mbedtls)
            up=$(curl -fsSL "${AUTH[@]}" "$GH_API/repos/Mbed-TLS/mbedtls/releases/latest" 2>/dev/null |
                json_str tag_name | sed 's/^mbedtls-//' || true)
            ;;
        dav1d)
            # code.videolan.org sits behind an anti-bot proof-of-work wall; the
            # GitHub mirror's tags are what a runner can read. A stale mirror
            # (< 1.0) is reported as unknown rather than as a real version.
            up=$(latest_from_tags "$GH_API/repos/videolan/dav1d/tags?per_page=40" '^[0-9]+(\.[0-9]+)*$' |
                sort -V | tail -1)
            case "$up" in 0.*) up="" ;; esac
            ;;
        curl)
            # curl tags are curl-8_23_0
            up=$(curl -fsSL "${AUTH[@]}" "$GH_API/repos/curl/curl/releases/latest" 2>/dev/null |
                json_str tag_name | sed 's/^curl-//; s/_/./g' || true)
            ;;
    esac
    report_row "$lib" "$have" "$deps" "$gmc" "$up"
done

# libmpv: ours is the deko3d variant, nobody else ships it — only report whether
# GMCA moved and how far mpv itself has gone.
have=$(pinned_ver libmpv)
# the deko3d build is the one we use; the plain switch-libmpv is a different asset
deps=$(deps_ver switch-libmpv-deko3d)
gmc=$(grep_ver "$GMCA_RAW/mpv/PKGBUILD")
up=$(curl -fsSL "${AUTH[@]}" "$GH_API/repos/mpv-player/mpv/releases/latest" 2>/dev/null |
    json_str tag_name | sed 's/^v//' || true)
report_row "libmpv (deko3d)" "$have" "$deps" "$gmc" "$up"

{
    echo "## Portlib versions"
    echo
    echo "Checked $(date -u +%Y-%m-%d). Read-only report: nothing is built or bumped."
    echo
    echo "| library | build installs | GMCA ci-deps | GMCA dev | upstream latest | verdict |"
    echo "|---|---|---|---|---|---|"
    printf '%s\n' "${rows[@]}"
    echo
    echo "\`GMCA ci-deps\` is where these binaries actually came from (release"
    echo "2026-09-23) — a snapshot, so it can be newer than GMCA's current tree."
    echo "\`GMCA dev\` is the fork \`scripts/switch\` was copied from: newer there means"
    echo "someone already did the patch rebase."
    echo "\`upstream latest\` is the library itself — where security fixes land; being"
    echo "behind it is the cost of frozen binaries, not a bug."
    echo
    echo "When an upgrade is actually needed the order is mbedtls → dav1d → ffmpeg"
    echo "(+ libmpv), cheapest patch first: see \`scripts/switch/README.md\`."
} >"$REPORT"

cat "$REPORT"
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then cat "$REPORT" >>"$GITHUB_STEP_SUMMARY"; fi
if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "drift=$drift" >>"$GITHUB_OUTPUT"; fi
exit 0
