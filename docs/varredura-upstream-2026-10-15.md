# Varredura do upstream: xenia-edge, Xenia Canary e game-patches (2026-10-15)

**Pedido:** analisar os commits e changelogs do xenia-edge (has207), do Xenia Canary e de
outros repositórios necessários, e listar o que vale trazer para o Xendroid+, desde que seja
útil e concreto.
**Entrega:** pesquisa e comparação; nada foi implementado nesta fatia. O veredito de cada
commit está nos relatórios por área:
- [CPU e backend A64](varredura-upstream-2026-10-15/cpu.md);
- [GPU e Vulkan](varredura-upstream-2026-10-15/gpu.md);
- [kernel](varredura-upstream-2026-10-15/kernel.md);
- [áudio, arquivos, memória e base](varredura-upstream-2026-10-15/misc.md).

Regras que valem para tudo abaixo:
- número de outro projeto não prova ganho aqui;
- o que depende de aparelho fica como validação pendente;
- nada da lista foi compilado nem executado.

## 1. Fontes e recortes

| Projeto | Repositório | O que foi lido | Recorte |
|---|---|---|---|
| **xenia-edge** | `github.com/has207/xenia-edge`, branch `edge` | 769 commits sem merge depois da base (HEAD `581ba93e7`), e as cópias antigas com hash reescrito | entraram de 2026-08-01 a 2026-10-15 |
| **Xenia Canary** | `github.com/xenia-canary/xenia-canary`, branch `canary_experimental` | 14 commits que o edge ainda não tem (HEAD `b7350635b`); o resto do Canary chega pelo edge | até 2026-10-14 |
| **game-patches** | `github.com/xenia-canary/game-patches` | 24 commits depois da cópia do Xendroid+ (HEAD `2c15efd`) | 2026-06-24 a 2026-10-15 (a cópia é de 2026-06-23) |
| **XenDroid** | `github.com/rfandango/XenDroid` | a base do fork; nenhum commit depois de 2026-08-20 | — |

