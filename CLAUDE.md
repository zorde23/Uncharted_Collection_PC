# uc3_pc — instruções para o Claude Code

Leia primeiro `docs/uc3/HANDOFF.md` (compilar, rodar, estado, problemas em aberto). Histórico em
`docs/uc3/DEVLOG.md` (mais novo no topo): atualizar o devlog e commitar a cada passo. Manter os
nomes `gow3`/`GOW3_`. Responder em português.

## Regras que não podem falhar

- Compilar só pelo Bash, com o ambiente MSYS2:
  `export PATH="/d/msys64/clang64/bin:/d/msys64/usr/bin:$PATH" MSYSTEM=CLANG64 MSYS2_ROOT=/d/msys64; bash build.sh`
  (erros em `out/gpu-build.log`).
- Nunca compilar nem rodar testes com o jogo aberto (`tasklist` antes); nunca mexer no jogo
  enquanto o jogador joga; só fechar processos que a própria sessão abriu.
- `python` do Windows é o stub da Store: usar `/d/msys64/clang64/bin/python`.
- Editar C/C++ com as ferramentas Edit/Write, não com heredoc (o `\n` vira quebra de linha real).
- Mudou layout de shader: validar os dumps (`GOW3_DUMP_SHADERS=1`) com `spirv-val`; SPIR-V
  inválido derruba o driver AMD na inicialização.

## Ferramentas

- **clangd (LSP)**: `.clangd` usa `out/gpu/compile_commands.json`, gerado pelo CMake em toda
  compilação (runtime C + shim + shadPS4). Use definição, referências e chamadas
  (`incomingCalls`/`outgoingCalls`) para seguir o código em vez de grep quando o símbolo é C/C++.
  Se o `out/gpu` sumir, rode o `build.sh` (ou só o `cmake -S gpu -B out/gpu ...` dele).
- **Graphify**: grafo só do código do port em `graphify-out/` (fora do git; `.graphifyignore`
  tira third_party, `_claude` e docs). Perguntas de arquitetura: `graphify query "..."`.
  Depois de mudanças grandes: `/graphify . --update` (AST, sem custo de tokens).
- **Skills**: diagnóstico com `debugging-and-error-recovery` e `doubt-driven-development`;
  mudanças em fatias com `incremental-implementation` e revisão com `/ponytail-review`;
  commits com `git-workflow-and-versioning`; DEVLOG/HANDOFF com `documentation-and-adrs`.
- **Do projeto** (`.claude/`): antes de cada commit, `/build-test` (compila e roda os testes que
  valem no Windows; só passa sem falhas novas) e o subagente `conventions-reviewer`. Logs grandes
  (`out/last-run.log`, capturas) vão para o subagente `trace-analyst`, não para o contexto principal.
