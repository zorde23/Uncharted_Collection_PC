#!/usr/bin/env bash
# Build + every test that works on Windows; exits 1 only on a failure not in KNOWN.
set -uo pipefail
cd -- "$(dirname -- "$0")/../../.."
export PATH="/d/msys64/clang64/bin:/d/msys64/usr/bin:$PATH" MSYSTEM=CLANG64 MSYS2_ROOT=/d/msys64
# Broken on Windows before this skill existed (2026-10-25): setenv, Lavapipe.
# Remove a name once it passes again.
KNOWN=" scene-resolution-test taa-shader-test upscaler-support-test "

if tasklist | grep -qiE 'gow3-probe|ucndc'; then echo 'The game is running: close it first.' >&2; exit 2; fi
bash build.sh || exit 1
[[ ${1:-} == --build-only ]] && exit 0

declare -A args=([mp3-test]=tests/data/sine.mp3 [videodec-test]=tests/data/tiny.h264)
targets=$(ninja -C out/gpu -t targets all | grep -o '^[a-z0-9-]*-test:' | tr -d ':' | sort -u)
[[ -n ${1:-} ]] && targets=$(grep -- "$1" <<<"$targets")
ninja -k 0 -C out/gpu $targets > out/test-build.log 2>&1
pass=0 new=() known=()
for t in $targets; do
    if [[ -f out/gpu/$t.exe ]] && timeout 120 "out/gpu/$t.exe" ${args[$t]:-} > "out/$t.log" 2>&1; then
        pass=$((pass + 1))
    elif [[ $KNOWN == *" $t "* ]]; then known+=("$t")
    else new+=("$t")
    fi
done
# Python suite; test_packaged_vulkan covers the Linux package only.
py=$(cd tests && python -m unittest discover -s . -p 'test_*.py' 2>&1 | grep -E '^(FAIL|ERROR):' | grep -v test_packaged_vulkan)
echo "C/C++: $pass passed, known broken: ${known[*]:-none}"
[[ ${#new[@]} -eq 0 && -z $py ]] && { echo 'OK: no new failures'; exit 0; }
echo "NEW FAILURES: ${new[*]:-}"; [[ -n $py ]] && echo "$py"
echo 'Details: out/test-build.log and out/<test>.log'
exit 1