- **Base do núcleo.** O primeiro commit com o código do Xendroid+ (`de2221288`, "Initial commit",
  2026-09-28) é o `main` do XenDroid de 14 a 20/08 (`7f6b17b71`), com 10 arquivos mudados
  localmente: nove da GPU e o `app/emulator_window.cc`. O XenDroid
  sincronizou o edge até `84cf20922` (2026-08-01, PR #75 dele). Depois disso o edge reescreveu
  o próprio histórico, e o mesmo commit hoje é `e25bfd824`.
- **XeniOS** (`github.com/xenios-jp/XeniOS`, o porte para iOS e macOS): o repositório está parado
  desde 2026-06-19, e o autor passou a mandar o trabalho direto ao edge (50 dos 769 commits: A64,
  CPU, Metal, POSIX). Pelo assunto dos commits, o que ficou fora do edge nas branches `xenios` e
  `preview` é quase todo de iOS, Metal e build. Sobram quatro:
  - `7e483f44` (atômicos LSE para `lwarx`/`stwcx`): superado pelo caminho inline do Xendroid+
    (`a64_native_reserved_ops`);
  - para avaliar (Talvez): `f22e2b65` (`LOAD_CLOCK` inline no A64), `7f0a3e72` e `8ade38bb`
    (revalidar texturas invalidadas por falso compartilhamento).
- **aX360e, X360 Mobile, DroidDeck, Bannerlator e Eden** já estão na
  [varredura de 2026-10-03](varredura-referencias-2026-10-03.md).

## 2. Método

1. **Candidatos.** Commits sem merge do edge e do Canary que a base não tem pelo hash: 1.877.
   Como o edge reescreveu hashes, isso inclui cópias de commits que o Xendroid+ já tem.
2. **Áreas fora do Android.** Saíram ~422 commits que só mexem em D3D12, Metal, backend x64,
   UI desktop ou CI.
3. **Presença automática.** Para cada commit, rodou `git apply --check` direto e reverso
   contra o núcleo do Xendroid+ (sem escrever nada). Também foi medida a fração das linhas
   adicionadas que já existem no mesmo arquivo. Saíram ~466 já presentes (e um vazio). O filtro
   não é exato: pelo menos um commit saiu como presente sem estar (`de4d24493`, o resumo do
   FPSCR; ver o relatório de CPU).
4. **Revisão um a um.** Os 988 restantes foram revisados por área contra o código do Xendroid+,
   com o veredito Portar, Talvez, Já tem ou Não se aplica. Cópias com hash reescrito e
   duplicatas edge/Canary ficaram, em geral, como Já tem ou Não se aplica, com o commit a usar.
   Três duplicatas ficaram como Portar ou Talvez, com a nota "portar só uma": `577fb8ee17` =
   `46e8fd8dcc`, `5ce7c336df` = `45827cc2ba` e `0c843efb32` = `7b2555f1f2`.
5. **Conferência à mão.** O relógio do guest (M3) e o `O_APPEND` (M2) foram conferidos no
   código.

## 3. Números

| Área | Revisados | Portar | Talvez | Já tem | Não se aplica |
|---|---|---|---|---|---|
| CPU e backend A64 | 235 | 53 | 64 | 37 | 81 |
| GPU e Vulkan | 301 | 38 | 41 | 122 | 100 |
| Kernel | 235 | 45 | 85 | 37 | 68 |
| Áudio, arquivos, memória e base | 217 | 27 | 36 | 40 | 114 |

Na CPU, 2 dos 53 são duplicatas do Canary: são 51 commits distintos.

- **Backend A64.** 51 commits são do backend A64 de Will Martin (oaknut), que o próprio edge
  reverteu. O A64 do Xendroid+ é o de xbyak_aarch64.
- **Escalonador de fibras.** O escalonador do edge já está no Xendroid+ e é o padrão
  (`guest_scheduler = true`); faltam as correções do tema K1.
- **NaN e FPSCR.** Os fixups de NaN do PPC estão desligados no Xendroid+ desde 2026-10-07, então
  a série de NaN/FPSCR fica em Talvez, salvo onde o caminho padrão diverge.

## 4. O que vale trazer, por tema

Cada tema tem um código para o plano da seção 6. Os commits, a evidência no código e as
dependências estão nos relatórios por área.

### CPU e backend A64

| Tema | O que corrige ou melhora | Commits | Esforço | Risco |
|---|---|---|---|---|
| **C1** Instruções PPC com resultado errado | `mcrf` (eq vira gt e SO some), `mtfsfi` (RN errado), `mtfsb0/1` (NOP hoje), `mcrfs`, `vupkhsh128`/`vupklsh128`, `vcmpbfp` e o CA das formas OE | `46e8fd8dcc`, `e1e4851f7a`, `7d7557f2f7`, `a5532a0d19`, `504cbee7eb`, `e4b13738c3`, `10da45ea40` → `4d83110dcd` | P (M nas formas OE) | Baixo (Médio no `10da45ea40`) |
| **C2** Fold de constantes do HIR | `Extract` com o lane errado e leitura fora do vetor; `VectorCompareEQ` e merge de compares tratavam float por bits. É o bug que o upstream ligou à promoção de VEC128 | `45827cc2ba`, `920f35c938`, `eddcd8f31e` | P | Baixo |
| **C3** MMIO no A64 | Load MMIO de 32 bits pelo sinal chega com os bytes invertidos; as faltas MMIO nunca são registradas, então todo acesso paga um SIGSEGV | `2cdfb45cbf`, `10ac2f5eff` | P | Baixo |
| **C4** Codegen A64 errado | Largura I8/I16, MAX/MIN com duas constantes, constante sobrescrita antes da última leitura, `lvrx` alinhado lendo a página seguinte, dot product com infinito, FPCR nas idas e voltas ao host | `565aad3186`, `fcd8e58a30`, `e6a86116f3`, `a5f59a48ae`, `7b778e2a3e`, `fa57fbdb5f`, `35d5c81d71`, `95b14f55fa` (parte A64) | M | Médio |
| **C5** Robustez do JIT em ARM64 | Entradas do JIT publicadas fora do lock (memória fraca), `EntryTable` O(n) sob o lock global, longjmp no prólogo, `bl` sem módulo, `assert` sumindo com NDEBUG | `8ffe24e372` → `a77db1ed3b`, `6fe1231561`, `df72008d9a`, `6e304ab866`, `c87c034153`, `619170cc7a` | P–M | Baixo–Médio |
| **C6** HLE, XEX e boot | `TranslateVirtual` chama `getpagesize()` em todo acesso de export; patch XEX sobreposto (crash do NG2+TU em aarch64); patch com mudança de base (XNA Indie Player); inicializadores estáticos antes da thread principal (Hydrophobia) | `44e05e470a`, `7dab7132b5`, `2fe7b7d635`, `e011d1eeab` | P (M no último) | Baixo |
| **C7** Desempenho do A64 | Stores de CR mortos e `b.cond` direto (upstream: −41,6% de stores de CR no Halo 3); codegen mais enxuto (imediatos, literal pool, `vperm`/`vsel`/`vsldoi`, `stvlx`/`stvrx`); chamadas indiretas e `CallExtern` mais baratos | `689590ba3c` + `f6f9a03768`, `aba5703fa0`; `af082b6d0d` (só as partes fora de desvios) e mais 12; `b416d19675` + `7a97888962`, `6fc16d118d` | M–G | Médio–Alto |

### GPU e Vulkan

| Tema | O que corrige ou melhora | Commits | Esforço | Risco |
|---|---|---|---|---|
| **G1** Endereços em janela espelho | Vertex fetch, índices e memexport com endereço em janela espelho (0xC0000000…) eram rejeitados e o draw descartado | `2fce2d8957` | P–M | Baixo |
| **G2** Texturas 1D e 1D largas | UI de 545407D4 amostrava com Y=0; bindings 1D largos de 555308CE; precisão e teto de memória do 1D largo | `fbdb1f2817` → `7cd47947b0` → `6260a87b85` | M | Médio |
| **G3** Escala de `num_format` inteiro por canal | A escala seguia o swizzle do host e não limitava o canal-fonte; afeta os quirks de Crysis 3 e Rise of the Tomb Raider | `6a45452087` → `052cb95f24` | P | Baixo |
| **G4** Resolve A8 seleciona o alfa | Resolve A8 (`k_8` com `LOW_BLUE`) passa a selecionar o alfa: o plano de opacidade de 5451080D e a neblina de 4D530808 | `2b3f0cb456` | M | Médio |
| **G5** Operando escalar com ALU vetorial de 3 fontes | O parse do microcódigo pegava o componente errado | `92ada8ebc0` | P | Baixo |
| **G6** Mip travado em fetch não normalizado | Popping de visibilidade em 555308B6 e 5553080B | `8486e97a06` | P | Baixo |
| **G7** Espera de memexport só até o último export | Os quirks de 4541098E e 53510823 esperavam a fila inteira | `1222c23f78` | P | Baixo |
| **G8** Layout e endereçamento de texturas | Mips de arrays e volumes e mip tail 3D; limites tiled 3D; strides de fatia; mip tail do nível 0; row pitch de 96bpp; nível base com `mip_min_level` (534307E2); bits de `k_DXT3A_AS_1_1_1_1`; páginas recém-alocadas não invalidadas (texturas velhas no Dynasty Warriors 5/6 Empires); textura em dois fetch constants carregada duas vezes | `bc6b7a0202`, `c9c8e483b1`, `c332733afd`, `81e3deaee2`, `ace153cb84`, `19da174033`, `4a863a0e1a`, `ac37478be6`, `d1541a579b`, `40cee11ab1` | P–M (cinco dos sete de layout aplicam limpos em sequência, e o `ac37478be6` também) | Médio |
| **G9** Resolves | Offset de resolve tiled abaixo de 32bpp (refração da água em 534307D5); espaçamento de fatias do resolve de volume; barreiras do resolve escalado gravadas dentro do render pass; checagem de MSAA no resolve clear | `89609297c7`, `d8731edc99`, `c8ac9258f0`, `830e605186` (só a checagem de MSAA; o resto é FSI) | P–M | Médio |
| **G10** Shaders | `kill` grava o resultado documentado (o SPIR-V devolvia sempre 0); a decodificação 7e3 para 8_8_8_8, ligada por padrão no Xendroid+, ignorava a write mask (Gears of War: Judgment) | `e7595ef605`, `a2b609d22d` | P–M | Baixo |
| **G11** Draws pulados com o cache de pipelines frio | Com `vulkan_async_skip_draws` ligado por padrão, todo draw cujo pipeline ainda compila é pulado; num render-to-texture que não se repete, o resultado fica perdido. Com draws únicos esperando o pipeline real, o upstream deixou de perder com o cache frio os arbustos do NFS The Run, Soul Calibur V, a estrada piscando no FM3 e a chuva do Halo: Reach. A espera acelera só o pipeline aguardado | `de8e60601c` → `8a4d405af6` → `e8c05cd05f` | M | Médio |
| **G12** Command processor e sincronização | `PM4_WAIT_IB_PFD_COMPLETE` não tratado descartava o resto do ring; ponteiro de leitura do ring publicado só no fim do burst; máscaras trocadas ao crescer o scratch buffer (barreira inválida); cache de conversão de índices não invalidado por escrita do guest; viewport com origem negativa (Blue Dragon); relaunch que matava threads e deixava o lock do relógio preso; viewport reaproveitado entre draws (CPU) | `3e9d62ffc4`, `29fcaeac32`, `915532b832`, `1b0e9d00ea`, `6579cc2a4c`, `2ab3083ef8`, `b308af27c7` | P–M | Baixo–Médio |
| **G13** Versão do cache de pipelines | Subir a versão de uma modificação nunca invalidava o cache de pipelines (o código tomava o máximo das versões em vez de somar). Pré-requisito dos temas que mudam shaders (G1, G2, G5, G6, G10) | `30a2ef6fc9` | P | Baixo |

### Kernel

| Tema | O que corrige ou melhora | Commits | Esforço | Risco |
|---|---|---|---|---|
| **K1** Escalonador de fibras: correções | Risco de crash numa espera alertável que lê o TLS depois de a fiber trocar de thread (visto no edge; aqui depende de o compilador reaproveitar o endereço do TLS); prompt de nome do Oblivion; menu do Halo: Reach | `d4b1ae936c`; `e0e1cf7e31` → `086b4421df`, `d275d8e30f` | M | Médio |
| **K2** `signal_state` escrito pelo guest | Crashes em jogos musou e trava no loading do Crackdown (rádio) | `54fd4a3078` | M | Médio |
| **K3** Timers e APCs | Uma XAPC por disparo de timer vazava até esgotar o heap; tutorial do Eternal Sonata; IRQL preso em APC_LEVEL | `75b54532c8` → `50d9996230`, `1c45c01643` | M | Baixo–Médio |
| **K4** I/O e objetos | `NtOpenFile` com argumentos trocados (erros de leitura no Dead Rising); `ObCreateObject` com nome derruba o app; `\??\` em links; handles do Burnout Paradise | `887beea697`, `20b97ca617`, `1689b263d3` → `b42d204e0a`, `b09650b146`, `33f3a47ce2`, `672bc8b81a` | P (M no último) | Baixo (Médio no `672bc8b81a`) |
| **K5** Conteúdo XAM | DLC instalada pelo jogo (BF3, FM3); fechar um pacote fechava todos os arquivos do título; saves do RR6 | `2f6baa7514`, `3e4a82b60d`, `ed0e06112c`, `0e5bc07239`, `8ae1c14856`, `aac25ad0c2` | P | Baixo |
| **K6** Winsock e XSocket | `select` bloqueante parava todas as fibers da CPU; `fWaitAll` invertido; estouro de buffer em `accept` | `43d796754f` e mais sete | M | Médio |
| **K7** Entrada com UI ativa | O Crackdown esgota o heap drenando teclas com um diálogo aberto (os diálogos Android marcam a UI como ativa) | `17c22494bf` → `3d06054b86` | M | Baixo |
| **K8** Perfil e cripto | Perfil com idioma lido como controle parental; settings sobrepostos; corrida nas propriedades do perfil; gamertag estourando em `CreateAccount`; RC4, SHA, RSA, MD5 e SHA-512 errados ou vazando; AES-CBC com bloco parcial escrevendo além do buffer | `ec38c30cd2`, `0d1a38e7e1`, `6ff2a56f6a`, `f41ee57bc3`, `907d92bf8c` (só o gamertag), `58e1db8e78`, `6db5a346d2`, `a0afe67b93`, `ac1ece3086`, `ddfe2b87f9`, `566a58ebf0` | P–M | Baixo |
| **K9** Avulsos | Música do Black Ops II (XMP); exceção C++ do guest chegando ao `catch(...)`; pausa de spin em arm64 (o mutex global gira 128 vezes sem pausa; medir); `XUserMuteListQuery` | `2f858c4629`, `8c74cfd0c0`, `7ea10c4f2d`, `581ba93e72` | P | Baixo |

### Áudio, arquivos, memória e base

| Tema | O que corrige ou melhora | Commits | Esforço | Risco |
|---|---|---|---|---|
| **M1** Decodificador XMA "new" | LEGO travando nas cutscenes, NBA Live 06 travando a thread de áudio, BGM com falhas nos musou, Tekken Tag 2 sem áudio. O app usa `xma_decoder = "new"` | `9d8210b32d` → `052365bc01` → `adf56b76c4` → `5dd1cdbbf5` | P | Baixo |
| **M2** E/S de arquivos no POSIX | `FILE_APPEND_DATA` abre com `O_APPEND`, e no Android o `pwrite` então ignora a posição: as escritas posicionadas vão para o fim e corrompem o arquivo (conferido em `base/filesystem_posix.cc:208`). Também rename, delete-on-close e `stat` | `c9536890c9`, `b9e00e6cce`, `b495439ba9`, `d8e5dae74e`, `f65fda3320`, `211e7d9531`, `037eef5403` | P–M | Baixo |
| **M3** Relógio do guest | `KeQueryInterruptTime` anda 100× rápido no Android: o contador vem em ns, e o guest espera unidades de 100 ns (conferido em `base/clock_posix.cc:70`). A hora do guest também fica adiantada depois de um relaunch | `a1c2b745ba`, `79feeb9095` | P | Baixo |
| **M4** Audio Worker fora da trava global | Ataca a parada de 62 ms que o PR #36 deixou em aberto | `12e3b4223d` (parte) | M | Alto (`audio_system.cc` muito próprio) |
| **M5** GoD/SVOD de disco como mídia óptica | O WatchDogs em GoD procurava a instalação no lugar errado; a cvar `media_type` força o tipo por jogo | `7be830a2dd` → `95a3d08948` | P | Baixo |
| **M6** Conversão PCM em NEON | No ARM64 a conversão é escalar e trunca; com NEON ela arredonda como o x86 | `b899a97a7e` → `32177527fa` | P | Baixo–Médio |
| **M7** Memória | O teto do `AllocRange` dá a volta perto de `UINT32_MAX`; `munmap(p, 0)` falha e vaza a cada jogo | `87c2411270`, `614fedfbe3` | P | Baixo |
| **M8** Avulsos | Driver nulo para "sem áudio" (adaptar à fila); dump de crash que desreferencia nulo; slot vazio no Madden 25; `plugins.toml` por título no POSIX; migração de `Headers`; `NtQueryEvent`; gate de `/dev/shm` | `1406d87095`, `7b2ea9f9ac`, `f213cc6a9f`, `9d08d64b5d`, `f7c90d0b5c`, `1c81b86747` (só o `PosixEvent::Query`), `b6e27cb4f9` (só a parte que tira o gate) | P | Baixo (Médio no `7b2ea9f9ac`) |

## 5. Patches de jogos

- **Cópia atual.** `patches/xenia-canary` é o `game-patches` `3553a5a` (2026-06-23).
- **Depois disso.** 24 commits: 22 patches novos, 3 removidos e 382 mudados; a maioria das
  mudanças troca links do redump ou faz limpeza.
- **Novos ou melhorados:**
  - Sonic & All-Stars Racing Transformed, Tornado Outbreak, State of Decay, Angry Birds Star Wars,
    Rumble Roses XX;
  - Fable II GOTY (tick rate) e Kameo (câmera);
  - Gears of War 2, 3 e Judgment em ultrawide;
  - FPS do Assassin's Creed e do Ticket to Ride, Black Edition do NFS Most Wanted, música do
    Bean (GoldenEye);
  - letterbox do Dragon's Dogma, pós-processamento do Wet e do Just Cause, oclusão no UE3.
- **Formato.** Os tipos usados (`be32`, `be8`, `be16`, `f32`, `string` e `array`) são todos aceitos pelo patcher
  do Xendroid+ (`patcher/patch_db.h`).
- **Como atualizar.** Copiar `patches/` do `game-patches` para `patches/xenia-canary/patches`;
  o build já empacota os `*.patch.toml` em `assets/game-patches/`. Esforço P, risco baixo.

## 6. Plano de porte em lotes

Primeiro o que é pequeno e corrige dados ou jogos; por último o que precisa de medição. Cada
lote é um PR a partir do `main`, com testes de host e os jogos citados quando houver.

| Lote | Temas | Por que nessa ordem | Esforço | Validação |
|---|---|---|---|---|
| 1 | M2, M3, M7, K4, K5 e os patches | Corrompem arquivos e saves ou o relógio do guest; quase tudo aplica limpo | P | Testes de host; boot de alguns jogos |
| 2 | G13 primeiro; depois M1, G1, G3, G5, G6, G7, G10, G12, K9, C1, C2 | Correções pontuais de jogos e de semântica. O G13 vem antes porque as correções de shader só chegam a quem já tem o cache de pipelines se ele for refeito | P–M | Jogos citados; Crysis 3 e RotTR para o G3; medir o `7ea10c4f2d` |
| 3 | K1, K2, K3, K6, K7, G11 | Crashes e travas sob o escalonador de fibras, que é o padrão; efeitos perdidos com o cache de pipelines frio | M | Oblivion, Halo: Reach, Crackdown, musou; NFS The Run e FM3 com o cache limpo |
| 4 | C3, C4, C5, C6, K8, M5, M8 | Codegen errado e corridas em ARM64; perfil e cripto; mídia de GoD | M | Testes de CPU do upstream; GTA IV: Episodes from Liberty City (japonês) para o SHA; jogos |
| 5 | G2, G4, G8, G9 | Adaptar ao resolve, ao fetch e ao cache de texturas próprios do Xendroid+ | M | 545407D4, 555308CE, 5451080D, 4D530808, 534307D5 |
| 6 | C7, M4, M6 e a promoção de VEC128 (depois do C2) | Desempenho: só com A/B no aparelho | M–G | A/B no POCO F7 |

### O que mudou ao portar (2026-10-16)

O lote 1 virou o PR #40 e o lote 2, o PR #41. Na revisão de cada um, alguns commits
saíram do lote, porque dependem de outro que ainda não está aqui ou porque esbarram em código
próprio do Xendroid+:

| Commit | Tema | Por que saiu | Vai com |
|---|---|---|---|
| `2f6baa7514` | K5 | O diretório de conteúdo criado pelo jogo precisa do `f5c8af81a`, que monta pacotes guardados como arquivo. Sem ele, o jogo copia um STFS cru que nunca é listado e que bloqueia os instaladores do Xendroid+ | `f5c8af81a` e `16ba84808` |
| `87c2411270` | M7 | Muda o comportamento do heap com alinhamento físico do Xendroid+ e reprova `physical_heap_test.cc` | O teste atualizado e um jogo que use `MmAllocatePhysicalMemoryEx` em 0xE0000000 |
| `d8e5dae74e` | M2 | Um arquivo com outro handle aberto deixa de poder ser sobrescrito, e o `WriteGpd` do perfil usa esse caminho | Truncar no lugar quando há handles abertos; vem primeiro no próximo lote de arquivos |
| `8ae1c14856` | K5 | O ramo novo nunca roda neste código | `b11458e49`, se fizer falta |
| `1222c23f78` | G7 | A espera de memexport do Xendroid+ (readback adiado, motivos de espera) tem outro desenho | Lote 5 |
| `10da45ea40`, `4d83110dcd` | C1 | O XER[OV]/[SO] das formas OE põe um load e um store de contexto em todo compare. O upstream compensa com o passe de stores de CR mortos, que não está aqui | Lote 6, com `689590ba3c` e `f6f9a03768` (C7) |

A revisão do lote 1 pediu três coisas, que entraram nele:
- `3ecd9d36b`: a mídia de inicialização é procurada pelo ponto de montagem;
- `3fbbc60f9`: as buscas de links simbólicos passam a usar o lock;
- uma checagem própria: um handle de diretório raiz que não é arquivo é recusado.

No lote 2, a revisão pediu duas correções próprias, que entraram nele: o ponteiro do ring publicado
no meio do burst também avisa o progresso da GPU, e a checagem de memexport dos vertex buffers usa o
endereço físico.

O lote 3 virou o PR #42. Saíram dele:
- `c097a6d319`: só limpeza;
- `de8e60601c`, `8a4d405af6` e `e8c05cd05f` (G11): conflitam em mais de 250 linhas com o cache de
  pipelines assíncrono do Xendroid+ e vão para um PR próprio, com teste no aparelho e cache limpo;
- `4e019f4090`, `b62d6f9edd` e `0554993759`: dependem do código de System Link, que não está aqui.

Entraram nele:
- o `728c93d652` (`SignalAndWait` entra na espera antes de sinalizar), que a triagem dera como
  presente;
- três correções próprias, achadas na revisão:
  - o `select` no Android (sockets fechados, `nfds` do POSIX, falhas);
  - a ordem das escritas ao liberar um mutant em ARM64;
  - o rundown de APCs na saída de thread.

O que a triagem e o porte deixaram passar, e como se confere agora:
- **Reordenações contadas como presentes.** A triagem automática mede a fração das linhas
  adicionadas que já existem no arquivo, e um commit que só muda linhas de lugar dá 100%. Foi o caso
  do `728c93d652`.
- **Presença parcial que esconde o essencial.** O `947075f88` (o 1D largo do upstream) estava 93%
  presente, mas os 7% que faltavam eram as correções: o centro da linha e o passo de 8192 texels. As
  partes que faltavam entraram no lote 5.
- **Hunks no lugar errado.** O `git am -3` aplica um hunk onde o contexto bate, e no lote 3 dois
  caíram em outra função com contexto idêntico: o "recv devolve 0" em `Send`/`SendTo` e uma marcação
  de re-poll no ramo errado de `WakeForSignal`. A revisão pegou os dois. Agora um script compara, em
  cada commit portado, as funções dos hunks com as do upstream. Nos lotes 1 e 2 não havia outro
  caso.

O lote 4 virou o PR #44. Saíram dele `ac1ece3086`, `ddfe2b87f9` e `566a58ebf0`: a cripto do kernel
reescrita sobre o Mbed TLS, que não estava aqui. Os bugs que eles corrigem são reais:
- o MD5 escreve além do estado do guest;
- o SHA-512 devolve o digest errado;
- o AES guarda as chaves de decifra na ordem errada e passa do fim do buffer num bloco parcial de
  CBC.

Ficaram para um PR próprio, que virou o PR #45 (abaixo).

Entrou a parte do `vec128.h` do `2a1097177`, um commit de x64 que a triagem deixou de fora: os
construtores de `vec128` viram `constexpr`, e o `a5f59a48ae` precisa disso para compilar.

O lote 5 virou o PR #43, que sai do lote 2 (#41) porque mexe nos mesmos arquivos. Entraram:
- as partes que faltavam do `947075f88`;
- o `2248561d8`, que dá um `Memory` aos testes do `PhysicalHeap`;
- uma correção própria: texturas 1D largas filtravam sempre com point.

O resolve A8 com fonte 4x vai pelo caminho do EDRAM, porque o shader direto de 4x lê RGBA com a
troca vermelho/azul.

Fica para depois o `7347cd12e` (resolve com o comando de cópia 2). Sem ele, as máscaras de Kinect do
5345084D ainda não funcionam, embora a ordem de bits de `k_DXT3A_AS_1_1_1_1` já esteja certa.

O lote 1 (PR #40) recebeu os commits de arquivos que tinham saído dele: `16ba848086`,
`f5c8af81a5`, `2f6baa7514` e `d8e5dae74e`. Com o `d8e5dae74e`, uma entrada com handles abertos
recusa ser apagada, por isso veio junto uma correção própria: sobrescrever um arquivo que outro
handle mantém aberto o trunca no lugar, como no NT. A revisão achou mais doze problemas nesses
commits ou expostos por eles, corrigidos no próprio lote em dez commits. Os principais:
- um device desmontado com handles abertos fica vivo até o último handle fechar;
- o app não cai mais com um pacote guardado como um arquivo só (miniatura) nem ao criar um
  diretório dentro de um pacote que o Xendroid+ mostra como arquivo;
- abrir um arquivo com exclusão pendente devolve `STATUS_DELETE_PENDING`, como no NT;
- sobrescrever um diretório não apaga mais a árvore inteira;
- handles e arquivos que ficavam abertos por engano agora fecham;
- o gerenciador de conteúdo, o instalador e a lixeira do app aceitam o pacote em arquivo.

O resto ficou anotado no PR #40.

A cripto do kernel virou o PR #45, que sai do lote 4 (#44). Ele traz:
- o Mbed TLS 3.6.7 copiado para a árvore, só os fontes de cripto que o build usa;
- os 11 commits da série do upstream, de `4101dfa07` a `ddfe2b87f9` (no upstream há dois
  commits de fora da série entre eles, `6ffd0fe70` e `113837b0a`);
- cinco correções próprias pedidas pela revisão:
  - as tabelas do AES como dados constantes: o Mbed TLS as monta na primeira chave atrás de uma
    flag sem sincronização (em ARM64 outra thread pode ver a flag antes das tabelas), e o
    `XeCryptAesEcb`/`Cbc` pode rodar sem que nenhuma chave as tenha montado;
  - limites no RSA e na compressão básica do XEX;
  - o link dos testes do kernel;
  - a licença do Mbed TLS no app e nos avisos; o `aes_128` sai da página de licenças.

Saves gravados por builds anteriores com HMAC incremental, SHA-512 ou bloco CBC parcial podem
falhar na verificação do jogo.

O lote 6 virou o PR #46, que sai de um merge dos lotes 2 e 4. Ele traz os 21 commits de
desempenho, com o passe de stores de CR mortos e o XER[OV]/[SO] que tinham saído do lote 2, e
duas correções próprias.
- **Diferença deliberada do upstream (`70a91c551`):** o passe de CR morto deixa passar o
  `CHECK_PREEMPT`. Com o escalonador de fibras ligado (o padrão, aqui e no upstream), há um em
  cada cabeça de laço, e o passe o tratava como leitura de todo o CR. Assim os stores de CR dos
  laços ficavam, e com eles o custo do XER[SO]: só com essa regra o passe compensa esse custo
  dentro de laços, como a tabela acima supõe. Um porte futuro do
  `dead_cr_store_elimination_pass.cc` precisa manter a regra.
- **Mixagem 5.1 → estéreo em NEON (`003912673`):** trata as amostras depois do último grupo de
  quatro.
- **A/B no POCO F7 (18/10):** Forza Horizon em jogo, no `.fork.ui`, quatro rodadas alternadas
  contra o merge dos lotes 2 e 4. O lote tira 11,4% das instruções por quadro das threads do guest
  e 7,7% das do processo inteiro, nos mesmos 30 fps e com o mesmo tempo de GPU.
- **Fusão de compare e desvio (`aba5703fa0`):** escrita para o emissor daqui, porque a
  infraestrutura de handoff do upstream não está aqui.
- **Ficaram de fora:**
  - as chamadas mais baratas (`b416d19675`, `7a97888962`, `6fc16d118d`): dependem do rastreio de
    FPCR por bloco e do alcance de desvios do upstream;
  - o ritmo novo do áudio do `12e3b4223d`: o top-up do Xendroid+ já repõe depois de uma parada;
  - a promoção de VEC128, que continua desligada: aqui ela deforma os corpos no Halo 4.

## 7. Fora da lista

- **Backend A64 de Will Martin (oaknut):** o próprio edge reverteu.
- **XeFu** (jogos do Xbox original pelo `xbox.xex`): grande, com dependências em CPU, kernel e
  memória (Talvez).
- **System Link** (túnel UDP e XLink Kai): daria para usar no Wi-Fi, mas o custo é G (Talvez).
- **I/O de arquivo assíncrono e modelo de tempo de leitura** (PGR3): Talvez, medir.
- **Série de NaN e FPSCR:** os fixups estão desligados no Xendroid+ (Talvez).
- **UBO dinâmico incondicional** (`bfca9b2d97`): o Xendroid+ mediu que o driver proprietário da
  Qualcomm lê zeros por esses descritores e só o liga no Turnip (`d9816ebe9`).
- **D3D12, Metal, backend x64, UI desktop e CI.**
