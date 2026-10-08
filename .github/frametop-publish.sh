#!/usr/bin/env bash
# Frametop: put files on the GitHub release for a frametop-* tag, each with its .sha256, making
# the release first if it isn't there (.github/workflows/frametop-build.yml's release job).
# Usage: frametop-publish.sh TAG FILE...   (GH_TOKEN and GITHUB_REPOSITORY set)
set -euo pipefail
tag=$1; shift
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
for f in "$@"; do
  (cd "$(dirname "$f")" && sha256sum "$(basename "$f")" > "$(basename "$f").sha256")
  cat "$f.sha256"
done
if ! gh release view "$tag" --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
  gh release create "$tag" --repo "$GITHUB_REPOSITORY" --verify-tag \
    --title "Frametop build of Vibepollo 2.0.0 ($tag)" \
    --notes-file "$root/.github/frametop-release-notes.md"
fi
files=()
for f in "$@"; do files+=("$f" "$f.sha256"); done
gh release upload "$tag" --repo "$GITHUB_REPOSITORY" --clobber "${files[@]}"
