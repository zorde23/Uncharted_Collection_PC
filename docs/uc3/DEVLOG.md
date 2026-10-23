# Devlog — Uncharted 3 PC

Diário do projeto: o que foi feito, o que foi decidido e por quê. Entradas mais novas no topo.

---

## 2026-10-23 — v0.3.5: manchas vermelhas do bloom, ícones de teclado e de Xbox, correções do UC3 no UC1 e UC2, proteção do crash do UC1

- **Manchas vermelhas redondas (UC3, todas as placas)** (`GOW3_OVERLAP_KEEP_VIEW`, `texture_cache.cpp`). Começou como "só NVIDIA" (RTX 3070 de um amigo), mas apareceu também no AMD do Matheus, na escada do cap. 4 (Londres). Os dumps dos mips (`GOW3_DUMP_MIPS=1`, no tonemap ps 3371ae8d) mostraram os mips 1 e 2 do bloom com uma cena velha e um retalho de 120x67 no canto do mip 0. Causa: o laço de sobreposição do `FindImage` zerava o mip/camada achado a cada imagem; uma tabela 512x512 (fmt 83, 0x1145620000) que divide a memória com os mips 1-2 devolvia {imagem, -1, -1}, e o render target do mip caía no mip 0. Correção: o laço mantém o mip/camada quando a imagem seguinte só se sobrepõe por acaso. Desligado no código, **sempre ligado pelo launcher nos três jogos**. Testes negativos, que ficaram desligados ou foram removidos: layout GENERAL no lugar do feedback loop, limpar mips indefinidos, limpar NaN.
- **Ícones de teclado** (launcher, janela Controles, "Ícones de teclado no jogo", desligado por padrão; desenho do Opus). Parte A, textos: o launcher redesenha a fonte de botões (`symbol3`, 512x512, dentro de `uXdata/data.psarc`) com as teclas do jogador (`scripts/key_icons.py`, rótulos curtos pt/en). Parte B, HUD: reescreve as texturas de botão dentro de uma cópia do pak do HUD, no layout tiled da GPU e do mesmo tamanho (`scripts/hud_icons.py`; UC1 `pak19/ps4/particle.pak`, UC2 `pak22/ps4/hud.pak`, UC3 `pak23/ps4/hud.pak`, a pasta da atualização primeiro). As cópias ficam em `user\icones` com um carimbo de hash; o port monta cada uma por cima do arquivo do jogo (`UC_ICON_ROOT`, `UC_ICON_FILES`, `runtime_file.c`, `MAX_MOUNTS` 16 → 32). O pacote instala o Pillow. Confirmado pelo Matheus nos três jogos.
- **Ícones de Xbox** (pedido do Matheus, 2026-10-23). A caixa virou a escolha "Ícones no jogo": Controle PlayStation (o jogo como é, padrão), Controle Xbox ou Teclado e mouse (`controls.icons` = `ps`/`xbox`/`keys` no launcher.json; o `key_icons` das betas vira `keys`). No Xbox, os ícones são refeitos no PC do jogador a partir da arte do próprio jogo (o Matheus pediu o estilo do jogo): nos botões de face o símbolo é apagado (o interior do disco é repintado a partir da borda escura), o brilho muda de cor (só o matiz) e entra a letra A verde, B vermelho, X azul ou Y amarelo com brilho; nos ombros, gatilhos, L3/R3 e Options/Share o texto é apagado e entra LB/RB, LT/RT, LS/RS, MENU ou VIEW na cor do texto do jogo, com sombra. Vale para o atlas `symbol3` e para o HUD (botões de face planos e em 3D, ombros, gatilhos; as texturas de transparência ficam as originais, mesmo formato). O touchpad do atlas vira o VIEW (feito a partir do glifo do Share); nas imagens do diário entra uma tecla View. Direcional e analógicos ficam os do jogo, iguais nos dois controles. Nenhuma arte da Microsoft e nenhuma arte do jogo vai no zip. Se um glifo não tiver o formato esperado, entra um desenho simples próprio. `key_icons.py`/`hud_icons.py` ganharam `style`; `KEY_ICONS_VERSION` 4. No UC1 os ombros e gatilhos do HUD têm letras escuras gravadas (não claras): o texto é achado como pixels escuros dentro da tampa cinza e o novo é desenhado escuro com a borda clara. No teclado, a animação de girar o analógico (Rotate-Analog) acendia W, D, S, A (horário); o jogo gira anti-horário (esquerda, baixo, direita, cima), agora A, S, D, W (o Matheus notou).
- **Correções do UC3 no UC1 e no UC2** (launcher): `SYNC_SMALL_TARGETS`, `JOB_SPIN`/`JOB_SLEEP`, `MIP_FIX`, `SCOPE_SPLIT_SCALAR`, `DEPTH_KEEP_CLEAR`. No UC1 a lanterna funciona com elas; no UC2 o `COMPUTE_PIPE` continua desligado (trem).
- **Launcher:** passa adiante as opções `GOW3_`/`UC_` definidas fora dele (os .bat de teste não funcionavam porque ele apagava todas) e só troca as que ele controla; mouse ligado por padrão e `input.ini` sempre escrito; versão 0.3.5.
- **Crash do UC1 no PC de um amigo (NVIDIA)** (`GOW3_UC1_BIND_RETRY`, `src/probe.c`). Dois crashes iguais: salto para o endereço 0 numa `NdJobWorkerThread`. Os diagnósticos (`UC_FAULT_WRITES`, `UC_CB_WATCH`) e a análise do Fable apontavam para o callback do command buffer, mas o watch não viu nenhum callback zerado. Lendo o código do UC1 1.02 com os registradores dos crashes: o salto é o `call *%rcx` em eboot+0x64011d, no laço que monta os recursos do shader atual. Cada item da lista tem um tipo (u16) que escolhe a função numa tabela (eboot+0x94a350); o tipo 0 não tem função. Nos dois crashes, o 5º e último item começava com zeros, no mesmo lugar da memória (ctx+0x22d8). A proteção: nesse salto a thread espera até 16 ms o item ganhar um tipo válido e roda de novo; se continuar vazio, pula só esse item (um recurso a menos num desenho). Ela grava a lista no log antes e depois da espera, para dizer se é uma corrida entre threads ou um dado zerado; a causa de verdade fica para depois desse log. Confere os bytes do laço; noutro executável fica desligada. Desligada no código, **ligada pelo launcher no UC1** (fixa, sem botão). Notas: `uc3/crashes/analise-uc1-handler-nulo.md` (pasta do projeto).
- **Removidas** (testadas, não mudavam nada): `UC_TEX_LOD_BIAS(_PS)`, `UC_SM_NAN`, `UC_SM_DS`.
- **Diagnósticos que ficam, desligados:** `UC_MIP_FIX_TRACE`, `UC_DUMP_MIPS`, `UC_MOUSE_U1_REWRITE_LOG`, `UC_MOUSE_AIM_U1_SETTLE_MS`.
- **Em aberto:** cenário piscando no UC2 em 4K (avisos `SanitizeCopyLayers`, o mesmo array de textura que cresce uma camada por vez; num log de 1080p de outro jogador não apareceu). `GOW3_EXPAND_FRESH` ainda vai ser testado nele. Halo em volta do Drake (provavelmente um render target 1920x1080 lido enquanto é desenhado).

## 2026-10-20 — v0.3.4: 1440p e 4K nos três jogos, cachecol, UC3 jogável do início ao fim

- **UC3 100% jogável do início ao fim** (jogado do início ao fim no port). Um único travamento no jogo inteiro, ainda não reproduzido. Algumas partes caem abaixo de 60 FPS, sempre acima de 30.
- **Resolução** (estudo do Opus, `uc3/resolucao/`). O jogo guarda os tamanhos de vídeo numa estrutura iniciada por uma função que é igual byte a byte em UC1, UC2 e UC3, nas versões 1.00 e 1.02. `GOW3_UC3_RES=LxA` troca os imediatos dela, e `GOW3_UC3_RES_ALL=1` troca também a saída. O UC3 1.02 usa endereços fixos; os outros executáveis são achados pelos bytes. A tabela de memória do jogo foi feita para 1080p: `GOW3_UC3_RTHEAP_MB` e `GOW3_UC3_RT_MB` aumentam os heaps de render targets, a região de GPU e o total que o jogo confere. Acima de 4 GiB a init do heap da GPU cortava o tamanho para 32 bits: na 1.02 vira um nop (`mov eax,eax`), na 1.00 uma troca de 33 bytes do Opus. O vídeo de abertura (cs c85d9816, NV12→RGB) tinha 1/1920 e 1/1088 fixos e lia T#s do tamanho da tela: `GOW3_UC3_MOVIE_FIX` troca os literais e aponta os T#s de volta para a imagem 1920x1080.
- **Testado** (1.02): UC3 em 1440p e 4K, UC2 e UC1 em 1440p e 4K, vídeos certos em todos. Sessões curtas, não um jogo inteiro. No monitor 4K, janela e janela sem borda deram o mesmo FPS (~40 parado no mesmo save). A 1.00 só foi conferida fora do jogo.
- **Launcher:** opção Resolução (1080p, 1440p, 4K) para os três jogos, com os valores testados (1440p: heap +512 MB e memória 6144 MB; 4K: +1792/+448 MB e 8192 MB). Desligado no código.
- **Cachecol do Drake no deserto** pulava com a fila lateral de readback: o launcher deixa `GOW3_READBACK_SIDEQ` desligado no UC3 (UC1 e UC2 continuam com ela).
- **Desligados, para estudo:** correção do A1 em sessões longas (readback avisa o A1), `GOW3_RING_COPY_STEAL`/`SPIN_US`, testes da fila lateral (`GOW3_SIDEQ_*`), captura por draw e dumps (`GOW3_CAPTURE_PER_DRAW`, `GOW3_DUMP_CAPTURE`), `GOW3_TEX_LOD_BIAS`.

## 2026-10-19 — Luzes, sombras e paredes piscando corrigidas

- **Sintoma.** No castelo e em outros lugares, uma parede ficava escura ou uma luz piscava por um instante. No log apareciam muitos avisos `SanitizeCopyLayers` com a contagem de camadas subindo (1→2→3…→8).
- **Causa** (projeto do Opus, `textura-piscando/projeto-correcao.md`). Eram arrays de mapas de sombra (profundidade, 1024–2048 px) que o cache recria com uma camada a mais sempre que o jogo reusa o mesmo espaço. Na recriação (`ResolveDepthOverlap`), a imagem nova registrava o HTILE com máscara de limpeza 0. A limpeza pendente das camadas que ainda iam ser desenhadas (o clear do HTILE por compute) se perdia, e a sombra seguinte era desenhada sobre profundidade velha.
- **Correção** (`GOW3_DEPTH_KEEP_CLEAR`, `texture_cache.cpp`). A imagem recriada herda o endereço do HTILE e a máscara de limpeza da imagem que ela substitui, e o HTILE é registrado de novo para ela depois que a velha é liberada. No teste do castelo, as 499 recriações mantiveram a limpeza (máscaras 0xfffffffe, 0xfffffffc…), e o pisca sumiu ali e em outro lugar. Desligado no código, **ligado pelo launcher no UC3**.
- **Testado e sem efeito no pisca, fica desligado:** `GOW3_EXPAND_FRESH` (imagem que cresce sai da memória do jogo quando a GPU não desenhou nela).
- **Diagnósticos novos, desligados:** `GOW3_EXPAND_TRACE` (cada expansão/recriação e cada liberação de array, com a linha de quem chamou) e `GOW3_A1_DIVERGENCE_LOG` (na divergência do A1, por que o desenho foi confiado e quais dwords diferem).
- **Ainda em aberto:** o array continua sendo recriado uma camada por vez (custo, não pisca).

## 2026-10-19 — Névoa dos cones de luz: cunhas pretas e chuvisco corrigidos

- **Sintoma.** Nos cones de luz com névoa (corredor da lâmpada, barco), apareciam cunhas pretas duras com pontos brancos. No PS4 a névoa preenche tudo, e as sombras são suaves.
- **Causa** (achada com o Opus, `medidas/chuvisco/nevoa/causa-cunhas.md`). O cs e34f5e2c monta os 512 raios da névoa epipolar com laços waterfall de `v_movreld`. O compilador do jogo pôs a `s_andn2_b64` que calcula o SCC do laço no meio dos movreld. `CFG::SplitDivergenceScopes` só tira as escalares das pontas do escopo, então essa ficava dentro do "if" por lane: as lanes que não casavam saíam do laço sem o movreld, e as linhas daquela wave ficavam com passo 0 (cunha preta ou chuvisco branco).
- **Correção** (`GOW3_SCOPE_SPLIT_SCALAR`, `control_flow_graph.cpp`). Uma escalar no meio de um escopo sobe para o início dele quando só há movrel antes dela. Se não puder subir, o escopo é dividido em volta dela. A 1ª versão (só dividir) estava certa no IR, mas a GPU errava as 11 primeiras lanes de algumas waves com a forma "if, escalar, if". A forma com a escalar no início é a mesma de um laço do mesmo shader que já funcionava. O simulador do IR do Opus bate 100% com a GPU nas 512 linhas.
- **Padrão:** ligado no launcher para o UC3 (desligado no código, como as outras).
- **Cache de shaders:** `ShaderBinaryVersion` 10 → 11, e o estado de `GOW3_SCOPE_SPLIT_SCALAR` entra na versão gravada. Caches antigos e a troca da opção retraduzem os shaders sozinhos, sem o launcher apagar nada.
- **Diagnósticos novos, desligados:** `GOW3_SAMPLER_LOG=<ps>` e `GOW3_SAMPLER_LOG_ADDR=<endereço>` (samplers e imagens de um pixel shader), e a linha de blend dos draws indiretos na captura de frame.

## 2026-10-18 — v0.3.3: imagem lavada corrigida, mouse ao escalar e pendurado, A1 experimental, correções da revisão do Fable

- **Imagem lavada (UC3)** (`GOW3_MIP_FIX`, `texture_cache.cpp`, `image_info.cpp`). O cache de texturas rejeitava render targets desenhados num mip de uma imagem maior (pitch diferente e checagem de tamanho), então os mips do bloom e da distorção ficavam zerados, e bloom zero soma uma névoa cinza no tonemap. Desligado no código, **ligado pelo launcher no UC3**.
- **Mouse depois de escalar.** As checagens dos campos da câmera desligavam o mouse para a sessão inteira quando a direção inclinava. Agora o mouse só vira analógico enquanto os campos estão estranhos e volta quando ficam certos. Follow e cobertura aceitam direção inclinada (|v|≈1 e x²+z² ≥ 0,49).
- **Mira escalando e pendurado** (`GOW3_MOUSE_EDGE_AIM`, `runtime_pad.c`). Hook na CameraEdgeAim (big3 1.02, arquivo 0x4dbf00): o up vem do quaternion do Locator anterior (rdx), o giro é limitado a 90° do início e o pitch a ±60°. Nos pontos pendurado onde a mira ficava lenta, só o UpdateLocation da Zoom (+0x150, arquivo 0x51f3e0) rodava. Um hook nele gira +0x11e0 quando a mira normal está parada há mais de 100 ms, e não roda com o jogo pausado. Corrigida a queda do mouse ao trocar de mão. Desligado no código, **ligado pelo launcher no UC3**.
- **Hooks:** `RuntimeGuestCallback.rdx` (0 = r14, 1 = rdx, 2 = endereço de retorno em r9), e o thunk guarda o endereço de retorno e o rbp do guest. A memória dos thunks passou para 16 KB.
- **Censo de câmeras** (`GOW3_MOUSE_CAMERA_LOG=1`). Hook no despacho da árvore de blend (arquivo 0x102498b) que loga cada UpdateLocation nova e se ela tem mouse. Numa sessão longa todas as câmeras de jogador tinham mouse, e as outras são roteirizadas (Manual, Animated, Spline, Fixed, ExplosionDeath). Lista em `cameras-mouse.md` (pasta do projeto).
- **A1 do Fable** (`GOW3_PREP_TRUST_A1`). Confiança no preparo dos desenhos com carimbos de página e região e tabela de labels da GPU: 0 divergências em sombra (ruínas e sessão longa), uns +5 FPS nas ruínas. Desligado no código; no launcher aparece como **Experimental** em Opções > Avançado (`launcher.json` 'experimental').
- **Revisão do Fable (2026-10-17).** Corrida no RING_DEDUP: o stage B mexia na lista de dedup do stage A sem sincronização, e agora só o A mexe. `stale_readback` passou a vir 0 também no default do ini e nos .bats.
- **Controle:** L2/R2 continuam segurados enquanto L3+R3 abre o menu.
- **Launcher 0.3.3:** aviso de versão por conteúdo, links para `zorde23/Uncharted_Collection_PC`.
- Diagnósticos novos, desligados: `GOW3_PRESENT_GAMMA_FIX`, logs de formato do VideoOut e de gamma.

## 2026-10-17 — v0.3.2: teclado e mouse nos três jogos, câmera direta pelo mouse, janela e aba Controles, F1 abre o menu

- **Entrada de teclado e mouse** (`src/runtime_pad.c`, `gpu/shim/window.cpp`, portado do uncharted_pc do SuperMedo, GPL). `input.ini` (via `GOW3_INPUT_CONFIG`) mapeia teclas, botões do mouse e roda para os botões do PS4 e os analógicos. Com o olhar pelo mouse ligado, o cursor fica preso na janela (modo relativo); Alt esquerdo, o menu do port ou a perda de foco o soltam.
- **Câmera direta pelo mouse.** Em vez de virar analógico, o mouse escreve na câmera do jogo por callbacks no início das funções de update (`runtime_guest_callbacks`, padrões de bytes procurados no executável inteiro, um por versão, 1.00 e 1.02; padrão achado mais de uma vez é ignorado). Campos achados pelos dumps de `GOW3_MOUSE_DISCOVER=1`:
  - Câmera normal (follow): UC1 +0x1310, UC2 +0x9ec0, UC3 +0x78c0, mais o arco vertical.
  - Mira (camerazoom/CameraControlZoom): UC3 +0x200/+0x11e0, UC1 +0x1680/+0x16b0, UC2 +0x1f30/+0xdc0/+0xd90.
  - Cobertura: UC3 +0x2f0 (direção)/+0x2e0 (pitch), UC2 +0x1c0/+0x1b0, UC1 (CameraControlCover, eboot 0xa9880 na 1.02) +0x240/+0x230.
  - Cada câmera confere se o jogo manteve o giro ("kept"); se não mantiver, o mouse volta a ser analógico, com giros de teste para voltar ao modo direto. `GOW3_MOUSE_AIM=0` / `GOW3_MOUSE_COVER=0` deixam mira/cobertura no analógico.
- **Jogo sem a atualização (1.00).** As funções de mira do UC3 (big3 0x13fa60) e do UC2 (0x194270) e a cobertura dos dois na 1.00 têm os mesmos campos da 1.02; o UC1 usa o mesmo padrão nas duas. Testado pelo Matheus com uma cópia dos saves.
- **Mira do UC1** (`GOW3_MOUSE_AIM_U1_ALL=1`, desligado no código, **ligado pelo launcher no UC1**). O objeto da mira tem mais dois vetores quase iguais (+0x1690, +0x1830) que puxavam o giro de volta e faziam a mira pular (trocas de modo 69 → 5); recebem o mesmo giro. No zoom ao puxar L2 (~0,4–0,5 s) o jogo repõe a direção a cada quadro: nessa janela (600 ms) o mouse vai como analógico, com giro de teste de 0,5°, e volta ao direto no primeiro quadro em que a direção escrita fica exata (< 0,05°). Testes que não deram certo: direção absoluta durante o zoom (prendia a direção da mira anterior) e girar a câmera follow durante a mira (ela continua rodando, mas não alimenta a mira). O Matheus aprovou ("ficou bom assim").
- **Launcher: janela Controles** (`launcher/uc_launcher.py`, captura de teclas do launcher do SuperMedo). Olhar com o mouse (desligado por padrão), sensibilidade, multiplicador ao mirar (padrão 1), inverter Y, tecla por ação (duas por ação com `|`), botões do controle e "Usar os controles originais do port". O `input.ini` só é escrito depois de salvar a janela. Teclas padrão novas, definidas pelo Matheus: Espaço X, Ctrl esquerdo/Q Círculo, F Quadrado, E Triângulo, G L1, R R1, botão direito L2, botão esquerdo R2, V L3, botão do meio R3, 1/2/3/4 ou roda o direcional, Tab o touchpad.
- **Menu do jogo: aba Controls** (`gpu/shim/gow3_overlay.cpp`). Todas as opções da janela Controles, com captura de tecla, botão ou roda (B cancela). Grava o `input.ini` e o jogo recarrega na hora; o launcher adota essas mudanças ao abrir de novo. Ligar/desligar o mouse avisa que só vale depois de reiniciar.
- **F1 também abre o menu** (além de Insert e L2+R2+L3+R3); Insert, F1 e Alt esquerdo ficam reservados na captura de teclas.
- **Launcher 0.3.2: aviso de versão nova** (thread de publicação). Consulta a última release no GitHub e mostra um aviso; dá para desligar em Opções. `UC_PRETEND_VERSION` simula uma versão antiga para testar.

## 2026-10-17 — v0.3.1: registradores sujos por palavra, tocha da caverna, threads de job dormindo, UC2 R ligado por padrão, novo atalho do menu no controle

