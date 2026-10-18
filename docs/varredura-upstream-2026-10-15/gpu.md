# Revisão de commits upstream: GPU e Vulkan (Xendroid+)

Revisão de 301 commits (298 de `has207/xenia-edge` e 3 de `xenia-canary`) que tocam a GPU compartilhada e o backend Vulkan, comparados com o núcleo do Xendroid+ em `emulator-core/src/main/cpp/xenia`.

**Sobre a base.** O núcleo do Xendroid+ já contém o histórico do edge até `e25bfd824` (2026-08-01), trazido pelo sync do XenDroid de 2026-08-05 (merges "Sync upstream xenia-edge ...", PR #75), e não só até 2026-06-29. Os 147 commits da lista com data até 2026-08-01 são todos ancestrais desse ponto; para cada um, as linhas adicionadas foram procuradas no Xendroid+ e, quando ausentes, no upstream em `e25bfd824` (para separar "superado depois no próprio upstream" de "divergência do Xendroid+"). Quase todos estão presentes ou foram superados upstream; as poucas divergências são adaptações deliberadas do XenDroid/Xendroid+ (conferidas uma a uma). O trabalho real está nos 154 commits posteriores.

**Cuidado com `apply=fwd`.** Alguns commits aplicam "limpo" mas já estão no Xendroid+ (ex.: `7c999ca76d`, cujo bloco já existe; `94847b4f37`, já substituído por `f8d15108c`). O veredito abaixo vem da leitura do código, não da coluna `apply`.

**Adreno.** O Xendroid+ registra que nenhum Adreno expõe `VK_EXT_external_memory_host` (`gpu/vulkan/vulkan_command_processor.cc:7937`), então os caminhos upstream de "host buffer" (guest RAM importada) não rodam lá; o equivalente local é o modo `readback_resolve=uma`. O caminho FSI (`render_target_path=accuracy`) exige `VK_EXT_fragment_shader_interlock`, que os drivers Adreno normalmente não expõem; o padrão é `performance` (render targets do host).

## Resumo

Commits na lista: 301. Revisados: 301.

| Veredito | Commits |
|---|---|
| Portar | 38 |
| Talvez | 41 |
| Já tem | 122 |
| Não se aplica | 100 |

Temas de maior valor:

1. Draws únicos esperam o pipeline real em vez de serem pulados com cache frio: o Xendroid+ pula por padrão (de8e60601c, 8a4d405af6, e8c05cd05f).
2. Conversão 7e3→8_8_8_8, ligada por padrão no Xendroid+, decodifica o buffer sem olhar a write mask (a2b609d22d).
3. PM4: `PM4_WAIT_IB_PFD_COMPLETE` descartava o resto do ring; read pointer publicado durante o burst (3e9d62ffc4, 29fcaeac32).
4. Endereços em janela espelho derrubavam draws de vertex fetch, índices e memexport (2fce2d8957).
5. Invalidação: cache de conversão de índices e páginas recém-alocadas sem watch, além de cargas de textura duplicadas (1b0e9d00ea, d1541a579b, 40cee11ab1).
6. Layout de texturas na memória do guest: arrays, volumes, 96bpp, tiled 3D, mip tail, base level (7 commits).
7. Resolves: A8 com alfa, offsets tiled abaixo de 32bpp, fatias volumétricas, MSAA >4x, barreiras do resolve escalado (5 commits).
8. Tradutor: operando escalar com ALU de 3 fontes, resultado do kill, mip travado (92ada8ebc0, e7595ef605, 8486e97a06).
9. Texturas com num_format inteiro e 1D largas (6a45452087, 052cb95f24, fbdb1f2817, 7cd47947b0, 6260a87b85).
10. Robustez: scratch buffer, versão do pipeline storage, viewport negativo, relaunch sem matar threads host, XeCryptAesCbc (915532b832, 30a2ef6fc9, 6579cc2a4c, 2ab3083ef8, 566a58ebf0).

Os 147 commits datados até 2026-08-01 já estão no Xendroid+ ou foram superados no próprio upstream (sync do XenDroid de 2026-08-05); por isso quase todos os vereditos "Já tem" e muitos "Não se aplica" estão nessa faixa. Não portar `bfca9b2d97` (UBO dinâmico incondicional): o Xendroid+ mediu que o driver Qualcomm proprietário lê zeros por esses descritores e só o liga no Turnip (d9816ebe9).

## Temas recomendados

Ordenados do maior para o menor valor. Caminhos relativos a `emulator-core/src/main/cpp/xenia/src/xenia/`.

| Tema | Commits (sha curto) | O que traz para o Xendroid+ | Evidência (arquivo/função) | Dependências | Risco de conflito | Esforço | Veredito |
|---|---|---|---|---|---|---|---|
| Draws únicos esperam o pipeline real em vez de serem pulados | de8e60601c → 8a4d405af6 → e8c05cd05f | Com `vulkan_async_skip_draws` ligado por padrão, todo draw cujo pipeline ainda compila é pulado. Num pass refeito a cada frame isso é só pop-in, mas um render-to-texture que não se repete (RT não desenhado nos últimos 4 frames, RT pequeno de dados gerados) fica perdido. Com essa regra o upstream corrigiu, com cache frio, os arbustos do NFS The Run, Soul Calibur V, a estrada piscando no FM3 e a chuva vertical do Halo Reach. 8a4d405af6 evita esperas inúteis (VS já desenhado, warm-up do storage) e e8c05cd05f cria só o pipeline aguardado, sem drenar a fila | `gpu/vulkan/vulkan_command_processor.cc:6683-6688` só espera em draws com memexport e chama `AwaitPipelineCompletion()` (fila inteira); não há `GetPipelineStandInWaitReason`/`TrackLastUpdateDrawTarget` em `render_target_cache` | Adaptar ao modelo deferred-bind do Xendroid+; o "expedite" pede uma busca na fila de prioridade própria (não precisa da refatoração cdfe727449) | Médio | M | Portar |
| Conversão 7e3→8_8_8_8 respeita a write mask | a2b609d22d | A conversão por valor no reuso de tiles 7e3→8_8_8_8 vem ligada por padrão no Xendroid+. A detecção de blend soma o destblend de cor e o de alfa sem olhar a write mask, e casa qualquer slot com o mesmo base EDRAM; um draw que só escreve vermelho fazia decodificar o buffer inteiro (cena embaralhada ao mirar em Gears of War: Judgment) | `gpu/render_target_cache.cc:245` (`value_convert_7e3_8888_reuse`, padrão true) e `IsTransferValueConverted7e3And8888` (~2141); a parte em `render_target_cache.cc/.h` aplica limpo | Nenhuma (o resto do diff só remove código morto de `edram_transfer_shader.cc`, que não existe aqui) | Baixo | P | Portar |
| Pacotes PM4 e ponteiro de leitura do ring | 3e9d62ffc4, 29fcaeac32 | `PM4_WAIT_IB_PFD_COMPLETE` não era tratado, e o resto do ring buffer primário era descartado sempre que um título o enviava. O read pointer passa a ser publicado a cada RB_BLKSZ dwords, liberando espaço no ring durante bursts longos (o guest deixa de esperar trabalho já feito, e some o impasse com um WAIT_REG_MEM no meio do burst) | `gpu/pm4_command_processor_implement.h` não tem o case (o opcode só existe em `gpu/xenos.h:1634`); `ExecutePrimaryBuffer` (~1943) publica só no fim; `gpu/command_processor.cc:1530` calcula `read_ptr_update_freq_` (hoje sem uso) numa unidade que o upstream corrigiu para dwords | Nenhuma | Baixo (3e9d aplica limpo); Médio (29fc mexe no laço quente) | P | Portar |
| Endereços em janela espelho derrubavam draws | 2fce2d8957 | Vertex fetch, índices (VGT_DMA_BASE) e memexport com endereço em janela espelho (0xC0000000 etc.) eram rejeitados como fora da shared memory, e o draw era descartado; agora o endereço é mascarado para físico na CPU e nos shaders | `gpu/vulkan/vulkan_command_processor.cc:7139` e `:7162` copiam `vfetch_constant.address` sem máscara; `gpu/primitive_processor.cc` usa `VGT_DMA_BASE` cru; `spirv_shader_translator_fetch.cc`/`_memexport.cc` sem `& 0x1FFFFFFF` | Nenhuma | Baixo (só o `kVersion` conflita) | P/M (dois pontos de vfetch próprios do Xendroid+) | Portar |
| Cache de conversão de índices | 1b0e9d00ea | Entradas do cache de conversão de primitivas que terminam dentro da faixa escrita pelo guest não eram invalidadas (`entry_end > physical_address_end`), e uma conversão feita enquanto a faixa era escrita ia para o cache: índices velhos e geometria quebrada | `gpu/primitive_processor.cc` (`MemoryInvalidationCallback`, `~CacheTransaction`); aplica limpo | Nenhuma | Baixo | P | Portar |
| Texturas: invalidação na alocação e cargas duplicadas | d1541a579b, 40cee11ab1 | `Alloc`, `AllocFixed` e `AllocRange` não invalidam páginas que um upload marcou válidas sem write watch, e o Xendroid+ marca assim as páginas sem acesso; a escrita do guest depois da alocação passa despercebida e a textura carrega bytes velhos (regressões em Dynasty Warriors 5/6 Empires, no upstream). Uma textura ligada a dois fetch constants entrava duas vezes no lote e era carregada duas vezes | `memory.cc` (`PhysicalHeap::Alloc` ~1960, `AllocFixed`, `AllocRange` sem `TriggerCallbacks`; só `Decommit`, `Release` e `Protect` chamam); `gpu/vulkan/vulkan_shared_memory.cc:892` (`skip_span`); `gpu/texture_cache.cc:501-504` e `LoadTexturesData` (~1032, snapshot antes de carregar) | Nenhuma | Baixo | P | Portar |
| Layout de texturas na memória do guest | bc6b7a0202, 19da174033, c9c8e483b1, ace153cb84, 81e3deaee2, c332733afd, 4a863a0e1a | Mips de arrays alinhados a 4 fatias e volumes com profundidade reduzida por nível; row pitch de mips lineares de 96bpp; limites de endereço tiled 3D (máscara de Z errada, bank swap, uma página extra puxada); mip tail empacotado no nível 0 lido do endereço base; strides de fatia com dimensões arredondadas; nível base preservado quando `mip_min_level` é não nulo com base e mip distintos (534307E2) | `gpu/texture_util.cc` (`GetSubresourcesFromFetchConstant`:91-97 zera o base com `mip_min_level`); `gpu/texture_util.h` (`GetTiledAddressLowerBound3D` usa `front & ~kTextureTileDepth`); `gpu/vulkan/vulkan_texture_cache.cc` `GetSamplerParameters` | Em ordem: bc6b, 19da, ace1, 81e3 e 4a86 aplicam limpo em sequência (testado numa cópia); c9c8 pede ajuste de contexto no cabeçalho, e c332 depende dele | Baixo/Médio | M | Portar |
| Resolves: formatos, endereços e barreiras | 2b3f0cb456, 89609297c7, d8731edc99, 830e605186 (só o check), c8ac9258f0 | Resolve A8 (k_8 + LOW_BLUE) seleciona o alfa: plano de opacidade de 5451080D e neblina de 4D530808. Offset tiled abaixo de 32bpp no RB_COPY_DEST_BASE (refração da água em 534307D5). Espaçamento de fatias de resolve volumétrico via RB_COPY_SURFACE_SLICE em vez de `copy_dest_height`, que pode incluir o topo do retângulo de origem. Resolve clear recusa MSAA acima de 4x (antes indexava arrays fora do limite em release). Barreiras do resolve escalado passam por `Push*Barrier`, que fecha a render pass | `gpu/shaders/resolve.xesli:351`/`:449` e `resolve_host_color.xesli:704`/`:773`; `gpu/draw_util.cc` `GetResolveInfo` (89609 e d8731 aplicam limpo); `gpu/render_target_cache.cc:1832` só tem `assert`; `gpu/vulkan/vulkan_render_target_cache.cc:4207-4290` grava `CmdVkPipelineBarrier` direto | 2b3f exige adaptar o resolve direto próprio (`resolve_host_color.xesli`) | Médio (2b3f); Baixo (demais) | M | Portar |
| Tradutor de shaders: semântica | 92ada8ebc0, e7595ef605, 8486e97a06 | Em operações escalares de dois componentes, o componente `b` é Z (não X) quando a ALU vetorial usa a fonte 3; o parse do microcódigo pegava o componente errado. Kills gravam 1.0/0.0 no destino, e o SPIR-V devolvia sempre 0 embora o demote deixe a execução seguir. Fetch 2D não normalizado com mip travado passa a usar o tamanho do mip (popping de visibilidade em 555308B6/5553080B) | `gpu/shader_translator.cc` (`ParseAluInstructionOperand`); `gpu/spirv_shader_translator_alu.cc:938-949`; `gpu/spirv_shader_translator_fetch.cc` sem `selected_mip_locked`; os três aplicam limpo | Subir a versão dos pipelines armazenados (ver 30a2ef6fc9) | Baixo | P | Portar |
| Escala de num_format inteiro por canal | 6a45452087 → 052cb95f24 | A escala que devolve valores inteiros percorria o swizzle do host (canais de 5 e 6 bits trocados, ex.: 5_6_5) e não limitava o canal-fonte ao último canal armazenado (k_16, k_5_6_5) | `gpu/texture_cache.cc:436` passa `binding.host_swizzle`; `GetIntegerScaleBits` (`:945`) sem o limite; vale para os títulos com `texture_integer_num_format` (quirks de Crysis 3 e Rise of the Tomb Raider) | 052c depois de 6a45 | Baixo | P | Portar |
| Fetch de texturas 1D e 1D largas | fbdb1f2817 → 7cd47947b0 → 6260a87b85 | tfetch1D com swizzle XY amostra como 2D (a UI de 545407D4 amostrava com Y=0); layout promovido escolhido em runtime (bindings 1D largos de 555308CE); texel do 1D largo calculado do operando original (precisão com larguras até 2^24); teto de linhas materializadas (32, depois 128), que evita texturas 2D enormes | `gpu/spirv_shader_translator_fetch.cc` (~958-1310, remapeamento 1D largo sem teto); `gpu/texture_cache.cc:1277` sem `kTexture1DWideMaxRows`; `gpu/shader_translator.cc` sem o caso de 2 componentes | Em ordem | Médio (hunk em ~847 do fetch não aplica) | M | Portar |
| Robustez Vulkan e cache de pipelines | 915532b832, 30a2ef6fc9, 6579cc2a4c, b308af27c7 | Máscaras de estágio e acesso trocadas ao crescer o scratch buffer (barreira com flags inválidas). Versão do pipeline storage como `max(data, contador)`, de modo que subir `Modification::kVersion` nunca invalida o cache (importa ao portar mudanças do tradutor). Viewport do host com origem negativa (tile com offset −608 do Blue Dragon 4D5307DF virava cunha). Cache do viewport do host entre draws (CPU do thread de comandos) | `gpu/vulkan/vulkan_command_processor.cc:5812-5813`; `gpu/vulkan/vulkan_pipeline_cache.cc:3227-3229`; `gpu/draw_util.cc` `GetHostViewportInfo` (o offset só vira float em `vulkan_command_processor.cc:10048`); `GetHostViewportInfo` chamado a cada draw (`:6944`) | Nenhuma | Baixo | P | Portar |
| Relaunch e reset sem matar threads host | 2ab3083ef8 | Relaunch/reset terminavam também o frame limiter e o worker de áudio; um relaunch travava no lock do relógio do guest deixado preso | `emulator.cc:1601-1604` e `:1749-1752` terminam todas as `XThread`; `XThread::is_host_thread()` já existe (`kernel/xthread.h:721`) | Coordenar com a revisão de kernel/emulador | Baixo | P | Portar |
| XeCryptAesCbc com bloco final parcial | 566a58ebf0 | O laço do CBC processa 16 bytes mesmo quando `inp_size` não é múltiplo de 16, lendo e escrevendo além dos buffers do guest | `kernel/xboxkrnl/xboxkrnl_crypt.cc:936`; o upstream troca por Mbed TLS, ausente aqui (`third_party/mbedtls-xenia`), então corrigir sobre `aes_128` | Coordenar com a revisão de kernel | Baixo | P | Portar |
| Espera de memexport só até a submissão do último export | 1222c23f78 | As esperas de fence/coerência de memexport aguardavam a fila inteira; passam a aguardar só a submissão do último draw com export | `gpu/command_processor_memexport.inc:53` e `:77` chamam `AwaitAllQueueOperationsCompletion()`; `game_quirks.cc:340` e `:380` ligam `memexport_await_fences` (4541098E, 53510823) | Nenhuma | Baixo | P | Portar |
| Ordem de bits de k_DXT3A_AS_1_1_1_1 | ac37478be6 | Vermelho (X) fica no bit menos significativo de cada grupo de 4 bits e alfa (W) no mais significativo, o contrário do que a decodificação supunha; achado no título Kinect 5345084D, mas vale para qualquer título que use o formato | `gpu/shaders/pixel_formats.xesli`; aplica limpo | Nenhuma (7347cd12ef, o resolve para esse formato, ficou em Talvez) | Baixo | P | Portar |

## Talvez

| Tema | Commits (sha curto) | O que traz para o Xendroid+ | Evidência (arquivo/função) | Dependências | Risco de conflito | Esforço | Veredito |
|---|---|---|---|---|---|---|---|
| Readback de resolve copiado no acesso da CPU (redesenho upstream) | ccb602b763 → 77f2cca800 → 935e038766 → 34387b31f5 → 776dda2e33 | Sem semear a RAM no primeiro resolve (o upstream viu crashes no AC Rogue com a semeadura); screenshots in-game (Beyond Good & Evil, RRXX); cópia por página só quando a CPU toca; clip das páginas de borda já escritas pela CPU; cópia fora do lock global; escala de resolução (Tekken Tag 2 a 2x) | O modo `uma` do Xendroid+ ainda semeia no primeiro resolve, e a descrição de `readback_resolve_uma_read_watch` (`gpu/command_processor.cc:168`) admite que a escrita parcial da CPU numa página devolve dados velhos. O desenho upstream supõe host buffer ou buffer de readback; no Adreno (sem `VK_EXT_external_memory_host`) o caminho é o `uma` | Série inteira | Alto | G | Talvez |
| Predicated tiling: offset de janela no base do EDRAM | e1f4a0d009 | Depth com lixo nos tiles de baixo em 4D530919 (Halo 4), 425607E5 e 5345080C, títulos com tiling predicado e depth prepass em resolução cheia | Conflita com `merge_tiling_bands` (Forza Horizon), que move draws por viewport/scissor; 478 linhas em RT cache, draw_util e estimador de extensão | 6579cc2a4c | Alto | G | Talvez |
| Aliasing cor/depth no mesmo base EDRAM | 9da693480d | Mantém o depth ligado só para leitura quando a cor escreve apenas o byte de stencil (nuvens e sprites atravessando a geometria em 4D530A26); nova cvar `aliased_depth_read_only` | Mexe no ownership do RT cache, que o Xendroid+ modificou muito (transferências in-pass, 4x como 1x) | Nenhuma | Alto | M | Talvez |
| Novo layout de amostras no EDRAM | 437a7280cf, 0bd090dbe9 | Um só esquema de endereçamento de amostras para 1x/2x/4x (passes 4x de meia resolução sobre a cena); 0bd090dbe9 corrige o host depth store nesse esquema (o deslocamento do 4x talvez exista também no esquema atual) | Conflita com `vulkan_depth_4x_as_1x` e o armazenamento 4x/2x próprios do Xendroid+ | 0bd0 depende de 437a | Alto | G | Talvez |
| Rasterizar nas posições de amostra do Xenos | cc4981a465 | Corrige sombreamento faltando em títulos UE3 com MSAA em tiles (4D5307D5) | Exige `VK_EXT_sample_locations` (verificar no Turnip e no driver Qualcomm); cvar `xenos_sample_positions` desligada no upstream | Nenhuma | Alto | G | Talvez |
| ZPD como contador contínuo e VIZ_QUERY | 3d233a5b2e, 692cd59cfe | Oclusão mais fiel (flares instáveis em 415608B5, títulos com QueryBatch) e visibility surveys com renderização condicional, que evitam overdraw | O Xendroid+ tem ZPD próprio (segmentos em transferências in-pass); VIZ usa `VK_EXT_conditional_rendering` (verificar no Adreno) | 692c parte do formato de 3d23 | Alto | G | Talvez |
| Precisão de fetch de texturas | 7b2555f1f2 (= 0c843efb32), c3cd8617b1, 669b4266f5, 2c9a7cd07a, 0f2980de44 | Arredondar fetch unsigned normalizado (4D5309C9, Forza Horizon, e 4D530AA4); texturas virtuais do idTech 5 (425307EC); `getBCF` (hoje erro de tradução); LOD implícito em 3D; bias de gradiente no cubemap implícito | 7b25 põe um ramo e arredondamento em todo fetch unsigned normalizado (custo no título mais otimizado do Xendroid+); c3cd é grande; 669b depende de c3cd, 8486e97a06 e fbdb1f2817 | Ver colunas | Médio/Alto | M/G | Talvez |
| Compilação assíncrona: complementos | 7ac465fd6e, 06fb359dee, eec245698e, a7d227aac3, 562e06ff94, cd822c4d04 | Refazer pipelines do storage que falham no início (hoje o slot fica nulo e os draws são descartados até o fim da sessão, `vulkan_pipeline_cache.cc:1278`); fim de submissão sem esperar criação (no modelo de pular draws, adiar o fim reduz perdas); draws medidos por query não devem ser pulados; VS do storage só conta como desenhado após o warm-up; corrida de layout no modo placeholder; analisar só shaders usados por pipelines armazenados (`vulkan_pipeline_cache.cc:3245` analisa todos) | Modelo deferred-bind do Xendroid+ difere do upstream | a7d2 complementa 8a4d405af6 | Médio | M | Talvez |
| Upload de texturas por compute com cópia imagem-imagem | a766c62c79, d5845a4941 | Escreve numa imagem de staging inteira e copia para a textura, que mantém a compressão (sem uso STORAGE) | Motivado pela NVIDIA; o Xendroid+ já carrega direto na imagem no Turnip com Adreno 750+ (`vulkan_texture_load_to_image_auto`), mas não onde STORAGE custa a compressão; medir lá | Nenhuma | Médio | M | Talvez |
| Depth bias e depth clamp | a8d034c00d, 25597a5465 | Bias em unidade constante via `VK_EXT_depth_bias_control` (decals com Z-fighting em depth reverso, 4D5307D5); `force_depth_clamp` global (494707EE, 41560881) | Sem a extensão nada muda (verificar no Turnip e no driver Qualcomm); o Xendroid+ já tem `vulkan_depth_clamp_without_depth`, mais restrito (`vulkan_pipeline_cache.cc:1762`) | Nenhuma | Baixo | P | Talvez |
| Escala de resolução 3x+ | bc9098deb6, 7d0a452637 | Fetch point no centro do pixel guest (explosões de vértices em 43430814 a 3x/4x); linhas com 1 pixel guest de largura via geometry shader (LUT do Dragon's Dogma) | Só importa com `draw_resolution_scale` ≥ 3, raro em celular | Nenhuma | Médio | M | Talvez |
| Aritmética do tradutor | 3cc741caa3, 3390fc219b, bea885689d | NaN/sNaN raros viram zero ou infinito no arredondamento de 21 bits (inclui o caminho rápido padrão do Xendroid+); cvar experimental de arredondamento para zero no `mulsc` (5451080D, 4B4D07F6, 5451086D); clamp de índice dinâmico de constantes (o Vulkan já tem `robustBufferAccess`) | `spirv_shader_translator_alu.cc:74-175` | Nenhuma | Baixo | P | Talvez |
| Memexport: esperar só exports de pixel shader | a7c39fa7d9 | Divide `memexport_await_fences` em pixel (ligado) e vertex (desligado); evita um round-trip de GPU por export de vertex shader | Validar se 4541098E e 53510823 leem na CPU export de VS | 1222c23f78 | Médio | M | Talvez |
| Diversos | 22708301ba, 7347cd12ef, 3989ba3a1b, 44f5b4a860, 0b041cd2a3, a5a18f5c75, fb4270fb3b | XPS inicial (heap 0x7F000000); resolve com comando de cópia 2 (visto só no título Kinect 5345084D); média MSAA do resolve reaproveitando endereços; vblanks atrasados entregues (o pacer do Xendroid+ descarta, o que pode atrasar a imagem em relação ao áudio); registrador RB_HSIO_INTERFACE_ALIGNER_VALUE; micro-otimização do relógio do guest; flags de estágio entre layouts de storage buffer | Ganho incerto ou título não citado | Nenhuma | Baixo/Médio | P/M | Talvez |

## Apêndice: todos os commits

Em ordem de data. Formato: `sha | veredito | motivo`.

- `1e67bd53ac | Já tem | Escala de resolução Vulkan presente (scaled resolve buffers em vulkan_texture_cache)`
- `a12910e953 | Já tem | Sync XenDroid 2026-08: caminho FSI com escala de resolução presente`
- `23d1b48820 | Não se aplica | Otimização SPIR-V em runtime removida upstream (f1dfb08dda); Xendroid+ não usa SPIRV-Tools`
- `60a8d4f933 | Não se aplica | Só bytecode SPIR-V regenerado; Xendroid+ gera os shaders no build`
- `e3961718a8 | Já tem | Pool de threads de criação de pipelines presente em vulkan_pipeline_cache`
- `fee7157176 | Não se aplica | Otimização SPIR-V em background removida upstream (697c116a36, f1dfb08dda)`
- `7ea203f009 | Não se aplica | Só bytecode DXBC (D3D12)`
- `11ee3fa587 | Já tem | Vsync reescrito upstream (39a65586ce); Xendroid+ usa pacer próprio (vblank_pacer.h)`
- `198ed5efec | Não se aplica | Título da janela/aviso de desktop`
- `e8b49d60be | Já tem | readback_resolve Vulkan presente, evoluído (modos fast/all/uma)`
- `ee31a7c66c | Não se aplica | Bytecode regenerado e build.py`
- `eff0ee1ecb | Não se aplica | Correção de vsync no Windows; código substituído por 39a65586ce`
- `596a741988 | Não se aplica | Build (premake)`
- `08d018748a | Já tem | cvar de versão SPIR-V presente`
- `9a6082fd47 | Já tem | Versão SPIR-V conforme capacidades do dispositivo presente`
- `240446ae3d | Não se aplica | texture_conversion.cc removido upstream (ea8ae81bf); inexistente no Xendroid+`
- `3bfcf8ca70 | Já tem | User clip planes presentes (nas system constants)`
- `4779f74e23 | Não se aplica | Flag de otimização SPIR-V em background removida upstream`
- `f395c775f5 | Não se aplica | Otimização SPIR-V inline removida upstream (f1dfb08dda)`
- `eeb65d6102 | Já tem | Superado: planos de recorte vivem nas system constants no Xendroid+`
- `8ade605b0a | Já tem | Superado por 9792c2b95 (readback via host_buffer); modos próprios no Xendroid+`
- `dcc7ccce8d | Não se aplica | Build de testes/subprojetos (premake)`
- `8058781888 | Já tem | Superado por semáforos de present por imagem (2f260a5dc), presente`
- `26df8c6fb8 | Já tem | vertex_fetch_bitmap em uso no Xendroid+`
- `635c5fc391 | Já tem | Sem geometry shader para rectangle list, presente`
- `dc2bc0fdb6 | Já tem | Superado pelo rework de readback_resolve (9792c2b95)`
- `d63156c1c9 | Não se aplica | Framework de testes, revertido upstream (21d631a352)`
- `32a5bf0820 | Já tem | Superado (54f63c6f3, d912a6ff80); estado final presente`
- `3b21f65ab7 | Já tem | Superado pelo roteamento de memexport em dois buffers (70ad7c527e), presente`
- `41c8354822 | Já tem | Superado pelo rework de readback_resolve (9792c2b95)`
- `9dd5c33cb5 | Não se aplica | Superfície Wayland (Linux desktop)`
- `1b544c1fb5 | Já tem | Superado pelo roteamento de memexport em dois buffers (70ad7c527e)`
- `f76c2e0bf4 | Já tem | Superado (70ad7c527e, e1fa5c7749, 9792c2b95)`
- `9c5c950977 | Já tem | Superado; clear_memory_page_state no estado de 2026-08-01`
- `f1f72ecb90 | Já tem | ZPD reescrito (397fc92841) presente: vulkan_zpd_query_pool`
- `0830086f28 | Já tem | Alpha to coverage substituído pelo A2C fixo do host (88cfab725)`
- `c7f64199b3 | Já tem | Alvo de storage kDepth presente`
- `3d3bb60477 | Já tem | Reversão já refletida no estado final de readback`
- `be3c16dc95 | Já tem | Melhorias de A2C presentes/adaptadas`
- `3957b9f732 | Não se aplica | Atalho F5 de desktop`
- `89b571b15b | Não se aplica | Atalho F5 de desktop (invalidação manual)`
- `f200c25c90 | Já tem | Superado por gamma_render_target_as_unorm16, presente`
- `1388ce68b2 | Já tem | Modo slow superado pelos modos atuais (00bf9daa8b, 9792c2b95)`
- `1c670bf3a6 | Já tem | submit_on_primary_buffer_end presente`
- `00bf9daa8b | Já tem | Modos de readback_resolve presentes, adaptados (none/fast/all/uma)`
- `a45d3e6b62 | Não se aplica | Diálogo de desempenho da UI de desktop`
- `19677648e6 | Já tem | Logging de compilação de shaders presente`
- `246ae2f3c2 | Já tem | Superado; passe de correção de gama atual presente`
- `405b2f3920 | Já tem | Invalidação de sets incompatíveis presente, reescrita com cache de valores`
- `496676f68b | Já tem | Superado; escala de rectangle list no estado atual`
- `590c1f9546 | Já tem | FXAA Vulkan presente`
- `68ca6429f2 | Já tem | Tesselação Vulkan presente`
- `11078f3f2c | Já tem | Texturas escaladas e pesos presentes`
- `56b12712a5 | Já tem | Logging de erros presente`
- `8493c8ee8f | Não se aplica | Validação de debug via SPIRV-Tools, removida upstream`
- `4ffd5b5e80 | Não se aplica | Mips com escala no D3D12; Vulkan carrega mips de memória escalada (f25003c0e)`
- `78f53264a0 | Já tem | Fatores em blend min/max presentes`
- `9bc8551ae5 | Não se aplica | D3D12; equivalente Vulkan (e584ada09b) presente`
- `bc6f2237f6 | Já tem | Parâmetros de tile EDRAM escalados presentes`
- `e584ada09b | Já tem | Readback de resolve com escala presente (evoluído)`
- `3cf3a852e7 | Já tem | Movido para guest_spirv_shader_cache.cc (b151c6ede), presente`
- `43fa7d1b56 | Já tem | Variantes por render targets ligados presentes`
- `94847b4f37 | Já tem | Substituído por f8d15108c (pré-máscara), presente`
- `11a82b0835 | Já tem | Compilação assíncrona presente, adaptada (draws pulados até pipeline pronto)`
- `28a39bcfe6 | Não se aplica | Cache de residência removido upstream (c2674b19d)`
- `d912a6ff80 | Já tem | Fast-path sem lock no SharedMemory presente`
- `3131653098 | Já tem | Fila de prioridade da compilação assíncrona presente`
- `7ccb831f26 | Já tem | Superado pelo downscale unificado (1b5326393e), presente`
- `12b28f6714 | Não se aplica | Build de shaders (spirv-remap)`
- `697c116a36 | Não se aplica | Remoção da otimização em background; estado final já no Xendroid+`
- `39a65586ce | Já tem | Throttling/vblank presente, com pacer próprio do Xendroid+`
- `5a010146d2 | Não se aplica | Revertido upstream (cdc894d15)`
- `529e43754a | Já tem | exp_bias só em RGB de 2_10_10_10_FLOAT presente`
- `f89f4076e3 | Já tem | Textura 3D-para-2D presente`
- `8c4c6a46e3 | Não se aplica | Build: geração de shaders no build`
- `521d73f2e2 | Não se aplica | Cache de shaders do D3D12; parte comum já presente`
- `f1aa4ea244 | Já tem | Shader storage Vulkan presente, com salvamento periódico do VkPipelineCache`
- `155636834b | Já tem | Interpolação precisa no pixel shader presente`
- `b4c34c199a | Já tem | Fórmula baricêntrica estilo AMD presente`
- `cd2f4cd3dc | Não se aplica | Substituído upstream pelo polygon offset (b988808569), presente`
- `0afbd81ebb | Não se aplica | Build de subprojetos`
- `03c717999c | Já tem | Substituído por versão própria (upload só da parte legível)`
- `21d631a352 | Não se aplica | Reversão do framework de testes`
- `3f27be4b61 | Não se aplica | cvar depth_bias_decal_clamp removida upstream`
- `5362f09c97 | Já tem | Write-combine tratado como gravável, presente`
- `2a87dae924 | Já tem | Clamping de k_16_16 no EDRAM presente`
- `707052a2ff | Já tem | Superado por 953c37963 (f16 de faixa estendida), presente`
- `e62ab58114 | Já tem | Superado por d119505289 (num_format nos packers), adaptado`
- `397fc92841 | Já tem | ZPD reescrito presente (vulkan_zpd_query_pool, xenos_zpd_report.h)`
- `0fdb5ff824 | Já tem | XenosReportController incorporado ao CommandProcessor, presente`
- `2d07d228ee | Já tem | Correções de ZPD presentes`
- `904ac7c31e | Já tem | Ciclo de vida de resolve nos backends presente`
- `95efc4cf67 | Já tem | occlusion_query padrão fast presente`
- `e8065ab248 | Já tem | Superado por 0fdb5ff824, presente`
- `5c2eeb9162 | Não se aplica | ZPD no caminho ROV do D3D12`
- `a12fdee02f | Não se aplica | Conversão do build para CMake`
- `f1dfb08dda | Já tem | Xendroid+ não depende de SPIRV-Tools em runtime`
- `16fc37fed3 | Não se aplica | Só macOS`
- `c9a6327ad1 | Já tem | Ajuste de saturação de ZPD presente`
- `d8a0f832a8 | Não se aplica | Barra de título da UI de desktop`
- `719be21416 | Não se aplica | UI Qt para wxWidgets (desktop)`
- `731b19d864 | Já tem | depth_float24_convert_in_pixel_shader presente`
- `29b03f9ea3 | Já tem | Barreira de buffers de readback presente`
- `3087c4c5dc | Já tem | Barreira entre uploads presente (com skip_if_equal=false)`
- `63dbbf094d | Já tem | kEarlyHint removido; lógica em guest_spirv_shader_cache.cc`
- `d1a4cf163e | Já tem | Slots de dynamic rendering presentes (adaptado em 41ffbbf0f)`
- `59629d7a6c | Já tem | Falha limpa sem provider presente`
- `ff096b01b2 | Não se aplica | Superfície Metal do macOS (MoltenVK)`
- `50a630c035 | Já tem | Overrides de cvar não vazam para o config global, presente`
- `fb6e066918 | Não se aplica | cvars do backend Metal`
- `b988808569 | Já tem | Polygon offset de RT do host presente`
- `1b5326393e | Já tem | Shader de downscale unificado presente`
- `c02060f228 | Já tem | Downscale corrigido presente (resolve_downscale.cs.slang)`
- `03c37a76b3 | Já tem | LOD implícito + bias em cubemaps presente (estendido a 2D por cvar)`
- `3d8d3149d5 | Já tem | gamma_render_target_as_unorm16 presente`
- `03df72df93 | Já tem | Float controls no geometry shader presentes`
- `3241ba91ef | Já tem | Vertex kill (oPts.z) presente`
- `06a4466bd0 | Não se aplica | Correções DXIL do D3D12`
- `113c76219f | Não se aplica | Placeholder PS do D3D12`
- `17861968fb | Não se aplica | Paridade DXIL/DXBC (D3D12)`
- `195e115a3e | Não se aplica | f16 estendido no ROV do D3D12`
- `2b103a3fa9 | Não se aplica | DXIL/ROV do D3D12`
- `51bc1c74e3 | Não se aplica | Paridade DXIL/DXBC (D3D12)`
- `7051ceb9a4 | Não se aplica | Fallback DXBC do D3D12`
- `743d2f2ca3 | Não se aplica | Baricêntricas no DXIL (D3D12)`
- `a4681813e2 | Não se aplica | Shaders de runtime do D3D12 em DXIL`
- `c9de260f10 | Já tem | Equivalente: planos de recorte nas system constants, comparados por draw`
- `d81e8d7065 | Não se aplica | getBCF no DXIL (D3D12)`
- `e2da4c1161 | Já tem | Reuso de descriptor sets próprio (e445ee4d9)`
- `ebe6531905 | Não se aplica | Indexação bindless no DXIL (D3D12)`
- `6947a91e61 | Já tem | Conversão de valores 7e3<->8_8_8_8 presente`
- `e08a7e8222 | Já tem | Reuso de tiles 7e3<->8_8_8_8 atrás de cvar, presente`
- `cb885e8a13 | Já tem | Superado por 9792c2b95 (readback via host_buffer), presente`
- `174214e68a | Não se aplica | Removido upstream (48718c13c2); estado final presente`
- `7db52eba8a | Já tem | Bias de gradiente por eixo presente, atrás de texture_gradient_exp_bias`
- `d119505289 | Já tem | Fetches com escala inteira e resolve 8_8_8_8_GAMMA presentes (adaptados)`
- `a281cc7530 | Já tem | Zero-copy presente (cvar shared_memory_zero_copy); Adreno não expõe a extensão`
- `70ad7c527e | Já tem | Roteamento de memexport em dois buffers presente`
- `7c999ca76d | Já tem | Correção da máscara de amostras FSI presente`
- `b9dac9d11c | Já tem | Superado por 9792c2b95, presente`
- `e1fa5c7749 | Já tem | Superado por 9792c2b95, presente`
- `651b51ef72 | Já tem | Barreiras do host buffer de memexport presentes`
- `856cbfcb6d | Já tem | Superado por 2f2d6b89c (validação de streams), presente`
- `9f49683b5d | Já tem | memexport_enable/await presentes, adaptados pelo Xendroid+`
- `a635ac64f5 | Já tem | Readback de resolve escalado via downscale CS presente`
- `ba06e42b45 | Não se aplica | Escopos de profiling (x64/microprofile)`
- `48718c13c2 | Já tem | Refresh de páginas watch-blind removido, presente`
- `8486e97a06 | Portar | Fetch 2D não normalizado com mip travado usa o grid do mip (popping em 555308B6/5553080B)`
- `da47dfaacf | Não se aplica | Bug só no DXBC; o memexport SPIR-V usa select(±0.5) e não é afetado`
- `090cecd1b8 | Já tem | Reaplicação via canary de 3241ba91ef, já presente`
- `0f2980de44 | Talvez | Bias por eixo já existe (cvar); falta o bias máximo no cubemap implícito`
- `1fdbe569e4 | Já tem | Reaplicação via canary de 3087c4c5dc, já presente`
- `2a6e9f4e2a | Já tem | Reaplicação via canary de 731b19d864, já presente`
- `3254ac20f8 | Já tem | Reaplicação via canary de 03c37a76b3, já presente`
- `658cdbb5de | Já tem | Checagem de anisotropia/filtragem presente em GetSamplerParameters (com fast path)`
- `ac42250688 | Não se aplica | Só comentários em Depth20e4To32`
- `cde5d85ec9 | Já tem | Reaplicação via canary de b988808569, já presente`
- `e519d59e40 | Já tem | Base do lerp entre camadas já corrigida (lerp_first)`
- `fbdb1f2817 | Portar | tfetch1D com coordenada XY vira fetch 2D (UI de 545407D4 amostrava com Y=0)`
- `25597a5465 | Talvez | force_depth_clamp global (494707EE, 41560881); Xendroid+ já tem variante restrita`
- `6a45452087 | Portar | Escala de num_format inteiro usa swizzle do host; canais 5/6 bits trocados`
- `052cb95f24 | Portar | Limita canal-fonte ao último armazenado (k_16, k_5_6_5) na escala inteira`
- `2b3f0cb456 | Portar | Resolve k_8 com LOW_BLUE seleciona alfa (5451080D, 4D530808); adaptar resolve_host_color`
- `fc48d37cdc | Não se aplica | Xendroid+ mantém essas cvars por título de propósito (custo de resolves completos)`
- `2fce2d8957 | Portar | Endereços de vfetch/índices/memexport em janela espelho derrubavam draws; mascarar físico`
- `92ada8ebc0 | Portar | Operando b do escalar usa Z quando o vetorial usa src3; parse de ucode`
- `ccb602b763 | Talvez | Redesenho do readback fast (sem semear RAM no 1º resolve); Xendroid+ usa modo uma próprio`
- `fe5a7a12dd | Não se aplica | Liga force_depth_clamp no Metal e na UI de debug de desktop`
- `052a205263 | Não se aplica | Traces do backend Metal`
- `0db64ec3a2 | Não se aplica | Slots de query de tempo de GPU do profiling (61810b48e0)`
- `1a87f56c16 | Não se aplica | Backend Metal (SPIRV-Cross)`
- `1d8f2ab756 | Não se aplica | Snapshot de EDRAM para traces (ferramenta)`
- `300ff21ff1 | Não se aplica | Backend Metal`
- `32ddae9206 | Não se aplica | Build Win32`
- `35917fa83a | Não se aplica | Remove arquivos de teste órfãos`
- `5dd1588c59 | Não se aplica | SpirvToDxil (D3D12)`
- `61810b48e0 | Não se aplica | Profiling de tempo de GPU; Xendroid+ tem diagnósticos próprios`
- `659012897d | Não se aplica | Instrumentação de profiling`
- `78516573ee | Não se aplica | Reprodução de traces (ferramenta)`
- `8e7b6ca27d | Não se aplica | Texturas em traces de frame (ferramenta)`
- `97ed99bbb6 | Não se aplica | Refatoração: move emissor de dump EDRAM para reuso no Metal/D3D12`
- `9f2ee44c4a | Não se aplica | Escopos de profiling`
- `aa2efeafac | Já tem | Equivalente próprio: resolves diretos de RT do host (resolve_host_color.xesli)`
- `aa7f9ff2d0 | Não se aplica | Backend Metal`
- `af8c760489 | Não se aplica | Screenshot de referência em traces (ferramenta)`
- `b5b201e9ec | Não se aplica | Build (alvos misc)`
- `d734608a7e | Não se aplica | Refatoração: move emissor de transferência para reuso no Metal/D3D12`
- `e1ba795298 | Já tem | Metal + emissor compartilhado; Xendroid+ já tem variantes 4 px/thread próprias`
- `e2f4c1580a | Não se aplica | Hosts de trace (ferramenta de desktop)`
- `e7595ef605 | Portar | kill grava 1.0/0.0 no destino; SPIR-V devolvia sempre 0 após demote`
- `0d395ce9ab | Não se aplica | Reorganiza cvars de debug no TOML, limpeza`
- `5cf409d6e5 | Não se aplica | Modernização C++20, cosmética`
- `0d38781760 | Já tem | Reaplicação via canary de 03df72df93, já presente`
- `2b071d9b09 | Já tem | Planos de recorte nas system constants, comparados a cada draw`
- `3ff230d23b | Já tem | Pack/unpack f16 estendido presente; só falta o clamp do caminho FSI`
- `66779fb873 | Já tem | Patch index do HS e endian por draw já presentes (tesselação nas system constants)`
- `7cd47947b0 | Portar | Layout de tfetch1D promovido escolhido em runtime (555308CE) e teto de linhas do 1D largo`
- `9e9d3cdd3f | Já tem | Clamp de palavras fora do buffer de vfetch já presente (var_main_vfetch_bound_)`
- `a5a18f5c75 | Talvez | Micro-otimizações; só a do relógio (sem MDC por chamada) interessa`
- `4a863a0e1a | Portar | Base level descartado quando mip_min_level≠0 com base≠mip (534307E2); BaseMap usa nível 0`
- `e87321b067 | Não se aplica | Renomeia cvar de debug (debug_msaa_2x_as_4x), cosmético`
- `7b2555f1f2 | Talvez | Arredonda fetch unsigned normalizado (4D5309C9 Forza Horizon, 4D530AA4); custo em todo fetch`
- `2d0f19746c | Não se aplica | Estende o resolve direto do upstream (não usado aqui) a destinos escalados`
- `437a7280cf | Talvez | Novo layout de amostras do EDRAM (1x/2x/4x); enorme e conflita com o MSAA próprio`
- `691c436b1b | Não se aplica | xenos_draw.hlsli (HLSL/D3D12)`
- `c00e7aead2 | Não se aplica | Remove DXBC do D3D12; no SPIR-V só comentário`
- `ebe86a57f2 | Não se aplica | D3D12`
- `4eb111560a | Não se aplica | Corrige o emissor de resolve direto do upstream em escala 3x; Xendroid+ usa o próprio`
- `6e9cb7e3f6 | Não se aplica | D3D12 (dxcompiler.dll)`
- `0c843efb32 | Talvez | Mesma mudança de 7b2555f1f2 com a parte DXBC; portar só uma`
- `3cc741caa3 | Talvez | NaN/sNaN raros viram zero ou infinito no arredondamento de 21 bits`
- `b308af27c7 | Portar | Reaproveita o viewport do host entre draws quando os argumentos não mudam (CPU)`
- `bfca9b2d97 | Já tem | Equivalente próprio (d9816ebe9): UBO dinâmico só no Turnip; driver Qualcomm lê zeros`
- `c8ac9258f0 | Portar | Barreiras do resolve escalado gravadas direto, podendo cair dentro de render pass`
- `ec5e0f40e7 | Já tem | Equivalente próprio (4bc9274d0): swizzle constante em fetch vazio, vídeos Bink do Halo 4`
- `fb4270fb3b | Talvez | Flags de estágio entre layouts de storage buffer; conferir quais sets se misturam aqui`
- `6db3b26e91 | Não se aplica | Remove .spv não usados`
- `915532b832 | Portar | Máscaras de estágio/acesso trocadas ao crescer o scratch buffer (barreira inválida)`
- `a766c62c79 | Talvez | Upload por compute + cópia imagem-imagem mantém compressão; medir onde load_to_image está off`
- `d5845a4941 | Talvez | Complemento de a766c62c79 (staging em GENERAL)`
- `22708301ba | Talvez | Suporte inicial a XPS (heap 0x7F000000, wrap de L2); poucos títulos`
- `29fcaeac32 | Portar | Ponteiro de leitura do ring publicado a cada RB_BLKSZ, não só no fim do burst`
- `3a44f20c7b | Já tem | Arredondamento de 21 bits já em todas as aproximações no SPIR-V; sem hack AC6`
- `3ea5e4212d | Não se aplica | Só logging de extensões ausentes`
- `a2b609d22d | Portar | Decodificação 7e3->8_8_8_8 (ligada por padrão no Xendroid+) ignora write mask; GoW Judgment`
- `f2442d6e99 | Não se aplica | Compilação assíncrona do Metal (macOS)`
- `30a2ef6fc9 | Portar | Versão do pipeline storage usa max(); subir Modification::kVersion nunca invalida o cache`
- `830e605186 | Portar | Só o check de MSAA acima de 4x no resolve clear; resto é FSI`
- `5c290c32e4 | Não se aplica | Separa host_buffer/memexport e staging; Xendroid+ tem fallback uma próprio`
- `7ac465fd6e | Talvez | Refazer pipelines do cache que falham no início; ordem de publicação é opcional`
- `8d75a1779a | Não se aplica | Especialização de pixel shaders FSI; Adreno não expõe interlock`
- `85e4f325d5 | Não se aplica | Placeholder no caminho FSI/ROV; Adreno usa render targets do host`
- `cdfe727449 | Não se aplica | Refatoração da fila compartilhada D3D12/Vulkan; Xendroid+ tem fila própria`
- `2c9a7cd07a | Talvez | LOD implícito em 3D contorna bug NVIDIA (Madden 25); no Adreno só simplifica`
- `9da693480d | Talvez | Alias cor/depth no mesmo EDRAM (sprites através da geometria em 4D530A26); RT cache divergente`
- `bea885689d | Talvez | Clamp de índice dinâmico de constantes; Vulkan já tem robustBufferAccess`
- `0413471dad | Não se aplica | Ferramentas de trace e bisseção de shaders; Xendroid+ tem frame dump próprio`
- `67e0804262 | Já tem | Equivalente próprio: sobe só páginas legíveis e marca as sem acesso como válidas`
- `d1541a579b | Portar | Alloc não invalida páginas marcadas válidas sem watch; texturas velhas (DW 5/6 Empires)`
- `1b242658e5 | Já tem | Xendroid+ já trava layouts_mutex_ no mesmo bloco (correção própria)`
- `8205e00e8a | Não se aplica | Caminhos UTF-8 no Win32`
- `462a1ac855 | Já tem | Publicação atômica de is_translated já presente (01d57d4ed)`
- `0bd090dbe9 | Talvez | Parte 2x segue o layout novo (437a7280cf); o deslocamento do 4x vale validar`
- `3d233a5b2e | Talvez | ZPD como contador contínuo; mais fiel (flares de 415608B5), mas reescrita enorme`
- `89609297c7 | Portar | Offset de resolve tiled abaixo de 32bpp no RB_COPY_DEST_BASE (refração da água, 534307D5)`
- `19da174033 | Portar | Row pitch de mips lineares de 96bpp errado`
- `bc6b7a0202 | Portar | Layout de mips de arrays (4 fatias) e volumes (profundidade reduzida); mip tail 3D`
- `c9c8e483b1 | Portar | Limites de endereço tiled 3D (máscara de Z errada, bank swap); ajustar cabeçalho`
- `776dda2e33 | Talvez | Escala de resolução (Tekken Tag 2 a 2x); depende do caminho de 77f2cca800`
- `77f2cca800 | Talvez | Cópia do resolve sob acesso da CPU; arquitetura alternativa ao uma, porte grande`
- `935e038766 | Talvez | Cópia sem lock global; só faz sentido junto com 77f2cca800`
- `d8731edc99 | Portar | Espaçamento de fatias de resolve volumétrico via RB_COPY_SURFACE_SLICE, não copy_dest_height`
- `de8e60601c | Portar | Draws únicos (RT não redesenhado, RT pequeno) esperam o pipeline em vez de pular`
- `81e3deaee2 | Portar | Strides de fatia do base empacotado com altura/profundidade arredondadas`
- `ace153cb84 | Portar | Mip tail empacotado no nível 0 vem do endereço base; mips não ficam faltando`
- `c332733afd | Portar | Limite superior tiled 3D exato; antes puxava uma página extra`
- `04085efaaf | Não se aplica | Só DXBC; o SPIR-V já faz NClamp da camada empilhada`
- `16df259815 | Não se aplica | Caminhos de host buffer/staging do upstream; Adreno não tem external_memory_host`
- `3390fc219b | Talvez | cvar experimental: mulsc arredonda para zero (5451080D, 4B4D07F6, 5451086D)`
- `b837246564 | Não se aplica | Cópia host buffer->device; sem host buffer no Adreno (sem external_memory_host)`
- `1222c23f78 | Portar | Esperas de memexport só até a submissão do último export, não a fila toda`
- `34387b31f5 | Talvez | Clip de páginas de borda escritas pela CPU; depende de 77f2cca800`
- `692cd59cfe | Talvez | VIZ_QUERY e renderização condicional (VK_EXT_conditional_rendering); grande`
- `aa3339970c | Já tem | Pacer próprio por deadline no relógio do guest (vblank_pacer.h, K11)`
- `c3cd8617b1 | Talvez | Corrige texturas virtuais idTech 5 (425307EC); grande, custo em fetch point`
- `44f5b4a860 | Talvez | Pacer do Xendroid+ descarta vblanks atrasados; upstream os entrega para manter A/V`
- `7d0a452637 | Talvez | Linhas com 1 pixel guest de largura em escala ≥3x (LUT do Dragon's Dogma)`
- `a7c39fa7d9 | Talvez | Esperar só exports de pixel shader; reduz stalls nos títulos com await ligado`
- `bc9098deb6 | Talvez | Fetch point no centro do pixel guest com escala 3x/4x (43430814)`
- `6579cc2a4c | Portar | Viewport do host com origem negativa (tiles com offset negativo, Blue Dragon 4D5307DF)`
- `e1f4a0d009 | Talvez | Offset de janela no base do EDRAM: depth lixo em tiles (Halo 4 4D530919); grande`
- `3e9d62ffc4 | Portar | PM4_WAIT_IB_PFD_COMPLETE não tratado descartava o resto do ring buffer`
- `6260a87b85 | Portar | Precisão em 1D largo com coordenadas grandes; teto de linhas 32->128 (5345084D)`
- `7347cd12ef | Talvez | Resolve com comando de cópia 2 para DXT3A_AS_1_1_1_1; visto só em 5345084D (Kinect)`
- `ac37478be6 | Portar | Ordem de bits de k_DXT3A_AS_1_1_1_1 invertida na decodificação (vermelho no bit 0)`
- `0b041cd2a3 | Talvez | Registrador RB_HSIO_INTERFACE_ALIGNER_VALUE (lê 0x00BBBBBB); título não citado`
- `1b0e9d00ea | Portar | Cache de conversão de índices não invalidava entradas que terminam na faixa escrita`
- `2ab3083ef8 | Portar | Relaunch/reset matava threads host e deixava o lock do relógio preso (trava)`
- `cc4981a465 | Talvez | Posições de amostra Xenos (4D5307D5, UE3); VK_EXT_sample_locations, cvar desligada`
- `669b4266f5 | Talvez | Implementa getBCF (hoje erro de tradução); depende de c3cd8617b1, 8486e97a06, fbdb1f2817`
- `f39e557728 | Não se aplica | Configs de jogos do Xbox original (XeFu), sem suporte no Xendroid+`
- `06fb359dee | Talvez | No modelo de pular draws, adiar o fim da submissão reduz draws perdidos`
- `13c8ebd78b | Não se aplica | Claim de tradução no D3D12 e remoção de helpers não usados`
- `3cd04f8e2c | Já tem | EndSubmission do pipeline cache já é chamado a cada submissão`
- `562e06ff94 | Talvez | Corrida de layout só no modo placeholder (desligado) ou sem async`
- `81112935c2 | Não se aplica | cvar de depuração para bisseção de stand-ins`
- `81df401e49 | Já tem | Espera em PostTranslation e captura do pipeline no record já existem`
- `8a4d405af6 | Portar | Refina a decisão: VS já desenhado não força espera; nada de stand-in no warm-up`
- `b7987c25a5 | Não se aplica | Aviso de compilação na barra de título de desktop`
- `c8503efe6a | Já tem | Modelo deferred-bind padrão já cria pipelines sem pixel shader em background`
- `dd63e936a6 | Não se aplica | Placeholder ROV do D3D12`
- `e8c05cd05f | Portar | Espera de draw acelera só o pipeline dele, sem drenar a fila inteira`
- `eec245698e | Talvez | Draw medido por query não deve ser pulado: conta zero e oculta objetos`
- `1be1bd269d | Já tem | Modelo deferred-bind do Xendroid+ não traduz shaders no thread de draw`
- `9a84c1d3c2 | Não se aplica | Só no modo placeholder, desligado por padrão no Xendroid+`
- `a7d227aac3 | Talvez | Complemento de 8a4d405af6: VS do storage só conta após warm-up`
- `cd822c4d04 | Talvez | Analisar só shaders usados por pipelines armazenados, em paralelo; acelera o início`
- `566a58ebf0 | Portar | XeCryptAesCbc lê/escreve além do buffer com bloco parcial; adaptar sem Mbed TLS`
- `3989ba3a1b | Talvez | Reaproveita endereços de amostra na média MSAA do resolve; micro-otimização a medir`
- `40cee11ab1 | Portar | Textura em dois fetch constants era carregada duas vezes no lote (snapshot antes da carga)`
- `a8d034c00d | Talvez | Depth bias em unidade constante via VK_EXT_depth_bias_control (decals, 4D5307D5)`
