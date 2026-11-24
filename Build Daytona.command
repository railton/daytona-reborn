#!/bin/bash
# Builds the game on macOS with Apple's command line tools and portable CMake
# and Ninja (scripts/macos-build.sh): no Homebrew, no administrator rights.
# Double-click it. The first time macOS may refuse to open a downloaded
# script: right-click it, Open, then Open again.
cd "$(dirname "$0")" || exit 1
bash scripts/macos-build.sh "$@"
status=$?
echo
read -r -p "Press Enter to close this window. " _
exit $status
