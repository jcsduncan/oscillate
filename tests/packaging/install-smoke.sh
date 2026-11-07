#!/bin/sh
# Install, launch and uninstall one package in a clean Ubuntu 26.04 container,
# as root. It needs only the base image: no Python, no build tools.
# Usage: install-smoke.sh deb PACKAGE.deb | install-smoke.sh appimage IMAGE.AppImage
set -eu

kind=$1
artifact=$(realpath "$2")
work=$(mktemp -d)
export HOME="$work/home" QT_QPA_PLATFORM=offscreen QT_QUICK_BACKEND=software
mkdir -p "$HOME"
startup_timeout=120s

fail() {
  echo "install-smoke: $*" >&2
  exit 1
}

# The app's own startup check exits 0 once the Quick window shows the project.
launch() {
  project="$work/project"
  out=$(timeout "$startup_timeout" "$@" --check-startup --create "$project") ||
    fail "create failed: $out"
  echo "$out"
  case $out in *"Project ready"*) ;; *) fail "create did not report the project" ;; esac
  test -f "$project/state/CURRENT" || fail "project was not written"
  out=$(timeout "$startup_timeout" "$@" --check-startup --open "$project") ||
    fail "reopen failed: $out"
  echo "$out"
  case $out in *"Project ready"*) ;; *) fail "reopen did not report the project" ;; esac
}

case $kind in
deb)
  export DEBIAN_FRONTEND=noninteractive
  # The minimised container image skips /usr/share/doc on install; a desktop
  # install keeps it, and the notices and SBOM live there.
  rm -f /etc/dpkg/dpkg.cfg.d/excludes
  apt-get update -qq
  apt-get install -y -qq "$artifact"
  docs=/usr/share/doc/oscillate
  for file in /usr/bin/oscillate /usr/bin/oscillate_vst3_scan_helper \
    /usr/share/applications/oscillate.desktop $docs/copyright $docs/README \
    $docs/sbom.spdx.json $docs/third-party/inventory.json; do
    test -f "$file" || fail "installed package lacks $file"
  done
  dpkg -L oscillate >"$work/files"
  launch oscillate
  apt-get remove --purge -y -qq oscillate
  if dpkg -s oscillate >/dev/null 2>&1; then fail "package still installed"; fi
  while read -r path; do
    if [ -f "$path" ] || [ -L "$path" ]; then fail "uninstall left $path"; fi
  done <"$work/files"
  ;;
appimage)
  # Containers have no FUSE; the runtime extracts to a temporary directory
  # and removes it when the app exits.
  export APPIMAGE_EXTRACT_AND_RUN=1 TMPDIR="$work/tmp"
  mkdir -p "$TMPDIR" "$HOME/Applications"
  installed="$HOME/Applications/Oscillate.AppImage"
  cp "$artifact" "$installed"
  chmod +x "$installed"
  launch "$installed"
  rm "$installed"
  test ! -e "$installed" || fail "AppImage still present"
  leftovers=$(find "$TMPDIR" -mindepth 1 -maxdepth 1)
  test -z "$leftovers" || fail "AppImage left extracted files: $leftovers"
  ;;
*)
  fail "unknown package kind: $kind"
  ;;
esac
rm -rf "$work"
echo "install-smoke: $kind install, launch and uninstall passed"
