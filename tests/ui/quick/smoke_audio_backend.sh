#!/usr/bin/env bash
set -Eeuo pipefail

app=$1
fixture=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-audio-backend.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
export XDG_CONFIG_HOME="$fixture/config"
export XDG_CACHE_HOME="$fixture/cache"

output=$("$app" --create "$fixture/project" --smoke --smoke-audio-device)
[[ $output == *"Qt Quick audio output selection and recovery ready"* ]]