- **Gargalo do castelo (vegetação).** O `PostDraw` varria um `bitset<1664>` de registradores sujos a cada draw (~9% da thread de comandos). `RegDirty` (`regs.h`) ganhou uma máscara por palavra de 64 bits e `ForEachBlock`. Assim o `PostDraw` (`vk_rasterizer.cpp`) só visita os blocos tocados. Vegetação do castelo de 42 para ~50 FPS. Está sempre ligado. `GOW3_REGDIRTY_VERIFY=1` compara com a varredura completa. Deu 0 divergências no castelo (17,8 M draws), nas ruas de Cartagena (49 M), no telhado do UC2 (11,5 M) e no UC1 Emboscada (9,4 M). O Matheus jogou o castelo sem defeito visual.
- **Diagnósticos novos, todos desligados:** `GOW3_REGDIRTY_VERIFY`, `GOW3_FAULT_CATEGORY`, `GOW3_READBACK_DONE_FAST`, `GOW3_FRAME_GATE`, `GOW3_HLE_INORDER_GFX`. Também a amostragem de RIP da thread de comandos.
- **Tocha da caverna escura (UC3).** A tabela 256x8 que a luz da tocha lê é desenhada uma vez no boot. Com a compilação assíncrona, esse draw era pulado e a tabela ficava zerada. `GOW3_SYNC_SMALL_TARGETS=1` (`vk_pipeline_cache.cpp`) compila na hora quando todos os alvos têm até 64K pixels. Sem custo de FPS.
- **CPU das threads de job (UC3).** As NdJobWorkerThread 0-5 giravam com PAUSE sem nunca dormir no gameplay. `GOW3_JOB_SPIN=N` e `GOW3_JOB_SLEEP=1` (`src/probe.c`, `patch_job_spin`, achado por bytes) reduzem o giro e deixam dormir. Com 100 + sono, a CPU cai de ~66% para ~50% com o mesmo FPS.
- **UC2: telhado a 60 FPS.** A configuração R (`GOW3_HLE_INORDER=1` + `GOW3_ACB_DISPATCH_PIPE=1`) deixa a perseguição nos telhados a 58-60 FPS. O pulo do trem do prólogo ainda é sensível ao tempo e às vezes exige pular antes. Ficou nos problemas conhecidos: desligar as duas em Opções > Avançado se o trem falhar.
- **Padrões do launcher (decisão do Matheus, 05:31Z).** Os toggles continuam desligados no código. O launcher liga a configuração R no UC2, e tocha + `JOB_SPIN=100` + `JOB_SLEEP` no UC3. Os dois últimos só no UC3, porque só lá foram testados. Todos aparecem em Opções > Avançado. Versão do launcher 0.3.1.
- **Launcher: dump com a atualização fundida (thread de publicação, commit d6fcbec, aprovado pelo Matheus às 05:40Z).** O launcher lê o APP_VER do `param.sfo` da pasta do jogo. Se for 01.02 ou maior, não avisa mais que falta a atualização, e o campo de atualização pode ficar vazio.
- **Menu do jogo no controle: L2+R2+L3+R3.** R3+L2 era o zoom da mira de uma arma no Uncharted. O menu agora abre só com os quatro botões apertados juntos (`gow3_overlay.cpp`, `ComboCompleted`). O Insert não mudou. Textos atualizados no título do menu, nos README, no LEIA-ME, no launcher e no cabeçalho do ini. O Matheus testou e funcionou.
- **Ficou para depois da 0.3.1** (detalhes em NEXT_STEPS):
  - Travamento por TDR: o driver da AMD derrubou o jogo depois de uma migração de buffer de ~1,5 GB em um quadro (castelo em chamas, ~30 min).
  - Parede e luz piscando: uma textura de array é refeita do zero cada vez que entra na tela (avisos `SanitizeCopyLayers`).
  - Ruínas no deserto a 35-38 FPS: ~4400 draws por quadro, limitado pela thread de comandos.
  - Granulado do céu noturno possivelmente mais forte que no PS4.

## 2026-10-16 (manhã, 13) — CPU: medição por thread e nomes das threads do jogo no Windows

- **Pergunta do Matheus.** O jogo usa ~65% de uma CPU de 16 threads na praça (50% no museu), igual com o jogo pausado (o UC3 continua desenhando atrás do menu).
- **Medição (`tools/uc_cpu_threads.ps1`, `D:\Uncharted\medir_cpu_threads.bat`).** Lê o tempo de CPU de cada thread do `ucndc.exe` em 10 s, sem mexer no jogo; acrescenta em `out/cpu-threads.txt`. Praça: 10,6 núcleos, com 10 threads quase cheias (0,83–0,92) e 8 a ~0,18 cada (batem com os 8 "GPU Interrupts Handler", GfxEop + Compute0–6, threads do jogo esperando em `sceKernelWaitEqueue`). Museu: 7,7 núcleos; 6 threads cheias e 4 a 0,42.
- **Nomes.** As threads do jogo só tinham nome no winpthreads, que o Windows não enxerga. `set_host_name` (`src/runtime_thread.c`) agora também chama `SetThreadDescription` (procurada em tempo de execução), então a medição e o Gerenciador mostram os nomes.
- **Medição com nomes (castelo, trecho com vegetação a ~45 FPS, 10,9 núcleos).** NdJobWorkerThread 0–5 a 0,8–1,0 cada (~5 núcleos, constantes com o FPS: giram esperando tarefa); `gow3:DrawRec`, `gow3:VkRecorder`, `shadPS4:GpuCommandProcessor` e `gow3:RingCopy0` a ~0,9 (caem para ~0,55 a 30 FPS); `gow3:DrawPrep0–7` a ~0,2; os handlers de interrupção da GPU abaixo de 0,05. (O script pedia o acesso errado para ler o nome; corrigido para `THREAD_QUERY_LIMITED_INFORMATION`.)
- **`GOW3_IMPORT_COUNT=1` (diagnóstico, desligado).** Cada import de função resolvido passa por um thunk que conta as chamadas (`lock inc` com r11, que não é registrador de argumento); a cada 10 s o log mostra os 15 imports mais chamados. Para ver por quais chamadas do sistema o laço dos workers passa. `D:\Uncharted\medir_chamadas.bat`.
- **Resultado das chamadas (castelo, ~2,6 milhões/s).** `memcpy` 670 mil/s, lock/unlock de mutex 285 mil/s cada, `sceGnmIsUserPaEnabled` 240 mil/s, `_FSin` 135 mil/s, `CondBroadcast` e `WaitEqueue` 80 mil/s. Nenhum `yield`/`usleep`/`sleep` no topo: o laço de espera dos workers gira dentro do código do jogo, sem chamar o sistema. O log agora mostra o NID dos imports sem nome.
- **`GOW3_RIP_SAMPLE=1` (diagnóstico, desligado, só Windows).** Uma thread suspende cada thread do jogo filtrada (`GOW3_RIP_SAMPLE_FILTER`, padrão `NdJobWorker`, vários nomes separados por `+`, `*` = todas) a cada ~1 ms, lê o RIP e copia o topo da pilha, e retoma; a cada 10 s o log mostra os 10 endereços mais vistos de cada uma (`eboot+0x...`) e os 5 primeiros endereços do jogo achados na pilha (o provável chamador, que diz o que a thread espera quando o RIP está numa função nossa). O `medir_rip.bat` amostra os workers e a thread principal. Para achar o laço de espera e decidir se dá para trocá-lo por uma espera que dorme. `runtime_thread_snapshot` (`src/runtime_thread.c`) dá o id de SO e o nome de cada thread do jogo. `D:\Uncharted\medir_rip.bat`.
- **Resultado do RIP (castelo, 44 FPS).** Nos workers, 55–65% das amostras caem em `eboot+0xda66a0/0xda66ba/0xda64e4` (big3-ps4_Shipping.elf, vaddr) e ~13% numa syscall do ntdll. É o laço ocioso do job system: procura tarefa em 8 filas; sem tarefa, se `[mgr+0x59]` é 0 gira com `pause` para sempre; senão gira 1000 vezes (`cmp $0x3e8,%r8` em 0xda6506) e então `scePthreadMutexLock` + `scePthreadCondTimedwait` (10 ms) + unlock. Com ~80 mil broadcasts/s, cada despertar gasta 1000 `pause` (curtos no Jaguar, ~140 ciclos nos Intel recentes). Ideia: toggle que reduz esse 1000.
- **`GOW3_JOB_SPIN=N` (desligado).** Troca o 1000 desse laço por N (0 = dorme na hora), achado pelos bytes (`cmp byte [rdi+0x59],0 ; je ; cmp r8,0x3e8 ; jl`). O mesmo laço existe nos três jogos (UC1 eboot 0x45d236, UC2 0xb3c4d6, UC3 0xda6506). Conferido antes: o `scePthreadCondTimedwait` espera mesmo 10 ms e a frequência do TSC é a do PC. `D:\Uncharted\medir_spin100.bat`, `medir_spin30.bat`, `medir_spin0.bat`.
- **Testes no castelo (câmera variando, margem de erro grande).** Original ~44 FPS; `JOB_SPIN=100` ~48; 30 ~47; 0 45–48 e a mesma CPU (10,5 núcleos, workers a ~0,85). O RIP com 0 mostra amostras em 0xda6541, o `pause; jmp` do caminho em que `[mgr+0x59]` é 0: no gameplay o jogo proíbe os workers de dormir (função em 0xd96440, que também baixa a prioridade deles para 767) e só libera em cutscenes e loading, onde a CPU cai para ~5%. A CPU acompanha a cena (olhando para longe da vegetação vai a 60 FPS e cai).
- **`GOW3_JOB_SLEEP=1` (desligado).** Tira o `je` desse teste: os workers giram N vezes (`GOW3_JOB_SPIN`, padrão 1000) e dormem também no gameplay. Se o jogo não avisar os workers por broadcast nesse modo, cada espera dura até 10 ms. `D:\Uncharted\medir_sono.bat` (100 voltas + sono).
- **Resultado do `JOB_SLEEP=1` + `JOB_SPIN=100` (castelo).** Olhando para a vegetação, 43–46 FPS, igual ao original, sem travadas novas. A CPU caiu de ~10,5 para ~8,5 núcleos (66% → 52–55%): cada worker foi de ~0,85 para ~0,47 núcleo.
- **Suspeitos (thread de pesquisa).** Workers de tarefas do jogo (NdJobWorkerThread 0–5) girando por trabalho; `sched_yield`/`usleep(0)` do jogo viram `SwitchToThread`; `WaitForSmallTimer` do equeue gira com `yield`; as esperas entre estágios (Drain, WaitDone, WaitHostCopies) nunca dormem. Próximo passo: medir com nomes, depois amostrar onde cada thread gira.

## 2026-10-16 (manhã, 12) — Nomes sem "gow3": `ucndc.exe`, `ucndc.ini` e variáveis `UC_*`

- **Pedido do Matheus.** Trocar os nomes herdados do gow3_pc que o jogador vê (partes 1 e 2 da proposta; os nomes internos de arquivos ficam).
- **Executável.** No Windows o build gera `out/ucndc.exe` (`OUTPUT_NAME` em `gpu/CMakeLists.txt`; o alvo do ninja continua `gow3-probe`). `run.py` usa `ucndc.exe` e só cai para `gow3-probe.exe` se o novo não existir (Linux, builds antigos). `run_uc3.ps1`, `testar_ambiente.sh`, `fechar_jogo.bat`/`ler_erros.bat` da raiz e as ferramentas `uc3_*` reconhecem os dois nomes.
- **Configuração.** `ucndc.ini`. Na primeira execução sem ele, `run.py` copia o `gow3.ini` para `ucndc.ini` (o antigo fica intacto). O jogo, sem `GOW3_CONFIG`, lê `ucndc.ini`, ou o `gow3.ini` enquanto só ele existir.
- **Variáveis.** Toda `GOW3_<NOME>` também pode ser dada como `UC_<NOME>`; se as duas existirem, a `UC_` ganha. O alias é feito em `run.py`, no `run_uc3.ps1` (inclusive no `-Set`) e no próprio exe (`src/probe.c`, construtor de prioridade 101, antes dos inicializadores da biblioteca da GPU). O código continua lendo `GOW3_*`, então os .bat antigos seguem funcionando.
- **Ícone.** O `ucndc.exe` usa o ícone do launcher (`launcher/uc.ico`, em `packaging/windows/gow3-probe.rc`) no lugar do ícone do God of War III.
- **Thread de publicação.** Ajusta o launcher, `windows\*.bat`, o empacotamento e o README para os nomes novos.

## 2026-10-16 (manhã, 11) — Menu do jogo (Insert / R3+L2) sem as opções do God of War III nos Uncharted

- **Pedido do Matheus (com a thread de publicação, que faz a tela de Opções no launcher).** O menu herdado do gow3_pc mostrava no Uncharted coisas que só funcionam no God of War III.
- **Mudança (`gpu/shim/gow3_overlay.cpp`).** `GodOfWar()` (título do param.sfo contém "God of War", ou título vazio) decide. Fora do GoW3 somem: a aba Game (trapaças, multiplicadores de orbes, barra de vida de inimigo), a seção de escala e nitidez (FSR 1/RCAS) e suas linhas de informação, "Render resolution" e "Engine frame rate" (patches do executável do GoW3), o item "Upscaler" do overlay e a linha "Game hooks". O `menu_tab` salvo como 0 vira a aba Image; L1/R1 percorrem só as abas visíveis. No GoW3 nada muda.

## 2026-10-16 (manhã, 10) — UC1: tiros com som (O_CREAT em /app0 abre o arquivo existente)

- **Causa (achada pela thread de pesquisa, comparando com o uncharted_pc do SuperMedo, commit 5c457e7, GPL).** O UC1 abre a configuração de som (`sound1/config/ps4/...`) com `O_CREAT`. O nosso `do_open` recusava qualquer `O_CREAT` em `/app0` (somente leitura) com `EROFS`, então o Scream nunca carregava os protótipos das armas e o tiro nem chegava a tocar, sem erro nenhum (ver entrada 9).
- **Correção (`src/runtime_file.c`, sempre ligada).** Como no FreeBSD, `O_CREAT` em montagem somente leitura abre o arquivo se ele existir; só a criação de um arquivo novo falha. O filtro do rastreio de `open` agora procura `sound` (o caminho real é `sound1/`).
- **Resultado.** O Matheus testou com `boot_uc1_low.bat`: os tiros têm som.

## 2026-10-16 (madrugada, 9) — UC2 fica na configuração I2; UC1: regras de fim de job do Ajm e patch do assert `m_gfxEopTick` (toggles desligados)

- **UC2, vagão do trem (decisão do Matheus: por enquanto, I2).** No prólogo o vagão cai cedo se o pulo é tardio. Precisa de `GOW3_COMPUTE_PIPE` e `GOW3_READBACK_ASYNC` juntos, e piora quanto mais liso o jogo roda a 60.
  - Descartados: timestamps, sobrescrita de label, landings sujos, ordem das faltas nas janelas da ACB, fila lateral (o par sozinho, N3, reproduz) e tempo das labels. Com `GOW3_TOGGLE_FILE` máscara 2199023255552 (RecorderFences desligado), O passou 3/3 só com o par, mas o ritmo ficou travado; P = todos os toggles + máscara 0/4.
  - O `ProcessCommands` já drena o B antes do `flush_request` (o `ASYNC_DRAIN` é redundante). O pisca de textura sem `SCALAR_COPY_HLE` é o antigo.
  - Configuração do UC2 por enquanto = I2 (sem `COMPUTE_PIPE`): `boot_uc2.bat`. Logs `out/trem-n1-0546.log`, `out/trem-O-0603.log`, `out/trem-P-0606.log`; notas em `investigacao-opus.md` (R.24).
- **UC1, som (`GOW3_AJM_END_RULES=1`, do uncharted_pc do SuperMedo, GPL).** Um job do Ajm termina sem pedir mais entrada quando o último sample já saiu, e a saída cheia depois de quadros decodificados encerra o job normalmente (`NOT_ENOUGH_ROOM` só quando nem o primeiro quadro cabe). No UC1 fala e efeitos já tocavam; o tiro continua mudo e o log não mostra erro do Ajm, então a causa é outra. Novo `GOW3_AUDIO_TRACE=2`: registra todos os jobs (`boot_uc1_audio.bat`).
- **UC1, tiro sem som (aberto).** Diagnósticos novos, todos desligados por padrão:
  - `GOW3_AUDIO_DUMP` agora grava o buffer cru do jogo (ordem PS4, antes de volume e downmix); com `GOW3_AUDIO_STATS`, o log mostra cada `sceAudioOutSetVolume`.
  - `GOW3_SCREAM_LOG=1` (só UC1 1.02, confere os bytes): liga as mensagens do motor de som (Scream/BoomRangBuss, função `0x3c0750`, e o relatório de erros de voz/banco em `0x407ae0`).
  - `GOW3_AUDIO_SURROUND=1`: o `sceAudioOutGetPortState` anuncia os 8 canais da porta em vez de 2.
  - Resultado: os jobs do Ajm voltam todos sem erro, todos os bancos são ATRAC9 mono, o volume é igual em todos os canais e o Scream não reporta erro nenhum nos tiros. O áudio cru mostra que o próprio jogo não mistura o tiro. Com 7.1 anunciado (o driver deixa de reduzir 8 para 2), o tiro continua mudo. Causa ainda desconhecida; bats `boot_uc1_audio*.bat`, `boot_uc1_scream.bat`, `boot_uc1_surround.bat`.
- **Assert `m_gfxEopTick` (`GOW3_EOP_TICK_WAIT=1`, do SuperMedo 0.3.2).** `patch_eop_tick_wait` acha a mensagem do assert na imagem e faz a checagem esperar a GPU (`pause; jmp` de volta ao `cmp`). No UC1 a checagem usa um byte SIB (`cmpq $0,0xa08(%rax,%rcx,1)` em `0x4d2776`) e o handler é um ponteiro de função, então o padrão foi ampliado para as duas formas.

## 2026-10-16 (madrugada, 8) — UC1 passa do assert offsetTableIndex: imagem carregada abaixo de 2 GiB (`GOW3_LOW_IMAGE=1`)

- **Causa.** O UC1 parava no assert `offsetTableIndex` (`TGenericOffsetPtr.h:100`) logo depois de montar o save. A `GenericOffsetTable` da Bluepoint (tabela em `0xdbc980`, contador em `0xdbc9a0`, iniciada em `0x7e14a`) só codifica ponteiros em [0, 2 GiB) e [0x1000000000, +2 GiB): 31 bits de offset e o bit 31 escolhe a tabela. O port mapeava a imagem em `0x800000000` (`LOW_MIN` do `runtime_low_map`), então o primeiro ponteiro para os dados do próprio jogo (`0x8007ba7ea`) não cabia em nenhuma faixa. No PS4 o executável fica em `0x400000`.
- **Correção.** `GOW3_LOW_IMAGE=1` (desligado por padrão) faz o `runtime_low_map` começar em `0x400000`. Imagem, traps, stubs e pilhas descem juntos, então os saltos rel32 dos hooks continuam ao alcance. O `probe.c` avisa se não houver faixa livre abaixo de 2 GiB. Nada no port classifica endereços do guest por `LOW_MIN`.
- **Resultado.** `boot_uc1_low.bat`: imagem em `0xbbf0000`, sem assert, o Matheus jogou e está tudo ok; 60 FPS (pior quadro 17,9 ms).
- Alternativa guardada (sugestão do Opus): corrigir a base da entrada 0 da tabela para a base real da imagem, só no UC1.

## 2026-10-16 (madrugada, 7) — UC1/UC2: primeiro boot; UC2 roda; vagão do trem cai cedo com compute pipe + readback assíncrono (toggles e diagnósticos novos, todos desligados)

- **Preparação para UC1/UC2 (OK do Matheus).**
  - `equeue.cpp`: o mapa de kqueues agora fica atrás de um mutex (`LockedEqueues`), como no uncharted_pc do SuperMedo (GPL).
  - UC2: dois NIDs que faltavam resolvidos em `src/runtime_services.c`: `sceNpScorePollAsync` (`np_poll`) e `sceNpSignalingGetPeerNetInfo` (`np_signed_out`). `tools/nid_names.py` atualizado.
  - Ajm (fim de job do áudio) ficou de fora por decisão do Matheus, até aparecer som faltando.
- **Primeiro boot.**
  - UC1 (`boot_uc1.bat`, eboot.bin): o menu da coletânea abre a 60 FPS, mas ao carregar o UC1 o jogo afirma `offsetTableIndex` (`GenerateOffset`, `TGenericOffsetPtr.h:100`) e depois lê `0xffff…` numa NdJobWorker, logo após montar o SAVEDATA01P. Log: `out/boot-uc1-assert.log`. Investigação pendente.
  - UC2 (`boot_uc2.bat`, big2-ps4_Shipping.elf): roda, sem asserts nem faltas, 51–60 FPS, saves próprios (02P/020/0CC).
- **Trem do prólogo do UC2: o vagão cai antes do Drake chegar.**
  - Relógios conferidos (process_time e contador em CLOCK_MONOTONIC, 1e9); `engine_fps` não se aplica ao UC2.
  - Bissecção do Matheus, com o critério final de **pular tarde** (pular cedo esconde o bug):
    - sem toggles novos: passa;
    - tudo ligado: falha;
    - tudo menos compute pipe (`uc2_trem_I2_sem_compute_pipe.bat`): passa;
    - tudo menos readback assíncrono/sideq (`uc2_trem_G2_sem_async.bat`): passa;
    - tudo menos leaf copy, ou menos o readback rápido: falha.
  - **Conclusão:** precisa de `GOW3_COMPUTE_PIPE` + `GOW3_READBACK_ASYNC` juntos.
  - Cronômetro do Matheus num percurso fixo: 17,6 s (I2, passa) contra 17,2 s (tudo ligado, falha). O Drake anda igual; o que muda é o momento em que o vagão cai.
