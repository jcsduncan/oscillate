#!/usr/bin/env bash
# Downloads the .deb of each of the newest releases that carry one, checked
# against that release's SHA256SUMS, for the apt repository
# (docs/install.md). Drafts are skipped.
# Usage: scripts/ci/fetch-release-debs.sh REPOSITORY DESTINATION COUNT
# Needs GH_TOKEN. Prints how many it fetched.
set -euo pipefail

repository=$1
destination=$2
count=$3
# Releases scanned for packages, newest first.
scan_limit=50

mkdir -p "$destination"
fetched=0
while read -r tag; do
  [[ $fetched -lt $count ]] || break
  release_dir=$(mktemp -d)
  if gh release download "$tag" --repo "$repository" --dir "$release_dir" \
    --pattern 'oscillate_*_amd64.deb' --pattern SHA256SUMS >/dev/null 2>&1; then
    for deb in "$release_dir"/oscillate_*_amd64.deb; do
      [[ -f $deb ]] || continue
      name=$(basename "$deb")
      grep -q " $name\$" "$release_dir/SHA256SUMS" || {
        echo "fetch-release-debs: $tag's SHA256SUMS does not list $name" >&2
        exit 1
      }
      (cd "$release_dir" && sha256sum --check --ignore-missing --strict --quiet SHA256SUMS)
      mv "$deb" "$destination/"
      fetched=$((fetched + 1))
    done
  fi
  rm -rf "$release_dir"
done < <(gh release list --repo "$repository" --exclude-drafts --limit "$scan_limit" \
  --json tagName --jq '.[].tagName')
echo "$fetched"
