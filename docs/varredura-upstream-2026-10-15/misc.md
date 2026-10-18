# Varredura upstream: áudio/XMA, VFS, HID, memória/base, emulator.cc, config/patcher, third_party (lista `misc`)

Base comparada: `emulator-core/src/main/cpp/xenia` (Xendroid+ em `d98916177`). Upstream: clones `full-xenia-edge.git` (ramo `edge`) e `full-xenia-canary.git` (`canary_experimental`). Todos os 217 commits da lista estão no ramo principal do seu repositório.

Como foi verificado: cada diff foi lido (stat e trechos relevantes) e comparado com o código do Xendroid+ por grep e leitura. O código (`src/`, sem os testes) de cada commit foi aplicado numa cópia descartável de `src/`, sozinho e, nas séries, em sequência; nada foi compilado nem executado. "Aplica limpo" no texto abaixo quer dizer isso.

## Resumo

Contagem (217 commits): **Portar 27 · Talvez 36 · Já tem 40 · Não se aplica 114**.

Convenções:
- **Duplicatas.** O edge tem cópias do mesmo commit (cópia antes do rebase e cópia de volta do canary). A cópia recomendada leva o veredito, e a outra fica como "Não se aplica (duplicata de …)": 7015473a77=9d8210b32d, 6bef1fbed1=052365bc01, aee0871dd7=adf56b76c4, 1ad151d124≈87c2411270, 19c23eb112≈a7a3eabdbb, 6f9840568b≈c9536890c9, 2e15802618≈f65fda3320, 669c9709d2≈b495439ba9, d8348b4f68≈73388a080f (versão canary). Já f21ebd49e9 e 6a67369f85 ficam os dois como "Já tem".
- **"Já tem" por gêmeo.** A série XMA de fevereiro, as correções de suspend POSIX de 2025/2026 e outros commits da lista são as cópias do edge de commits que o XenDroid já trouxe pelo canary (patch-id igual ou conteúdo igual), por exemplo f4848c875f=ea056849fa e b04d96a9cb=6e5b2a95b0. O `xma_context_new.cc` do Xendroid+ é o do edge `7e98ae6de` (a reescrita RexGlue de maio, b575c6841) mais as mudanças próprias (contadores de bail, `ConsumeKick`, fence de release para ARM).

Temas mais valiosos:
1. **XMA "new": quatro correções de jogos** (9d8210b32d → 052365bc01 → adf56b76c4 → 5dd1cdbbf5). Corrigem os LEGO que travavam nas cutscenes, o NBA Live 06 travando a thread de áudio, a BGM com falhas dos musou e o Tekken Tag 2 perdendo o áudio. Aplicam limpos em sequência, e o app usa `xma_decoder = "new"`.
2. **E/S de arquivos no POSIX** (c9536890c9, b9e00e6cce, b495439ba9, d8e5dae74e, f65fda3320, 211e7d9531, 037eef5403). Hoje, quando o jogo pede `FILE_APPEND_DATA` junto com escrita, o `O_APPEND` manda as escritas posicionadas para o fim do arquivo (corrompe dados); além disso um arquivo renomeado não é achado pelo nome novo e o delete-on-close usa a entrada depois de liberá-la.
3. **Relógio do guest no POSIX** (a1c2b745ba, 79feeb9095). No Android `KeQueryInterruptTime` anda 100× rápido (ns em vez de 100 ns), e depois de um relaunch a hora do guest fica adiantada.
4. **Audio Worker fora da trava global** (12e3b4223d, parcial). Ataca a parada de 62 ms na trava global que `docs/audio-pipeline.md` deixou em aberto.
5. **GoD/SVOD de disco como mídia óptica + cvar `media_type`** (7be830a2dd, 95a3d08948). Corrige o WatchDogs em GoD e dá um ajuste por jogo.
6. **Conversão de áudio NEON** (b899a97a7e, 32177527fa). No ARM64 a conversão é hoje escalar e trunca, e o NEON arredonda igual ao x86.
7. **Memória**: o teto do `AllocRange` (87c2411270) e o release com tamanho 0 que vaza no POSIX (614fedfbe3).
8. **Pequenas correções do núcleo**: o driver nulo de áudio para `apu=nop` (1406d87095, com adaptação), o dump de crash que desreferencia nulo (7b2ea9f9ac), o slot de controle marcado vazio no Madden 25 (f213cc6a9f), o `plugins.toml` por título (9d08d64b5d), a migração de Headers (f7c90d0b5c), o `NtQueryEvent` (1c81b86747, parcial) e o gate de `/dev/shm` (b6e27cb4f9, parcial).
9. **Talvez, grande**: a série XeFu (Xbox original pelo `xbox.xex`, tabela de páginas de modo usuário), com 15 commits aqui e dependências nas listas cpu e kernel.
10. **Talvez, medir**: decodificar XMA na thread que chuta (1fae8f43a5), o multi-wait por futex (6435e79904) e a proteção de páginas em hosts de 16 KB (ca42e669c9).

