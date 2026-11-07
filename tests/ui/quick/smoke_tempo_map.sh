#!/usr/bin/env bash
set -Eeuo pipefail
trap 'status=$?; printf "Quick tempo-map smoke failed at line %s (exit %s): %s\n" "$LINENO" "$status" "$BASH_COMMAND" >&2' ERR

app=$1
fixture=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-quick-tempo-map.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
export XDG_CONFIG_HOME="$fixture/config"
export XDG_CACHE_HOME="$fixture/xdg-cache"

edited=$("$app" --create "$fixture/tempo-map" --smoke --smoke-tempo-map)
[[ $edited == *"Qt Quick tempo map edited, paged, undone and saved"* ]]
reopened=$("$app" --open "$fixture/tempo-map" --smoke --smoke-verify-tempo-map)
[[ $reopened == *"Qt Quick tempo map reopened"* ]]
