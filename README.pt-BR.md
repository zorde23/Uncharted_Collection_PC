# Uncharted: The Nathan Drake Collection para PC (port nativo)

[English](README.md) | **Português (Brasil)**

Port nativo para Windows 10 e 11 dos jogos da **Uncharted: The Nathan Drake Collection** (PS4,
Bluepoint, 2015). O foco é o **Uncharted 3: Drake's Deception**, jogável do início ao fim a 60 FPS em 1080p, sem upscaling nem geração de quadros; o Uncharted 2
é jogável e o Uncharted 1 roda (experimental).

> **Nenhum arquivo do jogo está incluído.** Você precisa do dump do seu próprio PS4
> (testado: **CUSA02344**, versão 01.00 com a atualização **01.02**).
> Este projeto não tem relação com a Sony Interactive Entertainment, a Naughty Dog ou a Bluepoint
> Games. Uncharted é marca registrada da Sony Interactive Entertainment.

## Baixar e jogar

Sem instalar nada e sem terminal:

1. Baixe o `ucndc_pc-v…-windows.zip` mais recente em [Releases](https://github.com/zorde23/Uncharted_Collection_PC/releases) e extraia em qualquer lugar
   (por exemplo `C:\Jogos\ucndc_pc`). Não rode de dentro do zip.
2. Abra o **`Uncharted Launcher.exe`**. Se o SmartScreen do Windows avisar, clique em
   *Mais informações* → *Executar assim mesmo* (o launcher não é assinado).
3. Escolha a pasta do dump do jogo (`CUSA02344`). A da atualização (01.02) é encontrada sozinha
   quando está ao lado dela.
4. Clique em **Jogar** no jogo que quiser. O launcher também cria atalhos na área de trabalho.

O botão **Opções** ajusta o modo de tela (janela, sem borda, tela cheia), o VSync, o limite de FPS e
o contador de FPS. Em *Avançado* dá para desligar qualquer otimização; com todas ligadas fica a
configuração testada. No jogo, **Insert** ou **F1** (ou L2+R2+L3+R3 no controle) abre o menu do port. Os saves ficam
na pasta `user`, ao lado do launcher, e o log da última sessão em `user\last_run.log`.

O botão **Controles** configura teclado e mouse: ligue *Olhar com o mouse*, ajuste a sensibilidade
(e um multiplicador separado ao mirar) e troque qualquer tecla ou botão do controle. As mesmas
opções estão no menu do jogo (aba *Controls*). Com o mouse ligado, ele gira a câmera direto nos três
jogos, andando, mirando e em cobertura; segure Alt esquerdo para soltar o cursor. Teclas padrão:
WASD anda, Espaço X, Ctrl esquerdo ou Q Círculo, F Quadrado, E Triângulo, G L1, R R1, botão direito
do mouse L2 (mirar), botão esquerdo R2 (atirar), V L3, botão do meio R3, 1/2/3/4 ou a roda o
direcional, Tab o touchpad, Esc ou Enter Options.

Precisa de Windows 10 (1903+) ou 11, 64 bits, e placa de vídeo com Vulkan 1.3. O port foi feito numa
AMD RX 6750 XT; placas Intel têm uma correção do Supermedo, mas ainda não foram testadas.

## Como funciona

O PS4 usa um processador x86-64, a mesma arquitetura do PC. Por isso o executável original do jogo
roda **direto na CPU**, sem emulação: um runtime escrito para o port substitui as bibliotecas de
sistema do PS4 (memória, threads, arquivos, áudio, vídeo, controle, saves). A parte gráfica é
traduzida: os comandos e shaders da GPU do PS4 (AMD GCN) viram Vulkan pelo núcleo de vídeo do
[shadPS4](https://github.com/shadps4-emu/shadPS4).

Este repositório é um fork do [port do God of War III](https://github.com/FelipePrado0/gow3_pc), que
vem do [port do Bloodborne para Windows](https://github.com/Supermedo/bloodborne_pc) do Supermedo e
do [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc) original do deadinside28. O que o
jogador vê já tem o nome deste port (`ucndc.exe`, `ucndc.ini`, opções `UC_*`); por dentro do código
muitos nomes ainda começam com `gow3` / `GOW3_`, e os nomes antigos continuam funcionando.

## Por que este projeto

A ideia inicial era portar só o **Uncharted 3**, o único da trilogia que eu ainda não tinha jogado e também o que o shadPS4 tem mais
dificuldade de rodar.
Como os três jogos da coletânea usam o mesmo motor e o mesmo dump, o Uncharted 2 e o Uncharted 1
vieram depois quase de graça, e agora o objetivo é a trilogia inteira.

## Situação

Atualizado em 23/10/2026.

| Jogo | Executável | Situação |
|---|---|---|
| Uncharted 1 | `eboot.bin` | Roda a **60 FPS**, com som (inclusive os tiros); pouco testado além do começo |
| Uncharted 2 | `big2-ps4_Shipping.elf` | Jogável, 51–60 FPS, com saves próprios |
| Uncharted 3 | `big3-ps4_Shipping.elf` | **100% jogável do início ao fim**, a **60 FPS em 1080p, sem upscaling nem geração de quadros**. Algumas partes ainda precisam de otimização e caem abaixo de 60 (perto de 50 com muita vegetação, no Castelo), sempre acima de 30 FPS. Um único travamento no jogo inteiro, ainda não reproduzido nem corrigido. Problemas conhecidos listados abaixo |

Novo na 0.3.5:
- **Ícones de teclado** nos três jogos: com "Ícones de teclado no jogo" ligado na janela Controles do launcher, os botões mostrados nos textos e no HUD aparecem com as suas teclas em vez dos botões do controle.
- UC3: as manchas vermelhas redondas que alguns jogadores viam (bloom, em todas as placas) foram corrigidas.
- UC1 e UC2 agora usam as correções do UC3 para sombras, luzes, imagem e a névoa dos cones de luz.
- UC1: proteção contra um travamento visto num PC (o jogo não fecha mais ali; em teste).

Novo na 0.3.4:
- **1440p e 4K** nos três jogos (launcher, Opções > Resolução). O jogo renderiza nativo no tamanho escolhido (sem upscaling), e os vídeos de abertura são ampliados para a tela. **Ainda não testado por completo**: foi testado em sessões curtas de cada jogo, não num jogo inteiro. Acima de 1080p a placa de vídeo trabalha mais e o jogo usa mais memória de vídeo (4K: cerca de 8 GB de memória do jogo). Em monitor 4K, "Janela sem borda" preenche a tela com o mesmo FPS da janela.
- UC3: a névoa dos cones de luz não mostra mais cunhas pretas nem chuvisco branco, e paredes e luzes não piscam mais onde os mapas de sombra são refeitos (ligado por padrão no launcher).
- UC3: o cachecol do Drake não pula mais no deserto.

Resolvido antes no UC3: cabelo branco do Drake jovem (`BUFFER_LOAD` com addr64),
texturas trocadas (colete do barman, piso do museu, porta do banheiro) e o desempenho, que saiu de
18–24 FPS nas ruas e no castelo para 60.

Novo na 0.3.2: controles de teclado e mouse, com o mouse girando a câmera direto no UC1, UC2 e UC3
(também no jogo 1.00, sem a atualização), janela Controles no launcher e aba Controls no menu do
jogo, F1 como segunda tecla do menu e aviso no launcher quando sai versão nova.

Também novo: o pacote para jogadores, com o launcher e a tela de Opções; o menu do port no jogo não
mostra mais nos Uncharted o que é só do God of War III (trapaças, orbes, upscaler, resolução de
render); e a tela preta em placas Intel corrigida (da versão 0.3.3 do Supermedo, não testada aqui).

Problemas conhecidos:

**Uncharted 3**
- **Um travamento no jogo inteiro, ainda sem correção.** Não foi possível reproduzir. Uma causa provável é o timeout do driver de vídeo depois de sessões longas: o port pode mandar de uma vez um bloco muito grande de memória (~1,5 GB) para a GPU, o quadro trava e o Windows/AMD fecha o jogo. Salve com frequência.
- **Quedas curtas no 1% low onde mapas de sombra são refeitos.** Um array de mapas de sombra é refeito uma camada por vez em vez de reaproveitado (quadros de 25-50 ms). As paredes escurecendo e as luzes piscando que isso causava foram corrigidas (`UC_DEPTH_KEEP_CLEAR`). Visto na Cidadela (capítulo em 37%).
- **Texturas piscam rapidamente na primeira vez que se entra numa área nova.** Shaders novos são compilados em segundo plano, e os objetos que os usam são pulados por alguns quadros. Os shaders ficam em cache, então isso não se repete nas próximas visitas.
- **Algumas partes ainda precisam de otimização.** O FPS cai abaixo de 60 em alguns lugares (com muita vegetação, como no Castelo, fica perto de 50), sempre acima de 30. O limite é o processador de comandos da GPU emulado na CPU.
- O jogo é travado em 60 FPS. Taxas maiores aceleram a lógica do jogo.
- Upscaling (FSR/DLSS) não funciona no Uncharted: os ganchos herdados são específicos do God of War III. No lugar, 1440p e 4K renderizam nativo (veja acima).
- **1440p e 4K ainda não foram testados por completo** (nos três jogos): só sessões curtas até agora. Se o jogo travar ou aparecer algo errado acima de 1080p, volte para 1920x1080 nas Opções e avise.

**Uncharted 2**
- **Texturas do cenário podem piscar em 4K** (e talvez em 1440p). Em 1080p não foi visto até agora. Ainda sem correção.
- No prólogo do trem o vagão pode cair cedo demais, então talvez seja preciso pular antes do que no PS4. Por isso o UC2 roda sem `UC_COMPUTE_PIPE`. Se o trem continuar falhando, desligue `UC_HLE_INORDER` e `UC_ACB_DISPATCH_PIPE` no launcher (Opções > Avançado) nessa parte; elas deixam a perseguição nos telhados a 60 FPS.
- Pouco testado. Avise se faltar algum som ou aparecer algum defeito.

**Uncharted 1**
- **Um travamento visto num PC** (duas vezes, no mesmo ponto do código do jogo). Na 0.3.5 o jogo espera um instante e continua em vez de fechar; no pior caso um objeto pode sair errado por um quadro. Se aparecer "UC1 binding retry" no `last_run.log`, mande o log.
- Pouco testado. Avise se faltar algum som ou aparecer algum defeito.

O diário do projeto, com o que foi feito e por quê, está em [docs/uc3/DEVLOG.md](docs/uc3/DEVLOG.md).

## Diferença para o uncharted_pc do Supermedo

O [uncharted_pc](https://github.com/Supermedo/uncharted_pc) do Supermedo é outro port nativo da mesma
coletânea, também GPL e da mesma família (os dois descendem do bloodborne_pc do deadinside28). Os dois
foram feitos em paralelo, e este aqui aproveitou várias correções dele, sempre com crédito (ver
[Créditos](#créditos)). A comparação abaixo é com a versão 0.3.1 dele (15/10/2026):

| | Este port (ucndc_pc) | uncharted_pc (Supermedo) |
|---|---|---|
| Origem | Fork do port do God of War III (gow3_pc) | Fork direto do bloodborne_pc (bbport) |
| Núcleo de vídeo | shadPS4 0.19.0 com correções da nightly de 12/10 (rastreador de memória, cache de texturas) | shadPS4 de 24/09 com backports do recompilador |
| Leituras da GPU pelo jogo | Modo **preciso** (o cenário é desenhado com os dados certos; foi isso que resolveu os problemas visuais aqui), acelerado para chegar a 60 FPS: leituras assíncronas, fila lateral, envio adiantado, cópias do ring em outra thread | Modo **relaxado** por padrão, contornando o problema com a HLE da cópia escalar |
| Como as otimizações entram | No código, cada uma atrás de uma opção **desligada por padrão**, medida e registrada no [DEVLOG](docs/uc3/DEVLOG.md); o launcher liga o conjunto testado, e *Opções → Avançado* desliga uma a uma | Ligadas por padrão, com opções para desligar |
| Outras diferenças nossas | Barreiras justas em buffers formatados, hash completo de imagens pequenas, MP3 no Ajm, ferramentas de diagnóstico (perfil de GPU, rastreio de faltas e escritas), mouse girando a câmera direto nos três jogos (andando, mirando e em cobertura) | — |
| O que ele tem e este ainda não | — | Upscalers e geração de quadros, opção de 120 FPS, menu da coletânea pelo `eboot.bin` |

Os dois projetos se ajudam: se algo funciona lá e não aqui (ou o contrário), vale comparar.

## Compilar a partir do código

Só é preciso para mexer no port; para jogar, use o [download](#baixar-e-jogar).


1. Instale o MSYS2 (`winget install MSYS2.MSYS2`), abra o terminal **MSYS2 CLANG64** e instale as
   dependências:

   ```bash
   pacman -S --needed git patch \
     mingw-w64-clang-x86_64-{toolchain,cmake,ninja,pkgconf,python} \
     mingw-w64-clang-x86_64-{vulkan-headers,vulkan-loader,vulkan-memory-allocator,glslang} \
     mingw-w64-clang-x86_64-{sdl3,fmt,boost,robin-map,xxhash,ffmpeg,zydis} \
     mingw-w64-clang-x86_64-{spirv-tools,spirv-headers,spirv-cross}
   ```

2. Clone com os submódulos:

   ```bash
   git clone --recursive https://github.com/zorde23/Uncharted_Collection_PC.git
   ```

3. Edite `windows\config.bat` com os caminhos do MSYS2, do jogo e da atualização.
4. Rode `windows\compilar.bat` (ou `bash build.sh` no terminal CLANG64). O resultado é
   `out\ucndc.exe`.
5. Opcional: `packaging\windows\empacotar.bat` gera o zip para jogadores (`dist\ucndc_pc-v<versão>-windows.zip`)
   com o launcher. Precisa do Python para Windows instalado (python.org ou `winget install Python.Python.3.13`).

### Jogar a partir do código

Dê dois cliques no `.bat` do jogo, dentro de `windows\`:

| Arquivo | O que faz |
|---|---|
| `config.bat` | Caminhos (MSYS2, jogo, atualização) e a lista de otimizações recomendadas |
| `compilar.bat` | Compila o port |
| `jogar_uc3.bat` | Uncharted 3 com as otimizações (60 FPS) |
| `jogar_uc2.bat` | Uncharted 2 (sem `UC_COMPUTE_PIPE`, por causa do trem) |
| `jogar_uc1.bat` | Uncharted 1 (experimental) |
| `fechar_jogo.bat` | Fecha o jogo se ele travar |

O log de cada sessão fica em `out\last-run.log` (o anterior em `out\previous-run.log`). Os saves
ficam em `user\`. No jogo, **Insert** ou **F1** (ou L2+R2+L3+R3 no controle) abre o menu do port.

Também dá para chamar o script direto, passando opções só para aquela execução:

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_uc3.ps1 -Set UC_READBACKS=1,UC_FRAME_STATS=1
```

### Opções (toggles)

Toda otimização nova entra **desligada por padrão** no código; o launcher e os `.bat` de `windows\`
ligam o conjunto testado. Todas também valem com o prefixo antigo `GOW3_`
(`UC_LEAF_COPY` = `GOW3_LEAF_COPY`; se as duas estiverem definidas, vale a `UC_`). As principais:

| Variável | Efeito |
|---|---|
| `UC_READBACKS` | `2` (padrão do `run_uc3.ps1`): leituras precisas da GPU, necessárias para o cenário aparecer; `1`: modo relaxado |
| `UC_READBACK_ASYNC`, `UC_READBACK_FAST*`, `UC_READBACK_SIDEQ`, `UC_DEFERRED_READBACK` | Leituras da GPU sem parar a fila inteira |
| `UC_SCALAR_COPY_HLE`, `UC_HLE_INVALIDATE` | Substitui um compute shader de cópia por código nativo (e invalida os caches que ele escreve) |
| `UC_COMPUTE_PIPE`, `UC_LEAF_COPY` | Atalhos no processamento de compute |
| `UC_EAGER_SUBMIT*`, `UC_ASYNC_SUBMIT`, `UC_DEFERRED_SUBMIT` | Envio de comandos à GPU mais cedo / em outra thread |
| `UC_RING_COPY_THREAD`, `UC_RING_DEDUP` | Cópias do ring buffer numa thread separada, sem repetição |
| `UC_FRAME_STATS` | Estatísticas de quadro no log |
| `UC_EXECUTABLE` | Qual jogo abrir (`big3-ps4_Shipping.elf`, `big2-ps4_Shipping.elf`, `eboot.bin`) |
| `UC_LOW_IMAGE` | Carrega o executável abaixo de 2 GB (necessário no UC1) |
| `UC_AJM_END_RULES` | Regras de fim de job do Ajm (decodificador de áudio), do SuperMedo (UC1) |
| `UC_EOP_TICK_WAIT` | O assert `m_gfxEopTick` espera a GPU em vez de parar o jogo |
| `UC_SYNC_SMALL_TARGETS` | Shaders de render targets pequenos compilam na hora, então uma tabela de iluminação desenhada uma vez no início não é pulada (corrige a tocha escura da caverna no UC3) |
| `UC_JOB_SPIN`, `UC_JOB_SLEEP` | `UC_JOB_SPIN=100` + `UC_JOB_SLEEP=1`: as seis threads de jobs do jogo giram menos e dormem durante o jogo; o uso de CPU cai de uns 66% para uns 50% com o mesmo FPS (UC3) |
| `UC_MIP_FIX` | O cache de texturas aceita render targets desenhados num mip de uma imagem maior, então o bloom e a distorção não se perdem (corrige a imagem lavada e acinzentada no UC3) |
| `UC_MOUSE_EDGE_AIM` | O mouse gira a câmera ao mirar pendurado num beiral ou escalando, e nos pontos pendurado onde a mira girava devagar (UC3) |
| `UC_DEPTH_KEEP_CLEAR` | Um array de mapas de sombra recriado com mais camadas mantém a limpeza de profundidade pendente da imagem que ele substitui (corrige luzes, sombras e paredes escuras piscando no UC3, por exemplo no castelo) |
| `UC_SCOPE_SPLIT_SCALAR` | O tradutor de shaders executa para a wave inteira uma instrução escalar que fica entre as instruções vetoriais de um bloco divergente, como a GPU faz (corrige as cunhas pretas e o chuvisco branco na névoa dos cones de luz do UC3) |
| `UC_PREP_TRUST_A1` | Experimental, desligado no launcher: o preparo dos desenhos reaproveita o trabalho que consegue provar que continua válido (uns +5 FPS nas ruínas do deserto no UC3) |
| `UC_HLE_INORDER`, `UC_ACB_DISPATCH_PIPE` | Deixam a perseguição nos telhados do UC2 a 60 FPS; desligue se o prólogo do trem falhar |

A lista completa, com medições, está no [DEVLOG](docs/uc3/DEVLOG.md).

## Créditos

- **deadinside28**: [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), o port nativo
  original (a base de tudo, também chamada de bbport).
- **Supermedo** (Mohammed Albarghouthi): o [port do Bloodborne para Windows](https://github.com/Supermedo/bloodborne_pc)
  e o [uncharted_pc](https://github.com/Supermedo/uncharted_pc), de onde vieram ideias e correções
  (HLE da cópia escalar de constantes, `BUFFER_LOAD` com addr64, regras de fim de job do Ajm, patch de espera do
  `m_gfxEopTick` da 0.3.2, abertura com `O_CREAT` em pasta somente leitura para o som do UC1, correção para placas Intel da 0.3.3).
- **Felipe Prado** e colaboradores: [gow3_pc](https://github.com/FelipePrado0/gow3_pc), o port do
  God of War III do qual este repositório é fork, com as correções de renderização do
  [cuesta4](https://github.com/cuesta4/shadPS4/tree/eltutz).
- **Mindro** (ManoharPadul): a [build do shadPS4 para a coletânea Uncharted](https://github.com/ManoharPadul/ShadPS4),
  cujas notas de versão mostraram quais problemas procurar (cache de texturas, configurações recomendadas).
- **Equipe do shadPS4**: o núcleo de vídeo [shadPS4](https://github.com/shadps4-emu/shadPS4), que
  faz toda a tradução de GPU e shaders para Vulkan.
- **Claude** ([Anthropic](https://www.anthropic.com)): desenvolvido com a ajuda do Claude (pesquisa,
  código, testes, empacotamento e documentação).

Também usados: [LibAtrac9](https://github.com/Thealexbarney/LibAtrac9), [FFmpeg](https://ffmpeg.org),
[SDL3](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui),
[sirit](https://github.com/shadps4-emu/sirit), [magic_enum](https://github.com/Neargye/magic_enum),
[miniz](https://github.com/richgel999/miniz), [xbyak](https://github.com/herumi/xbyak),
[Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator),
[FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), [MSYS2](https://www.msys2.org) e
[LLVM](https://llvm.org).

O README original do port do God of War III está em [docs/gow3/README.gow3.pt-BR.md](docs/gow3/README.gow3.pt-BR.md).

## Licença

GNU GPL v2 ou posterior ([LICENSE](LICENSE)), a mesma do shadPS4 e dos ports dos quais este deriva.
Componentes de terceiros mantêm suas próprias licenças.
