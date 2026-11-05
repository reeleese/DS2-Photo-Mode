#!/usr/bin/env bash
# Builds xinput1_3.dll from this source twice, in two different folders, checks the two
# are byte-identical and prints the sha256 to compare with the one published for the
# release (SHA256SUMS in the GitHub release, and the Nexus file description).
#   ./verify.sh [EXPECTED_SHA256]
# Exit status 0 when the build is reproducible (and matches EXPECTED_SHA256, if given).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
PM_BUILD_ROOT="$work/a" "$here/build.sh" one >/dev/null
PM_BUILD_ROOT="$work/b" "$here/build.sh" two >/dev/null
a="$(sha256sum "$work/a/one/xinput1_3.dll" | cut -d' ' -f1)"
b="$(sha256sum "$work/b/two/xinput1_3.dll" | cut -d' ' -f1)"
if [[ "$a" != "$b" ]]; then
  echo "NOT reproducible: $a vs $b" >&2
  exit 1
fi
echo "sha256  $a  xinput1_3.dll"
if [[ -n "${1:-}" ]]; then
  if [[ "${1,,}" == "$a" ]]; then echo "matches the expected hash"; else echo "DIFFERS from the expected hash $1" >&2; exit 1; fi
fi
