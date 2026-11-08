#!/usr/bin/env bash
# Builds the signed apt repository served under the documentation site's
# /apt/ (docs/install.md) from a directory of release .debs, with aptly.
# aptly publishes it unsigned; gpg then writes InRelease and Release.gpg with
# the archive key, and the result is checked against the exported public key.
# Usage: scripts/ci/build-apt-repository.sh DEB_DIR OUTPUT_DIR
# Needs APT_GPG_PRIVATE_KEY (ASCII-armoured secret key) and, when the key has
# one, APT_GPG_PASSPHRASE.
set -euo pipefail

deb_dir=$1
output=$2
# The packages are built for Ubuntu 26.04.
distribution=resolute
component=main
architecture=amd64
keyring=oscillate-archive-keyring

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
export GNUPGHOME="$work/gnupg"
install -d -m 700 "$GNUPGHOME"
printf '%s\n' "$APT_GPG_PRIVATE_KEY" | gpg --batch --quiet --import
fingerprint=$(gpg --batch --with-colons --list-secret-keys | awk -F: '$1 == "fpr" { print $10; exit }')
[[ -n $fingerprint ]] || {
  echo "build-apt-repository: APT_GPG_PRIVATE_KEY holds no secret key" >&2
  exit 1
}
sign=(gpg --batch --yes --pinentry-mode loopback --local-user "$fingerprint" --digest-algo SHA512)
if [[ -n ${APT_GPG_PASSPHRASE:-} ]]; then
  printf '%s' "$APT_GPG_PASSPHRASE" >"$work/passphrase"
  sign+=(--passphrase-file "$work/passphrase")
fi

cat >"$work/aptly.conf" <<EOF
{"rootDir": "$work/aptly", "architectures": ["$architecture"]}
EOF
aptly=(aptly -config="$work/aptly.conf")
"${aptly[@]}" repo create -distribution="$distribution" -component="$component" oscillate >/dev/null
"${aptly[@]}" repo add oscillate "$deb_dir" >/dev/null
"${aptly[@]}" publish repo -skip-signing -origin=Oscillate -label=Oscillate oscillate >/dev/null

public="$work/aptly/public"
dists="$public/dists/$distribution"
"${sign[@]}" --clearsign --output "$dists/InRelease" "$dists/Release"
"${sign[@]}" --armor --detach-sign --output "$dists/Release.gpg" "$dists/Release"

mkdir -p "$output"
cp -R "$public/dists" "$public/pool" "$output/"
gpg --batch --export "$fingerprint" >"$output/$keyring.gpg"
gpg --batch --armor --export "$fingerprint" >"$output/$keyring.asc"
gpgv --quiet --keyring "$output/$keyring.gpg" "$output/dists/$distribution/InRelease"
gpgv --quiet --keyring "$output/$keyring.gpg" "$output/dists/$distribution/Release.gpg" \
  "$output/dists/$distribution/Release"
echo "$fingerprint" >"$output/$keyring.fingerprint"