- **Hipóteses testadas, todas atrás de toggles desligados:**
  - `GOW3_HLE_READBACK_WAIT`: o HLE de cópia escalar espera readbacks da origem/destino e cai para a GPU se a origem foi escrita pela GPU. Não resolveu.
  - `GOW3_HLE_COMPUTE_SAFE=1/2`: o HLE cai para a GPU com escrita pendente, ou vindo da fila de compute. Nunca agiu no UC2.
  - `GOW3_READBACK_ASYNC_DRAIN`: a drenagem em `Rasterizer::ReadMemory` roda na thread que faltou, onde não faz nada (`!OnStageA`); agora drena na thread de comandos. Melhorou (3–4 de 7, pulando cedo), mas não resolve.
  - `GOW3_READBACK_LABEL_GUARD`: um readback que chega atrasado não sobrescreve rótulos/fences escritos depois dele. Nunca agiu.
  - `GOW3_READBACK_DIRTY_RETRY`: readback "sujo" (a GPU reescreveu a janela no meio) é lido de novo, síncrono. Agiu (~13 por partida), não resolveu.
  - `GOW3_READBACK_ASYNC_ACB_SYNC`: faltas em janelas escritas pelo compute assíncrono (marcadas na etapa A) usam o caminho síncrono. Agiu (112 janelas, 2 mil leituras), não resolveu.
  - `GOW3_TIMESTAMP_DIAG`: estatística dos timestamps de GPU escritos em rótulos. No UC2 são raros (marcadores de perfil, ~1/s por slot), iguais nos dois casos; não são a base de tempo do jogo.
- **Para jogar o UC2 hoje:** `uc2_trem_I2_sem_compute_pipe.bat`.
- **Próximo:** entender o que o compute pipe muda na ordem entre a fila de compute assíncrona e as leituras do jogo; a mesma família pode explicar a animação do UC3 que às vezes "volta um quadro".

---

## 2026-10-16 (madrugada, 6) — Texturas trocadas: o HLE de cópia escalar agora avisa os caches (`GOW3_HLE_INVALIDATE`, desligado); aviso de espera presa da GPU

- **Sintoma.** Com o `jogar_60`, o Matheus viu texturas trocadas de forma intermitente:
  - o colete do garçom no pub;
  - o chão do museu no primeiro carregamento de um New Game (some com reload);
  - a porta do banheiro.
- **A/B do Matheus (New Game até o museu):**
  - sem readback adiado: o bug continua;
  - sem a thread de cópia do ring: o bug continua;
  - sem o HLE de cópia escalar: sem trocas, mas as roupas piscam (o pisca antigo de antes do HLE) e o jogo travou uma vez.
- **Causa.** `CopyGuestThroughBacking` (`vk_shader_hle.cpp`) escreve pela visão de backing, que passa por fora da proteção de páginas. Os caches de buffer e de textura nunca ficam sabendo da escrita, e draws seguintes podem usar a cópia velha que está na GPU.
- **Correção (`GOW3_HLE_INVALIDATE=1`, desligada por padrão).** Antes da escrita, faz o que uma falta de escrita da CPU faria:
  1. espera as cópias do ring já enfileiradas;
  2. resolve readbacks pendentes da origem (`CpuRead`);
  3. chama `InvalidateMemory(dst, size, assume_locks=true)`.

  Depois da escrita, a invalidação baixaria os bytes velhos da GPU por cima da cópia; por isso vem antes. A ordem foi revisada pela pesquisa (Opus).
- **Resultado.** O Matheus jogou do New Game até a praça com `jogar_60_hle_corrigido.bat` sem nenhuma textura trocada, sem perda de FPS visível. Foram ~5,4 milhões de cópias, com 0 readbacks de origem.
- **Travamento sem o HLE.**
  - O despejo (minidump lido com um parser próprio + addr2line) mostrou espera mútua: todas as threads do port ociosas, a thread de comandos girando no laço de submissões e threads do jogo girando à espera da GPU.
  - Suspeita (Opus): um `WaitRegMem` gráfico esperando um rótulo ligado a um tick que nunca foi submetido. O laço só drena enquanto a etapa B tem trabalho; depois só cede.
  - Entrou um aviso sempre ligado: `Liverpool::ReportStuckWait`. Quando um `WaitRegMem` (gráfico ou compute) fica mais de 2 s sem ser atendido, o log mostra endereço, valor esperado e atual, fence pendente, tick atual, sinais adiados e chunks na fila. Para isso foram criados `Scheduler::DeferredSignalCounts()` e `QueuedChunks()`.

---

## 2026-10-16 (madrugada, 5) — R.22: preparo de draw reaproveitado com validação por valor (`GOW3_PREP_TRUST`, desligado); a 120 Hz, +2 a 4 FPS só no modo sem validação

- **A 120 Hz o limite é a CPU.** O Matheus pediu os testes com o FPS destravado (`GOW3_VBLANK_HZ=120`, `medir_120*.bat`). Sem nada novo, o Castelo (save 038) fica em 61–66 FPS, com piores quadros de 25–34 ms. A etapa A leva ~17 ms por flip.
- **Onde vai o custo.** O perfil (`GOW3_STAGEB_PROFILE`) agora divide a checagem do pipeline preparado, que custa 1,63 µs por draw:
  - walker da SRT: 1,18 µs;
  - `GetParams`: 0,13 µs;
  - comparação do flat: 0,11 µs.
- **R.22, em cima do R.21:**
  - O worker anota, além das páginas, o valor de cada ponteiro (8 bytes) e de cada folha (4 bytes) que o walker leu. Guarda o endereço da visão de backing, a mesma leitura sem proteção que o walker usa.
  - Esperas e pacotes sem faixa deixam de invalidar tudo. Depois deles, o draw vale se os valores lidos não mudaram. Escritas da GPU ainda pendentes continuam nos carimbos de página.
  - Escritas grandes (mais de 64 páginas) entram num anel de 256 faixas. Só invalidam quem leu dentro delas.
- **Sombra a 120 Hz:** ~80% dos draws confiáveis, 0 divergências. Em milhões de validações, os valores nunca mudaram depois de uma espera. O resto falha por escrita de página.
- **`GOW3_PREP_TRUST=1` (seguro):**
  - O walker cai para ~0,3 µs por draw e o `GetParams` para 0,01.
  - Mas a checagem inteira continua em ~1,5 µs: ler de novo os valores custa o mesmo que o walker (são as mesmas leituras espalhadas).
  - FPS sem ganho (58–69).
- **`GOW3_PREP_TRUST=3` (sem validar por valor, só para medição):** 63–70 FPS contra 61–66, e piores quadros de ~25 ms.
- **Fica desligado por padrão.** O resto da etapa A pesa mais: `A:draw` 46%, `A:post` 41%, `P:ring` 20%.
- **Velocidade a 120 Hz:** nos testes, o combate começou e o Drake morreu parado antes do que nas execuções a 60 Hz. Isso pode indicar jogo acelerado; precisa ser medido antes de recomendar 120.

---

## 2026-10-16 (madrugada, 4) — Placas Intel: `VK_EXT_vertex_input_dynamic_state` desligada (correção do SuperMedo 0.3.3)

