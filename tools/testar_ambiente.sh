#!/usr/bin/env bash
# uc3: confere o ambiente numa maquina nova: compila, roda alguns testes e o --check-imports.
# Uso (de qualquer lugar): D:\Uncharted\testar_port.bat   (log em out/teste-ambiente.log)
export PATH="/d/msys64/clang64/bin:/d/msys64/usr/bin:$PATH" MSYSTEM=CLANG64 MSYS2_ROOT=/d/msys64
cd /d/Uncharted/uc3_pc || exit 1
mkdir -p out
log=out/teste-ambiente.log
exec > >(tee "$log") 2>&1
echo "== $(date)"
if tasklist 2>/dev/null | grep -qiE "gow3-probe|ucndc"; then echo "PARADO: o jogo (ucndc.exe) esta aberto"; exit 1; fi
echo "== 1. compilacao"
if bash build.sh; then echo "COMPILOU OK"; else echo "FALHOU A COMPILACAO (veja out/gpu-build.log)"; tail -40 out/gpu-build.log; exit 1; fi
ls -la out/ucndc.exe
echo "== 2. testes"
for t in memory-test fiber-test readback-queue-test page-state-test aliasing-test services-test png-decode-test; do
  if ninja -C out/gpu "$t" > "out/teste-build-$t.log" 2>&1; then
    (cd out/gpu && timeout 300 "./$t.exe" > "../teste-$t.log" 2>&1); r=$?
    echo "$t: $([ $r -eq 0 ] && echo PASSOU || echo "FALHOU ($r)")"
  else echo "$t: NAO COMPILOU (out/teste-build-$t.log)"; fi
done
echo "== 3. check-imports"
export GOW3_GAME_DIR=D:/Uncharted/Jogos/CUSA02344/CUSA02344 GOW3_PATCH_DIR=D:/Uncharted/Jogos/CUSA02344/CUSA02344-UPDATE GOW3_EXECUTABLE=big3-ps4_Shipping.elf
/d/msys64/clang64/bin/python run.py --check-imports 2>&1 | tail -40
echo "== FIM"