## Temas recomendados

| Tema | Commits (sha curto) | O que traz para o Xendroid+ | Evidência (arquivo/função) | Dependências | Risco de conflito | Esforço | Veredito |
|---|---|---|---|---|---|---|---|
| Correções do decodificador XMA "new" | 9d8210b32d, 052365bc01, adf56b76c4, 5dd1cdbbf5 | Destravam os LEGO nas cutscenes de abertura (Star Wars 3, LotR, Batman 2), o NBA Live 06 (som de menu travava a thread de áudio do guest), a BGM dos musou (frame pulado, decodificação agora igual à do master) e o Tekken Tag 2 (`loop_start` fora do frame silenciava o contexto). O app usa `xma_decoder = "new"` (`default_config.toml`). | `xma_context_new.cc`: `GetNextPacketReadOffset` (avança sequencialmente, não pelo skip do sub-stream), `Work` (sai com subframes pendentes), `GetPacketInfo` e `Decode`. Arquivo do Xendroid+ = edge `7e98ae6de` + mudanças próprias | Nesta ordem (5dd1cdbbf5 reescreve o trecho de adf56b76c4) | Baixo: os quatro aplicam limpos em sequência | P | Portar |
| E/S de arquivos do host no POSIX | c9536890c9, b9e00e6cce, b495439ba9, d8e5dae74e, f65fda3320, 211e7d9531, 037eef5403 | `FILE_APPEND_DATA` junto com escrita abre `O_APPEND`, e no Linux/Android o `pwrite` ignora o offset: toda escrita posicionada vai para o fim do arquivo (dado corrompido). Também cobre o diretório aberto com escrita (EISDIR → "no such file"), o arquivo renomeado (o nome vira o caminho inteiro com `\`), o delete-on-close (apaga a entrada no primeiro fechamento, com outros handles ainda apontando para ela), o `stat` sem checagem (tamanho/data lixo) e o link de `ObCreateSymbolicLink` para `\Device\Content\N` (canonicalizado sem a barra final, o `OpenFile` do pai falha) | `base/filesystem_posix.cc` `FileHandle::OpenExisting` (linha 208) e `ListFiles`; `vfs/entry.cc` `Entry::Rename` (linha 151); `vfs/devices/host_path_file.cc` `Destroy`; `host_path_entry.cc` `update`; `virtual_file_system.cc` `MountPathMatches` | b9e00e6cce depois de c9536890c9 | Baixo: todos aplicam (f65fda3320 com contexto reduzido) | P–M | Portar |
| Relógio do guest no POSIX | a1c2b745ba, 79feeb9095 | No Android `QueryHostInterruptTime` devolve ns de `CLOCK_MONOTONIC_RAW`, mas `KeTimeStampBundle.interrupt_time`/`KeQueryInterruptTime` esperam 100 ns: o guest vê esse relógio 100× rápido. Além disso a hora do sistema do guest fica adiantada pelo atraso de inicialização e, depois de um relaunch em processo, pela sessão inteira anterior | `base/clock_posix.cc` `QueryHostInterruptTime`; `kernel/kernel_state.cc` `UpdateKeTimestampBundle`; `emulator.cc:309` `set_guest_system_time_base` | Nenhuma | Baixo: aplicam limpos | P | Portar |
| Audio Worker fora da trava global | 12e3b4223d (parte do mutex) | A tabela de clientes ganha um mutex próprio, e assim um detentor da trava global não atrasa mais o pump. O Worker toma `global_critical_region_` três vezes por pump, e `docs/audio-pipeline.md` registra uma parada de 62 ms "provavelmente na trava global" e lista "menos idas dele à trava global" como não testado | `apu/audio_system.cc` `WorkerThreadMain`, `SubmitFrame`, `RegisterClient`, `UnregisterClient`, `Shutdown` | O `callback_mutex` por cliente (já existe) | Alto: `audio_system.cc` muito customizado (fila adaptativa, topup, estatísticas) | M | Portar (parcial; a parte de pacing "a partir do prazo perdido" se sobrepõe a `apu_pump_topup`/`apu_adaptive_queue`, medir antes) |
| GoD/SVOD de disco como mídia óptica | 7be830a2dd, 95a3d08948 | Rips de disco em GoD/SVOD passam a ser ODD (`XTitleGetDeploymentType`, `XContentQueryVolumeDeviceType`), o que corrige o WatchDogs em GoD (procurava a instalação no lugar errado). A cvar `media_type` (auto/hdd/odd) força o tipo por jogo | `emulator.cc` `LaunchStfsContainer` (hoje sempre `kDownload`), `LaunchDefaultModule` | 95a3d08948 depois de 7be830a2dd | Baixo: aplicam limpos em sequência | P | Portar (muda todo GoD; expor `media_type` nos quirks/config por jogo) |
| Conversão PCM do XMA e dobra 5.1 em NEON | b899a97a7e, 32177527fa | No ARM64, `ConvertFrame` e `sequential_6_BE_to_interleaved_2_LE` (usado no callback AAudio por `xe_audio_block_renderer.h`) caem no laço escalar com `byte_swap` por amostra, e o escalar trunca (`static_cast<int16_t>`) onde o x86 arredonda. Com NEON o arredondamento e a saturação ficam iguais ao x86, e o ganho de CPU não foi medido | `apu/xma_context.cc` `ConvertFrame` (ramo `#else`); `apu/conversion.h` | 32177527fa depois de b899a97a7e; 7a3775d866 opcional | Baixo/Médio: b899a97a7e aplica com contexto reduzido (o ramo escalar foi alterado no Xendroid+) | P | Portar |
| Driver nulo de áudio | 1406d87095 | Com `apu = nop` (opção "sem áudio" do app), `CreateDriver` devolvia `NOT_IMPLEMENTED`: `XAudioRegisterRenderDriverClient` falhava e o título seguia com um handle nunca escrito | `apu/nop/nop_audio_system.cc` | Adaptar: o `NopAudioDriver` precisa informar a fila como cheia (`ReportsQueuedFrameCount`/`GetQueuedFrameCount`), ou o `apu_pump_topup` chama o callback do jogo em dobro | Baixo: aplica limpo | P | Portar |
| Alocador e memória do host | 87c2411270, 614fedfbe3 | `AllocRange` arredonda o teto para cima no alinhamento e pode alocar acima do endereço pedido; `high_address` perto de `UINT32_MAX` dá a volta a zero (é a mesma função cujo erro de página já quebrou Far Cry 3/4 e Watchdogs). No POSIX, `DeallocFixed(..., 0, kRelease)` chama `munmap(p, 0)`, que falha: o array de flags de `gpu/shared_memory.cc:138` vaza a cada jogo | `memory.cc` `BaseHeap::AllocRange` (linhas 1283-1288); `base/memory_posix.cc` `DeallocFixed` | Nenhuma | Baixo: aplicam limpos | P | Portar |
| Dump de crash seguro em thunk JIT | 7b2ea9f9ac | Falha num thunk do code cache (sem `Function`) fazia o próprio dump desreferenciar nulo (o `assert_not_null` some em release), escondendo o crash original | `emulator.cc:2108-2114` `ExceptionCallback` (`MapMachineCodeToGuestAddress`) | Nenhuma | Médio: o dump do Xendroid+ é próprio (mod_off, pilha) | P | Portar |
| HID: tipo de dispositivo ≠ desconexão | f213cc6a9f | Título que consulta outro tipo de dispositivo marcava o slot vazio (conectar/desconectar em loop no Madden 25) | `hid/input_system.cc` `GetStateForUI` | Nenhuma | Baixo: aplica limpo | P | Portar |
| Pequenas correções do núcleo | 9d08d64b5d, f7c90d0b5c, b6e27cb4f9 (parcial), 1c81b86747 (parcial) | `plugins.toml` por título montado com `"{:08X}\\plugins.toml"`: no POSIX nunca é achado. A migração de dados podia apagar `Headers` depois de uma cópia falha (o `ec` da segunda cópia sobrescreve o da primeira) e podia lançar exceção de `std::filesystem`. O gate de `/dev/shm noexec` no `Emulator::Setup` roda no Android (`XE_PLATFORM_LINUX=1`), lê `/proc/mounts` e só pode falhar à toa (o Android usa `ASharedMemory`). `PosixEvent::Query` faz `assert_always` e `NtQueryEvent` devolve tipo e estado errados | `patcher/plugin_loader.cc`; `emulator.cc` `DataMigration` e `Setup:317-339`; `base/threading_posix.cc:1821` | Nenhuma | Baixo | P | Portar |

## Talvez

| Tema | Commits (sha curto) | O que traz para o Xendroid+ | Evidência (arquivo/função) | Dependências | Risco de conflito | Esforço | Veredito |
|---|---|---|---|---|---|---|---|
| XeFu: jogos de Xbox original via `xbox.xex` (tabela de páginas de modo usuário) | f55adea3fe, 8358e0d532, 02d8afd7ed, 7646f6e683, b9fa366e52, 4c60e0b2f7, 8249bbe381, 2e6898afc1, c30cab1ae6, 1e02ea790f, 04c298a0db, 59427fe60a, e23721bd4c, 4d15111d84, f11c262bef | Recurso novo: rodar jogos OG Xbox pelo emulador do 360 (exige a partição de compatibilidade de um HDD real) | `memory.cc` não tem tabela de modo usuário | 32a95fff0, b5cc59e85, 2ea595d6c, 7873b1ed9 (lista cpu), 4c6992b39 (lista kernel); views de 4 KB não funcionam em host de 16 KB | Alto | G | Talvez |
| Proteção de páginas em hosts de 16 KB | ca42e669c9 (+ trecho de 00a57b3815) | Hoje `ShouldSkipHostCommit` pula todo commit com página > 4 KB: a página zero e faixas no-access/só-leitura ficam graváveis. Só afeta aparelhos com kernel de 16 KB | `memory.cc:125-133`; `base/memory_posix.cc` `AllocFixed` | O trecho de `AccessViolationCallback` vem de 00a57b3815 | Médio | M | Talvez |
| Endurecimento de threads POSIX | 73388a080f (d8348b4f68 é a versão canary) | `NanoSleep(100)` vira `nanosleep(0)` (`XThread::Delay` com prioridade baixa); `Terminate` em si mesmo gira em `for(;;)`; `pthread_join` com o `mutex_` preso; `WaitSuspended` com um token só. No Android os sinais RT enfileiram e o `Terminate` é por sinal, então o ganho é pequeno | `base/threading_posix.cc` `NanoSleep` (linha 268), `Terminate`, `post_execution`, `WaitSuspended` | Nenhuma | Alto (arquivo com WFE, hand-off AC6 e diagnósticos próprios) | M | Talvez |
| Multi-wait por futex | 6435e79904 | Upstream mediu −21% de ciclos no Lost Odyssey trocando o polling por futex. O Xendroid+ já tem uma variante (condvar global com teto de 1 ms) | `threading_posix.cc` `PokeMultiWaiters`/`WaitMultiple` | Nenhuma | Alto | M | Talvez (medir) |
| Decodificar XMA na thread que chuta | 1fae8f43a5 | O mecanismo já existe (`WriteRegister` faz `Work()` inline com `use_dedicated_xma_thread=false`); upstream só trocou o default para false, já que a thread dedicada com espera não dá paralelismo | `apu/xma_decoder.cc` `WriteRegister`, `WaitForKick` | Nenhuma | Baixo | P | Talvez (medir no aparelho) |
| Relaunch/reset em processo | 58298b1fd0, d33e440e5c, 9cdf7a3227, 1e834f8a8c | Serializa launches e relaunches, descarta pedidos superados e volta à lista quando falha. O `RelaunchTitle` do Xendroid+ é próprio (barreira de join, exclusão da thread chamadora) | `emulator.cc:1562` `RelaunchTitle` | 1e834f8a8c só com b11458e49 (lista kernel) | Alto | M/G | Talvez |
| Áudio sem dispositivo | 699ad42f29 | Se o driver não abre, segue em silêncio com pacing (cliente e XMP) | `audio_system.cc` `RegisterClient` | 1406d87095 e o estado do canary (ALSA, `clients_[].lock`) | Alto | M | Talvez |
| E/S concorrente em SVOD | 955e1d1723 | Leituras posicionadas por fragmento; deixa o SVOD usar o pool de I/O | `svod_container_file.cc` (seek+fread num `FILE*` compartilhado) | 5a9104a20 (lista kernel) | Médio | M | Talvez |
| Ordem de listagem igual ao NTFS | accaef3f07, a7a3eabdbb | A ordem de enumeração de diretórios fica igual à do Windows; `upper_ascii` hoje converte para minúscula (sem uso por enquanto) | `base/filesystem.cc`, `base/utf8.cc:39` | a7a3eabdbb antes | Médio | P | Talvez |
| Diversos | 13e380a1d4, 4d4b9fa07d, 6290cb2748, efbac5e9f6, 76ce245b8f, a13309e331, 2372a54105, 4101dfa07a, 1019316797 | Migração de aliases bool em config (`vsync`, `readback_resolve`); padding 0xFF em diretórios GDF; bump do FFmpeg d980192e→c4f44071 (submódulo ausente, diff não verificado); handlers de falha com o FPCR do host; estacionar o fiber na troca de disco (o provedor Android espera num `condition_variable`); `QueryProtect` em falha; arredondamento do caminho escalar (dispensável com NEON); Mbed TLS (pré-requisito do XeCrypt da lista kernel); `ClearDialogs` que não acorda quem espera num diálogo ImGui durante um relaunch | `config.cc`; `disc_image_device.cc` `ReadEntry`; `exception_handler_posix.cc`; `xe_android_disc_swap.cpp`; `memory_posix.cc`; `ui/imgui_drawer.cc:783` | Ver apêndice | Baixo–Médio | P | Talvez |

Observação sem commit correspondente: no Android o `Terminate` de outra thread é um sinal cujo handler chama `pthread_exit` (`threading_posix.cc:2164`), e o bionic não desenrola a pilha C++. Uma thread morta enquanto espera num `WaitMultiple` deixa `g_multi_waiters` incrementado para sempre, e a partir daí o `PokeMultiWaiters` (inclusive o do callback de áudio) passa a tomar o mutex global. Nem o `ScopedMultiWaiter` de addda160ca nem o cancelamento adiado de d2724c3033 cobrem esse caso no Android.

## Apêndice: todos os commits

Formato: `sha | veredito | motivo`, na ordem da lista.

- 00a57b3815 | Não se aplica | Caso só Windows (SEC_RESERVE); no POSIX página nunca alocada já lê zero
- 0142894016 | Não se aplica | Threading só macOS (threading_mac.cc)
- 01c399164d | Não se aplica | Reverte ff4fcc7b6b; o par tem efeito nulo
- 027b25b7a3 | Já tem | Normalização de barras já em Emulator::RemountAndResolveLaunchPath
- 02d8afd7ed | Talvez | Série XeFu/tabela de páginas modo usuário; depende de commits das listas cpu/kernel
- 02e498d869 | Não se aplica | xbyak é o assembler x64; Android usa xbyak_aarch64
- 035039270c | Não se aplica | mesa/spirv_to_dxil é só D3D12/Metal
- 036f6a66e2 | Não se aplica | Submódulo/build; sem efeito no build Android
- 037eef5403 | Portar | Link ObCreateSymbolicLink para \Device\Content\N sem barra final falhava no OpenFile
- 042cd9f147 | Não se aplica | Correção do diálogo Qt de troca de disco
- 04c298a0db | Talvez | Série XeFu (partição de compatibilidade); só faz sentido com a série inteira
- 052365bc01 | Portar | XMA new: drena o frame antes de sair; NBA Live 06 travava áudio
- 0525b974fb | Não se aplica | Só remove comentários obsoletos
- 075283d850 | Já tem | Gêmeo c9eba5daf8 já está no XenDroid
- 08d89ed7b7 | Não se aplica | Cabeçalhos do Metal Shader Converter (macOS)
- 09902dbd57 | Não se aplica | Script binutils de testes
- 09a80fe710 | Não se aplica | Só adiciona submódulo oaknut; nada o usa no Xendroid+
- 0a8208052d | Não se aplica | gamecontrollerdb do SDL; Android não compila HID SDL
- 0c7829c10a | Não se aplica | Botão Guide na UI desktop/SDL
- 0d0fadb5e1 | Não se aplica | Driver de teclado desktop (GTK/Win32)
- 1019316797 | Talvez | ClearDialogs não acorda quem espera; só importa com diálogo ImGui no relaunch
- 125ad5d254 | Não se aplica | Fila de timers Win32
- 12bdb0a96b | Já tem | SaveConfigValueState/RestoreConfigValueState já em base/cvar.h
- 12e3b4223d | Portar | Tira o Audio Worker da trava global (travamento de 62 ms documentado)
- 13aa88b246 | Não se aplica | Default 'old' depois revertido por 38c2385cc3, que o Xendroid+ tem
- 13e380a1d4 | Talvez | Migração de aliases bool (vsync, readback_resolve) falhava; depende de configs legadas
- 1406d87095 | Portar | apu=nop: registro de cliente de áudio falhava; adaptar ao apu_pump_topup
- 1508e3cc27 | Já tem | Pins de Vulkan-Headers/VMA do Xendroid+ já iguais ao edge atual
- 199e30af83 | Não se aplica | Cvar guide_button da UI desktop
- 19c23eb112 | Não se aplica | Duplicata canary de a7a3eabdbb
- 1ad151d124 | Não se aplica | Duplicata edge de 87c2411270 (troca guardas por assert)
- 1d25455159 | Não se aplica | Mutex global só Apple (os_unfair_lock)
- 1e02ea790f | Talvez | Restart de fiber para o modo usuário da série XeFu
- 1e834f8a8c | Talvez | Corrige regressão de b11458e49 (lista kernel); só junto dele
- 1f4bb50082 | Não se aplica | Driver SDL
- 1fae8f43a5 | Talvez | Mecanismo inline já existe; só troca default, medir no aparelho
- 211e7d9531 | Portar | Entrada host não atualizava datas de escrita/acesso após escrita
- 2248561d80 | Não se aplica | Só testes
- 266f2d4e39 | Já tem | Gêmeo a26bbaa674; FFmpeg do Xendroid+ já em d980192e
- 26866aef91 | Não se aplica | UI Qt de config por jogo
- 2a10971772 | Não se aplica | Emissor x64
- 2b06bce7a6 | Já tem | Gêmeo 6f18c98502 já está no XenDroid
- 2bbd9c4687 | Já tem | asio já vendorizado no mesmo commit do edge
- 2e15802618 | Não se aplica | Duplicata edge de f65fda3320
- 2e6898afc1 | Talvez | Log da reserva modo usuário; série XeFu
- 2f6f46b5fd | Já tem | exception_handler_posix.cc já encadeia ao handler anterior
- 30bd55e5af | Não se aplica | microprofile; profiler desligado no build Android
- 32177527fa | Portar | NEON igual ao x86 em NaN/estouro; vai junto de b899a97a7e
- 32aff9af0d | Já tem | Gêmeo d9747704be já está no XenDroid
- 34634201d3 | Não se aplica | Revert do xbyak x64
- 3494b56a0b | Já tem | Clamp equivalente no chamador (kernel/xtimer.cc)
- 36b803ad21 | Não se aplica | Progresso de ZAR no fluxo desktop; Android usa CompressDiscToZarchive próprio
- 38c2385cc3 | Já tem | xma_decoder 'new' + UPDATE_from_string em xma_decoder.cc
- 38e877500b | Já tem | Via cd5df622a (canary); API ch_layout e FFmpeg d980192e
- 3cf954e7a0 | Não se aplica | Submódulo de dados de patches; Xendroid+ embute os seus
- 3d02eae07d | Não se aplica | Backend Metal
- 3dec621b66 | Não se aplica | GDBStub só Windows
- 4101dfa07a | Talvez | Mbed TLS: pré-requisito dos XeCrypt DES/DH/HMAC da lista kernel
- 4350091117 | Não se aplica | Remove dependência GTK do build desktop
- 44cb87328c | Não se aplica | Seletor GTK; Android usa provedor nativo de troca de disco
- 492e7c958c | Não se aplica | Superado: reescrita de maio, edge/canary atuais não têm
- 4ae54ec133 | Não se aplica | Driver ALSA desktop
- 4b39c27909 | Não se aplica | Timers Win32
- 4c60e0b2f7 | Talvez | Bit C da PTE modo usuário; série XeFu
- 4d15111d84 | Talvez | Cache de shaders por jogo OG Xbox; série XeFu
- 4d4b9fa07d | Talvez | Padding 0xFF em diretórios GDF; sem jogo 360 citado
- 4dd03fa6df | Já tem | Gêmeo 81dd887e74 já está no XenDroid
- 4f7e47aa23 | Não se aplica | microprofile; profiler desligado
- 4f9107156f | Já tem | Seleção do backend A64 já no Xendroid+
- 5376439e1b | Já tem | Gêmeo 6265698ee (canary) já presente; stall detection corrigida
- 5434060715 | Não se aplica | UI Qt e menu de contexto
- 55bcfbeac7 | Não se aplica | Threading macOS
- 57ce1e884a | Já tem | Campo filename de PatchFileEntry já existe; resto é UI de patches
- 582603188b | Não se aplica | Config por jogo como argumentos ao processo filho; Xendroid+ usa LoadGameConfigForFile
- 58298b1fd0 | Talvez | Serializa relaunch; RelaunchTitle do Xendroid+ é próprio
- 59427fe60a | Talvez | Leitura de XBE OG Xbox; série XeFu
- 5d4a90b3e9 | Não se aplica | Remove driver XInput Windows
- 5dd1cdbbf5 | Portar | XMA new: loop_start fora do frame silenciava contexto (Tekken Tag 2)
- 5f3e28236e | Não se aplica | CI/macOS
- 614fedfbe3 | Portar | munmap tamanho 0 falhava; vaza em gpu/shared_memory.cc a cada jogo
- 626f143a84 | Já tem | should_cancel/cancelled_ já em ExtractContentFiles e InstallContentPackage
- 6290cb2748 | Talvez | Bump do FFmpeg d980192e->c4f44071; submódulo ausente, conferir diff antes
- 62fd1129d0 | Não se aplica | Profiler e escopos D3D12
- 6398071f67 | Não se aplica | Build macOS (renome temporário de threading_posix)
- 6435e79904 | Talvez | Multi-wait por futex (-21% ciclos Lost Odyssey); Xendroid+ tem variante própria
- 669c9709d2 | Não se aplica | Duplicata canary de b495439ba9
- 68bd11df09 | Não se aplica | Console apps; excluídos do build Android
- 6a67369f85 | Já tem | mutex.cc já guarda o tid em thread_local
- 6bef1fbed1 | Não se aplica | Duplicata idêntica de 052365bc01
- 6f464b6343 | Já tem | byte_swap já em string_buffer.cc; caminho só x86
- 6f483f7a70 | Não se aplica | Backend Metal
- 6f9840568b | Não se aplica | Duplicata canary de c9536890c9
- 7015473a77 | Não se aplica | Duplicata idêntica de 9d8210b32d
- 71061ff480 | Não se aplica | Build macOS
- 71dcd50040 | Já tem | Cvar volume em apu_flags.cc; resto é UI desktop
- 72c4104ec6 | Não se aplica | Contorno Wine; to_local está comentado no Xendroid+
- 7301cf24cf | Não se aplica | Chamadores Android mapeiam só leitura, offset 0; resto é savestate
- 73388a080f | Talvez | Join fora da trava, NanoSleep, Terminate próprio; ganho pequeno no Android
- 755ef33dd5 | Já tem | Gêmeo ac00d09244 já está no XenDroid
- 7646f6e683 | Talvez | Série XeFu (PTE sem frame)
- 76ce245b8f | Talvez | Ideia: estacionar o fiber; Android espera em condition_variable próprio
- 78a2ab421f | Não se aplica | premake
- 79feeb9095 | Portar | Hora do guest adiantada pela sessão anterior após relaunch
- 7a3775d866 | Não se aplica | Limpeza; com NEON o ramo escalar fica morto
- 7a85e6fb50 | Não se aplica | Patch binutils de testes
- 7b2ea9f9ac | Portar | Dump de crash deref nulo em thunk JIT esconde o crash
- 7be830a2dd | Portar | GoD/SVOD de disco viram ODD; corrige WatchDogs
- 7ef873b0d5 | Já tem | Proteção equivalente via callback_mutex (estilo 8aa50e0e07)
- 8249bbe381 | Talvez | Reserva da janela modo usuário; série XeFu
- 8358e0d532 | Talvez | Self-map da tabela de páginas; série XeFu
- 83f5968d56 | Não se aplica | Pin antigo de dados de patches
- 8512a65a05 | Já tem | Gêmeo d68ff7243a já está no XenDroid
- 8705cd19cb | Não se aplica | Alinhamento de rsp x64; sp AArch64 já alinhado
- 873c9f064a | Não se aplica | Pin antigo de dados de patches
- 87c2411270 | Portar | AllocRange podia alocar acima do teto pedido
- 8831095273 | Já tem | SIGBUS já encadeado em exception_handler_posix.cc
- 891c7cfe47 | Não se aplica | zstd BMI2 no Windows (premake)
- 89b7047ddc | Não se aplica | XInput via SDL
- 8aa50e0e07 | Já tem | callback_mutex por cliente já em audio_system
- 8ca0062f13 | Não se aplica | ZAR de STFS no fluxo desktop; CompressDiscToZarchive do Xendroid+ já lê pela VFS
- 8d99ccefb2 | Não se aplica | GetUserFolder só usado pelo xenia_main desktop
- 8e44485d9c | Não se aplica | Só build; objetos não referenciados já ficam fora
- 909008870f | Não se aplica | Argumentos não ANSI no Windows
- 92f5b712d0 | Não se aplica | Remove arquivos de janela Win32/GTK
- 95171aea9d | Não se aplica | Driver SDL
- 955e1d1723 | Talvez | Depende do pool de I/O 5a9104a20 (lista kernel)
- 95a3d08948 | Portar | Cvar media_type (auto/hdd/odd) para corrigir detecção por jogo
- 98b33fff7e | Já tem | Gêmeo 470e62af22 já está no XenDroid
- 98b6319c55 | Já tem | Equivalente próprio em memory.cc (blocos inteiros armados)
- 9cdf7a3227 | Talvez | Falha de launch volta à lista; fluxo Android é outro
- 9d08d64b5d | Portar | plugins.toml por título com barra invertida nunca carrega; app expõe allow_plugins
- 9d8210b32d | Portar | XMA new: cadeia de skip por sub-stream; LEGO travava em cutscenes
- a11cc81d01 | Já tem | SetVolume de sessão já em apu_flags.cc; resto é UI
- a13309e331 | Talvez | QueryProtect devolvia lixo em falha; consumidores raros no Android
- a1c2b745ba | Portar | KeQueryInterruptTime corria 100x rápido no POSIX
- a20f7bf692 | Já tem | Dump de crash do Xendroid+ já imprime LR
- a545f52554 | Não se aplica | Edge atual voltou a passar alignment; assert não existe mais
- a7a3eabdbb | Talvez | upper_ascii errado mas sem uso hoje; pré-requisito de accaef3f07
- accaef3f07 | Talvez | Ordem de listagem igual ao NTFS; nenhum jogo citado
- addda160ca | Não se aplica | Aviso de saída já existe; RAII não age sem unwind no bionic
- adf56b76c4 | Portar | XMA new: frame cruzando pacote era pulado (BGM de musou)
- aee0871dd7 | Não se aplica | Duplicata idêntica de adf56b76c4
- af26835e35 | Não se aplica | Log do processo de UI desktop
- b04d96a9cb | Já tem | Gêmeo 6e5b2a95b0 já está no XenDroid
- b0e3dda66d | Não se aplica | Build/setup de repositórios de dados
- b11d194c44 | Não se aplica | Renome Canary->Edge
- b495439ba9 | Portar | Rename no POSIX gravava o caminho inteiro como nome
- b6bd8f306b | Não se aplica | Submódulos Metal
- b6e27cb4f9 | Portar | Só a parte que remove o gate /dev/shm do Emulator::Setup
- b899a97a7e | Portar | NEON para ConvertFrame e downmix 5.1; hoje ARM64 usa escalar
- b96b6255c9 | Não se aplica | Tamanho da janela desktop
- b9e00e6cce | Portar | Abrir diretório com escrita falhava (EISDIR) no POSIX
- b9fa366e52 | Talvez | Série XeFu (entrada 4 KB ausente)
- bba7701d8e | Não se aplica | wxWidgets
- bdcc36f3b3 | Não se aplica | ResumeHostContext só x64
- bf547da02e | Não se aplica | premake snappy
- bfa261b8c2 | Já tem | Espera bloqueante já em threading_timer_queue.cc (Android)
- c30cab1ae6 | Talvez | Views por página; série XeFu
- c731a47d5d | Não se aplica | Profiler desligado
- c9536890c9 | Portar | O_APPEND mandava toda escrita posicionada ao fim do arquivo
- c9716fbdca | Não se aplica | Relaunch multiprocesso desktop
- c99ef5ab85 | Não se aplica | Força SDL no XMP; seria prejudicial no Android
- c9e512fab0 | Não se aplica | mesa macOS
- ca42e669c9 | Talvez | Proteção de páginas em hosts de 16 KB; só aparelhos 16 KB
- cbc767df13 | Não se aplica | Mutex macOS
- cc08b85226 | Não se aplica | Profiler
- cfb81bfe54 | Não se aplica | memfd no GNU/Linux; Android usa ASharedMemory
- cfd59c998c | Não se aplica | GDBStub
- d059e17718 | Não se aplica | Lista de patches na UI wx; o lado Android não usa
- d111615ee4 | Já tem | xma_context_fake.cc já presente
- d2724c3033 | Já tem | Código presente, desligado no Android (sem pthread_cancel)
- d33e440e5c | Talvez | Endurece relaunch/reset; RelaunchTitle do Xendroid+ é próprio
- d379c156e3 | Já tem | Gêmeo 2d6b7c6ca1 já está no XenDroid
- d659fcd8b7 | Não se aplica | Driver de teclado desktop
- d8348b4f68 | Não se aplica | Versão canary de 73388a080f; Xendroid+ segue linha edge
- d8e5dae74e | Portar | delete-on-close liberava entrada com handles abertos (uso após liberação)
- db6efc74c9 | Não se aplica | Thread de SDL
- db7eac3d01 | Já tem | Gêmeo dac9883473 já está no XenDroid
- dcd5bba81a | Não se aplica | Relatório de exceção do Windows
- dce39db98c | Não se aplica | Build de dados
- dd924f2d8a | Não se aplica | Só log em testes
- ddd89c293f | Não se aplica | Metal
- e040cd328b | Não se aplica | premake asio
- e043a40f5e | Não se aplica | Relaunch via processo de UI desktop
- e23721bd4c | Talvez | Lista de jogos OG Xbox; série XeFu, parte UI desktop
- e25bfd8242 | Já tem | Handshake de kick próprio cobre os dois casos (xma_decoder.cc)
- e2b3c38473 | Não se aplica | Atribuição de slots na UI desktop
- e3b335fcbb | Não se aplica | Metal
- e522d05e87 | Já tem | ContentInstallEntry/InstallContentPackage já no núcleo; resto é UI Qt
- e59f0d831e | Não se aplica | Máscara de features x64
- ea335a7af9 | Não se aplica | Log no relaunch multiprocesso desktop
- eac7a2d9ca | Não se aplica | Hotkeys de controle desktop
- ef5d527c1e | Não se aplica | Processo de UI desktop
- efbac5e9f6 | Talvez | Handlers de falha rodam com FPCR do guest; benefício pequeno
- f0e05d5eaa | Não se aplica | mesa macOS
- f11c262bef | Talvez | xbox.xex x XeFu; série XeFu
- f1c14eb25e | Não se aplica | Submódulo mesa
- f213cc6a9f | Portar | Tipo de dispositivo errado marcava slot vazio (Madden 25)
- f21ebd49e9 | Já tem | Mesmo efeito de 6a67369f85; tid já em cache
- f4848c875f | Já tem | Gêmeo ea056849fa já está no XenDroid
- f521cb3ef0 | Já tem | Superado: Xendroid+ tem detecção de stall corrigida (ade7e610b)
- f55adea3fe | Talvez | Série XeFu (segmentos pela tabela de páginas)
- f57e11a1b4 | Não se aplica | premake-cmake
- f63a14e6dc | Não se aplica | shm_open; Android usa ASharedMemory
- f65fda3320 | Portar | stat sem checagem gerava tamanho lixo; classifica por st_mode
- f73c521230 | Não se aplica | Só compilação de macros UPDATE_
- f7a59b9f22 | Não se aplica | macOS
- f7c90d0b5c | Portar | Migração podia apagar Headers após cópia falha e lançar exceção
- f9479a91ce | Não se aplica | Refatoração usada só pela série XeFu
- f9ed4cd520 | Não se aplica | Driver de teclado
- fb3d24c04c | Não se aplica | mesa
- ff4fcc7b6b | Não se aplica | Revertido por 01c399164d
- 103257c5aa | Não se aplica | Driver SDL; ideia de subtipo por slot serve ao driver Android
- 1c81b86747 | Portar | Só PosixEvent::Query (NtQueryEvent); resume perdido já corrigido
- 2372a54105 | Talvez | Arredonda o caminho escalar; dispensável se portar NEON
- 699ad42f29 | Talvez | Fallback silencioso sem dispositivo; muito acoplado ao canary
- 97bec07804 | Não se aplica | Só nível de log