- No Windows, o driver da Intel não desenha nada com `VK_EXT_vertex_input_dynamic_state`: tela preta na HD 630.
- A correção vem do Supermedo/uncharted_pc 0.3.3 (commit 68c8cf9, GPL; issue #56 deles). O crédito é dele.
- `vk_instance.cpp`: a extensão só é pedida quando o fornecedor da placa não é Intel (0x8086).
  - `GOW3_VERTEX_INPUT_DYNAMIC=1/0` força ligada ou desligada.
- Na RX 6750 XT (AMD) nada muda: compilou e a extensão continua ativa (sem aviso de indisponível no log).
- Ainda não foi testado numa placa Intel.

---

## 2026-10-16 (madrugada, 3) — R.20/R.21 confiança no pipeline preparado (`GOW3_PREP_TRUST`, desligado): correto, mas ~0% de acerto

- Ideia (pesquisa, Opus): na etapa A, quando os registradores batem com os do worker de preparação, reaproveitar o `flattened_ud_buf` que o worker já calculou, sem refazer `GetParams` e o walker da SRT (~1,55 µs por draw).
- R.20 usava uma época global, que mudava a cada escrita ou espera do command stream: ~0 acertos.
- R.21 troca a época por uma tabela de páginas de 4 KiB (64k entradas, sem tag) com números de sequência:
  - O worker anota as páginas que o walker lê.
  - Escritas do CP, `NoteGuestWrite` e bindings gravados marcam as páginas que cobrem. Escritas acima de 64 páginas marcam um carimbo "grande".
  - Esperas (`MemSemaphore`, `WaitRegMem`, `Rewind`, `WaitOnCeCounter`) e pacotes que escrevem sem anotar a faixa marcam um carimbo global.
  - Cópias da CPU (`DumpConstRam`, `CopyBuffer` no host, HLE) marcam de novo depois de escrever.
- Modo sombra (`GOW3_PREP_TRUST=2`, `medir_r920_sombra.bat`), Castelo (save 038):
  - 60 FPS, imagem correta, 0 divergências.
  - Porém ~0 acertos: todas as falhas vêm de evento global. Por segundo, são ~50 mil esperas e ~80 mil pacotes de escrita sem faixa anotada, contra ~115 mil draws.
  - Como o worker prepara uma submissão à frente, quase sempre há uma espera no meio.
- Fica no código, desligado por padrão (`=1` confia, `=2` sombra). Para dar resultado seria preciso anotar a faixa dos pacotes de escrita e não tratar toda espera como global. O Matheus pediu para parar as implementações novas, então isso fica para depois.

---

## 2026-10-16 (madrugada, 2) — O quadro de 33 ms a cada 5 s vinha do relatório de diagnóstico: Castelo e ruas a 60 FPS cravados

- Com `medir_r916b.bat` (= `medir_r916` sem `GOW3_PERF_DIAG`):
  - Castelo (save 038): 60,0 FPS em todas as janelas depois de assentar, pior quadro ~18 ms, 0 quadros acima de 1,5x a mediana.
  - Ruas (save 036): 60,0 na maioria das janelas, com um ou outro 59,4–59,6.
- Com `GOW3_PERF_DIAG=1`, o relatório de 5 s (`Readback profile`, impresso sob o `readback_mutex` na thread da GPU, mais o perfil de esperas) causava um quadro de ~33 ms por janela. Sugestão da pesquisa (Opus). Para medir FPS, não use `PERF_DIAG`.
- O perfil da etapa A (`GOW3_STAGEB_PROFILE=1`) agora divide o "A:gfx pipeline". No Castelo: publish/pump 0,04 µs por draw; checagem do pipeline preparado ~1,55 µs, que refaz o `GetParams` e o walker da SRT por estágio. É o alvo do R.20 (pesquisa), para dar folga.

---

## 2026-10-16 (madrugada) — Esperas síncronas pela GPU: readback adiado de imagens no `gow3.ini` + leitura de buffer na fila de transferência (R.9.15/R.9.16, desligados); Castelo ~53 → 59,4–60 FPS

Com `GOW3_PERF_DIAG=1` no Castelo (save 038), ~19% do tempo eram esperas síncronas pela GPU:

- **`texture_cache.cpp` `ProcessDownloadImages`:** `scheduler.Finish()` ~100 vezes por segundo. As imagens são 2x2/4x4/8x8 RGBA16F, uma cadeia de exposição.
  - Causa: o `gow3.ini` do Matheus estava com `deferred_readback=0`, e o `run.py` reexporta `GOW3_DEFERRED_READBACK` a partir do ini, então `-Set` sozinho não liga a opção.
  - Com `deferred_readback=1`, a espera some: Castelo de ~53 para 57–59 FPS, imagem correta. O Matheus autorizou ligar no ini dele; o original ficou em `out/gow3.ini.antes-deferred`.
- **`buffer_cache.cpp` `DownloadMemory`:** cerca de 45 leituras por segundo, feitas pela própria GPU, faziam `CopyBuffer` + `Finish()`.
  - Com `GOW3_READBACK_FAST=1` (já existia), todas viram cópia lateral, porque a escrita já terminou.
  - Mas a cópia lateral na fila gráfica espera o trabalho enfileirado depois da escrita (~5% do tempo).
- **R.9.15 (`GOW3_READBACK_FAST_WAIT=1`):** se a escrita está num command buffer já fechado, `SideDownload` espera só o tick dela em vez de cair no `Finish()`. Quase nunca é necessário (0–1 vez por janela).
- **R.9.16 (`GOW3_READBACK_FAST_SIDEQ=1`, requer `GOW3_READBACK_SIDEQ=1`):** `SideDownload` envia a cópia na fila de transferência, esperando o tick da escrita no timeline (já sinalizado), como o `SideQueueCopy`. O custo das cópias laterais caiu de 260–320 ms para ~47 ms a cada 5 s.
- **Resultado (`medir_r916.bat`):** Castelo 59,4–60 FPS; ruas 59–60 FPS. A etapa B fica 65% ocupada, a espera por ticks da GPU fica em 0%, a imagem está correta e não houve asserts. Resta ~1 quadro de ~34 ms a cada 5 s.

---

## 2026-10-15 (noite, 4) — Correções de segurança no R.9.12/R.9.13 (revisão Opus); ruas 59,8–60 FPS, O Castelo ~55 FPS

- **Thread de cópia do anel:** passa a ler a memória do jogo sempre pela visão de backing (`CopySparseMemoryBacking`), sem tocar páginas protegidas. Antes, uma falta de página dentro dela podia esperar a GPU, que esperava a própria cópia (travamento raro).
- **`DeferSubmission`:** removido o laço que esperava as cópias de host anotadas. Era redundante (a ordem já vem do `copy_seq` e do `Gow3Copy::WaitAsync`) e podia travar.
- **Resultado com `medir_r913_dedup.bat` (R.9.13 + R.9.10):**
  - ruas (save 036): 59,8–60 FPS, pior quadro 18 ms, imagem correta;
  - O Castelo (save 038, em combate): média ~55 FPS (49–58), pior quadro ~35 ms, imagem correta. A etapa B fica 68% ocupada, com 1100–1900 draws por quadro, e espera ticks da GPU 11–18% do tempo.

---

## 2026-10-15 (noite, 3) — R.9.13: o envio antecipado não espera mais a thread de gravação (`GOW3_DEFERRED_SUBMIT=1`, desligado): ruas ~56 → ~58,5 FPS

Teste nas ruas (save 036, início), com `medir_r913.bat`: R.8.4 + HLE + R.9.1 (128 pacotes / 1 ms) + R.9.4 + R.9.6 + R.9.12, sem perfil.

- **Causa:** cada envio antecipado à GPU (~3000 por segundo) custava ~38 µs na etapa B. Desse tempo, ~21 µs eram a espera da thread de gravação Vulkan terminar o command buffer (`SyncRecording`), mais o `end()` e o novo command buffer.
- **R.9.13 (`GOW3_DEFERRED_SUBMIT=1`, requer `GOW3_ASYNC_SUBMIT=1`):** no envio assíncrono com gravação em thread, a etapa B registra o fim do command buffer como mais um comando (`Scheduler::DeferSubmission`) e segue em frente. A thread de gravação, ao chegar nele:
  1. espera as cópias de memória anotadas até ali e as threads de cópia;
  2. dá `end()`;
  3. põe o command buffer na fila da thread de envio;
  4. começa o próximo command buffer.

  O pool de command buffers só é usado por essa thread enquanto a gravação está adiada. Os outros envios (síncronos, `Finish`, fallback quando o `on_submit` grava direto) esperam a gravação antes, para manter a ordem na fila. Os sparse binds do `on_submit` tomam o `submit_mutex`.
- **Resultado:** 57–58,6 FPS, imagem correta, a etapa B fica 68% ocupada. A etapa B agora espera a thread de cópia do anel 8,9% do tempo (ela fica 48% ocupada).

---

## 2026-10-15 (noite, 2) — R.9.12: cópias do anel de constantes numa thread própria (`GOW3_RING_COPY_THREAD=1`, desligado): ruas ~48 → ~55–56 FPS; T3/T3b sem ganho

Teste nas ruas (save 036, início), com R.8.4 + HLE + R.9.1 + R.9.4 + R.9.6.

- **T3 (`GOW3_EARLY_VO_LABEL=1`, `videoout/driver.cpp`):** no `SubmitFlip`, libera o label do buffer de vídeo anterior em vez de esperar o próximo flip. Resultado: ~47,5–48 FPS, sem ganho.
- **T3b (`GOW3_VO_SKIP_DRAIN=1`, `liverpool.cpp`):** no WaitRegMem de label de vídeo, pula o `DrainDrawPipe` quando o label já está livre. Resultado: a espera só migrou para o `Rasterizer::OnSubmit` (o dreno do `sceGnmSubmitDone`, obrigatório pelas fences). ~48 FPS.
- **R.9.12 (`GOW3_RING_COPY_THREAD=1`, novo `vk_ring_copier.h`):** a etapa A (thread do CP) só enfileira as cópias de memória do jogo para o anel de constantes, numa fila SPSC de 64 mil entradas. A thread `gow3:RingCopy` faz as cópias e o flush.
  - O cabeçalho de cada pacote do `DrawPipe` leva `copy_seq`. A etapa B espera as cópias anteriores ao pacote antes de rodá-lo, então a etapa B e a GPU veem os mesmos bytes de antes.
  - A etapa A só espera todas as cópias antes das escritas na memória do jogo que ela mesma faz: `MemSemaphore` (sinal, gráfico e compute) e `DumpConstRam`. Os pacotes `RunInOrder` (WriteData, EOP, ReleaseMem do compute) rodam na etapa B e já esperam. Os opcodes não pipelinados drenam a etapa B.
  - Uma primeira versão esperava em todo opcode fora de registradores e draws, e também no `NoteGuestWrite`. Isso deixou a etapa A 16% esperando e o FPS ficou igual. Agora são 0 esperas na etapa A.
  - Linha nova em `GOW3_FRAME_STATS`: "ring copy thread: busy …; stage B waited …".
- **Resultados nas ruas:**

  | Configuração | FPS |
  |---|---|
  | Com perfil (`medir_r912.bat`) | 53–54 |
  | Sem perfil (`medir_r912_semperfil.bat`) | ~55 |
  | Envio antecipado a cada 128 pacotes ou 1 ms (`medir_r912_e128.bat`) | ~56 |

  Com o envio a 128 pacotes / 1 ms, a etapa B fica 73% ocupada (80% com 64/0,5 ms). Imagem correta. A thread de cópia fica ~45% ocupada.
- **Gargalo agora: a etapa B (gravação).** Cada envio custa ~38 µs na etapa B: ~21 µs esperando a thread de gravação Vulkan, ~10 µs no `end()`/fila e ~6 µs no novo command buffer. Por dispatch, `BindBuffers` custa ~0,9 µs.

---

## 2026-10-15 (noite) — R.9.3–R.9.10: o caminho crítico é a etapa A; perfis e quatro tentativas atrás de interruptores (todos desligados), sem ganho de FPS

Teste nas ruas (save 036, início), com R.8.4 + HLE + R.9.1 (N=64, 0,5 ms): ~50–51 FPS. Com as opções padrão + HLE (`medir_so_hle.bat`): ~46 FPS, sem regressão.

- **R.9.3 (`GOW3_STAGEB_PROFILE=1`):** perfil por passo das etapas A e B (`RunDrawPacket`, `DrawRecord`, `DispatchRecord`, `BindResources`, `BindBuffers`, `Draw`, `DispatchDirect`, `PostDraw` e as partes do anel de constantes) e do envio (`SubmitExecution`).
- **R.9.4 (`GOW3_ASYNC_SUBMIT=1`):** o `vkQueueSubmit` passa para uma thread própria. A etapa B caiu de 72% para 67% ocupada; FPS igual.
- **R.9.5/R.9.7 (`GOW3_STALL_MS=N`):** registra quadros longos e o tempo de cada flip (jogo, etapa A, etapa B). Resultado: o jogo entrega o quadro de −8 a +4 ms do flip anterior, mas a etapa A só chega ao pacote de flip 16–23 ms depois. **A etapa A (thread do CP) é o caminho crítico.**
- **R.9.6 (`GOW3_READBACK_SIDEQ=1`):** a cópia dos readbacks assíncronos vai para uma fila de transferência separada, esperando o tick da GPU por semáforo. 1438 de 1732 readbacks foram por ela, e a espera das threads do jogo caiu à metade; FPS igual.
- **Perfil da etapa A:** ~280 mil pacotes/s. `PostDraw` ocupa 53% da etapa A, e a cópia de constantes para o anel (`CollectRingBindings`) 43%. São ~2,5 GB/s: ~650 mil cópias/s de ~3,7 KB, ~50 MB por quadro. O memcpy já roda a ~11 GB/s; o custo é o volume.
- **R.9.10 (`GOW3_RING_DEDUP=1`):** reaproveita a cópia de constantes do mesmo (endereço, tamanho) dentro do mesmo envio. Invalida quando:
  - começa um novo envio;
  - há esperas que sincronizam com a CPU (WaitRegMem, MemSemaphore, Rewind, WaitOnCeCounter);
  - há escritas sobrepostas anotadas na ordem do fluxo (`NotePendingGpuWrite`, `NoteGuestWrite` na HLE, em `CopyBuffer`, em `DumpConstRam` e no ReleaseMem da fila de compute).

  O anel ganhou `Pin` (fila de mínimos), que segura a região reusada até o pacote que a usou terminar. Revisão de correção pela Pesquisa com Opus (R.12). Resultado: 29% de acerto, de 2,5 para 2,1 GB/s, FPS igual (~50) e imagem correta.
- **R.9.11 (desfeito):** copiar só os bytes que o shader lê. Quase todas as leituras de constantes do jogo são cargas vetoriais ou têm índice variável, então nada pôde ser cortado.

Próximos candidatos: o T3 da Pesquisa com Opus (liberar o label de vídeo logo após o `PrepareFrame`) e o custo por dispatch da etapa A (~4,7 µs; ~3900 dispatches por quadro nas ruas).

---

## 2026-10-15 — R.9.1: envio antecipado dos comandos à GPU (`GOW3_EAGER_SUBMIT=N`, desligado): ruas ~46 → ~50 FPS; R.9.2 sem efeito

**Causa (diagnóstico do Fable, R.9):** a build só enviava comandos à GPU quando o jogo terminava de submeter o quadro. A gravação (~13 ms) e o trabalho da GPU (~12 ms) corriam em série, e a GPU ficava ociosa metade do quadro.

**R.9.1:** a etapa B (`RunDrawPacket`) agora corta e envia o buffer de comandos no meio do quadro (`scheduler.Flush()`) a cada N pacotes ou a cada `GOW3_EAGER_SUBMIT_US` µs (padrão 1500). Só corta se ainda houver pacotes atrás na fila (`DrawPipe::MoreBehind`). O resumo de estatísticas mostra "eager submissions".

| Ruas (036, início) | FPS | Quadros > 1,5× mediana | GPU ticks bloqueado | Etapa B ocupada |
|---|---|---|---|---|
| Sem corte | ~46 | ~70 | 21% | 60% |
| N=256, 1,5 ms (`medir_r91.bat`) | ~50 | ~47–52 | 13% | 62% |
| N=64, 0,5 ms (`medir_r91_64.bat`) | ~50,6 | 47 | 8% | 70% |

A imagem estava correta e o jogo fechou limpo. A porta do beco não foi testada com esse interruptor. O gargalo agora é a etapa B (gravação, ~70% ocupada). O ritmo continua bimodal (mediana 16,8 ms, p99 34 ms): o quadro que passa de 16,7 ms espera o próximo vblank.

Falta conferir: alguns usos de `CurrentTick()` fora da etapa B podem ver um tick velho depois do corte. A auditoria não achou problema, mas não provou tudo; a revisão foi pedida à Pesquisa com Opus.

**R.9.2 (`GOW3_ACB_FENCE_NOTE=1`, desligado):** registrava os ReleaseMem da fila de compute como fences pendentes para evitar drenagens. Não mudou nada: as esperas da etapa A em `ProcessGraphics:1394` são o jogo esperando os labels de flip (VideoOut), não fences de compute.

---

## 2026-10-15 — R.8.5: a HLE espera só uploads pendentes do mesmo destino (`GOW3_HLE_PRECISE_WAIT=1`, desligado); correto, mas sem ganho

**Desenho do Fable (R.8.5):** as cópias de host adiadas leem a memória do jogo depois, nas threads de gravação e de cópia. Agora cada uma soma 1 num contador por página de 64 KiB da origem (tabela com hash de 65536 posições; colisão só causa uma espera a mais) ao ser emitida, e subtrai ao terminar (`NotePendingHostRead`, em `SmallGuestCopy`, nos grupos assíncronos e em `RunGuestCopy`). A HLE da cópia escalar só chama `WaitHostCopies()` se alguma página do **destino** tiver leitura pendente (`HasPendingHostReads`).

**Resultado:** a HLE nunca precisou esperar (0 esperas, ~130 mil cópias liberadas) e a porta do beco continua certa. Mas o FPS não mudou:

| Lugar | Espera completa | Espera precisa | Sem espera (`GOW3_SCALAR_COPY_NOWAIT`) |
|---|---|---|---|
| Ruas (036, início) | ~45 | ~45 | ~46 |
| Porta do beco (save 4%, início) | ~54 | ~54 | — |

A espera custava no máximo ~1 FPS. Os ~53 da versão sem espera eram de outro ponto do mapa. Fica desligado; `medir_hle_preciso.bat` liga.

---

## 2026-10-15 — Cópia escalar em memória (HLE do SuperMedo), cabelo branco corrigido, R.8.4 e limite de quadros à frente: ruas ~42,5 → ~45 FPS, sem bugs visuais conhecidos

Itens trazidos do port [Supermedo/uncharted_pc](https://github.com/Supermedo/uncharted_pc) (GPL, mesma família bbport/shadPS4), com crédito ao SuperMedo. Todos atrás de interruptores.

**Cabelo branco do Drake jovem (corrigido, ligado por padrão):** o recompilador ignorava o `addr64` do `BUFFER_LOAD` (GCN2), e o shader do cabelo lia lixo ao procurar a cascata de sombra. Agora vira um `ReadConst` com deslocamento por lane, servido por uma janela de dwords que o walker SRT copia na hora do bind (`ReadConstWindowFlag`). Arquivos: `vector_memory.cpp`, `flatten_extended_userdata_pass.cpp`, `srt.h`, `emit_spirv_context_get_set.cpp`, `shader_info_collection_pass.cpp`. Depois de mudar o recompilador, mova o cache de shaders (`user/cache/CUSA02344`) para o lado.
- `GOW3_NO_ADDR64=1` traz o cabelo branco de volta (prova).
- `GOW3_WINDOW_DWORDS` (16..4096, padrão 256), `GOW3_WINDOW_ADDR64_ONLY=1` (janela só para leituras vindas do addr64) e `GOW3_WINDOW_TRACE=1` (lista os shaders com janela) servem para investigar. Nas ruas são 20 shaders × 13 leituras por lane, todas vindas do addr64.
- A alça da bolsa do Nathan piscou em vermelho duas vezes logo depois da correção. O Matheus não viu de novo em nenhuma build. Fica em observação.

**Cópia escalar em memória (`GOW3_SCALAR_COPY_HLE=1`, desligada por padrão):** os shaders de compute `2d215c3b` (1 dword por grupo) e `6a6422a9` (64 dwords por grupo) só copiam um bloco de constantes. Quando nenhum dos dois lados tem dados da GPU, a cópia é feita na memória do jogo pela visão de backing e o dispatch não roda. Isso tira o walker SRT da espera pelos argumentos de dispatch que o `2d215c3b` escrevia na GPU (R.7). Ao lado de dados da GPU, roda o dispatch na GPU (e também a cópia na memória, se a origem não veio da GPU e tem menos de 1 MiB). Cópias de 1 MiB ou mais sincronizam a imagem de depth antes (`GOW3_DEPTH_COPY_SYNC`, padrão ligado). Esses dispatches esvaziam o draw pipe e rodam na thread de comandos (`IsGuestMemoryShaderHLE`).
- Dois bugs na adaptação. (1) Nunca disparava: nosso `info.buffers` inclui o flatbuf, então contamos só os `!IsSpecial()`. (2) A textura da porta do beco (save 034) e as bordas do cabelo vinham erradas: a cópia passava na frente das cópias de host pendentes (uploads adiados para as threads de gravação e de cópia), e desenhos anteriores subiam as constantes novas. Agora espera `WaitHostCopies()` antes (`GOW3_SCALAR_COPY_NOWAIT=1` = comportamento antigo). Porta certa depois da correção.
- Ruas (036), mesma build e mesmo ponto: **~42,5 → ~45 FPS**. O walker leu 0 dwords de volta. Por intervalo: 200 mil cópias na memória, 3,9 mil nos dois, 71 mil só na GPU. O Matheus jogou com `medir_so_hle.bat` e não achou bugs visuais; os NPCs que piscavam ou perdiam a textura não apareceram mais.

**Proteção do depth associado (`GOW3_STENCIL_DEPTH_ONLY=1`, desligada):** `GetAssociatedDepth` agora recebe o formato da view e só associa depth a views de stencil de 8 bits (`eS8Uint`, `eR8Uint`, `eR8Unorm`). Ainda não testada a fundo.

**R.8.4 (desenho do Fable), com `GOW3_COMPUTE_PIPE=1` + `GOW3_LEAF_COPY=1`:**
- Descarga assíncrona das folhas: cada cópia de folha para o anel também copia os 4 bytes para um staging (`QueueLeafDownload`). No `OnSubmit`, as que terminaram e não foram reescritas descem para a memória do jogo (`CompleteLeafDownloads`). As descargas chegam (~31 mil por intervalo), mas as cópias na GPU não caíram (~530 mil).
- Saída do jogo: esperas de semáforo em fatias de 2 s com log "stuck", `Semaphore::SettleAll(2000)` antes de sair, e log dos sinais adiados presos. Todas as saídas foram limpas (6+ vezes e a sessão do Matheus).
- Ruas ~41,3 → **42,5 FPS**, museu ~50,5 → **57,6 FPS**. Sessão do Matheus (`jogar_r84.bat`): castelo 9 min, 39 FPS de média; Novo Jogo 40 min, 52,6 de média.

**Limite de quadros à frente (`GOW3_MAX_FRAMES_AHEAD=N`, desligado):** depois do submit+flip, o jogo espera (no máximo 2 s) a GPU chegar ao flip N quadros atrás. Mira o assert `m_gfxEopTick`. Custo nas ruas dentro do ruído (~39,5 com 1 ou 2, ~40 sem).

**Diagnóstico em `texture_cache.cpp`:** `GOW3_SMALL_STRICT` e `GOW3_RELOAD_AT=<s>` (testes negativos do cabelo branco).

---

## 2026-10-15 — R.8.3: compute sem espera antes de cada pacote (`GOW3_COMPUTE_PIPE=1`, desligado): ruas 28 → 41 FPS, museu 47 → 50,5

**Achado:** a espera "compute" do draw pipe (~58 mil por segundo nas ruas, 15-17% do tempo da thread de comandos) acontecia antes de todo pacote da fila de compute. Com o diagnóstico por opcode, quase todas vinham do `ReleaseMem`, um por dispatch: a fila de compute esperava a thread de gravação esvaziar a cada dispatch, e o pipe perdia a sobreposição.

**O que mudou (desenho do Fable, R.8.3), com `GOW3_COMPUTE_PIPE=1`:**
- `ProcessCompute` não espera mais no topo do laço. Como no laço de gráficos, só espera antes de pacotes que mexem no cache ou na memória na hora. Não esperam: Nop, SetShReg, SetQueueReg, AcquireMem, IndirectBuffer, Rewind, WaitRegMem (cede a vez), EventWrite, os dispatches (vão pelo pipe, ou esperam sozinhos) e o DmaData que só corrige as dimensões de um DispatchDirect seguinte (indirect_patches).
- `ReleaseMem` vai para a thread de gravação em ordem com os draws (`RunReleaseMem`, como o `EventWriteEop`): grava o rótulo pela visão de backing depois das cópias anteriores, sem esperar. Cópia de GDS continua síncrona nessa thread.
- O motivo de cada espera de compute agora é o opcode, nas estatísticas.

**Resultado (imagem correta nas duas cenas):** ruas (036) ~28,1 → **~41,3 FPS**: o pipe passa de ~93 mil para ~230 mil draws/s e as esperas de 58 mil para ~400 por segundo. Museu (035) ~47 → **~50,5 FPS**.

**Atenção:** depois do teste do museu, o processo do jogo não terminou ao fechar (a janela ficou "Não respondendo" e `taskkill /f` não conseguiu finalizar). Pode ser deste modo; fica desligado até uma sessão de jogo confirmar.

**Cópia das folhas (R.8.2):** o teste do fixup já era por byte (`gpu_modified_ranges`). Nas ruas, todas as folhas zeradas estão lá, nenhuma só numa janela assíncrona, e são sempre os mesmos endereços (0 novos por intervalo). Com a cópia ligada, os bytes nunca descem para a memória do jogo, então cada passada do walker copia de novo. Para resolver, os bytes precisam descer de forma assíncrona depois da cópia.

---

## 2026-10-15 — R.8.2 (B.3 + B.4): cópia das folhas na GPU funciona, mas não ganha; achado o travamento do anel de constantes (ruas 25 → 28 FPS)

**Anel de constantes travado (corrigido, vale para todos):** `ConstantRing::Stamp` saía cedo quando o fim do pacote era igual ao anterior. Com o anel cheio, todo pacote tem o mesmo fim, então nada era publicado, nada era liberado e o anel ficava cheio para sempre: nas ruas ~2,4 milhões de cópias por intervalo caíam no caminho antigo. Agora o `Stamp` publica o pendente também nesse caso. Ruas (036), sem a cópia das folhas: **~25,0 → ~28,1 FPS**, imagem correta; o anel quase não enche mais (33 mil vezes por intervalo, antes sempre). Museu (035): ~47 FPS no início do save; lá o anel nunca enchia, então a correção não muda nada (o 49,8 do R.8.1 foi outro momento).

**Cópia das folhas (`GOW3_LEAF_COPY=1`, desligada por padrão):**
- B.3: nas threads do lado da GPU, `SrtFixup` grava 0 no dword achatado de cada folha escrita pela GPU e guarda {dword, origem}. Folhas que caem num sharp (buffer, imagem, sampler, fmask) continuam com `ReadMemory` síncrono (`SrtReadDword`).
- B.4: o PostDraw anota, para cada folha, o deslocamento no anel; o estágio B copia os 4 bytes da origem para o anel na GPU (`RecordLeafCopies`, `CopyBuffer`) antes do draw, com barreira no `BindBuffers`.
- Resultado nas ruas: **leituras síncronas depois do walker 375 → 0** (meta do Fable), leituras síncronas do lado da GPU 670 → 230. Mas o FPS fica em ~25: são ~90 mil cópias de 4 bytes por segundo (quase uma por draw, porque a página inteira conta como escrita pela GPU) e o estágio B passa de 25% para 42% ocupado. Fica desligada até as cópias serem agrupadas.
- Shaders com folhas mas sem binding do flatbuf (ex. 713b51cd, flat de 20 dw sem recursos): `GOW3_LEAF_DEAD=1` descarta as folhas; sem ele, leem de volta.
- Diagnóstico: contadores "leaf copy" e "constant ring" na linha de estatísticas; `GOW3_SRT_TRACE=1` mostra shaders com folhas e sem anel.

---

## 2026-10-15 — R.8.1 (B.0 + B.1' + B.2'): o walker SRT não falha mais nas páginas da GPU

O walker JIT agora recebe um contexto (terceiro argumento). Cada ponteiro de tabela passa por uma função em C (`SrtLoadPtr`, page_manager.cpp) que lê o ponteiro, baixa os 8 bytes se vieram da GPU e devolve a visão sem proteção (backing) da tabela; as folhas leem dali e gravam o endereço de origem de cada dword numa tabela. Depois da passada, `SrtFixup` relê só os dwords cuja página tem proteção de leitura e cujos bytes a GPU escreveu (`ReadMemory` de 4 bytes). Versão do cache de shaders 9→10 (o código do walker mudou).

- Só nas threads do lado da GPU: nas outras threads as tabelas seguem pela memória do jogo (com o backing nelas o jogo travava no menu com o assert `m_gfxEopTick`).
- O filtro é a página com observador de leitura; checar `IsGpuWritten` por página em cada passada custava caro (museu 41 FPS).
- Ruas (036): ~25,0 FPS, imagem correta; 375 passadas/s, cada uma com exatamente 1 dword escrito pela GPU. Museu (035): ~49,8 FPS (48 com o caminho do R.7 na mesma cena).
- Sem ganho de FPS ainda: cada dword lido de volta é um `ReadMemory` síncrono. Próximo: B.3 + B.4 do Fable (mascarar as comparações e copiar os 4 bytes na GPU).
- Diagnóstico: `GOW3_SRT_MODE` (bits: 1 ponteiros, 2 tabelas, 4 fixup, 8 todas as threads; padrão 7), `GOW3_SRT_TRACE=1`.

---

## 2026-10-15 — Teste A do R.8: os valores escritos pela GPU que o walker achata mudam a imagem

`GOW3_SRT_STALE=1` (experimento, desligado) deixa o walker ler o valor antigo da memória do jogo quando os 4 bytes vieram da GPU, sem esperar. Nas ruas, a iluminação e as sombras somem: a cena fica chapada, com manchas pretas nas bordas. O FPS cai para ~16,5, porque a página nunca é baixada e cada passada do walker volta a falhar (~120 mil exceções por segundo). Conclusão: esses dwords decidem a imagem. Precisa da cópia na GPU (R.8 B).

---

## 2026-10-15 — SRT walker lê pela cópia sem proteção quando os bytes não vieram da GPU (desenho do Fable, R.7)

**O que mudou:** quando o SRT walker (o código JIT que copia as tabelas de descritores do jogo) falha numa página protegida, o tratador de falhas (`page_manager.cpp`, `EmulateSrtWalkerLoad`) decodifica a instrução. Se os bytes lidos não estão em `gpu_modified_ranges`, nem num readback assíncrono pendente, nem num readback de imagem pendente, ele lê o valor pela visão de backing, põe no registrador (`rdi` ou `r10d`) e pula a instrução. A página continua protegida para o jogo. `GOW3_SRT_BACKING=0` desliga.

**Resultado:** ligado por padrão, imagem correta.
- Ruas: ~25,9 → ~26,2 FPS. Cerca de 40% das leituras do walker (≈52/s) agora saem pela cópia.
- Museu: ~57 FPS (antes ~57,7, dentro do ruído). Lá ~650/s das leituras saem pela cópia.

**Por que o ganho foi pequeno:** com `GOW3_FAULT_TRACE=1`, as leituras que ainda esperam (~80/s nas ruas, ~105/s no museu) pegam bytes que **a GPU escreveu de verdade**. Por exemplo, 0x103f25c4c0 (12 bytes) são os argumentos de dispatch que o shader `2d215c3b` acabou de escrever. O walker achata esses valores nos dados do shader seguinte, então precisa deles na hora da gravação. Não é compartilhamento falso de página. Isso continua com o Fable.

---

## 2026-10-15 — Readback assíncrono (A2): museu 41 → 57,7 FPS, ruas 22,5 → 25,9 (desligado por padrão)

**O que mudou:** com `GOW3_READBACK_ASYNC=1`, uma falha de página em memória escrita pela GPU (leitura ou escrita do jogo) não esvazia mais a GPU a partir da thread da GPU. Antes, a thread da GPU gravava a cópia, chamava `scheduler.Finish()` e ficava parada até a GPU terminar tudo; a GPU depois ficava ociosa enquanto essa thread voltava a gravar trabalho.
- Agora a thread da GPU grava a cópia num staging reservado (`staging_pool.Request(..., deferred=true)`), submete com `Flush()` e volta ao trabalho.
- A thread do jogo que falhou espera o tick no semáforo da timeline, escreve os dados de volta na memória e só então tira a proteção da página (`CompleteAsyncReadback`).
- Se a GPU ligar uma dessas janelas para escrita nesse meio tempo (`NoteAsyncWrite`), a proteção fica e o acesso repetido falha de novo para pegar os dados novos.
- Uma falha da thread da GPU numa janela ainda pendente espera a pendente terminar (`WaitAsyncReadbacks`). O staging é liberado na thread da GPU.
- Continua síncrono: falhas das threads do lado da GPU (processador de comandos, gravação, captura de shader), `GOW3_READBACK_ALL`, unmaps.

**Resultados (FPS médio estável):**

| Cena | Antes (deca501) | Só leituras | Leituras + escritas |
|---|---|---|---|
| Ruas (save 036) | ~22,9 | ~23,4 | ~25,9 |
| Museu (save 035) | ~41 | — | ~57,7 |

Imagem correta nas duas cenas, sem assert. Novo contador no log: `uc3 readbacks: N async, ms esperados, deixados protegidos, esperas de sobreposição; sync: lado da GPU, escritas, outros`.

**O que ainda para a GPU nas ruas:** ~120 por segundo de readbacks síncronos (`DownloadMemory` + `Finish`, ~24% do tempo da thread da GPU). Diagnóstico com `GOW3_FAULT_TRACE=1` (novo) e `GOW3_WRITE_TRACE=1` (novo):
- Quem falha é o SRT walker (o código JIT de `flatten_extended_userdata_pass.cpp` que copia as tabelas de descritores do jogo), rodando no processador de comandos e na captura de shaders.
- A página que ele lê (0x103f25c000) é a mesma onde o shader de compute `2d215c3b` escreve 12 bytes (argumentos de dispatch indireto). Os descritores não são escritos pela GPU; é compartilhamento falso de página de 4 KiB.
- Próximo passo (para o Fable desenhar): ler os descritores sem passar pela proteção quando os bytes lidos não estão em `gpu_modified_ranges`.

Outros: `probe.c` imprime `Image base` no início; `.bat` novos `medir_async.bat` e `medir_async_diag.bat`.

---

## 2026-10-15 — Barreira falsa nos buffers formatados: ruas 16,5 → 22,5 FPS, museu 38,5 → 41

**O que estava acontecendo:** o perfil de GPU apontou o shader de compute `8f067540`, despachado ~1.700 vezes por quadro nas ruas com grupos 1x1x1, gastando ~22 ms por quadro. Quase todo dispatch levava uma barreira de memória: ~1.570 barreiras por quadro.

**Por quê:** com `GOW3_BARRIER_TRACE=8f067540` (novo, imprime as 300 primeiras ligações de buffer do shader) apareceu o padrão:
- Cada dispatch liga o **mesmo array de structs três vezes**, nos campos de deslocamento 0, 0xC e 0x14 (stride 28), como buffers formatados graváveis.
- O rastreador de barreiras registrava cada ligação com o tamanho cheio do V# (`stride * registros`). Para o campo em 0x14, a faixa passava 0x14 bytes do fim do array e caía no array do dispatch seguinte, que começa logo depois.
- Resultado: uma dependência escrita→escrita que não existe, e uma barreira por dispatch.

O toggle 2048 (leituras arredondadas a 4 KiB), suspeito do Fable, não mudava nada; o toggle 512 também não.

**Correção** (`vk_rasterizer.cpp`, ligações de buffer): um buffer formatado com stride acessa um elemento por registro. Por isso o último byte tocado é `(registros - 1) * stride + tamanho do elemento`, e só essa faixa vai para o rastreador de barreiras (`IsBufferAccessed`/`bound_buffers`). O descritor Vulkan e o `ObtainBuffer` continuam com o tamanho cheio. `GOW3_TIGHT_FORMATTED=0` volta ao comportamento antigo.

| | Antes | Depois |
|---|---|---|
| Barreiras por 5 s nas ruas | ~117.000 | ~12.000 |
| `8f067540` por quadro | ~22 ms | ~1,8 ms |
| Ruas, FPS (sem perfil) | ~16,5 | **~22,5** |
| Museu, FPS | ~38,5 | **~41** |

A imagem ficou igual nos dois lugares. Nas ruas, o que sobra do quadro de ~45 ms é sobretudo a GPU parada esperando a CPU (bolha das falhas de readback). O próximo passo é a proposta A2 do Fable.

Logs: `out/barrier-trace-ruas.log`, `out/tight-ruas.log`, `out/tight-fps-ruas.log`, `out/tight-fps-museu.log`.

## 2026-10-15 — Perfil de GPU nas ruas: metade do quadro é GPU ociosa e um shader de compute leva 22 ms

**Correções no `GOW3_GPU_PROFILE`:**
- Sem o upscaler ele nunca fechava quadros, porque `NoteFrameStart` só rodava com o camera motion. Agora ele também fecha no passe de display quando o perfil está ligado.
- A leitura das queries usava `eWait`, que segurava a thread da GPU e fazia a tela de título cair na assertiva `m_gfxEopTick` (duas vezes seguidas). Agora o quadro sem resultado é descartado.
- O `mark()` do dispatch passou para antes de `FlushBarriers`, como o Fable pediu, para a barreira contar no próprio dispatch.

**Contadores novos** em "uc3 draw stats": fins de render pass, barreiras de memória (e quantas com estágios gráficos no srcStage), transições de layout (e quantas em imagens 1080p+).

**Ruas (save 036), por quadro do jogo, ~66 ms.** O perfil fecha 2 "quadros" por quadro do jogo: o passe de display aparece duas vezes.

| O quê | ms | quantidade |
|---|---|---|
| GPU ociosa entre submissões | ~35 | ~15 vkQueueSubmit |
| `dispatch cs 8f067540` (1x1x1) | ~22,5 | ~1.700 dispatches |
| todos os render passes | ~3 | |
| resto | ~5 | |

Barreiras: ~1.570 por quadro, só ~41 com estágios gráficos (é serialização compute→compute). Fins de pass: ~51. Transições: ~100, das quais ~45 em imagens 1080p+.

**Leitura:** o limite não é o trabalho de render. É a GPU parada esperando a CPU (a bolha da falha de readback, proposta A2 do Fable) e um único shader minúsculo despachado ~1.700 vezes com barreira. Resultados enviados ao Fable (`/mnt/project-files/uc3/resultados-readback-opus.md`). Log: `out/gpu-ruas.log`. Atalho: `medir_gpu.bat`.

## 2026-10-14 — Cópia lateral do readback (`GOW3_READBACK_FAST=1`, desligada): funciona, mas não ganha FPS

Seguindo a investigação do Fable (`/mnt/project-files/uc3/investigacao-fable.md`, seção 1.4).

**Passo 0, diagnóstico** (sempre ligado, na linha "uc3 draw stats"): o cache guarda o tick do
scheduler da última escrita da GPU em cada janela de 512 KiB (`NoteGpuWrite`, chamado em
`ObtainBuffer`), e cada read fault é classificado (`ClassifyFault`). `GOW3_READBACK_TRACE=1`
imprime as 200 primeiras falhas. Nas ruas, por 5 s: ~190 com a escrita no command buffer
aberto, **~710 com a escrita já concluída na GPU**, 0 em voo. Zero "em voo" porque cada falha
anterior já esvaziou a fila com `Finish()`.

**Passo A, versão simples** (`GOW3_READBACK_FAST=1`, desligado por padrão): quando a escrita
da janela já terminou na GPU, a cópia vai num command buffer próprio na mesma fila gráfica
(`Scheduler::SubmitSide`, sob o `submit_mutex`), com uma barreira global antes. Ela roda logo
depois do trabalho já submetido e não submete nem espera o command buffer que está sendo
gravado. Não usei uma segunda fila: dispensa mudar o modo de compartilhamento das arenas.
O tick anotado é o de quando o buffer foi ligado; o desenho pode cair no command buffer
seguinte se houver um submit no meio, então a cópia lateral exige que o tick **seguinte**
também tenha terminado. O detalhe que o Fable apontou no `GOW3_READBACK_HOT` foi
corrigido (limitar a janela à arena antes de consultar `IsRegionGpuModified`).

| Lugar | Base | HOT | FAST | FAST + HOT |
|---|---|---|---|---|
| Ruas | ~16,5 | ~17,5 | ~17,1 | — |
| Museu | ~38,5 | ~42,8 | ~40,0 | ~43,2 |

FAST leva ~57% das falhas pelo caminho lateral (0,2 ms cada nas ruas, 0,6 ms no museu), sem
defeito visível, mas o tempo nos tratadores de falha por quadro lento quase não muda
(ruas ~29 ms). **Conclusão:** as falhas baratas já eram baratas, porque a falha anterior
tinha acabado de esvaziar a fila. O custo está na primeira falha de cada rajada, que lê dados
do command buffer aberto e tem de esperar a GPU de qualquer forma. Para isso só servem a
fence EOP honesta (passo B do Fable) ou menos trabalho de GPU.

**As ruas são limitadas pela GPU**, não pelo readback: ~65 ms por quadro, 1.100 desenhos e
1.300 a 3.100 dispatches por quadro, GPU ticks ~52% do tempo. É o próximo alvo: descobrir
qual trabalho da GPU custa tanto ali (captura F11 com tempos de GPU).

Logs: `out/diag-ruas.log`, `out/fast-ruas.log`, `out/fast-museu.log`, `out/fasthot-museu.log`.
Atalhos: `medir_rapido.bat` e `medir_rapido_hot.bat` em `D:\Uncharted`. Os saves 035 e 036
foram restaurados de novo depois dos testes.

## 2026-10-14 — Readback antecipado (`GOW3_READBACK_HOT=1`, desligado por padrão): museu +11%, ruas +6%

No modo preciso, cada leitura da CPU numa página escrita pela GPU baixa uma janela de 512 KB e
espera a GPU (`scheduler.Finish()`). Esperar só o tick da última escrita não ajuda: `Finish()`
já espera só o tick atual e a fila roda em ordem. O que dá para cortar é o **número** de esperas.

**O que mudou** (`buffer_cache.cpp`/`.h`, `gow3_toggles.h`, `driver.cpp`):
- O cache guarda as últimas 32 janelas que sofreram read fault, com o quadro em que isso
  aconteceu (`NoteHotWindow`, `frame_index` sobe em `TickFrame`).
- Com `GOW3_READBACK_HOT=1` (só no modo preciso), um read fault baixa a janela pedida **e** as
  janelas quentes dos últimos 4 quadros que ainda estão modificadas pela GPU, numa única cópia
  com um único `Finish()` (`DownloadWindows`, no molde de `DownloadAllGpuModified`).
- "uc3 draw stats" mostra `N with hot windows (+M windows)`.
- `medir_hot.bat` em `D:\Uncharted` roda com `GOW3_FRAME_STATS=1,GOW3_READBACK_HOT=1`.

**Medições** (janela, vsync ligado, parado no ponto do save, ~50 s, logs em `out/fps-*-{museu,ruas}.log`):

| Lugar (save) | FPS base | FPS HOT | read faults/quadro lento | tempo neles |
|---|---|---|---|---|
| Museu (2%, slot 035) | ~38,5 | ~42,8 | 10,4 → 3,3 | 32 → 24 ms |
| Ruas (5%, slot 036) | ~16,5 | ~17,5 | 6,4 → 3,0 | 30 → 28 ms |

Imagem sem defeitos visíveis nos dois lugares. Nas ruas o número de faults cai pela metade,
mas o tempo quase não muda: cada espera ainda pega o quadro inteiro da GPU, que lá é o
gargalo (GPU ticks ~52% do tempo, ~16 FPS). As ruas são o ponto mais lento medido até agora.

**Armadilha:** carregar um save e continuar faz o jogo gravar o checkpoint de volta no mesmo
slot. Os testes reescreveram 035 e 036; os originais foram restaurados a partir de
`D:\antigo\uc3_pc`, e as versões gravadas no teste ficaram em `out/saves-apos-teste/`.

**Próximo:** deixar desligado por padrão até uma sessão longa jogando com ele ligado. O ganho
grande nas ruas não está no readback, está no custo da GPU por quadro.

## 2026-10-14 — Máquina nova (D:\Uncharted); janela ganha 2–3 FPS sobre tela cheia

Repositório restaurado do pacote de continuidade (bundle git, `e1fb21d`) em `D:\Uncharted\uc3_pc`;
jogo em `D:\Uncharted\CUSA02344` (1.00) e `CUSA02344-UPDATE` (1.02), sem as junctions
`app0`/`patch` (o `tools/run_uc3.ps1` cai nessas pastas quando `app0` não existe). Saves copiados da
instalação antiga (`D:\antigo\uc3_pc\user\savedata`). Compilação, testes (memory, fiber,
readback-queue, page-state) e `--check-imports` (0 faltando) ok na RX 6750 XT.

Medição do jogador, mesma cena: **tela cheia 21–22 FPS, janela 24–27 FPS**. No port, "Fullscreen"
é exclusivo no modo da área de trabalho (`WindowSDL::ApplyDisplayMode`); "Windowed" cria a janela
com o tamanho da imagem do jogo (1920x1080). O monitor do jogador também é 1920x1080, então o tamanho
do swapchain é o mesmo nos dois modos: a diferença não vem de resolução. Ainda sem causa medida
(candidatos: apresentação exclusiva/FIFO do driver AMD versus composição do DWM).

Medição controlada (mesmo ponto parado do checkpoint `SAVEDATA038`, `GOW3_FRAME_STATS=1`, ~50 s
cada; logs `out/fps-*-castelo.log`):

| Modo | FPS | Mediana do quadro |
|---|---|---|
| Janela, VSync on | 32–33 | 33,2 ms |
| Tela cheia exclusiva, VSync on | 35–37 | 33,0 ms |
| Janela, VSync off (Immediate) | 32–33 | 33,2 ms |

Aqui a tela cheia **não** é mais lenta, e desligar o VSync não muda nada: o quadro fica em ~33 ms
porque ~55 % do tempo é espera pelos *ticks* da GPU (linhas `Stall:`), não apresentação. A diferença
que o jogador viu (21–22 vs. 24–27) foi em outra cena/momento; não reproduz parado. Nos quadros
lentos aparecem também falhas de leitura do readback preciso (ex.: 7 falhas = 20 ms num quadro de
50 ms), então o próximo passo de desempenho é o readback. `gow3.ini` ficou
com `display_mode=windowed`.

---

## 2026-10-13 — O Castelo (27%): port, shadPS4 0.19.0 e nightly no mesmo nível; readback preciso custa pouco aqui (`ad17165`)

Checkpoint `SAVEDATA038` ("O Castelo", combate), mesma cena nas quatro execuções:

| Execução | FPS |
|---|---|
| Port, nightly `23c4133`, modo **preciso** | 18–20 (jogador: 22–24 na 0.19.0) |
| Port, modo **relaxado** (`GOW3_READBACKS=1`, cenário/texturas somem) | 20–24 |
| shadPS4 nightly (executável 1.02 direto) | ~20 (jogador: "quase o mesmo do port") |
| shadPS4 0.19.0 | 20 no print, ~18 de média segundo o jogador |

Conclusões: (1) nessa região o próprio shadPS4 roda a 18–20 FPS, então o limite é a emulação
da cena (espera pela GPU de ~30 ms por quadro com a GPU a 68 %: muitas submissões e
sincronizações, 145 por quadro), não o port; (2) o readback preciso custa só 2–4 FPS aqui, ao
contrário do museu/ruas, onde as falhas de leitura dominavam. O custo do readback varia por
cena: vale medir sempre nos dois modos antes de atacar.

Lateral: a nightly do shadPS4 não abre o jogo pelo `eboot.bin` (a normalização nova de caminhos,
#5332, apaga o `..` de `/app0/../CUSA02344-UPDATE/`); contorno:
`shadPS4.exe -g <jogo>\big3-ps4_Shipping.elf -- eboot.bin ispartofuccollection` (o overlay de
update resolve o ELF 1.02). O log do port passa a dizer o modo de readback na inicialização.

---

## 2026-10-13 — Nightly `23c4133` do shadPS4 fundida; crash da captura F11; sessão de 2 h na 0.19.0 (`e90e5fd`)

### Sessão do jogador na 0.19.0 (`c8b0bf7`)
1h55 de jogo sem crash, do museu até os templos (áreas nunca rodadas antes). FPS por janela de
5 s: mediana 32, mínimo 13. Nos 7099 quadros lentos: 229 s esperando a GPU, **166 s em tratadores
de falha de leitura** (56 mil falhas, ~3 ms cada: cada uma baixa a janela da GPU e espera a fila
inteira) e 103 s em falhas de escrita. Alguns glitches visuais (16 capturas F11 em
`user/captures/frame_20261013-19*`), a investigar. Log em `out/player-0.19.0-session.log`.

### Crash depois do F11 (`c8b0bf7`)
O processo sumia poucos segundos após a captura, sem "Fault", sem assertiva e sem registro do
Windows. Causa provável: a captura lia os buffers **pelo ponteiro do jogo**; no modo preciso uma
página escrita pela GPU está protegida contra leitura e a falha era tratada no próprio thread de
*binding*, de onde ela pode recursar até estourar a pilha (um estouro mata o processo antes de o
tratador escrever qualquer coisa). Correções: a captura copia pela cópia de apoio
(`CopySparseMemory`); `SetThreadStackGuarantee` em todos os threads, para que um estouro vire
"Fault: exception 0xc00000fd" no log; o fechamento da janela passa a escrever
"Window closed (SDL quit event)" (era `LOG_INFO`, oculto, e parecia crash). Captura por arquivo
(`GOW3_CAPTURE_TRIGGER`) testada nas ruas: processo sobrevive. `tools/uc3_sendkey.ps1` ganhou
`-Hold` (teclado do port: Espaço = X, Enter = Options) e `-Post`.

### Nightly 23c4133 (12/10): 71 commits depois da 0.19.0, 40 no subconjunto vendorado
Mesmo método (ramos `upstream2`/`port2` em `D:/Uncharted/ref/merge`), 18 arquivos em conflito.
- **Cache de texturas** (`texture_cache.*`, `image.*`, `sampler.*`, `common/lru_cache.h`): fica a
  versão do port. A reestruturação #5219 (tabela de páginas, LRU intrusiva, samplers em slot
  vector, mutex por imagem) colide com o aliasing, a fila de *readbacks* e os caches de
  `FindImage`/`UpdateImage` do port; a mesclagem automática produzia uma mistura incoerente.
  Transplantados: metadados CMask/FMask/HTile com dono e contagem de amostras (#5046, inclusive o
  preenchimento do CMask no `buffer_cache`), `AssociateStencil` também na cópia de profundidade
  (#5296), `RefreshImage` das subresources novas no overlap de profundidade (#5229).
- **Levados do upstream:** index buffer limitado à capacidade do `IndexBufferSize` e buffer nulo
  quando nada está mapeado (#5278/#5327, `bindIndexBuffer2KHR` com maintenance5/6), `bound_buffers`
  com o tipo de acesso (#5262), `DrawIndirectCountMulti` (#5341), `is_indirect` na runtime info
  (#5348), `depth_before_shader`, `NormalizeMinMaxBlend` por cima do *blend rewrite* do port,
  `SurfaceFormat` constexpr no header (o `TrySurfaceFormat` do port continua), `CompMapping` do
  AmdGpu nas views, subresources em `u16`, `OpNClamp` acrescentado ao sirit (#5346).
- **Bug encontrado no caminho:** o upstream subiu `NUM_BUFFERS` de 40 para 56 e os índices
  `motion_param`/`scene_size` que o port acrescenta ao bloco de *push constants* passaram a
  apontar além dos membros declarados. SPIR-V inválido (`spirv-val` acusa na hora), e o compilador
  do driver AMD **cai** em `createComputePipeline` em vez de recusar: crash na inicialização,
  "Fault: write access ... amdvlk64.dll". Agora o bloco declara quatro membros de offsets (56
  bytes), `motion_param` em 72 e `scene_size` em 76, com `static_assert` dos dois lados.
  `GOW3_DUMP_SHADERS=1` + `spirv-val` em todos os dumps: 725 shaders válidos.
- `ExtendWriteFault` reescrito sobre o region manager novo (#5277: `UpdateProtection`/`UpdateState`).

### Resultado
Templo (checkpoint do jogador, cena de combate): nightly 18–20 FPS contra 22–24 FPS dos últimos
minutos da sessão na 0.19.0 (condições não idênticas: o probe fica parado levando tiro). A
decomposição dos quadros lentos é a mesma nas duas: ~30 ms de espera pela GPU e ~25 ms em falhas
de leitura (2200/min, 3,7 ms cada). **A nightly não muda o gargalo**, que é do port.

### Próximo alvo: o custo das falhas de leitura do modo preciso
Cada falha de leitura (`Rasterizer::ReadMemory` → `DownloadMemory` → `scheduler.Finish()`) drena
a fila da GPU inteira, ~3 a 4 ms, e há dezenas por quadro nas cenas pesadas. Caminhos: esperar só
o *tick* da última escrita naquela janela em vez de `Finish()`; baixar na EOP as faixas que a CPU
leu no quadro anterior (previsão); ou marcar como "lidas pela CPU" e baixar em lote por quadro.

---

## 2026-10-13 — Núcleo gráfico atualizado para o shadPS4 v.0.19.0 (`7628587`)

### O que foi feito
O `gpu/shadps4` saiu do commit `b4e7ae7` (24/09) para a tag **v.0.19.0** (`c7e065d1`, 02/10): 79
commits do upstream, 96 arquivos do subconjunto vendorado alterados (+2703/−2743). O método foi
uma mesclagem de três vias com o git, num repositório de trabalho à parte
(`D:/Uncharted/ref/merge`: ramos `base` = subconjunto em b4e7ae7, `upstream` = v.0.19.0, `port` =
a árvore vendorada; quebras de linha normalizadas). O clone de consulta fica em
`D:/Uncharted/ref/shadPS4` (worktrees `shadPS4-b4e7ae7` e `shadPS4-v0.19.0`). O port muda 81
arquivos do núcleo (~11,5 mil linhas) e acrescenta 45; só 25 arquivos entraram em conflito, e
quase sempre "o port reescreveu a região e o upstream mexeu ao lado".

### Decisões por área
- **Rastreador de memória** (`region_manager.h`, `memory_tracker.h`, `page_manager.*`): versão
  **inteira do upstream** (reescrita #5100: bitsets por palavra, mutex em vez de spin lock,
  tabela de páginas em dois níveis que exige `PageManager::OnGpuMap` para cada mapeamento, já
  chamado em `Rasterizer::MapMemory`), com os acréscimos do port reaplicados: janela de falha
  (`ExtendWriteFault`), checagem sem lock dos *bindings* só de leitura, vigias de leitura dos
  *readbacks* de imagem (`UpdatePageWatchers(addr, size, write_op, read_op)`), `PageState::Perms`
  negando tudo enquanto há vigia de leitura (`page_state.h` mantido, com o teste), o tratador de
  falhas com `IsGpuSideThread`/`OnWriteFault`/estatísticas e a sonda de 8 bytes limitada à página
  (#5150). **Removido:** `GOW3_HOT_PAGES` (opt-in, medido como prejudicial); `tracker_faults`
  passa a contar falhas, não páginas.
- **Dados de usuário dos shaders:** o upstream (#5181) lê os registradores do *flatbuf* em vez de
  *push constants*; adotado. `PushData` perde `ud_regs` e fica com `motion_param` em 56 e
  `scene_size` em 60 (64 bytes). Caches de shader/pipeline invalidados (versões 8/8/6).
- **Fetch shader:** o decodificador novo do upstream (por palavra, sem o `GcnDecodeContext`)
  dentro do embrulho do port (*snapshot* do compilador assíncrono e cache por thread);
  `ParseFetchShader` tem as duas formas (bool + `optional`), e `Gcn::ToOptional` converte o
  `FetchShaderData` por valor do upstream (`Empty()`) para o `optional` que a seleção de pipeline
  do port usa. O campo `semantic` virou o índice do atributo.
- **Scheduler:** as "sessões" com *command buffer* de upload do upstream **não** entraram (ninguém
  as usa na 0.19.0 e conflitam com a gravação em thread do port).
- **Barreiras (`vk_runtime`):** listas de intervalos por buffer do upstream (o `BarrierTracker`
  foi removido lá), com o memo e o arredondamento das leituras a 4 KiB do port por cima.
- **Rasterizer:** `OnFence()` = `ProcessDownloadImages()` do port (drena o *draw pipe* antes);
  `BufferCache::TickFrame` passa a ser dono do `fault_process_pending`; rejeição de S# inválidos
  (#5137) dentro do `BindSamplers` do port; `GetDrawOffsets` continua com `optional`.
- Demais 70 arquivos: versão do port com as correções pequenas do upstream aplicadas pelo git.

### Shim e build
- `shim/common/logging/log.h` apagado: vale o `log.h` do upstream (classes como enum, `VLog`);
  `shim/common/logging/log.cpp` implementa `g_class_levels` + `VLog` com a saída em stderr de
  sempre (`GOW3_GPU_LOG`). `common/logging/classes.cpp` e `common/slot_vector.cpp` entram no
  CMake. `EmulatorSettingsImpl::IsInlineFetchShader()` = false. `gpu/VENDOR.txt` atualizado.
- Cadeias de `#include` que vinham do `log.h` antigo (`<algorithm>`, `path_util.h`) explicitadas.

### Testes
31 alvos passam (`memory-test`, `videodec2-test`, `page-state-test`, `pipeline-binary-cache-test`,
`async-fetch-snapshot-test`, `aliasing-test`, …). Ajustados: `test_scene_resolution.cpp`
(`SlotVector::Insert`/`IsAllocated`, capacidade no construtor), `test_async_fetch_snapshot.cpp`
(stubs do log novo), `test_gpu_runtime_stubs.cpp` (`sigjmp_buf` no Windows, stub de
`runtime_memory_region_backing`). Não contam: `upscaler-support-test` (`setenv`, só Linux),
`cheat-hooks-test` (offsets do GoW3) e `scene-resolution`/`taa-shader`/`camera-motion`
(chamada ao endereço 0 **também no commit anterior**, verificado numa worktree em
`D:/Uncharted/ref/uc3-base`: testes feitos para Lavapipe, nunca rodaram no Windows).

### Resultado no jogo
Probe no modo preciso (`GOW3_READBACKS=2`, padrão do `run_uc3.ps1`), `GOW3_FRAME_STATS=1`, sem
cache de shaders: título e menu a 60 FPS, "Continuar" carrega o museu de Cartagena corretamente
(`out/update-4.png`: cenário completo, iluminação igual à de antes). No museu, parado no
checkpoint: **32–48 FPS, mediana ~40** (antes da atualização: 32–35; shadPS4 nightly: 41), pior
quadro 34–50 ms, sem crash, sem assertiva, sem `Stall` por *readback* (0 ms em 74 quadros lentos;
o tempo vai para espera da GPU e para os tratadores de falha de leitura/escrita). O cabelo claro
e os pontos de poeira continuam iguais (defeitos do núcleo também na 0.19.0, ver entrada
anterior). Falta o teste do jogador: sessão longa, mudança de área e um "Novo jogo" (as texturas
ao recarregar) com o núcleo novo.

### Próximos passos
1. Nightly `23c4133` (71 commits depois da 0.19.0: otimizações do cache de texturas #5219,
   rastreio de vertex/index/indirect #5262, `TryWriteBacking` #5316): mesmo método, a partir do
   repositório de mesclagem.
2. Desempenho do modo preciso e os defeitos do núcleo (cabelo, poeira) continuam em aberto.

---

## 2026-10-13 — Cabelo branco e pontos de poeira: defeitos do núcleo (iguais no shadPS4 0.19.0)

### Sintomas (museu de Cartagena, jovem Drake, capítulo 2)
- Cabelo do Drake **branco/brilhante** ao carregar do checkpoint ("Continuar"); num "Novo jogo"
  fica normal. Permanente enquanto a cena dura.
- **Pontos amarelos** no ar em toda a cena: as partículas de poeira, que no jogo original são
  quase invisíveis (captura de um gameplay nativo enviada pelo usuário).

### O que foi descartado no port
- Interruptores: com **todas** as otimizações do GoW3 desligadas ao vivo (`GOW3_TOGGLE_FILE`,
  máscara **decimal** 2251796593508351 = 0x7ffff400fffff; o arquivo é lido com `%llu`, hex vira 0
  e a primeira tentativa foi inválida por isso), cabelo e pontos continuam iguais.
- Formatos: nenhuma textura amostrada no quadro usa formato sem suporte na GPU (captura F11).
- *Hash* das imagens pequenas (`MaybeCpuDirty`): corrigido para a imagem inteira (`43de974`),
  sem efeito no cabelo.
- `GOW3_READBACK_ALL`: desligado (ver entrada anterior), sem efeito nestes dois.

### Teste decisivo: shadPS4 0.19.0 no mesmo checkpoint
Os saves do port foram copiados para o shadPS4 (`tools/uc3_saves_to_shadps4.py`: gera o
`param.sfo` de cada pasta a partir de um modelo, com o nome da pasta e os textos do `param.bin`).
"Continuar" lá carrega o museu, e a imagem é **idêntica**: cabelo branco e pontos amarelos
(`out/shad-hair.png` vs `out/hair-base3.png`). O shadPS4 cai na assertiva `m_gfxEopTick` na
primeira tentativa (compila shaders na thread da GPU) e passa na segunda, com o cache quente.
→ Os dois defeitos são do núcleo gráfico (commit base `b4e7ae7`, mesmo comportamento na
0.19.0). Hipótese para os dois: superfícies com alfa (fios de cabelo e partículas suaves) saindo
opacas e claras. Fica para quando trouxermos commits posteriores do shadPS4; não é prioridade
do port.

### shadPS4 nightly de 2026-10-12 (`23c4133`): o mesmo
Baixado em `tools/shadps4-nightly` (config e saves copiados; `--template` no script de saves para
uma instalação nova). Precisa de 3 execuções até o cache de shaders cobrir o carregamento (cai na
assertiva nas duas primeiras). Resultado: **cabelo branco e pontos iguais** (`out/nightly-hair.png`),
a 41 FPS na mesma cena em que o port faz 32–35. Ou seja, ainda não está corrigido no shadPS4 em
12/10, e o núcleo puro está mais rápido que o nosso modo preciso ali: os commits de buffer cache /
memory tracker / page manager posteriores ao `b4e7ae7` valem também para desempenho.

### Ferramentas desta etapa
- `tools/uc3_screenshot.ps1`: captura a janela do jogo (ou `-ProcessName shadPS4`) em PNG, trazendo
  a janela para a frente antes; permite ver o resultado sem o usuário.
- `tools/uc3_sendkey.ps1`: manda teclas a uma janela (shadPS4: `n` = X, `enter` = Options, setas).
- `tools/uc3_saves_to_shadps4.py`: saves do port → shadPS4 (backup automático dos do shadPS4).
- O *probe* carrega o checkpoint do jogador direto ("Continuar" é o primeiro item do menu).

### Decisão: atualizar o núcleo gráfico (opção 1)
Plano: (1) clonar o shadPS4 em `ref/shadPS4`; (2) diferença entre o `gpu/shadps4` vendorizado
(subconjunto `common`, `core`, `shader_recompiler`, `video_core`: 372 arquivos, 88 com marcas
`gow3`, 383 linhas marcadas, mais edições sem marca e as `uc3`) e o `b4e7ae7` → `port.patch`;
(3) alvo `v.0.19.0` primeiro, nightly depois; (4) copiar o subconjunto do alvo e reaplicar o
`port.patch` em três vias, adaptando `gpu/shim` às APIs; (5) build, testes, *probe* com capturas,
comparar FPS com o nightly (41 FPS no checkpoint do museu).

### Próximos passos (do port)
1. Desempenho do modo preciso (~30 FPS nas cenas pesadas; ~170 submissões por quadro lento).
2. Piscadas em preto: não reapareceram; rastreio (`GOW3_FLASH_TRACE=1`) pronto se voltarem.
3. Estabilidade: desde `6681ee4` sem crashes relatados.

---

## 2026-10-13 — Texturas quebradas ao recarregar: culpa do *readback* agrupado (desligado) (`afb0bfa`)

### Experimento do usuário
Com a versão `6681ee4` (modo preciso, *readback* agrupado ligado): **Continuar** do checkpoint →
quase tudo certo (só o cabelo do Drake errado); **voltar ao menu → Novo jogo** → texturas e
*lightmaps* quebrados (manchas pretas e ciano nas paredes e no teto do banheiro), igual ao
"sumiço ao recarregar" do início do projeto. Sem crash em 20 min, 30 FPS com quedas.
Com `GOW3_READBACK_ALL=0`, a mesma sequência: **texturas certas**, e o desempenho pareceu
**melhor**, não pior.

### Por quê
`DownloadAllGpuModified` baixa **todas** as faixas marcadas como modificadas pela GPU, de todas
as arenas, e grava na memória do jogo. Uma faixa é marcada ao ser *ligada* como escrita
(`is_written`), não quando a GPU escreve de fato; e quando o jogo reaproveita essa memória para
dados novos escritos pela CPU (texturas lidas do disco depois de recarregar), a cópia velha da
arena é gravada por cima dos dados novos. O download por janela do shadPS4 só toca o trecho que
a CPU está lendo, onde o dado da GPU é justamente o esperado. O agrupado fica como opção
(`GOW3_READBACK_ALL=1`), **desligado por padrão**. As medições anteriores de "97 → 16 quadros
lentos" valem só para o percurso do *probe*; na prática o custo (600 MB/s de downloads e
varredura das arenas) não compensa.

### Em aberto
- **Cabelo do Drake branco/brilhante** ao carregar do checkpoint (intermitente; o resto do
  personagem e a cena normais). Separado das texturas do cenário.
- Desempenho do modo preciso: continua a espera por página; sem o agrupado, ~30 FPS nas cenas
  pesadas. Alvo: descobrir o que a CPU lê a cada quadro e tratar essas faixas em bloco **sem**
  tocar nas demais (ex.: só as faixas que a GPU escreveu de verdade, por página do rastreador).
- Corrida no `ProcessCommands` corrigida em `6681ee4`: sem crashes desde então.

---

## 2026-10-13 — Crash na área externa do museu: corrida no `ProcessCommands` (`6681ee4`)

### Sintomas (usuário, modo preciso, Cartagena/jovem Drake)
1. **TDR** às 05:40: Windows registrou `LiveKernelEvent 141` com dump `WATCHDOG` (driver da
   GPU reiniciado); o jogo morre sem escrever nada no log. Como achar: `Get-WinEvent`
   (Application, provedores "Windows Error Reporting" e "Application Error").
2. Depois, dois crashes de host com a mesma assinatura: **leitura em `0x20000`**, em
   `RecordChunk::Push` (via `RecordHostCopy`) e em `KickRecording`. `0x20000` é o deslocamento
   do campo `used` do `RecordChunk` (128 KiB de armazenamento antes dele): o `record_chunk`
   do scheduler estava **nulo**, isto é, outra thread estava no meio da troca do bloco.
3. Também visto: Drake todo preto e manchas ciano no banheiro numa execução; na seguinte, tudo
   iluminado e normal. Não é fixo. O rastreio das piscadas (`GOW3_FLASH_TRACE=1`) não acusou
   nada em 4 min, e não houve piscadas nessa sessão.

### Diagnóstico
O *probe* vai direto ao checkpoint ("Continuar" é o primeiro item) e reproduz em minutos. O
relatório de falha agora imprime o **nome da thread** (`GetThreadDescription`): a que caiu era
`gow3:DrawRec` (estágio B), gravando um desenho (`DrawRecord` → `UpdateDynamicState` →
`Record`). Nas pilhas (`GOW3_FAULT_HOLD` + lldb), a thread da GPU (estágio A) estava em
`ProcessCommands` executando o *readback* de uma falha de página de uma thread do jogo
(`DownloadAllGpuModified` → `Finish` → `SyncRecording`), **sem ter drenado o estágio B**.
O `ProcessCommands` lia `num_commands` antes do laço (e só então drenava) e de novo na
condição do laço: um comando que chegasse entre as duas leituras rodava com o estágio B ainda
gravando. Bug herdado do GoW3; no modo relaxado os comandos são raros, no preciso são ~150/s.
A thread de gravação Vulkan também caiu (`RecordChunk::Execute`) num bloco corrompido pela
mesma corrida: comandos corrompidos explicam o TDR.

### Correção
A drenagem acontece dentro do laço, na primeira iteração que realmente tem um comando.
Verificação: 5 minutos na área externa do museu sem falha nem assertiva (antes: crash em
2–4 min). FPS ali: 32–34 (cena pesada: ~1100 desenhos e ~1000 dispatches por quadro lento,
171 submissões por quadro lento, 1,5 s em "GPU ticks" e 1,5 s em handlers de leitura nos 58
quadros lentos de 5 min).

### Próximos passos
- Desempenho do modo preciso na área externa: ~170 submissões por quadro ainda; reduzir os
  *readbacks* (o que a CPU lê a cada quadro) continua sendo o alvo.
- Drake preto / manchas ciano: intermitente; comparar com o shadPS4 e capturar um quadro quando
  acontecer.

---

## 2026-10-13 — Modo preciso: *readback* agrupado (46–54 FPS no banheiro), 4º impasse, rastreio das piscadas (`03a919a`)

### Perfil dos quadros lentos (97 quadros acima de 40 ms, 7,1 s somados, teste anterior)
GPU ticks 3,0 s · handlers de falha de leitura 2,0 s (1001 falhas, ~10/quadro) · handlers de
falha de escrita 2,0 s · **~150 submissões por quadro**. Cada acesso da CPU a uma página escrita
pela GPU era uma falha, um download de 512 KiB e um `Finish()` (espera completa da GPU).
Script: `stalls.py` (agrega as linhas `Stall:` do log).

### *Readback* agrupado (`BufferCache::DownloadAllGpuModified`)
No modo preciso, o primeiro *readback* traz **todas** as faixas modificadas pela GPU, de todas
as arenas, com uma só espera; os acessos seguintes não encontram nada pendente.
`GOW3_READBACK_ALL=0` volta ao download por janela. Resultado (4 min, mesmo percurso):
- quadros lentos 97 → 16 (7,1 s → 2,3 s); banheiro 29–38 FPS → **46–54 FPS**; sem travar.
- Custo: ~150 *readbacks*/s de ~4 MB (600 MB/s de GPU→CPU) durante o gameplay; 0 em cena parada.
  A linha `uc3 draw stats` mostra "readbacks: N all-at-once, X MB".

### 4º impasse (corrigido): a captura começava antes de drenar o estágio B
A thread de captura segurava o mutex de submissão (`Finish()` → `SyncRecording`) enquanto o
estágio B, ainda ativo, tentava um `Finish()` no EOP (`ProcessDownloadImages`) e travava no
mesmo mutex; a thread da GPU esperava o `Drain`. O `DrainDrawPipe` passou para **antes** de
iniciar o *job* de captura.

### Piscadas em preto (usuário: tela inteira, um quadro, a qualquer momento)
Não reproduzíveis pelo *probe*. Hipóteses e rastreio (`GOW3_FLASH_TRACE=1`, com tempo em s):
- `RefreshImage`: uma imagem escrita pela GPU só é recarregada da memória do jogo quando o
  *hash* dos bytes muda; um *readback* que grave dados de *buffer* na memória de um *render
  target* (a arena pode não ter os pixels) muda o hash → imagem preta por um quadro. Log:
  "Flash trace: T s: GPU-written image WxH ... re-uploaded".
- Flip em branco (índice -1): "Flash trace: T s: blank flip".
O jogador anota o segundo aproximado de cada piscada e compara com o log.

### Estado / próximos passos
- `run_uc3.ps1` usa o modo preciso por padrão (`c5db78c`).
- Reduzir os *readbacks* (600 MB/s): descobrir o que a CPU lê a cada quadro (endereços das
  falhas) e limitar o download às arenas/faixas tocadas, ou fazê-lo uma vez por quadro no EOP.
- Piscadas: esperar o rastreio do jogador.

---

## 2026-10-13 — **O cenário aparece no modo preciso** (`dd96d90`)

Rodado `GOW3_READBACKS=2` com o `uc3_hang_probe.py` depois da revisão. Três impasses em
sequência, cada um diagnosticado com as pilhas do lldb, e no fim **4 minutos no banheiro a
60 FPS, ~600 desenhos por quadro, e o usuário viu o cenário**. O bug principal é do modo
relaxado (padrão) de leituras da GPU; não é tradução de shader.

### Ferramentas novas
- `GOW3_FAULT_HOLD=<s>`: o loader segura o processo depois de uma falha fatal (a assertiva do
  motor termina numa leitura de `-1` de propósito) para anexar o lldb e ler todas as threads.
- `GOW3_WRITE_LOG=1` passou a conferir, no despejo da falha, os rótulos de *fence*/WriteData
  recentes contra a memória do jogo (lida pela visão física) e a avisar quando um *readback* de
  buffer sobrescreve uma *fence* (`NoteDownload`). Resultado: nenhum rótulo sobrescrito; a
  assertiva `m_gfxEopTick` é o **relógio de "GPU travada" do motor**: a última *fence* tinha
  sido escrita 2,56 s antes, com quadros de 16 ms.
- `WaitHostCopies` avisa no log quando fica mais de 2 s esperando (emitidas/feitas/fila).

### Impasse 1 — a minha correção da revisão
`ResolveHostCopy` dentro do callback de `ForEachUploadRange`, que segura o spinlock da região:
o *readback* reentrava no mesmo lock (estágio B travado em si mesmo) e a thread da GPU esperava
o `Drain`. Tela preta a 37 FPS logo no início. Correção: a leitura prévia fica só nas cópias do
*ring* de constantes (`CollectRingBindings`, thread da GPU, fora de lock), com pré-checagem
`IsRegionGpuModified`; os outros caminhos de upload já evitam faixas modificadas pela GPU. A
conclusão da revisão ("modo preciso lia dados velhos") valia só para esse caminho.

### Impasse 2 — a thread de captura de shaders
`QueueShader` roda a tradução, a especialização e a captura do *fetch shader* numa thread
auxiliar (`gow3:ShaderCapture`) e **a thread da GPU espera por ela**. No modo preciso essa
thread lia uma página protegida contra leitura, caía no handler de falha como thread comum e
mandava a leitura para a thread da GPU (`SendCommand`), que esperava a captura. Correção: a
thread de captura é "lado da GPU" (`Rasterizer::MarkCaptureThread`, falha resolvida nela
mesma, como no estágio B) e o *draw pipe* é drenado antes de esperar a captura. Passou da tela
de título e carregou o banheiro.

### Impasse 3 — contagem das cópias do host
Já no banheiro: thread da GPU em `WaitHostCopies` (dentro de um `Finish()` de *readback*) com a
thread de gravação ociosa e a fila vazia; duas threads do jogo esperando por ela. O contador
`host_copies_done` guardava o **número de sequência** da última cópia executada; com cópias
registradas por mais de uma thread (estágio B, captura), uma cópia antiga executada por último
deixava "feitas" abaixo de "emitidas" para sempre. Agora emitidas e feitas são **contagens**
atômicas. Não reapareceu em 4 minutos (sem o aviso de 2 s no log); fica a observação.

### Estado
- Modo preciso: tela de título, cutscene, banheiro com cenário, 60 FPS no fim; pior quadro
  668 ms no início de um vídeo. Durante o carregamento, 26–32 FPS com 50–65% do tempo em
  "GPU ticks" (o jogo esperando a GPU): custo dos *readbacks* precisos.
- Modo relaxado (padrão): cenário sumido, como antes.
- Jogando (usuário): **muito travado no começo, depois melhora grande** (numa cutscene). Nos
  trechos lentos, GPU a 70% e CPU a 40%: nenhum dos dois saturado, ou seja, sincronização
  (cada *readback* preciso é um `Finish()`, CPU e GPU se esperando). **Piscadas em preto** em
  alguns momentos; suspeita: submissões no meio do quadro (um *readback* ou o `Finish()` da
  thread de captura dentro do desenho encerra o *render pass* e perde o conteúdo do quadro).

### Próximos passos
1. Decidir o padrão do Uncharted: modo preciso no `gow3.ini`/`run_uc3.ps1`, ou achar no modo
   relaxado o que o preciso corrige (dados escritos pela GPU e lidos pela CPU do jogo).
2. Jogar mais tempo no modo preciso (o usuário) e anotar travamentos; o aviso de
   `WaitHostCopies` e o `GOW3_FAULT_HOLD` já ficam prontos para o próximo diagnóstico.
3. Desempenho do modo preciso nos carregamentos ("GPU ticks").

---

## 2026-10-13 — Revisão do código do port: 9 achados, todos corrigidos (`5d6561b`)

(As duas entradas abaixo, datadas de 14 e 15/10, também são de 13/10: os commits mostram a data.)

Revisão dos 23 commits da branch `uc3` em relação ao GoW3 (`windows-v2.1.7`), com foco em
erros reais. As fibras, o LoadExec (mesma semântica de `argv` do shadPS4) e o analisador de
quadros passaram sem achados. O que foi corrigido:

### Correção (comportamento)
1. **Modo preciso lia dados velhos** (`gow3gpu.cpp`, `buffer_cache.cpp`). A correção do impasse
   (`3305573`) fazia as threads de cópia lerem pela visão física as páginas protegidas contra
   leitura, mas elas estão protegidas justamente porque **a GPU tem dados mais novos**; o
   shadPS4 faz o *readback* na falha, antes de copiar. Agora `MemoryManager::ResolveHostCopy`
   traz os dados da GPU para a memória do jogo **na thread da GPU, antes de despachar a cópia**
   (`SmallGuestCopy`, grupos do `UploadCopies`, pedaços do `ObtainBufferForImage` e toda
   `CopySparseMemory` feita na thread da GPU), e a cópia lê pela visão física só para atravessar
   uma proteção ganha depois disso (o conteúdo da CPU é o certo nesse ponto do fluxo de
   comandos). Candidato forte para a assertiva `m_gfxEopTick` do modo preciso: **testar**.
2. **Aviso de desmapeamento antes de o mapeamento falhar** (`place()`): o `HOOK_UNMAP` do
   mapeamento substituído era enfileirado antes do `host_map`; se ele falhasse, a GPU perdia o
   rastreamento de um mapeamento ainda vivo. Agora é enfileirado depois do `host_map` bem-sucedido.
3. **Videodec2 tratava `EAGAIN` como erro**: uma unidade de acesso que gera duas imagens deixava
   a segunda presa no decodificador e o `avcodec_send_packet` seguinte devolvia `EAGAIN` →
   `0x811D0100` e a cutscene parava. Agora as imagens pendentes são postas de lado (`set_aside`,
   fila de 8) e saem uma por chamada, na ordem.
4. **Argumentos do LoadExec com espaços** se partiam na ida e volta por `GOW3_GUEST_ARGS`. Agora
   cada argumento vai numa variável (`GOW3_GUEST_ARG_COUNT`, `GOW3_GUEST_ARG_<i>`);
   `GOW3_GUEST_ARGS` (separado por espaços) continua valendo para uso manual.

### Arquitetura e limpeza
5. **`sceKernelDlsym` só achava símbolos das tabelas de importação.** Agora o NID é calculado do
   nome (`runtime_nid`: SHA-1 do nome + sal do SDK, 8 bytes invertidos, base64 com `-`), como
   o `nid_names.py`, e tentado em cada biblioteca que as tabelas mencionam; conferido contra o
   Python em 7 nomes (inclusive um que ocupa dois blocos do SHA-1).
6. **IDs do GoW3 duplicados** entre `probe.c` e `patches.py`: o loader ganhou `--game-hooks`, que
   o `run.py` passa quando o jogo tem perfil de patches (só o GoW3 tem). A lista em C saiu.
7. **Teste do gerenciador de memória** (`tests/test_memory.c`, alvo `memory-test`, Windows e
   Linux): divisão de mapeamento flexível por `mprotect` parcial (cada metade com as próprias
   páginas físicas; liberar uma não zera a outra), mapa fixo sobre memória mapeada (a GPU recebe
   `unmap` e depois `map`) e a leitura pela visão física. Com a linha antiga do `split_at`, o
   teste falha em 3 pontos. Precisa do `--disable-high-entropy-va`, como o loader.
8. **Videodec2 alocava um frame NV12 por imagem** (o decodificador por software entrega
   YUV 4:2:0 planar, então toda imagem passava pelo swscale + `av_frame_get_buffer`). Agora o
   croma é intercalado direto na cópia para o buffer do jogo; o swscale fica só para outros
   formatos, com o frame de destino reaproveitado.
9. **Busca dupla de VMA na cópia do modo preciso**: `runtime_memory_region_backing` devolve a
   região e o ponteiro da visão física numa só consulta (o cache por thread guarda o `phys`).
   `runtime_memory_read_backing` saiu.

Testes: `memory-test`, `videodec2-test` e `fiber-test` OK no Windows; `memory-test` e
`fiber-test` OK no Linux (WSL). Build completo OK.

### Onde parou
O cenário sumido continua. Próximo passo: rodar `-Set GOW3_READBACKS=2` de novo com a
correção 1 e ver se o modo preciso passa da tela de título (e se o cenário aparece).

---

## 2026-10-15 — Cenário: argumentos indiretos descartados, memória do runtime, modo preciso

O cenário **continua sumido** no modo normal depois de tudo abaixo (testado pelo usuário).

### 1. Os desenhos indiretos não são o cenário
A captura com `ReadMemory` dos argumentos (`5fb48bf`) mostrou valores normais calculados pela
GPU: `36 1 0 0 0`, `24 …`, `36 …`, `1236 …` (índices, instâncias). 36 índices = uma caixa,
1236 = uma esfera: são **volumes de luz** da iluminação adiada (só profundidade, depois cor).
A conclusão da entrada anterior ("é o cenário") estava errada; a pista foi descartada.

### 2. Descoberta do usuário: o cenário depende do carregamento
- Primeiro "Novo jogo": no banheiro o cenário falha **por um instante e depois aparece**.
- Voltar ao menu e carregar de novo: **some e não volta**.
- Ao contrário, as listras na areia da tela de título **se corrigem** ao voltar ao menu depois
  de um carregamento.
→ Dados velhos/novos na memória entre carregamentos: aponta para sincronização CPU↔GPU ou
gerência de memória, não para tradução de shaders.

### 3. Gerência de memória do runtime
- **Mapeamento fixo por cima de outro** (`MAP_FIXED`) não avisava a GPU que o conteúdo antigo
  sumiu. Corrigido (`53499e4`: `place()` enfileira `HOOK_UNMAP` antes do `HOOK_MAP`; log
  "(over a mapping)"). **Não acontece no Uncharted** (0 ocorrências), mas o bug era real.
- Log de mapeamentos com endereço físico, apelidos e os 40 primeiros desmapeamentos (`b733295`).
  Resultado: só há duas alocações físicas (1,6 GiB em `0x1000000000` e 2,9 GiB em
  `0x1100000000`). O jogo **move blocos de 1 MiB** do heap: desmapeia `0x100fc00000` e mapeia a
  mesma memória física (`phys=0xfc00000`) em `0x1300000000`. Nenhum apelido simultâneo.
  O desmapeamento no núcleo (`UnmapMemory`) é idêntico ao do shadPS4 0.19.0 (comparado com o
  código oficial).
- **Bug herdado do GoW3** (`3305573`): `split_at` só ajustava o endereço físico da parte direita
  de um mapeamento **direto**. Num mapeamento **flexível** dividido (ex.: `mprotect` parcial), a
  parte direita ficava com o endereço físico da esquerda e, ao ser liberada, **zerava e
  devolvia ao pool páginas ainda em uso**. Corrigido; não resolveu o cenário.

### 4. Modo de leitura preciso (`GOW3_READBACKS=2`)
O shadPS4 que desenha o cenário usa `readbacks_mode: 2` (preciso); o port usa relaxado (1) por
padrão, e o preciso nunca tinha sido testado (`-Set GOW3_READBACKS=2`).
- Primeiro resultado: **tela preta** logo no início. As pilhas (lldb) mostraram um impasse: as
  threads de cópia do port (`CopySparseMemory`) liam páginas protegidas contra leitura, a falta
  pedia à thread da GPU o *readback*, e a thread da GPU esperava essas mesmas cópias
  (`WaitHostCopies`).
- Correção (`3305573`): no modo preciso as cópias leem pela visão física sem proteção
  (`runtime_memory_read_backing`, par do `write_backing`).
- Agora chega à tela de título a 60 FPS, mas o jogo encerra na assertiva `m_gfxEopTick`
  (`fg-draw-mgr.cpp:234`, "GPU quadros para trás"; pior quadro 349 ms). O caminho de escrita do
  EOP e o `DownloadMemory` são iguais aos do shadPS4. Causa ainda não encontrada.

### Onde parou
- Modo normal: cenário sumido, sem pista ativa. Hipótese principal: algo entre CPU e GPU que o
  modo preciso resolveria no shadPS4 (dados escritos pela GPU e lidos pela CPU do jogo).
- Próximos caminhos:
  1. fazer o modo preciso passar da tela de título (entender a assertiva `m_gfxEopTick` nele) e
     ver se o cenário aparece;
  2. atualizar o núcleo gráfico para o shadPS4 0.19.0 (~70 commits: buffer cache, memory
     tracker, page manager).
- Diagnóstico de travamentos com CPU ocupada: o `uc3_hang_probe.py` só detecta jogo parado; para
  laços, rodar o jogo, esperar e usar `lldb -p PID -b -o 'thread backtrace all' -o 'process detach'`.

---

## 2026-10-14 — Investigação: o cenário some (gameplay); estado atual

### Sintomas (vistos pelo usuário no bar de Londres, cap. 1)
- **Paredes, chão e teto do cenário não aparecem** (fundo liso). Personagens, objetos soltos e
  divisórias aparecem. Personagem do fundo fica preto (sem a iluminação do cenário).
- Atraso no rosto, roupa entrando no corpo, textura esticando em cenas rápidas, NPC piscando.
- Tela de título: listras laranja na areia (**também no shadPS4**: defeito do núcleo, não do port);
  falta a névoa/gradiente do céu (**só no port**).
- FPS: 30–46 em combate (GPU 53–77%).

### Teste decisivo: shadPS4 0.19.0 desenha o cenário
`tools/shadps4` (portátil: `user/` ao lado do exe; `user/config.json` com
`direct_memory_access_enabled`, `readbacks_mode: 2`, `redzone_patches`). Ele precisa das
pastas reais com os nomes `CUSA02344` e `CUSA02344-UPDATE` (junctions confundem o relançamento),
então o jogo foi reorganizado (contagens conferidas, nada perdido):
```
D:\Uncharted\CUSA02344\CUSA02344          jogo base (real)
D:\Uncharted\CUSA02344\CUSA02344-UPDATE   1.02 (real)
D:\Uncharted\CUSA02344\app0, patch        junctions para o port
```
Comando: `shadPS4.exe -g D:\Uncharted\CUSA02344\CUSA02344\eboot.bin` (menu → Uncharted 3).
No bar, **o cenário aparece no shadPS4** → o problema é do port. O shadPS4 cai mais adiante na
assertiva `m_gfxEopTick` (compila shaders na thread da GPU; ela fica quadros para trás).

### Descartado (o cenário continua sumido)
| Hipótese | Teste |
|---|---|
| Leituras adiadas/"stale" da GPU | desligadas no menu (`deferred_readback=0`, `stale_readback=0`; o FPS melhorou e ficou assim) |
| Otimizações do GoW3 | todas desligadas ao vivo (`GOW3_TOGGLE_FILE`, máscara `0x7ffff400fffff`) |
| Pipeline de desenho em duas threads | `GOW3_DRAW_PIPE=0` desde o início |
| Compilação de shaders em segundo plano | desligada (sem ela: assertiva `m_gfxEopTick`, como no shadPS4; religada no `gow3.ini`) |
| *Direct memory access* | `GOW3_DIRECT_MEMORY_ACCESS=1` com cache de shaders novo |
| Desenhos descartados pelo port | `uc3 draw stats`: ~600 desenhos/quadro, **0** sem pipeline; `FilterDraw` e `RefreshGraphicsKey` iguais ao upstream |
| Consultas de oclusão / modo PS4 Pro | iguais ao upstream / `sceKernelIsNeoMode` = 0 |
| Carregamento de arquivos | `GOW3_FILE_TRACE`: `ldn-pub-bathroom.pak` (11,9 MB) lido, **0 falhas** de leitura/abertura |
| Correções do recompilador 0.19.0 | 2f40f87b5 (ballot/MBCNT), d9cf41ba0, 4c7b287f1 trazidas (`4ae3bbb`) |

### O que o analisador de quadros mostrou (bar, ~60 etapas)
Adaptado ao Uncharted (`5fb48bf`): grava a partir da primeira etapa depois de armado e registra
desenhos/dispatches indiretos. Disparo automático: `GOW3_CAPTURE_TRIGGER=<arquivo>` + criar o
arquivo; saída em `user/captures/`. Estrutura do quadro:
- `#0` pré-profundidade (58 desenhos) · `#2` passagem principal (130, RGBA16F + D32S8) ·
  `#4`/`#7` sombras.
- **`#8`–`#15`: 4 pares (só profundidade, depois cor em 2× RGBA16F) com 1 `DrawIndexIndirect`
  cada** (`max 1`, passo 20, argumentos em `0x1127020200/220/240/260`). **É o cenário**,
  desenhado pela GPU.
- Os argumentos são escritos pelo **compute `9bd0e806`** (etapa `#6`, 4 dispatches de 1×1×1;
  *slots* 2/3/5 entradas, 4/6 = buffers de índices de 3 MiB, 7 = argumentos). Antes dele, o
  compute `6a6422a9` (768 grupos).
- No desenho: CPU vê `0 0 0 0 0`, o cache diz **`cpu modified 0, gpu modified 1`** → o port usa
  o valor da GPU (sincronização correta).

### Onde parou
Falta saber **o valor que a GPU calculou** para os argumentos. Já está no código (`5fb48bf`,
compilar antes de testar): com captura ativa, `DrawIndirect` chama `ReadMemory(args, 20, true)`
para trazer o valor da GPU antes do desenho, e a nota da captura mostra os 5 números.
- **Contagem 0** → o problema está no compute de descarte (`9bd0e806`/`6a6422a9`): tradução do
  shader, entradas (*user data*, DMA) ou dados que ele lê.
- **Contagem normal** → os desenhos saem invisíveis: buffers de índices/vértices (os de 3 MiB),
  estado (profundidade, viewport) ou *vertex shader* `1bf62085`.

Rodar: `tools/uc3_hang_probe.py` (aperta X sozinho, chega ao banheiro em ~110 s) com
`GOW3_CAPTURE_TRIGGER` e um gatilho criado aos ~105 s.

### Outras descobertas desta etapa
- **Modo coletânea:** o menu (`eboot.bin`) roda no port (0 importações faltando com `unlink`) e o
  `LoadExec` reinicia o port com `big3` + `eboot.bin ispartofuccollection` (`ba38fdd`). Mas o
  `big3` nesse modo fica em **tela cinza** no port (a 60 FPS, sem threads presas); no shadPS4
  segue normal. Possivelmente a mesma causa do cenário (espera algo que não fica pronto).
- O analisador original caía num buffer sem cópia na CPU (corrigido).
- Diferenças restantes para a 0.19.0: ~70 commits do núcleo não trazidos (buffer cache/memory
  tracker, page_manager, user data via *flat buffer* `8c91be2b4`, `ConstructSharpFetch`
  `41c0fca5e`…). Atualizar o núcleo inteiro é a alternativa se a investigação pontual travar.

## 2026-10-14 — "Novo jogo" travava: impasse na GPU; a cutscene de abertura roda

### Sintoma
Depois de "Novo jogo" (diálogo de save, barra de progresso, segundo decodificador de vídeo), o
jogo **parava no carregamento** com quase nenhuma CPU em uso. Em outra execução, caiu numa
assertiva do próprio motor:
```
ASSERTION: GetRenderFrameParams(frameToFreeInstancesFor)->m_gfxEopTick
  Function: FrameBegin   File: ndlib\render\fg-draw-mgr.cpp   Line: 234
```
O motor confere que a GPU sinalizou o fim (EOP) de um quadro antigo antes de liberar a memória
dele. A GPU nunca terminou o quadro.

### Diagnóstico
`lldb -p <pid> -b -o 'thread backtrace all'` com o jogo parado (o `lldb` vem no toolchain
CLANG64). O depurador anexa, captura e solta sem matar o processo.
- **Thread da GPU** (`Liverpool::ProcessCompute` → `PipelineCache::GetProgram`): esperando o
  `programs_mutex` do cache de pipelines de forma **exclusiva**.
- **11 threads** em `PageManager::GuestFaultSignalHandler` → `Rasterizer::ReadMemory` →
  `Liverpool::SendCommand`: falharam ao ler uma página monitorada e esperavam a thread da GPU
  devolver os dados.
- Entre elas, **trabalhadores de preparação de desenho** (otimização do port do GoW3,
  `vk_draw_prep`, `GetProgram` com `sel.worker`). Eles seguram o `programs_mutex`
  **compartilhado** enquanto leem memória do jogo de forma especulativa
  (`runtime_fault_recover`: uma falha deveria só abandonar o trabalho).

Ciclo: o trabalhador segura o *lock* e espera a GPU; a GPU espera o *lock*. O loader tratava a
falha passando **primeiro** pelo rastreamento de páginas da GPU (que espera a thread da GPU) e só
depois pelo ponto de recuperação, então o "abandonar" nunca acontecia. Bug latente do port do
GoW3 que o padrão de memória do Uncharted dispara.

### Correção (`4e8bbdb`)
`src/probe.c` (Windows e Linux): com `runtime_fault_recover` ativo, a falha volta direto ao
ponto de recuperação, sem consultar o rastreamento da GPU. A thread da GPU refaz o trabalho, e
as falhas dela são tratadas na hora.
Resultado: **a cutscene de abertura roda**, sem erros no log.

### Ferramenta: `tools/uc3_hang_probe.py`
Leva o jogo ao "Novo jogo" apertando X por script (`GOW3_PAD_FILE`, lido pelo
`runtime_pad.c`) e, quando o log para (ignorando o eco do próprio script) com pouca CPU, salva
as pilhas de todas as threads em `out/hang-backtrace-full.txt`.
Cuidado: parar o script pelo gerenciador de tarefas fecha o jogo junto (processo filho).

### Visto pelo usuário na cutscene
**"Delay de render" no rosto dos personagens.** Hipótese: algo do rosto (animação facial ou
*skinning*) usa dados do quadro anterior. A parte gráfica do GoW3 tem leituras adiadas e cópias
do quadro anterior (`GOW3_DEFERRED_READBACK`, `GOW3_STALE_READBACK`) e o pipeline de desenho em
duas threads (`GOW3_DRAW_PIPE`). `tools/run_uc3.ps1 -Set CHAVE=VALOR,...` liga e desliga essas
opções para comparar.

## 2026-10-14 — Fase 3: **o Uncharted 3 chega ao menu principal**

### Importações: 69 → 0
Detalhes em [IMPORTS.md](IMPORTS.md). Commits `48fc866` (stubs e mapeamentos), `45a888b`
(fibras), `c01e2a0` (Videodec2, `sceKernelUtimes`).
- **Mapeamentos:** muitas funções já existiam (`open`/`read`/`write`, `scePadRead`,
  `sceRtcGetTime_t`); faltava só o nome dos NIDs. `src/import_names_uc3.inc` tem os NIDs do
  `big3`.
- **Stubs offline** em `runtime_services.c`/`runtime_kernel.c`/`runtime_content.c`.
  O `ErrorDialog` registra no log o código do erro que o jogo quis mostrar, e o `LoadExec`
  registra o executável pedido e encerra (trocar de executável fica para depois).
- **Fibras** (`src/runtime_fiber.c`): troca de contexto em assembly. Uma fibra só fica
  "suspensa" depois de salvar os registradores, o estado por thread é relido depois de cada
  troca (a fibra pode ter migrado) e os limites de pilha do TEB acompanham a fibra no Windows.
  Teste `tests/test_fiber.c` (alvo `fiber-test`): 8 threads, 128 fibras, ~165 mil trocas,
  **30/30 no Windows (Clang) e no Linux (GCC)**. O primeiro erro do teste foi justamente o
  risco previsto: o compilador do *teste* guardou o endereço do TLS de antes da troca.
- **Videodec2** (`src/runtime_videodec2.c`): layouts, códigos de erro e posição da *picture
  info* conferidos no código do shadPS4 (`b4e7ae7`, baixado só para consulta), com uma
  verificação a mais (pixels + info precisam caber no buffer). Teste `tests/test_videodec2.c`.
- O último NID (`0Cq8ipKr9n0`) é **`sceKernelUtimes`**, achado no `aerolib.inl` do shadPS4.

### Primeira execução de verdade
1. Chegou longe de primeira: fibras `NdJob Fiber (512 KiB)`, 6 `NdJobWorkerThread`, threads de
   GPU e de I/O (Fios), ~50 `DCLoader`, PlayGo com 100 chunks e o vídeo de abertura
   (`Videodec2 first picture 1920x1080`). Então parou com **`STOP: GPU hook queue overflow`**.
2. Causa: o `runtime_memory.c` guarda os avisos de mapeamento para a GPU numa fila fixa de 64
   posições. O Uncharted mapeia memória direta em **206 pedaços de 1 MiB**, e um desmapeamento
   sobre a faixa toda gera um aviso por pedaço. **Correção:** a fila cresce sob demanda
   (`eeea09e`).
3. Segunda execução: **2 minutos sem travar** (encerrada pelo limite de tempo). O usuário
   jogou na janela: **menu principal navegável, "Novo jogo" funcionando (saves `SAVEDATA03P` e
   `SAVEDATA0CC` criados), som correto**. Único defeito visto: **alguns artefatos na imagem de
   fundo do menu**.

### Observações
- Os ~50 `open(.../u3data/build/main/effect1/*.bin) -> not found` são normais: a pasta
  `effect1` não existe e os mesmos recursos estão em pacotes `.pak` (`pak23/`, `actor33/`). O
  motor procura versões soltas opcionais antes.
- Avisos `SanitizeCopyLayers` do Vulkan aparecem junto com o vídeo de abertura; podem ter
  relação com os artefatos.
- `tools/run_uc3.ps1` roda o jogo sem limite de tempo e grava `out/last-run.log`.

### Próximos passos
- Entrar no gameplay ("Novo jogo" até o fim do carregamento) e ver o que acontece.
- Investigar os artefatos do menu (captura de tela; comparar com o shadPS4 0.19.0; testar
  `GOW3_DIRECT_MEMORY_ACCESS=1` e as opções de leitura da GPU).

## 2026-10-13 — Fase 2: o Uncharted 3 passa pelo loader; 69 funções faltando

### Executável escolhível
- `prepare.py`, `link_libc.py` e `link_modules.py` ganharam `--executable` (padrão
  `eboot.bin`, então o GoW3 continua igual).
- `run.py` lê **`GOW3_EXECUTABLE`** e repassa aos scripts.
- O runtime usa o mesmo nome no `argv[0]` (`/app0/big3-ps4_Shipping.elf`), no `getargv` e no
  `__progname` (`runtime_executable_name()` em `src/runtime.c`).

Comando completo:
```powershell
$env:GOW3_GAME_DIR='D:/Uncharted/CUSA02344/app0'
$env:GOW3_PATCH_DIR='D:/Uncharted/CUSA02344/patch'
$env:GOW3_EXECUTABLE='big3-ps4_Shipping.elf'
python run.py --check-imports     # no MSYS2 CLANG64; MSYS2_ROOT=D:/msys64
```

### Resultado
O `big3` 01.02 é preparado e linkado sem erro, e a GPU inicializa (327 funções HLE). **69
importações sem implementação**, 68 delas identificadas. A lista, organizada por prioridade,
está em **[IMPORTS.md](IMPORTS.md)**, que é o checklist da fase 3. Resumo:

| Grupo | Qtd | Tipo |
|---|---|---|
| `libSceFiber` (fibras) | 7 | **real**, prioridade máxima |
| `libSceVideodec2` (cutscenes) | 10 | real (reaproveitar o FFmpeg do `runtime_videodec.c`) |
| Módulos/executáveis (`LoadStartModule`, `Dlsym`, `LoadExec`, unwind) | 4 | real |
| `libScePosix` (arquivos, rede) | 17 | mapear / stub de rede |
| kernel, controle, relógio, PlayGo, AppContent | 15 | mapear / stub |
| online e diálogos (NpTus, NpSignaling, ErrorDialog…) | 16 | stub |

### O que as strings do `big3` mostram
- **Fibras por toda parte:** `NdJobWorkerThread %d`, `NdJob Fiber (%lu KiB)`, `CreateFiber`,
  `SwitchToFiber`, `Suspending fiber job`. É o sistema de jobs da palestra da Naughty Dog na
  GDC 2015 (*Parallelizing the Naughty Dog Engine Using Fibers*). Os travamentos antigos do
  shadPS4 (`NdJobWorkerThread`, `condvar`) vinham daqui, então **`libSceFiber` precisa ser
  sólida**: cada fibra tem pilha e registradores próprios, e trocar de fibra não pode
  confundir o TLS/TCB da thread (que o loader redireciona para o slot TLS do TEB no Windows).
- Referências a `/app0/eboot.bin`, `/app0/big2-ps4_Shipping.elf` e
  `/app0/big3-ps4_Shipping.elf`: o `sceSystemServiceLoadExec` é o "voltar ao menu" e o
  "trocar de jogo".
- Carrega módulos do sistema em tempo de execução (`libSceFiber.prx`, `libSceNpManager.prx`,
  `libSceAjm.prx`…). O runtime já serve essas bibliotecas, então o `LoadStartModule` só precisa
  de um handle coerente.
- Caminho de build original: `d:/dev/CP9/u3final-ps4/big3/...`.

### Bug corrigido: a visão "jogo + atualização" não era removida no Windows
Na primeira execução, a limpeza da visão temporária falhou (`WinError 145` em `sce_sys`).
Causa, no `game_update.py` original do GoW3: sem o modo de desenvolvedor, os links são
*junctions*/*hard links*, guardados num registro interno (`mods.LINKED`). Quando a atualização
também tem `sce_sys`, o link vira uma pasta de verdade, mas o nome **ficava no registro**, e a
limpeza tentava remover a pasta como se fosse link.
- Correção: `remove()` tira o caminho do `LINKED` ao remover um link.
- **O dump não foi afetado:** contagem conferida antes e depois (app0 15.165 arquivos,
  58,58 GB; patch 208, 2,05 GB). A sobra foi removida apagando só *hard links* e *junctions*,
  sem entrar nelas.
- Efeito colateral bom: **2 testes que falhavam no GoW3 original no Windows passaram**
  (`test_game_update`: `removing_the_view_leaves_the_dumps_whole` e
  `folders_the_update_leaves_alone_are_linked_whole`). Windows: 9 falhas (eram 11), todas
  limitações conhecidas da plataforma.

### Ferramenta
`tools/nid_names.py`: dá nome aos NIDs de um log do `--check-imports`, calculando o hash de
nomes candidatos do SDK.

### Próximo passo: fase 3
Seguir o [IMPORTS.md](IMPORTS.md): primeiro os stubs e mapeamentos baratos (para o jogo
avançar) e o `LoadStartModule`; depois **fibras** e **Videodec2**. Em seguida, rodar de
verdade e corrigir o que parar.

## 2026-10-13 — Dump do jogo extraído

### O pacote
- `D:\Uncharted\CUSA02344\CUSA02344.pkg` (40,55 GB), backup da cópia do usuário.
- `EP9000-CUSA02344_00-UNCHARTEDTRILOGY`: *Nathan Drake Collection* europeia, **versão 01.00**
  (jogo base, sem a atualização 1.02), SDK 2.508. É o mesmo ID do relato *Ingame* no shadPS4 0.19.0.

### Extração
- Ferramenta: **PkgTool 0.2.231** do [LibOrbisPkg](https://github.com/maxton/LibOrbisPkg)
  (LGPL, código aberto), em `tools/pkgtool`.
- `pkg_extract --passcode 00000000000000000000000000000000` → 58,6 GB, 15.146 arquivos.
- O `pkg_extract` **não** grava os metadados do `sce_sys` (`param.sfo`, ícones, playgo,
  troféus): eles são entradas do pacote. Extraídos um a um com `pkg_extractentry` (IDs 11–29 do
  `pkg_listentries`).
- Armadilha: em PowerShell, um `[ordered]@{}` com chaves inteiras é indexado **por posição**;
  os nomes saíram trocados na primeira tentativa. Use uma lista de pares.
- Pasta final: **`D:\Uncharted\CUSA02344\app0`** (nome do PS4 para a pasta do jogo).

### Estrutura do jogo — descoberta importante
```
app0\
├── eboot.bin                 10,6 MB  menu da coletânea (e talvez o Uncharted 1)
├── big2-ps4_Shipping.elf     16,4 MB  Uncharted 2
├── big3-ps4_Shipping.elf     22,9 MB  Uncharted 3   ← o nosso alvo
├── sce_module\ libc.prx, libSceFios2.prx
├── u1data\ u2data\ u3data\ shareddata\
└── sce_sys\
```
"big" é o codinome da série na Naughty Dog. Cada jogo da coletânea tem **o seu próprio
executável**, e o `eboot.bin` provavelmente lança os outros (algo como
`sceSystemServiceLoadExec`). Isso explica o relato antigo do shadPS4 em que "o Uncharted 3 não
abria", mesmo com o 1 e o 2 funcionando. Para o port, a ideia é **carregar o `big3` direto**.

Os três executáveis são *fake SELF* sem criptografia nem compressão (flags dos segmentos com
`0x800` e sem `0x2`/`0x8`), o formato que o `prepare.py` aceita.

### Validação
`prepare.py` no `eboot.bin`: 429 importações, 19.243 realocações, 52 módulos requisitados,
`_init_env` da libc verificado. O dump está bom.

### Atualização 1.02
- `D:\Uncharted\CUSA02344\1.02.pkg` (0,96 GB), mesmo `CONTENT_ID`, categoria `gp` (patch),
  **`APP_VER` 01.02**.
- Extraída para **`D:\Uncharted\CUSA02344\patch`** (2,05 GB), com 42 entradas de `sce_sys`
  (`param.sfo`, `changeinfo/`, `origin-deltainfo.dat`, troféus…) conferidas pelo tamanho.
- Traz **executáveis novos** para os três jogos (`big3`: 22,9 → 26,0 MB), sem criptografia, e
  dados novos em `u1data`/`u2data`/`u3data`. Não traz `sce_module` (vale o do jogo base).
- Para usar: `GOW3_PATCH_DIR=D:\Uncharted\CUSA02344\patch`. O `run.py` monta uma visão
  combinada temporária (`scripts/game_update.py`, só links) sem alterar nenhuma das pastas.
- **Vamos trabalhar com a 01.02**, a versão dos relatos do shadPS4. Para scripts que não passam
  pelo `run.py`, o `big3` certo é o de `patch\`.

### Próximos passos (fase 2)
1. Ensinar o `prepare.py`/`link_modules.py` a usar outro executável além do `eboot.bin`
   (o `big3-ps4_Shipping.elf`, versão 01.02).
2. `--check-imports` no `big3` para listar o que falta no runtime.
3. Descobrir como o `eboot.bin` lança o `big3` (argumentos, parâmetros) para reproduzir isso.

## 2026-10-13 — WSL (Ubuntu) como ambiente de apoio

### Por quê
O port vai rodar no **Windows**, e o build principal continua sendo o MSYS2. O WSL é apoio:
- roda os testes que dependem do Linux (os 11 que falham no Windows);
- é o **plano B para travamentos de threads**. As ferramentas de diagnóstico Linux do loader (o
  *watchdog* `--timeout`, que mostra a pilha de cada thread, e o `gdb`) não existem na versão
  Windows, e o Uncharted 3 tem histórico de travar no sistema de jobs da Naughty Dog
  (`NdJobWorkerThread`, `condvar`). A ideia é rodar o loader em `--cpu-only` no WSL quando isso
  aparecer.
- **Não** serve para jogar: no WSL2 o Vulkan passa por tradução para D3D12 (Dozen), lento e
  diferente do driver real da AMD.

### Instalação
- A virtualização (AMD SVM) estava desligada na BIOS; ligada pelo usuário.
- `wsl --install -d Ubuntu` (admin) ligou os recursos do Windows, mas a distribuição não ficou
  instalada depois do reinício. Resolvido com `wsl --install -d Ubuntu --no-launch`, sem admin.
- **Ubuntu 26.04.1 LTS** no WSL2 (WSL 3.0.3, kernel 6.18), usuário `matheus`, 16 threads,
  15 GB de RAM.
- O disco virtual foi movido do C: (pouco espaço) para **`D:\WSL\Ubuntu`**
  (`wsl --manage Ubuntu --move D:\WSL\Ubuntu`).
- Pacotes: `build-essential cmake ninja-build pkg-config python3-pil git gdb`.

### Repositório no Linux
- `~/uncharted/uc3_pc`, branch `uc3`, clonado do repositório do Windows. Fica **dentro** do
  sistema de arquivos do Linux, porque o acesso a `/mnt/d` é lento.
- Remotos: `windows` (`/mnt/d/Uncharted/uc3_pc`, para trazer os commits) e `upstream` (GoW3,
  só *fetch*). Para atualizar: `git pull windows uc3`.

### Resultado: testes
**Linux: 159 testes, todos OK (34 pulados).** As 11 falhas no Windows são limitações da
plataforma (links simbólicos, Nix), não problemas do código.

### Plano B: build completo no Linux (pacotes já conferidos no 26.04)
```bash
sudo apt install -y libsdl3-dev libavcodec-dev libavutil-dev libavformat-dev libswscale-dev \
  libboost-dev libfmt-dev robin-map-dev libxxhash-dev libvulkan-memory-allocator-dev \
  glslang-dev glslang-tools spirv-cross libspirv-cross-c-shared-dev libxbyak-dev libzydis-dev \
  spirv-headers libvulkan-dev libx11-dev libxcb1-dev libwayland-dev
```
`magic_enum` e `miniz` vêm dos submódulos (não há pacote no Ubuntu).

## 2026-10-12 — Fase 1: separar o que é do GoW3

### Resultado do estudo
O caminho principal (preparar → linkar → carregar → rodar) já é quase todo genérico:

| Parte | Específico do GoW3? |
|---|---|
| `scripts/prepare.py`, `link_libc.py`, `content_profile.py` | Não |
| `scripts/link_modules.py` | Exigia `libSceFios2.prx` → **corrigido** |
| `scripts/patches.py` | Tabela `GAMES` com os IDs do GoW3; sem perfil, não aplica patch nenhum. Ok por enquanto; o perfil do Uncharted entra aqui na fase 5 |
| `run.py` | Só o padrão `GOW3_GAME_DIR=../CUSA01623`; basta definir a variável |
| `src/probe.c` | Hooks de trapaça do GoW3 rodavam em qualquer jogo → **corrigido** |
| `src/runtime_*.c` | Genérico (é o "PS4" que o jogo enxerga) |
| `gpu/shim/gow3_overlay.cpp` | Abas de trapaça do GoW3, mas já se desativam sozinhas quando os hooks não existem |
| `launcher/`, `packaging/` | Totalmente GoW3 (nome, cores, ícones). Fica para a fase 5 |

### Núcleo gráfico (shadPS4 embutido)
- Base: shadPS4 commit `b4e7ae7` (2026-10-02), praticamente a mesma época da **0.19.0**, a
  versão em que a Nathan Drake Collection chegou a *Ingame*. Bom sinal.
- 286 alterações marcadas com `gow3:`. Quase todas são de **arquitetura e desempenho**, não
  truques do GoW3: pipeline de desenho em duas threads, leituras adiadas, compilação de shaders
  em segundo plano, estatísticas e suporte a Windows. Poucas são calibradas para o GoW3 (vetores
  de movimento e upscaler temporal, que já ficam desligados por padrão).
- Interruptores para testar se o Uncharted renderizar errado (todos variáveis de ambiente):
  - `GOW3_DIRECT_MEMORY_ACCESS=1`: **desligado por padrão, e o shadPS4 diz que a NDC precisa
    dele.** É o primeiro a testar.
  - `GOW3_DRAW_PIPE`, `GOW3_VK_RECORD_THREAD`: voltam ao desenho numa thread só, como no shadPS4.
  - `GOW3_DEFERRED_READBACK`, `GOW3_STALE_READBACK`, `GOW3_READBACK_LINEAR`: leituras da GPU
    (equivalem ao *precise readback* do shadPS4).
  - `GOW3_ASYNC_SHADERS`, `GOW3_DMEM_MB` (memória direta informada ao jogo, padrão 5056 MiB).
- Plano B: se algo renderizar errado só no port, comparar com o shadPS4 0.19.0 puro
  (`tools/shadps4`) para saber se o problema é do núcleo ou das mudanças do GoW3.

### Mudanças
1. **`src/probe.c`:** os hooks do GoW3 (trapaças, multiplicadores, barra de vida) só são
   procurados quando o `TITLE_ID` é CUSA01623/01715/01720. Eles acham o código por padrões de
   bytes, e num jogo diferente uma coincidência alteraria código do Uncharted sem ninguém
   perceber. Em outro jogo o log mostra `Game hooks: none for title ...`. O título padrão da
   janela (sem `param.sfo`) virou "PS4 game".
2. **`scripts/link_modules.py`:** só o `libc.prx` é obrigatório. Se o jogo não trouxer o
   `libSceFios2.prx`, o linker avisa e as importações dele vão para o runtime (o
   `--check-imports` vai mostrar o que faltar). Com `--modules` explícito continua estrito.

### Verificação
- Build completo OK.
- Testes Python: 159 rodados, mesmas 11 falhas do GoW3 original (testes que dependem do Linux:
  links simbólicos, Nix). Nenhuma falha nova.
- Ainda não deu para testar as mudanças com um jogo de verdade (falta o dump).

### Armadilha (de novo): CRLF nos submódulos
Os submódulos são clonados com o `core.autocrlf=true` global do Windows, e o patch do
FSR-Vulkan falha (`patch não se aplica`). Correção: `core.autocrlf=false` em cada submódulo e
checkout de novo (`git submodule foreach`).

### Próximos passos
- Fase 1 concluída no que dá para fazer sem o jogo.
- **Dump da Nathan Drake Collection** → fase 2: `prepare.py` + `link_modules.py` +
  `gow3-probe.exe --check-imports`.
- Antes disso, se quiser: testar a coletânea no shadPS4 0.19.0 para ter uma referência.

## 2026-10-12 — Início do projeto

### Decisão: versão de PS4, não de PS3
O Uncharted 3 original é de PS3 (Cell: PPU PowerPC big-endian + 6 SPUs). Um port nativo
exigiria recompilar o código PPU e centenas de programas de SPU, e traduzir a GPU RSX. Isso é
um projeto de anos, e o RPCS3 já cobre esse caminho.

A *Nathan Drake Collection* (PS4) é x86-64 + GCN, a mesma situação de Bloodborne e GoW3
Remastered, que já têm ports nativos. Escolhemos ela.

### Pesquisa: estado no shadPS4
Relatos no [rastreador de compatibilidade](https://github.com/shadps4-compatibility/shadps4-game-compatibility):
- [CUSA02344](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/441)
  (Europa, Windows): **Ingame na 0.19.0**. Em 10/10/2026, uma hora de jogo sem crash com
  *precise readback*; só um glitch de iluminação num templo subterrâneo.
- [CUSA02320](https://github.com/shadps4-compatibility/shadps4-game-compatibility/issues/147)
  (EUA, Linux): precisa de *direct memory access*. Em versões antigas o **Uncharted 3** travava
  em `NdJobWorkerThread` e `condvar`, no sistema de jobs da Naughty Dog.
  → No nosso runtime, threads e sincronização (`runtime_thread.c`, `runtime_mutex.c`) vão
  precisar de atenção.
- Outros IDs da coletânea: CUSA02343, CUSA02826, CUSA01399.

### Decisão: base no port do GoW3
Comparamos os dois ports que já tínhamos:
- **GoW3** ([FelipePrado0/gow3_pc](https://github.com/FelipePrado0/gow3_pc), fork do Bloodborne
  Windows do Supermedo): mais novo, build com MSYS2/Clang, tem decodificador de vídeo H.264
  (`runtime_videodec.c`) e MP3, hooks por assinatura. **Escolhido como base.**
- **Bloodborne** ([Mrsuss60](https://github.com/Mrsuss60/bloodborne_pc_windows_port)): outro
  fork Windows do mesmo original, com compatibilidade Win32 própria (`win32_*.c`). Fica em
  `ref/bloodborne_pc` como segunda opinião para funções que o GoW3 não implementa.

### Ambiente
- MSYS2 em `D:\msys64` (no C: só havia ~14 GB livres), CLANG64, pacotes do
  `packaging/windows/README.md`.
- O port do GoW3 compilou sem alterações (`bash build.sh`, com `MSYS2_ROOT=/d/msys64`).
- Teste Vulkan (`gow3-probe.exe --vulkan-only`): **AMD Radeon RX 6750 XT; PASS**.
- `gow3-gpu-capabilities.exe` avisa: a RX 6750 XT não faz blit de D32S8, então a escala de
  cena ao vivo fica desligada. O launcher do GoW3 já trata isso; não bloqueia nada.
- shadPS4 0.19.0 em `tools/shadps4`, para comparar o comportamento do jogo.

### Armadilha: quebras de linha
O git do Windows estava com `core.autocrlf=true`, o que transformava `build.sh` em CRLF e
quebrava o bash. Este repositório usa `core.autocrlf=false`. Repita isso em qualquer clone novo.

### Repositório
- `D:\Uncharted\uc3_pc`, branch `uc3`, a partir da tag `windows-v2.1.7` do GoW3.
- Remoto `upstream` = GoW3 no GitHub, só para *fetch* (push desativado).
- **Os nomes `gow3-*`/`GOW3_*` ficam por enquanto.** Renomear tudo agora geraria um diff enorme
  e conflitos sempre que trouxéssemos correções do upstream. Vamos renomear quando o jogo
  estiver rodando.

### Estudo do código (começo)
- `src/probe.c` (loader): quase todo genérico. Carrega a imagem do eboot, aplica as
  realocações, liga as importações ao runtime (ou a armadilhas que param com
  `STOP: first unsupported PS4 import`), aplica patches e entra no código do jogo.
  **Específico do GoW3:** `install_cheats`, `install_actor_watch`, `install_orb_multiplier` e
  `install_stat_multipliers` (+ `src/*_hook.h`), além do título padrão "God of War III".
- `scripts/prepare.py`: genérico. Lê o SELF já descriptografado, monta a imagem de memória, as
  realocações e a lista de importações (`analysis.json` com o relatório).
- `--check-imports` lista as importações sem implementação sem rodar o jogo. Esse será o
  primeiro passo com o dump.

### Próximos passos
1. Terminar o estudo: `scripts/link_*.py`, `run.py`, `gpu/shim/` (o que é GoW3 lá).
2. Fase 1: desligar ou isolar o código específico do GoW3 sem quebrar o build.
3. Conseguir o dump da Nathan Drake Collection.
