#!/bin/sh
# Follows docs/install.md's apt instructions on a clean Ubuntu 26.04
# container (run as root, so without sudo): add the archive key and the
# source, install oscillate, check the installed files, remove it.
# Usage: tests/packaging/apt-install-smoke.sh APT_URL
set -eu

url=${1:?usage: apt-install-smoke.sh APT_URL}
keyring=/etc/apt/keyrings/oscillate-archive-keyring.gpg

fail() {
  echo "apt-install-smoke: $*" >&2
  exit 1
}

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl

curl -fsSLo /tmp/oscillate-archive-keyring.gpg "$url/oscillate-archive-keyring.gpg"
install -D -m 0644 /tmp/oscillate-archive-keyring.gpg "$keyring"
echo "deb [arch=amd64 signed-by=$keyring] $url resolute main" |
  tee /etc/apt/sources.list.d/oscillate.list >/dev/null
# apt refuses a repository whose InRelease the key does not verify.
apt-get update -qq || fail "apt-get update rejected the repository"
apt-get install -y -qq oscillate

for file in /usr/bin/oscillate /usr/bin/oscillate_vst3_scan_helper; do
  test -x "$file" || fail "installed package lacks $file"
done
installed=$(dpkg-query -W -f='${Version}' oscillate)
candidate=$(apt-cache policy oscillate | sed -n 's/^ *Candidate: //p')
[ "$installed" = "$candidate" ] || fail "installed $installed, newest in the repository is $candidate"
apt-get remove --purge -y -qq oscillate
echo "apt repository: installed and removed oscillate $installed"
