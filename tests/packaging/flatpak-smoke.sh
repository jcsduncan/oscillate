#!/bin/sh
# Install, launch and uninstall the Flatpak bundle, headless, in a container
# that has flatpak and the org.kde.Platform//6.10 runtime installed system-wide
# (the flatpak-github-actions KDE image, run privileged so bubblewrap works).
# Usage: flatpak-smoke.sh BUNDLE.flatpak
set -eu

app=io.github.jcsduncan.Oscillate
bundle=$(realpath "$1")
work=$(mktemp -d)
export HOME="$work/home"
mkdir -p "$HOME"
startup_timeout=120s
# The project folder the sandbox may write (--filesystem=~/.oscillate:create).
# --create needs the parent folder, as the app's own projects folder is.
project="$HOME/.oscillate/projects/flatpak-smoke"
mkdir -p "$(dirname "$project")"

fail() {
  echo "flatpak-smoke: $*" >&2
  exit 1
}

run() {
  timeout "$startup_timeout" flatpak run --env=QT_QPA_PLATFORM=offscreen \
    --env=QT_QUICK_BACKEND=software "$app" "$@"
}

flatpak install --system --noninteractive -y --bundle "$bundle"
flatpak info --system "$app" >/dev/null || fail "not installed"
installed=$(flatpak info --system --show-location "$app")
test -d "$installed" || fail "no installation at $installed"
for file in bin/oscillate bin/oscillate_vst3_scan_helper \
  share/applications/$app.desktop share/metainfo/$app.metainfo.xml \
  share/icons/hicolor/scalable/apps/$app.svg share/doc/oscillate/README \
  share/doc/oscillate/SOURCE-OFFER.txt share/doc/oscillate/third-party/NOTICES.txt; do
  test -f "$installed/files/$file" || fail "installed app lacks $file"
done

# The app's own startup check exits 0 once the Quick window shows the project.
out=$(run --check-startup --create "$project") || fail "create failed: $out"
echo "$out"
case $out in *"Project ready"*) ;; *) fail "create did not report the project" ;; esac
test -f "$project/state/CURRENT" || fail "project was not written"
out=$(run --check-startup --open "$project") || fail "reopen failed: $out"
echo "$out"
case $out in *"Project ready"*) ;; *) fail "reopen did not report the project" ;; esac

flatpak uninstall --system --noninteractive -y --delete-data "$app"
if flatpak info --system "$app" >/dev/null 2>&1; then fail "app still installed"; fi
test ! -e "$installed" || fail "uninstall left $installed"
test ! -e "$HOME/.var/app/$app" || fail "uninstall left the app's data folder"
rm -rf "$work"
echo "flatpak-smoke: install, launch and uninstall passed"
