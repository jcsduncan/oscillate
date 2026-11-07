#!/usr/bin/env bash
set -euo pipefail

app=$1
fixture=$(mktemp -d "${TMPDIR:-/tmp}/oscillate-real-startup.XXXXXX")
trap 'rm -rf -- "$fixture"' EXIT
export HOME="$fixture/home"
export XDG_CONFIG_HOME="$fixture/config"
export XDG_CACHE_HOME="$fixture/cache"
export XDG_STATE_HOME="$fixture/state"
export QT_QPA_PLATFORM=offscreen
export QT_QUICK_BACKEND=software
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"

welcome=$("$app" --check-startup)
[[ $welcome == 'Welcome ready: OSCILLATE — Welcome' ]]

project="$fixture/song"
created=$("$app" --check-startup --create "$project")
[[ $created == 'Project ready: OSCILLATE — song' ]]
[[ -d $project ]]

reopened=$("$app" --check-startup --open "$project")
[[ $reopened == "$created" ]]

cp "$project/state/CURRENT" "$fixture/current.backup"
printf 'invalid revision\n' >"$project/state/CURRENT"
if "$app" --check-startup --open "$project" >"$fixture/error" 2>&1; then
  echo 'Damaged project unexpectedly opened' >&2
  exit 1
fi
[[ $(<"$fixture/error") == *'Project:'* ]]
cp "$fixture/current.backup" "$project/state/CURRENT"
[[ $("$app" --check-startup --open "$project") == "$created" ]]

if "$app" --check-startup --open "$fixture/missing" >"$fixture/error" 2>&1; then
  echo 'Missing project unexpectedly opened' >&2
  exit 1
fi
[[ $(<"$fixture/error") == *'Project:'* ]]

if "$app" --check-startup --create "$project" >"$fixture/error" 2>&1; then
  echo 'Existing project was overwritten' >&2
  exit 1
fi
[[ $(<"$fixture/error") == *'Project:'* ]]

if "$app" --check-startup --bogus "$project" >"$fixture/error" 2>&1; then
  echo 'Invalid startup option was accepted' >&2
  exit 1
fi
[[ $(<"$fixture/error") == *'Usage:'* ]]
