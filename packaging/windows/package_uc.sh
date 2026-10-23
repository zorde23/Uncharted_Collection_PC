#!/usr/bin/env bash
# Builds dist/ucndc_pc/ and dist/ucndc_pc-v<VERSION>-windows.zip for players: "Uncharted Launcher.exe"
# (launcher/uc_launcher.py frozen with PyInstaller, so no Python is needed), ucndc.exe (the port) with the
# MSYS2 CLANG64 DLLs it loads, run.py and the preparation scripts. Nothing to install, no terminal.
# Run from an MSYS2 CLANG64 shell after `bash build.sh` (or through packaging/windows/empacotar.bat).
# Freezing uses a Windows Python 3.10+ (python.org; WINPYTHON overrides) and a private venv in out/pyenv.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
exe=out/ucndc.exe
[[ -f $exe ]] || exe=out/gow3-probe.exe  # builds from before the rename
[[ -f $exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }
version=$(sed -n "s/^VERSION = '\(.*\)'/\1/p" launcher/uc_launcher.py)
name="Uncharted Launcher"

python=${WINPYTHON:-}
if [[ -z $python ]]; then
    for candidate in /c/Python3*/python.exe "${LOCALAPPDATA:-/c/Users/$USER/AppData/Local}"/Programs/Python/Python3*/python.exe \
                     /c/Users/*/AppData/Local/Programs/Python/Python3*/python.exe; do
        [[ -x $candidate ]] && python=$candidate
    done
fi
[[ -n $python ]] || { echo 'Need a Windows Python 3 (python.org) or WINPYTHON=path\to\python.exe' >&2; exit 1; }
export USERPROFILE=${USERPROFILE:-$(cygpath -w "/c/Users/$(id -un)")}
[[ -x out/pyenv/Scripts/python.exe ]] || "$python" -m venv out/pyenv
# Pillow: the launcher draws the keyboard icons for the games' text (scripts/key_icons.py).
out/pyenv/Scripts/python.exe -m pip install -q --disable-pip-version-check pyinstaller pillow
# run.py and the scripts run inside the launcher (--run/--script): their standard modules come along.
hidden=()
for module in argparse base64 collections hashlib json re shutil shlex struct tempfile xml.etree.ElementTree \
              urllib.request ctypes.wintypes zipfile; do
    hidden+=(--hidden-import "$module")
done
out/pyenv/Scripts/python.exe -m PyInstaller --noconfirm --clean --log-level WARN --windowed \
    --name "$name" --icon "$(cygpath -w "$PWD/launcher/uc.ico")" --distpath out/pyi-dist-uc \
    --workpath out/pyi-work-uc --specpath out/pyi-work-uc --paths "$(cygpath -w "$PWD/scripts")" "${hidden[@]}" \
    "$(cygpath -w "$PWD/launcher/uc_launcher.py")"

dest=out/stage-uc/ucndc_pc
rm -rf -- out/stage-uc
mkdir -p "$dest/bin" "$dest/launcher"
cp -r "out/pyi-dist-uc/$name/." "$dest/"
cp launcher/uc.ico "$dest/launcher/"
llvm-strip --strip-debug -o "$dest/bin/ucndc.exe" "$exe"
# Every DLL ucndc.exe loads from the CLANG64 tree (SDL3, FFmpeg, Vulkan loader, ...).
ldd "$dest/bin/ucndc.exe" | awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
    cp -u "$dll" "$dest/bin/"
done
cp -r scripts patches "$dest/"
cp run.py LICENSE "$dest/"
cp packaging/windows/LEIA-ME.txt "$dest/LEIA-ME (README).txt"
find "$dest" -name __pycache__ -prune -exec rm -r {} +
# No game files and nothing a player made (saves, shader caches, settings, logs) may ship.
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname '*.elf' -o -iname sce_module -o -iname sce_sys \
    -o -iname 'savedata*' -o -iname user -o -iname cache -o -iname '*.log' -o -iname gow3.ini -o -iname ucndc.ini \
    -o -iname launcher.json -o -iname '*.spv' -o -iname '*.pkg' -o -iname '*.sprx' -o -iname '*.prx' \
    -o -iname '*.dmp' -o -iname 'CUSA*' \) -print)
if [[ -n $forbidden ]]; then
    echo "Package holds files that must not ship:" >&2
    echo "$forbidden" >&2
    exit 1
fi
mkdir -p dist
zip="dist/ucndc_pc-v$version-windows.zip"
rm -f "$zip"
(cd out/stage-uc && powershell -NoProfile -Command "Compress-Archive -Path ucndc_pc -DestinationPath ../../$zip")
du -sh "$dest" "$zip"
echo "Pacote pronto: $PWD/$zip"
