# Revisão de commits upstream: kernel (Xendroid+)

Lista: 235 commits (231 do xenia-edge, 4 do xenia-canary) que
mexem em xboxkrnl, XAM, threads e objetos, escalonador de fibras, sockets e
System Link. Leitura feita contra o repositório do Xendroid+ (núcleo em
`emulator-core/src/main/cpp/xenia`), sem compilar nem executar nada.

**Base real do Xendroid+.** O XenDroid sincronizou o edge até `84cf20922`
(2026-08-01, `[APU] Signal kick completion...`); no clone atual do edge o mesmo
commit é `e25bfd824` (o edge reescreveu hashes depois). O primeiro commit do
Xendroid+ (2026-09-28) é igual ao HEAD do XenDroid (2026-08-20) nos arquivos do
kernel. Resultado: `xboxkrnl_io.cc`, `xboxkrnl_ob.cc`, `xboxkrnl_crypt.cc`,
`xfile.cc`, `xsocket.cc`, `xam_input.cc`, `content_manager.cc`,
`user_tracker.cc`, `xlivebase_app.cc`, `xmp_app.cc` e outros são idênticos ao
edge daquele ponto; divergem `guest_scheduler.cc` (mudanças do XenDroid e do
Xendroid+), `xobject.cc`, `xthread.cc`, `xboxkrnl_threading.cc`, `xevent.cc`,
`xam_ui.cc` (diálogos Android), `xam_info.cc`, `xam_content.cc` e `emulator.cc`.
Os 58 commits da lista anteriores a esse ponto estão no histórico do XenDroid;
aparecem na lista porque código posterior os reescreveu ou reverteu.

**Convenções.** Commit duplicado (cópia no canary de um commit do edge),
revertido ou substituído por outro da própria lista leva "Não se aplica", com
o commit a usar no motivo; assim a lista "Portar" é exatamente o que aplicar.
O Android define `XE_PLATFORM_LINUX=1` e `XE_PLATFORM_xendroid=1`
(`base/platform.h:39-40`), então correções "Linux/POSIX" valem para o
Xendroid+. O Xendroid+ roda com `headless=true` (`kernel_flags.cc:12`): os
diálogos XAM são respondidos sem janela ou por diálogos Android
(`HostDialogScope` em `xam_ui.cc`).

## Resumo

235 commits: **Portar 45**, **Talvez 85**, **Já tem 37**, **Não se aplica 68**.

Temas de maior valor (detalhes na tabela abaixo):

1. **Correções do escalonador de fibras** (`d4b1ae936c`, `e0e1cf7e31`, `086b4421df`, `d275d8e30f`): crash por TLS lido depois de troca de fiber em espera alertável; prompt de nome do Oblivion; travamento no menu do Halo: Reach. O escalonador já é o padrão no Xendroid+.
2. **Reconciliar o `signal_state` que o guest escreve direto** (`54fd4a3078`): crashes em jogos musou e trava no loading do Crackdown (rádio) sob o escalonador.
3. **Timers e APCs** (`75b54532c8` → `50d9996230`, `1c45c01643`): XAPC vazada a cada disparo de timer até esgotar o heap do sistema; tutorial do Eternal Sonata; IRQL do alvo preso em APC_LEVEL quando um timer ou worker insere APC.
4. **Correções pequenas de I/O e objetos** (`887beea697`, `20b97ca617`, `1689b263d3`, `b42d204e0a`, `b09650b146`, `33f3a47ce2`, `672bc8b81a`): erros de leitura no Dead Rising, `FatalError` em `ObCreateObject` com nome, `\??\` em links e caminhos, handles do Burnout Paradise.
5. **Conteúdo XAM** (`2f6baa7514`, `3e4a82b60d`, `ed0e06112c` e outros): instalação de DLC dentro do jogo (BF3, FM3), arquivos do título fechados por engano ao fechar um pacote, saves do RR6.
6. **Winsock/XSocket** (`43d796754f` e mais sete): `select` bloqueante parava todas as fibers da mesma CPU; `fWaitAll` invertido em `WSAWaitForMultipleEvents`; estouro de buffer do host em `accept`.
7. **Entrada com UI ativa** (`17c22494bf` → `3d06054b86`): o Crackdown esgota o heap drenando teclas com diálogo aberto; os diálogos Android do Xendroid+ marcam a UI como ativa.
8. **Perfil e cripto pequenos** (`ec38c30cd2`, `0d1a38e7e1`, `6ff2a56f6a`, `58e1db8e78`, `6db5a346d2`, `a0afe67b93`, `ac1ece3086`, `ddfe2b87f9`): perfil com idioma lido como controle parental, settings sobrepostos, RC4/SHA/RSA/MD5/SHA-512 errados ou vazando memória.
9. **Avulsos de baixo custo** (`2f858c4629`, `8c74cfd0c0`, `7ea10c4f2d`, `581ba93e72`): música do Black Ops II, exceções C++ do guest chegando ao `catch(...)`, pausa de spin real em arm64 (o mutex global gira 128 vezes sem pausa hoje), `XUserMuteListQuery` respondendo.

Fica como **Talvez** (grande ou precisa de medição no aparelho): I/O de arquivo
assíncrono (PGR3), modelo de tempo de leitura, System Link (túnel UDP e XLink
Kai; usável em Wi-Fi, custo G), modo usuário para XNA/XeFu e a troca da cripto
para Mbed TLS.

## Temas recomendados

Ordem: maior valor primeiro. Caminhos relativos a `src/xenia/` do Xendroid+.

| Tema | Commits (sha curto) | O que traz para o Xendroid+ | Evidência (arquivo/função) | Dependências | Risco de conflito | Esforço | Veredito |
|---|---|---|---|---|---|---|---|
| Escalonador: correções que faltam | `d4b1ae936c`; `e0e1cf7e31` → `086b4421df`, `d275d8e30f` (`c097a6d319` opcional) | Fim de um crash (ponteiro de `XThread` nulo/errado numa espera alertável depois de a fiber mudar de thread); prompt de nome do Oblivion volta; menu do Halo: Reach não trava sob prioridade estrita (janela de fundo de ~1 ms por vblank nas CPUs 2-5, como no console) | `CooperativeWait` relê `XThread::GetCurrentThread()` a cada volta (`kernel/xobject.cc:249,261`) e o boost do `WaitMultiple` também (`:724,738,774`); `XThread::SetActiveCpu` não move a thread da fila pronta (`kernel/xthread.cc`); `DequeueReady` sem janela; `KeQueryBackgroundProcessors`/`KeSetBackgroundProcessors` só na tabela (`xboxkrnl_table.inc:142,166`) | `e0e1cf7e31` (`repoll_preempt`, `TakeHeadRequeue`) antes de `086b4421df` e `d275d8e30f`; `d4b1ae936c` é independente | Médio: `guest_scheduler.cc` tem mudanças próprias (fast path de yield, `guest_scheduler_dispatch_cpus`, `guest_yield_sleep_us`, preempção forçada) | M | Portar |
| Reconciliar o dispatch header escrito pelo guest | `54fd4a3078` (não aplicar `dcf2994ea1`, cópia do canary) | Evento/semáforo/mutante cujo `signal_state` o jogo zera com store inline (KeResetEvent/KeInitializeEvent inline) deixa de acordar errado: crashes em jogos musou e trava no loading do Crackdown com o rádio, sob o escalonador | `XEvent`, `XSemaphore`, `XMutant` sem `SyncFromGuest`; `GetNativeObject` não reconcilia (`kernel/xobject.cc`) | `59c08cd462` só renomeia `X_DISPATCHER_FLAGS` → `X_OBJECT_TYPES` (opcional, reduz atrito) | Médio: `xevent.cc` do Xendroid+ tem diagnóstico próprio (`RecordCreator`/`RecordSetter`) | M | Portar |
| Timers e APCs | `75b54532c8` → `50d9996230`; `1c45c01643` | Um `XAPC` por timer (antes um por disparo, vazando até esgotar o heap do sistema quando a thread espera sem ser alertável); o disparo do timer seta um evento e acorda waiters cooperativos (tutorial de batalha do Eternal Sonata); timers cancelados antes de desmontar o kernel; APC inserida por thread host não sobe/desce o IRQL do alvo | `XTimer::SetTimer` chama `cb_thread->EnqueueApc` a cada disparo e não acorda waiters (`kernel/xtimer.cc`); `XThread::EnqueueApc` usa o contexto do alvo fora de thread guest (`kernel/xthread.cc:838`), e `xeKeInsertQueueApc` faz acquire/release do spinlock mudando o IRQL dele (`xboxkrnl_threading.cc`) | `50d9996230` usa `apc_ptr_`/`InsertOwnedApc` de `75b54532c8`; `1c45c01643` aplica direto (`fwd`) | Baixo a Médio | M | Portar |
| I/O e objetos: correções pequenas | `887beea697`, `20b97ca617`, `1689b263d3` → `b42d204e0a`, `b09650b146`, `33f3a47ce2`, `672bc8b81a` | `NtOpenFile` com a ordem certa de argumentos: hoje as opções de abertura saem do argumento de compartilhamento (`FILE_SHARE_READ` = 1 vira `FILE_DIRECTORY_FILE`) e `FILE_SYNCHRONOUS_IO_*` se perde (erros de leitura de disco no Dead Rising); `ObCreateObject` com nome não derruba o app; `\??\X:` em links e caminhos; nome nulo não lê o endereço guest 0; tamanho e datas atuais em `NtQueryFullAttributesFile`; objetos do loader fora da numeração de handles (Burnout Paradise indexa arrays por handle) | `NtOpenFile_entry` passa `open_options` no lugar de `share_access` (`xboxkrnl_io.cc`); `xeObSplitName` = `FatalError` (`xboxkrnl_ob.cc:27`); `NtOpenSymbolicLinkObject` valida antes de tirar `\??\`; `ObDeleteSymbolicLink` não tira | `b42d204e0a` usa o helper de `1689b263d3`; `672bc8b81a` é base dos eventos pooled do tema I/O | Baixo (`xboxkrnl_io.cc`/`xboxkrnl_ob.cc` iguais ao edge do sync); `672bc8b81a` Médio (object table, xobject, xthread) | P; `672bc8b81a` M | Portar |
| Conteúdo XAM | `2f6baa7514`, `3e4a82b60d`, `ed0e06112c`, `0e5bc07239`, `8ae1c14856`, `aac25ad0c2` | Instalação de DLC/arquivos pelo próprio jogo (BF3, FM3); fechar um pacote não fecha mais todos os arquivos do título; miniatura não aparece como save (RR6); TU montado como volume HDD; `license_mask=1` passa a valer | `CloseOpenedFilesFromContent` usa prefixo `""` se o link sumiu (`kernel/xam/content_manager.cc:526`); `license_mask > 1` (`:419`); `xeXamContentResolve` ignora `create_directory` (`kernel/xam/xam_content.cc:88`) | nenhuma entre si | Baixo (`content_manager.cc` igual ao edge do sync; `xam_content.cc` com 98 linhas próprias) | P | Portar |
| Winsock/XSocket | `43d796754f`, `63b6bf5887`, `522ce657d3`, `ad74dabc0d`, `4e019f4090`, `0554993759`, `b62d6f9edd`, `f16dcba84b` | `select` com timeout não para mais as fibers da mesma CPU; `WSAWaitForMultipleEvents` com `fWaitAll=FALSE` esperava todos e sempre retornava 0; EOF do TCP vira 0; `WSAGetLastError` correto; `accept` sem estouro de buffer do host; `SO_EXCLUSIVEADDRUSE` aceito; socket aceito herda modo não bloqueante; byte order de `getsockname` | `NetDll_select_entry` chama `select` bloqueante (`kernel/xam/xam_net.cc:987`); mapa `{~0x0004u, ~SO_REUSEADDR}` gera opção inválida (`kernel/xsocket.cc:71`) | `4e019f4090`, `0554993759`, `b62d6f9edd` trazem trechos de System Link (`StreamAccept`, `net::Endpoint`) a retirar | Médio (resto igual ao edge do sync, mais `network_enabled`) | M | Portar |
| Entrada XAM com UI ativa | `17c22494bf` → `3d06054b86` | Com diálogo aberto, `XamInputGetKeystrokeEx` dá `EMPTY` (o Crackdown esgota o heap drenando teclas); `ANY_USER` não dá mais "desconectado" com controles ligados; `XamInputGetState` zera o estado em vez de deixar o buffer do jogo como estava; guia oculto para títulos | `XamInputGetState`/`GetKeystrokeEx` retornam sucesso sem escrever com UI ativa (`kernel/xam/xam_input.cc:110,195`); `HostDialogScope` do Xendroid+ marca a UI ativa nos diálogos Android (`kernel/xam/xam_ui.cc:202`) | `3d06054b86` depois de `17c22494bf` | Baixo (`xam_input.cc`/`input_system.cc` iguais ao edge do sync; `input_system.h` +7 linhas) | M | Portar |
| Perfil XAM | `ec38c30cd2`, `0d1a38e7e1`, `6ff2a56f6a`, `f41ee57bc3`, `907d92bf8c` (só a parte do gamertag) | Perfil com idioma deixa de parecer com controle parental; settings de perfil não se sobrepõem nem leem fora da entrada GPD; tamanho máximo de propriedade certo; corrida nas propriedades; gamertag sem estouro em `CreateAccount` | `IsParentalControlled` testa `kLanguageMask` (`kernel/xam/xam.h:94`); `get_max_size` com `&` (`user_data.h:157`); `copy_and_swap_truncating(..., sizeof(account.gamertag))` (`profile_manager.cc:575`) | nenhuma | Baixo (arquivos iguais ao edge do sync) | P | Portar |
| Cripto: correções | `58e1db8e78`, `6db5a346d2`, `a0afe67b93`; `ac1ece3086`, `ddfe2b87f9` | RC4 com chave de tamanho qualquer (antes sempre `key[x % 16]`); `XeCryptShaFinal` como o console (GTA IV: Episodes from Liberty City japonês); RSA certo; MD5 sem escrever além do estado guest; SHA-512 com o digest certo; sem vazar um contexto FFmpeg por chamada | `XeCryptRc4Key_entry` (`xboxkrnl_crypt.cc:391`); `XeCryptMd5*`/`XeCryptSha512*` alocam `av_md5_alloc`/`av_sha512_alloc` sem liberar e `XeCryptSha512Final` recebe `XECRYPT_SHA256_STATE` | `ac1ece3086`/`ddfe2b87f9` usam Mbed TLS (`4101dfa07`, lista misc); alternativa: correção local com FFmpeg | Baixo | P; os dois últimos M | Portar |
| XMP | `2f858c4629` (não `0c6dc9b628`, nem a cópia `12a0558b8f`) | Música do Black Ops II; só `XmpPlaybackControllerChanged` é primado, o que evita os travamentos de Halo 4/Borderlands 2 da primeira versão | `RegisterNotifyListener` não prima XMP (`kernel/kernel_state.cc`) | nenhuma | Baixo (`fwd`) | P | Portar |
| Avulsos | `8c74cfd0c0`, `7ea10c4f2d`, `581ba93e72` | Exceção C++ do guest entregue ao `catch(...)` (465607E5 no menu); `isb` como pausa de spin em arm64 no mutex global e nos spinlocks (no Android `XE_PLATFORM_LINUX=1` ativa o mutex de 128 voltas, hoje sem pausa entre os CAS); `XUserMuteListQuery` responde sucesso em vez de cair em mensagem não implementada | `HandleCppException` só registra (`kernel/xboxkrnl/xboxkrnl_debug.cc:107`); `_mm_pause` só em AMD64 (`base/mutex.cc:133,221`, `xboxkrnl_threading.cc:1302`); `xlivebase_app.cc` sem o caso `0x0005800E` | `8c74cfd0c0` deixa alguns quadros de pilha host por `throw` (custo aceito pelo edge); `7ea10c4f2d`: medir | Baixo | P | Portar |

## Escalonador de fibras do edge

### O que é e como funciona

`GuestScheduler` (`kernel/guest_scheduler.{h,cc}`) troca "uma thread host por
thread do guest" por um escalonador cooperativo dentro do kernel:

- Cada uma das 6 CPUs lógicas do Xenon ganha uma thread host de despacho
  (`RunLoop`). Cada `XThread` do guest vira uma fiber host fixada pela
  `current_cpu` do guest. Fibers de CPUs diferentes rodam em paralelo; as da
  mesma CPU se alternam.
- Trocas acontecem em pontos de yield (esperas, `KeDelayExecutionThread`,
  `NtYieldExecution`, saída) e em safepoints do JIT, que testam
  `PPCContext::preempt_requested`. O flag é levantado pelo watchdog no fim do
  quantum (`guest_scheduler_quantum_us`, 1000 µs), por uma thread de prioridade
  maior que fica pronta, ou por um wake. Filas prontas por prioridade 0-31
  (bitmap `ready_summary`), prioridade estrita, quantum com decaimento e boost
  como o NT.
- Esperas (`XObject::Wait`, `WaitMultiple`, `SignalAndWait`) viram
  poll+estacionamento (`CooperativeWait` → `BlockCurrentThread`). Event,
  Semaphore, Mutant e conjuntos de até 8 objetos só re-pollam quando a "época"
  de sinal muda; os demais re-pollam a cada 1 ms; um backstop força re-poll a
  cada 64 ms.
- Chamadas bloqueantes do host: no Xendroid+ `RunBlockingHostCall` manda a
  leitura de arquivo a um worker de I/O e estaciona a fiber. No edge isso virou
  `PostHostCall` + espera num `XEvent` (`04b3b63d93`), com pool de até 4
  workers (`5a9104a205`) e leituras assíncronas de verdade (`c6cc761676`).
- Um watchdog acorda a cada quantum para marcar preempção e detectar travas
  (`ReportNoProgress`, relatório de stall). Um único `std::mutex lock_` guarda
  todas as filas.

### Ligado por padrão?

Sim, nos dois. Cvar `guest_scheduler` (categoria Kernel), padrão `true` no edge
desde `34357e2578` (2026-08-01), com
`UPDATE_from_bool(guest_scheduler, 2026, 8, 1, 13, false)` para virar configs
antigas; vale só depois de reiniciar. No Xendroid+: `kernel/kernel_flags.cc:23-29`,
`emulator-core/src/main/assets/config/default_config.toml:245` e
`SettingsSchema.kt:94` (padrão `true`, exposto na UI). Outras cvars no
Xendroid+: `guest_scheduler_quantum_us` (1000; a UI aceita 250-8000),
`guest_scheduler_stats`, e as próprias do Xendroid+
`guest_scheduler_dispatch_cpus` (6) e `guest_yield_sleep_us` (0). O edge
acrescentou depois `guest_scheduler_spin_quanta` (0, `83abd43576`) e, no VFS,
`storage_request_timing` (`true`, `e987fd7f50`).

### Série de commits e dependências

Já no Xendroid+ (sync do XenDroid até `e25bfd824` mais cherry-picks):
fundação de junho `46dca60a53` → `9079e0bbfe` → `6f2e8a5897` → `afa25f8a01`;
julho `c159d855de`, `e268e113be` (safepoints por flag, lista cpu), `264374a5f5`,
`5a05e2a2cc`, `7aec08288e`, `e9bed6b4da`, `7976f8af09`, `65f9686cb4`,
`055fac08e3`, `dfac87cd24`, `256cc3a904`, `6f78967c3f`, `34357e2578`; agosto
(rfandango, vindos do XenDroid) `137d68bdc4`, `404bfa3a52`, `1fad140848`,
`f94aa54cd9`, `833dd7df7d`, `64a0242af3`, `bb798a0b84`, `9a232c8a1a`; e
`728c93d652`. Mudanças próprias do Xendroid+ por cima: retorno imediato de
`NtYieldExecution` sem nada pronto (`b17b9a13f`, equivale a `44599705bc`),
`guest_yield_sleep_us` (`ff489e77a`), detecção de migração em
`YieldCurrentThread` (`e048e2e06`), estacionamento por progresso da GPU
(`73f71a138`).

Faltando, em ordem de aplicação:

| Ordem | Commit | O que faz | Veredito |
|---|---|---|---|
| 0 | `c097a6d319` | Remove o caminho `sole_waiter` (código morto também no Xendroid+: `WakeForSignal(this, nullptr)` em `xobject.cc:401`). Só reduz conflito | Não se aplica (opcional) |
| 1 | `e0e1cf7e31` | `repoll_preempt` separado de `preempted`; `TakeHeadRequeue` | Portar (pré-requisito) |
| 2 | `d275d8e30f` | Janela de fundo: CPUs 2-5 preferem prioridades 0-9 por ~1 ms a cada vblank (máscara `0xEDB403FF`, intervalo mínimo de 16 ms); implementa `KeQuery/SetBackgroundProcessors`. Halo: Reach | Portar |
| 3 | `bf300b65f8` → `83abd43576` | Contadores; hack opt-in contra inanição (`guest_scheduler_spin_quanta`) | Não se aplica / Talvez (quirk) |
| 4 | `d4b1ae936c` | Resolve o waiter uma vez, sem TLS depois de yield | Portar |
| 5 | `086b4421df` | `MigrateForAffinity` quando o guest muda a afinidade. Oblivion | Portar |
| 6 | `5a9104a205` → `27db26839b` → `c6cc761676` → `04b3b63d93` → `5e9eae601b` → `e987fd7f50` | Pool de I/O, wake só da CPU certa, leitura assíncrona, espera por evento, modelo de tempo | Talvez (tema I/O) |
| 7 | `44599705bc` | Pula yield vazio | Já tem (equivalente) |
| 8 | `80d9a5c58a` | Boost só para waiter que bloqueou (depende da assinatura de `d4b1ae936c`) | Talvez |
| 9 | `50d9996230` | Timer acorda waiters cooperativos (precisa de `75b54532c8`) | Portar |
| 10 | `43d796754f`, `e69277661a`, `41a01dbed6`, `ef6cebf6a3` | `select` e operações de arquivo sem bloquear a thread de despacho | Portar (`43d796754f`) / Talvez |
| 11 | `11583551bd`, `6aa3406e80` | Modo usuário (fibers extras por thread) | Talvez |
| 12 | `7598c1779a` | APC de kernel interrompe esperas não alertáveis | Talvez |
| 13 | `97dbf7bc36` | Tira a preempção forçada depois de 4096 safepoints em DISPATCH_LEVEL | Talvez (risco) |
| — | `8237a12d92`, `182cd53257` | Profiler por fiber (profiler desligado no build Android), diagnóstico de crash | Não se aplica |

Do lado da CPU (lista cpu): `e268e113be` (o Xendroid+ já emite o teste,
`A64Emitter::EmitPreemptCheck` em `cpu/backend/a64/a64_emitter.cc:814`) e
`6fc16d118d` (endereço do handler de preempção no contexto do backend a64 e
handler zerado no `Shutdown`).

### O que ele toca fora do kernel

- JIT: `EmitPreemptCheck` emite `ldrb` + `cbnz` em `preempt_requested` na
  cabeça de blocos (laços), com cauda fria chamando `preempt_yield_handler`;
  `spin_backoff_yield_handler` em `a64_seq_memory.cc` cede a fiber quando um
  laço de `lwarx`/`stwcx.` colapsado gira; `log_safepoint_pc` opcional.
- Threading base: `xe::threading::Fiber`, uma thread por CPU, eventos; crash
  dentro de fiber: `Emulator::ExceptionCallback` desvia para
  `HaltCrashedFiberThunk`.
- Spinlocks do kernel: `xeKeKfAcquireSpinLock` cede a fiber se o dono está na
  mesma thread de despacho.
- XAM/rede: `XSocket::RunCooperatively` (recv/send/connect); no edge também
  `select` e DNS.
- VFS: `supports_concurrent_io` (`5a9104a205`) e modelo de tempo
  (`5e9eae601b`).
- UI/GPU: `GuestScheduler::WaitOnFence` para diálogos; no Xendroid+, também a
  espera da GPU estacionada.

### Ajuda ou atrapalha no Android

O que o código mostra:

- Com o escalonador, as dezenas de threads de um jogo viram fibers em 6
  threads de despacho, mais o watchdog e um worker de I/O. O escalonador do
  Android vê poucas threads ocupadas em vez de dezenas, e prioridade e
  afinidade do guest não dependem do host (o Xendroid+ já ignora as do host:
  `ignore_thread_priorities` e `ignore_thread_affinities` padrão `true`). Isso
  tende a reduzir trocas de contexto e disputa com as threads da GPU e do
  áudio.
- Até seis threads de despacho ocupadas ainda são mais que os 1-3 núcleos rápidos
  de um Snapdragon; se o Android puser uma delas num núcleo lento, todas as
  fibers daquela CPU ficam lentas juntas.
- O watchdog acorda a cada quantum (1 ms) e pega o lock do escalonador: cerca
  de mil despertares por segundo enquanto o escalonador roda.
- Esperas viram poll: waiters não gated re-pollam a cada 1 ms, e um wake
  perdido custa até 64 ms. Qualquer chamada bloqueante do host feita na fiber
  para a CPU inteira (era o caso do `select`, `43d796754f`, e do open/stat de
  arquivo, `e69277661a`).
- Prioridade estrita cooperativa pode travar jogos em que uma thread de alta
  prioridade gira esperando trabalho de uma de baixa: é o que `d275d8e30f`
  corrige do jeito do console e o que `83abd43576` contorna como hack.
- TLS lido depois de troca de fiber é uma classe de bug: o Xendroid+ já achou
  um caso (`e048e2e06`) e lista o risco como "não corrigido" em
  `performance-tests/cloud-2026-09-28-results.md`; `d4b1ae936c` corrige o caso
  de `CooperativeWait`, que no edge derrubou o emulador. No Xendroid+ o efeito
  depende de o compilador reaproveitar o endereço do TLS entre chamadas
  (possível com LTO, ligado no Release); a correção vale de qualquer forma.

O que precisa de medição no aparelho antes de portar:

- Pool de I/O e leitura assíncrona: até 4 threads host a mais contra menos
  paradas da thread de render (PGR3).
- Janela de fundo: em ~6% do tempo das CPUs 2-5 as prioridades 0-9 passam na
  frente; pode tirar fatia de threads de alta prioridade.
- `97dbf7bc36`: o XenDroid pôs a preempção forçada em DISPATCH_LEVEL
  (`137d68bdc4`) para escapar de livelock; tirá-la por fidelidade (motivo:
  XeFu) pode trazer o livelock de volta.
- `80d9a5c58a` e `10e20f3bf0` mudam a dinâmica de prioridade (boost).
- `27db26839b` e o fim do `WakeAll` por conclusão de I/O: menos wakes, ganho
  provável de CPU/energia, não medido.

Recomendação: portar o bloco de correções (`e0e1cf7e31`, `d4b1ae936c`,
`086b4421df`, `d275d8e30f`, `54fd4a3078`, `75b54532c8`, `50d9996230`,
`1c45c01643`, `43d796754f`) e testar Halo: Reach, Oblivion, Crackdown e um
jogo de streaming pesado; deixar I/O assíncrono e `97dbf7bc36` para uma
segunda rodada com medição.

## Talvez

| Tema | Commits (sha curto) | O que traz para o Xendroid+ | Evidência (arquivo/função) | Dependências | Risco de conflito | Esforço | Veredito |
|---|---|---|---|---|---|---|---|
| I/O de arquivo assíncrono | `c6cc761676`, `04b3b63d93`, `5a9104a205`, `27db26839b`, `10e20f3bf0`, `8a027ef4f9`, `e60b90ac04`, `beb3230fe9`, `8c8c550aec`, `90f47ed85a`, `7598c1779a`, `e69277661a`, `41a01dbed6`, `78315ec19e` | Leitura em handle assíncrono retorna `PENDING` e termina num pool (PGR3 sem quedas de quadro na corrida); open/stat/rename fora da thread de despacho; semântica NT de evento, IOSB e APC. Traz regressões já corrigidas dentro da série (Split/Second, Cars, PGR3) | `NtReadFile_entry` sempre conclui antes de retornar (`if (true \|\| file->is_synchronous())` em `xboxkrnl_io.cc`); evento do arquivo auto-reset (`kernel/xfile.cc:24`) | Ordem cronológica; `78315ec19e` precisa de `77f2cca800` (lista gpu); `90f47ed85a` precisa de `887beea697`; `04b3b63d93` usa os eventos de host de `672bc8b81a` | Alto (`guest_scheduler.cc`, `xobject.cc`; o caminho de APC do `NtReadFile` foi corrigido pelo XenDroid para o Fable II) | G | Talvez |
| Modelo de tempo de leitura | `5e9eae601b`, `e987fd7f50` | Leituras do disco do título no ritmo de um HDD do 360 (35 MB/s, 6 ms de seek). Corrige UEFA Champions League 2006-2007; deixa os outros jogos carregarem mais devagar | `storage_request_timing` (padrão `true` no edge) | `04b3b63d93` | Médio | M | Talvez, como quirk por título |
| Escalonador: ajustes a medir | `83abd43576` (+ `bf300b65f8`), `80d9a5c58a`, `97dbf7bc36` | Hack opt-in contra inanição (útil em `game_quirks.cc`); boost só de waiter que bloqueou; DPC sem troca forçada | ver seção do escalonador | `e0e1cf7e31`; `97dbf7bc36` depende do modo usuário | Médio a Alto | M | Talvez |
| Timers e DPCs fiéis ao console | `2741af8430`, `54729c29fc`, `0e7e29aaee`, `fb1c5bf08b` | `KeSetTimerEx`/`KeCancelTimer` para KTIMER do guest (hoje só na tabela); expiração no tick de 1 ms; DPC aninhada; DPC no processador alvo | `KeCancelTimer` só em `xboxkrnl_table.inc:98` | `50d9996230` | Médio | M | Talvez |
| Modo usuário (XNA e XeFu) | `11583551bd`, `6a39ea6839`, `856ebcdb34`, `6aa3406e80`, `1df3d93fc1`, `4c6992b397`, `70f1237cce`, `a9fe61e036`, `fc46978664` | Runtime .NET CF dos jogos XNA (indie) e o emulador de Xbox original do 360 (XeFu) | Ke*UserMode ausentes | `32a95fff0`, `b5cc59e854`, `123209cb03` (lista cpu); entrega de falha em a64 POSIX desligada no próprio edge (`2416330549`) | Alto | G | Talvez |
| Cripto via Mbed TLS | `dc715b17f4`, `e257c1437e`, `6c32d37781`, `81c23a1b0e` | RSA, HMAC com chave > 64 bytes, paridade DES e `XeCryptDesCbc` (falta no Xendroid+) | `xboxkrnl_crypt.cc` sem `XeCryptDesCbc` | `4101dfa07` (lista misc), `89f90684f5` (lista cpu) | Baixo | M | Talvez |
| Objetos com tipo do título | `889d93e134` + `fe40c0d68f`, `db3cbc2c47` | `X_OBJECT_HEADER` com 0x10 bytes como no 360 (hoje 0x18); `ObInsertObject` para tipos definidos pelo jogo | `X_OBJECT_HEADER` em `kernel/xobject.h:100` | os dois primeiros juntos; `db3cbc2c47` lê `flags` do header novo | Médio | M | Talvez |
| Versão do sistema e sessões | `28f05bdddd`, `e71a423883`, `5b116d57ca` | `XamGetSystemVersion` real e kernel 17559 por padrão (hoje 1888 e 0); XSession guarda estado e responde GetDetails/Modify | `XamGetSystemVersion_entry` retorna 0 (`kernel/xam/xam_info.cc:214`) | `5b116d57ca` em cima de `e71a423883` (System Link) | Médio | M | Talvez |
| Conteúdo: extras | `f5c8af81a5`, `3ecd9d36bf`, `16ba848086`, `cf3dff81c1`, `d633c5e784` | Pacotes STFS soltos na pasta de conteúdo montados direto; mídia de boot achada pelo mount; pacote achado com caixa diferente (só em FS que diferencia caixa; o `storage_root` padrão fica no armazenamento externo do app, que no Android costuma ignorar caixa); Internet Explorer | `ContentPackage` só monta diretório | `3ecd9d36bf` mexe em `emulator.cc` (585 linhas próprias) | Médio | M | Talvez |
| Loader XAM | `2b7d553c4d`, `be780154b0`, `74c4e4acba` | `XamLoaderLaunchTitleEx` e flags 2/4; `XamNavigateBack`; exports faltantes | relançamento in-process próprio em `xam_info.cc` (`XE_PLATFORM_xendroid`) | — | Médio | M | Talvez |
| Rede: stubs e DNS | `1bdece5250`, `2585d3cd69`, `5629c82ab1`, `a0e1ba8313`, `5e1ae7db19`, `ef6cebf6a3` | XHttp*, XAuth, LiveHive, DNS de verdade | `NetDll_XNetDnsLookup_entry` devolve erro (`xam_net.cc:590`) | `ef6cebf6a3` precisa de `e69277661a` | Baixo | P | Talvez |
| Perfil: extras | `80679bcadd` + `159337dbba`, `8c98ef0280`, `d9bf601e1c` | `XamUserGetSubscriptionType`, enumerador de títulos jogados, `XapipGetLocale`/`XamReadString` | stub em `xam_user.cc:1050` | — | Baixo | P | Talvez |
| XMP e microfone | `1c5059dccf`, `6d3efdd31c`, `5f55dc7ff9`; `367d22d269`, `4d8b3d650e`, `afd282d505` | Stubs e retornos de XMP; `MicDeviceRequest` (Rock Band 2, Guitar Hero World Tour) | microfone desligado por padrão | — | Baixo | P | Talvez |
| Avulsos | `0da2a16134`, `b2741d1baf`, `82d0cd1f44`, `3ba9c86eb0` | TU incompatível desligado por padrão; XMsg overlapped pendente por um quadro; mais funções de UI XAM; structs de entrada | `allow_incompatible_title_update=true` (`kernel/kernel_state.cc:39`) | — | Baixo | P | Talvez |

### System Link (rede local)

Commits: `6ffd0fe704` → `72649ed117` → `ba539b60c2` → `0d909f040b` →
`06c98e28b9` → (`e71a423883`, `3b21734278`, `ec197944a4`, `1f15d3e437`,
`fb59484428`, `c51b5fb096`, `127853227a`, `a378d948c2`) → (`64676a60c5`,
`07fbf220c1`, `379bb943f8`, `5b116d57ca`). Veredito: Talvez, esforço G.

- O que é: o protocolo seguro do System Link do 360 (troca Diffie-Hellman,
  associações, TCP sobre XNet) num diretório novo `src/xenia/net` (~5.500
  linhas, mais ~4.500 de testes), e a integração em XAM/XSocket: sockets UDP/VDP/TCP de
  título com chave de LAN vão para o System Link em vez da rede do host.
- Transportes (`systemlink_transport`): `tunnel` (padrão) manda UDP na porta
  36020 para outras instâncias do Xenia na LAN, por broadcast ou para um IP
  fixo (`systemlink_tunnel_address`); `kai` passa por uma engine do XLink Kai
  (`systemlink_kai_engine`), que alcança consoles reais e jogadores pela
  internet; `off` desliga.
- No celular: o túnel é UDP comum e deve funcionar em Wi-Fi entre aparelhos
  da mesma rede. `c51b5fb096` troca broadcast por unicast quando o par já é
  conhecido, porque o Wi-Fi segura broadcast para estações em economia de
  energia: bom para celular. `06c98e28b9` usa `getifaddrs` (existe no bionic).
  Não verificado: em muitos aparelhos a recepção de broadcast/multicast em
  Wi-Fi depende de `WifiManager.MulticastLock` e é filtrada com a tela
  apagada; isso seria no app Kotlin, nada no edge cuida disso. Com Kai, o
  celular pode apontar para uma engine num PC ("Allow remote DDS connections").
- Custo: ~16 commits; dependência nova Mbed TLS (`4101dfa07`, lista misc);
  mudanças grandes em `xsocket.cc`/`xam_net.cc`, onde o Xendroid+ tem
  `network_enabled` e quirks que desligam sockets (`game_quirks.cc`); UI
  Kotlin para as quatro cvars; os testes não entram no APK. `a378d948c2`
  passa a reportar o link de rede ligado; sem System Link funcionando isso
  pode levar jogos a tentar a LAN.

## Apêndice: todos os commits

Ordem da lista de trabalho. Formato: `sha | veredito | motivo`.

- `04b3b63d93` | Talvez | I/O síncrono esperando evento do pool; base do I/O assíncrono, grande
- `0554993759` | Portar | SO_EXCLUSIVEADDRUSE falhava; socket aceito herda modo não bloqueante
- `06c98e28b9` | Talvez | System Link: túnel UDP difunde por todas as sub-redes
- `07fbf220c1` | Talvez | System Link: segmentos TCP sobre XNet seguro (~3000 linhas)
- `086b4421df` | Portar | Reloca thread pronta ao mudar afinidade; prompt de nome do Oblivion
- `08b1d460f1` | Não se aplica | App desktop (macOS): drenar loop de UI ao parar
- `0b0d57a1bc` | Não se aplica | Kinect (5345084D); não há NUI no Android
- `0c2da08dd1` | Não se aplica | Só log do handshake de launch data
- `0c6dc9b628` | Não se aplica | Versão inicial revertida no edge (Halo 4/BL2); portar 2f858c4629
- `0d1a38e7e1` | Portar | Limita leituras de settings/GPD: blobs sobrepostos e leitura além da entrada
- `0d909f040b` | Talvez | System Link: liga XLink Kai/túnel UDP ao XAM e XSocket
- `0da2a16134` | Talvez | TU incompatível desligado por padrão; decisão de produto, aviso ImGui não serve
- `0e5bc07239` | Portar | XContentQueryVolumeDeviceType responde HDD para raiz em pacote (TU)
- `0e7e29aaee` | Talvez | DPC que se reenfileira não recursa mais (estouro de pilha); sem jogo citado
- `107d80230f` | Não se aplica | Revertido no próprio edge; Xendroid+ usa user_language inteiro
- `10e20f3bf0` | Talvez | Boost IO_DISK_INCREMENT ao concluir I/O; parte do tema I/O
- `11583551bd` | Talvez | Ke*UserMode (XNA .NET CF, XeFu); grande, depende de memória/JIT
- `1210c26c81` | Não se aplica | Revert de d811c1ac4c; o par se anula
- `127853227a` | Talvez | System Link: datagramas ao próprio endereço do console
- `12a0558b8f` | Não se aplica | Cópia do canary de 2f858c4629; aplicar só um
- `12f5084b93` | Não se aplica | UI XAM de perfil (apagar títulos); Xendroid+ tem UI própria
- `159337dbba` | Talvez | XamUserGetSubscriptionType sem overlapped; par de 80679bcadd
- `1689b263d3` | Portar | `\??\` em links simbólicos: open/delete falhavam (remapear GAME:/D:)
- `16ba848086` | Talvez | Pacote com caixa diferente; o armazenamento externo do Android costuma ignorar caixa
- `17c22494bf` | Portar | XamInputGetKeystrokeEx dá EMPTY com UI ativa (Crackdown esgota heap)
- `182cd53257` | Não se aplica | Só diagnóstico de crash de fiber e SignalAndWait
- `19b223dea4` | Não se aplica | Remove cvar headless, de que o Xendroid+ depende
- `1af8dc6b27` | Não se aplica | Diálogo desktop para escolher jogo indie no launcher XNA
- `1bdece5250` | Talvez | XHttpCrackUrl; nenhum jogo citado
- `1c0ec20fae` | Não se aplica | UI Qt desktop
- `1c45c01643` | Portar | APC inserida por thread host não mexe no IRQL do alvo
- `1c5059dccf` | Talvez | Stub XMPGetMediaSource; valor baixo
- `1df3d93fc1` | Talvez | Trap frame no formato que o XeFu lê (modo usuário)
- `1e9c3f5fe9` | Não se aplica | UI desktop (painel de info, grade)
- `1f15d3e437` | Talvez | System Link: envios esperavam até 25 ms
- `20b97ca617` | Portar | ObSplitName era FatalError: ObCreateObject com nome derruba o app
- `2226749150` | Não se aplica | Depende do refactor XContent do canary que o edge descartou
- `22a39ec9d6` | Não se aplica | Substituído por 28f05bdddd
- `2416330549` | Não se aplica | Falhas de modo usuário só em x64 POSIX; a64 segue desligado
- `2585d3cd69` | Talvez | Stubs XampXAuth*; nenhum jogo citado
- `2741af8430` | Talvez | KeSetTimerEx/KeCancelTimer para KTIMER do guest; nenhum jogo citado
- `27db26839b` | Talvez | Acorda só a CPU do waiter de I/O; 04b3b63d93 substitui
- `28f05bdddd` | Talvez | XamGetSystemVersion real, kernel 17559 por padrão; muda versão para todos
- `2b7d553c4d` | Talvez | XamLoaderLaunchTitleEx e flags; conflita com relançamento do Xendroid+
- `2f6baa7514` | Portar | XamContentResolve cria o diretório; DLC instalado no jogo (BF3, FM3)
- `2f858c4629` | Portar | Prima XmpPlaybackControllerChanged: música do BO2 sem travar Halo 4/BL2
- `2feb0117e9` | Não se aplica | UI Qt da lista de jogos
- `33c3248b44` | Já tem | XThread::SelfSuspend (xthread.cc) cobre a auto-suspensão
- `33f3a47ce2` | Portar | NtQueryFullAttributesFile relê tamanho e datas da entrada
- `34357e2578` | Já tem | guest_scheduler=true com UPDATE_from_bool (kernel_flags.cc:23-29)
- `34f752fe78` | Já tem | Substituído por SelfSuspend com variável de condição
- `364dc1d770` | Não se aplica | Só structs de NtDeviceIoControlFile
- `367d22d269` | Talvez | Stub MicDeviceRequest (Rock Band 2, GHWT); microfone fica desligado
- `379bb943f8` | Talvez | System Link: sockets TCP na LAN do System Link
- `3b21734278` | Talvez | Perfil Live local para hospedar System Link (Ikaruga); UI wx fora
- `3ba9c86eb0` | Talvez | Structs de entrada; XamInputGetCapabilities via cópia EX
- `3d06054b86` | Portar | Controles com UI ativa: estado zerado, packet number, guia oculto
- `3e4a82b60d` | Portar | Fechar conteúdo liberava todos os handles do título se o link sumiu
- `3ecd9d36bf` | Talvez | Mídia de boot pelo mount, não GAME:/D:; emulator.cc muito divergente
- `41a01dbed6` | Talvez | Ajuste de RunBlockingIo (tema I/O)
- `43d796754f` | Portar | select() deixa de bloquear a thread de despacho sob o escalonador
- `44599705bc` | Já tem | Equivalente: YieldExecution/NothingElseToRun no guest_scheduler.cc
- `45b7917efa` | Já tem | MountPathMatches e prefixo sem caixa já presentes
- `46dca60a53` | Já tem | Escalonador base presente (versão evoluída)
- `48889c3d2e` | Já tem | Posição do XFile é atômica (substitui o mutex)
- `4905a1c3fa` | Não se aplica | XeFu: lançamento em outro processo no desktop
- `4b925cb4f2` | Não se aplica | launch_data.txt do app desktop; Xendroid+ relança no processo
- `4c6992b397` | Talvez | Páginas de modo usuário em 4 KB e proteção da PTE (XeFu)
- `4d8b3d650e` | Talvez | XMic: detalhes e código de retorno; microfone desligado
- `4e019f4090` | Portar | accept estourava buffer do host; getsockopt escondia erros
- `4fc57ca433` | Não se aplica | Só log de NtCreateFile
- `50d9996230` | Portar | Timer acorda waiters cooperativos ao expirar (tutorial do Eternal Sonata)
- `522ce657d3` | Portar | recv devolve 0 no fechamento ordenado do TCP, não erro
- `54729c29fc` | Talvez | Timers expiram no tick de 1 ms como no console
- `54fd4a3078` | Portar | Reconcilia signal_state escrito pelo guest; musou e Crackdown no escalonador
- `5629c82ab1` | Talvez | Stubs XHttpStartup/XHttpOpen (títulos offline listados)
- `581ba93e72` | Portar | XUserMuteListQuery responde sucesso em vez de erro a cada chamada
- `58e1db8e78` | Portar | RC4 usava 16 bytes de chave fixos; chave de outro tamanho errada
- `59c08cd462` | Não se aplica | Renomeia X_DISPATCHER_FLAGS; cosmético, mas facilita cherry-picks
- `5a9104a205` | Talvez | Pool de I/O em dispositivos concorrentes; até +4 threads host
- `5aa0d3dea5` | Já tem | xeXamEnumerate já zera a contagem na falha
- `5b116d57ca` | Talvez | Estado de XSession (GetDetails/Modify); depende de e71a423883
- `5d4dc8a88a` | Não se aplica | Depende do refactor XContent do canary descartado pelo edge
- `5e1ae7db19` | Talvez | XNetDnsLookup de verdade; títulos podem buscar servidores mortos
- `5e9eae601b` | Talvez | Modelo de tempo de leitura (UEFA CL 06-07); carrega mais devagar
- `615c357ef7` | Não se aplica | Launcher desktop do XeFu
- `63b6bf5887` | Portar | WSAWaitForMultipleEvents: fWaitAll invertido e retorno sempre 0
- `64676a60c5` | Talvez | System Link: connect simultâneo invalidava endereço do par
- `65b481ed29` | Já tem | Partes de kernel presentes; relançamento in-process próprio
- `672bc8b81a` | Portar | Objetos do loader fora da numeração de handles (Burnout Paradise)
- `6785a3c87a` | Não se aplica | Assert vira log
- `6a39ea6839` | Talvez | KeContextToKframes e flag de modo usuário (XeFu)
- `6a63399744` | Não se aplica | UI desktop
- `6aa3406e80` | Talvez | Trap frame de código de modo usuário interrompido (XeFu)
- `6b5c3233d5` | Já tem | Tabela já mapeia 0xC0000240 para 0x4D3; bug só existia após a5262a968a
- `6c32d37781` | Talvez | Paridade de XeCryptDesParity; exige Mbed TLS
- `6d3efdd31c` | Talvez | XMP: retornos e structs faltantes
- `6d530a1ab9` | Não se aplica | Só structs de IoCreateDevice
- `6db5a346d2` | Portar | XeCryptShaFinal deixa o bloco preenchido (GTA IV EFLC japonês)
- `6e9bac0dff` | Já tem | XamXStudioRequest já é stub em xam_nui.cc
- `6f2e8a5897` | Já tem | Re-poll, parada de fiber com crash e teardown presentes
- `6fcd7bb916` | Não se aplica | Relançamento in-process do desktop (Linux/Windows)
- `6ff2a56f6a` | Portar | Tamanho máximo de propriedade usava máscara em vez de mínimo
- `6ffd0fe704` | Talvez | System Link: núcleo do protocolo seguro (Mbed TLS)
- `7044f215fc` | Não se aplica | App desktop: migração de dados
- `70f1237cce` | Talvez | Exports LDI (LZX) usados pelo XeFu
- `7140403ad1` | Já tem | xlivebase_app.cc igual ao edge do sync
- `72649ed117` | Talvez | System Link: registro de pares e portas do guest
- `74c4e4acba` | Talvez | Exports XAM faltantes e callback de launch; nenhum jogo citado
- `7598c1779a` | Talvez | Conclusão de I/O como APC de kernel; muda esperas, grande
- `75b54532c8` | Portar | Uma APC por timer: XAPC vazada a cada disparo esgotava o heap
- `7803acc3df` | Não se aplica | Revertido por b2c51e0a09 (áudio do Afro Samurai)
- `78315ec19e` | Talvez | Resolve antes de leitura parcial (PGR3); depende de commits de GPU
- `7aec08288e` | Já tem | Gate por época das re-polls presente
- `7b1c15137a` | Não se aplica | UI Qt de conquistas
- `7d8db5a2cd` | Não se aplica | lpvoid_t por pointer_t; cosmético
- `7ea10c4f2d` | Portar | Pausa de spin real (isb) em arm64 nos spinlocks e mutex
- `7f631aa954` | Já tem | XSocket do Xendroid+ é o do edge no sync (asio)
- `80679bcadd` | Talvez | XamUserGetSubscriptionType lido do perfil
- `80d9a5c58a` | Talvez | Boost só se o waiter bloqueou (como o NT); medir
- `81c23a1b0e` | Talvez | XeCryptDesCbc (faltava); exige Mbed TLS
- `8237a12d92` | Não se aplica | Log de profiler por fiber; profiler desligado no build Android
- `828014b908` | Já tem | Xendroid+ (como o edge) não envia StorageDevicesChanged no boot
- `82d0cd1f44` | Talvez | Mais funções de UI XAM; Xendroid+ responde diálogos sem janela
- `83abd43576` | Talvez | Hack opt-in contra inanição; útil como quirk por título
- `856ebcdb34` | Talvez | Pilha para contexto de modo usuário sem pilha (XeFu)
- `86327a3e77` | Já tem | Locks de xfile/xmutant/xtimer presentes
- `882cd7e917` | Não se aplica | Biblioteca de jogos do app desktop
- `887beea697` | Portar | NtOpenFile lia OpenOptions do argumento errado (Dead Rising)
- `889d93e134` | Talvez | X_OBJECT_HEADER no layout do 360; par de fe40c0d68f
- `8a027ef4f9` | Talvez | Evento do arquivo manual-reset como no NT; com o tema I/O
- `8ae1c14856` | Portar | XamContentDelete aceita XCONTENT_DATA_INTERNAL
- `8b81fe77d9` | Já tem | SelfSuspend com variável de condição presente
- `8c74cfd0c0` | Portar | Exceção C++ do guest chega ao catch(...) (465607E5 no menu)
- `8c8c550aec` | Talvez | Ordem de escrita de overlapped/scatter; tema I/O
- `8c98ef0280` | Talvez | Enumerador de títulos jogados corrigido
- `8d8ba9b6f7` | Não se aplica | Refactor do canary que o edge não adotou
- `8db2ce2603` | Não se aplica | UI desktop
- `9079e0bbfe` | Já tem | Esperas cooperativas presentes
- `907d92bf8c` | Portar | Só a parte do gamertag (overflow em CreateAccount), como o edge fez
- `90f47ed85a` | Talvez | Rejeita I/O assíncrono sem offset (XeFu); exige 887beea697
- `913ec6b9b2` | Já tem | Uma instância de UI do guest; partes de kernel presentes
- `91ee517898` | Já tem | Partes que o edge manteve estão presentes
- `97dbf7bc36` | Talvez | Remove preempção forçada em DISPATCH_LEVEL (XeFu); risco de livelock
- `99361c54e5` | Não se aplica | App desktop
- `99b3ccad26` | Não se aplica | Renomeia structs de XamTaskSchedule
- `9a0474a2f8` | Não se aplica | Flags e structs
- `9a232c8a1a` | Já tem | Cherry-pick do XenDroid 99da95bd9 (ReportNoProgress)
- `9be56834e0` | Já tem | No Android TerminateTitle e ExitToDashboard já terminam em quick_exit
- `a0afe67b93` | Portar | Divisão longa do bignum errada: RSA errado na maioria das chaves
- `a0e1ba8313` | Talvez | Stubs XamQueryLiveHive e XNetLogonGetMachineID
- `a1c36f2f18` | Não se aplica | UI Qt
- `a378d948c2` | Talvez | Link de rede ligado no boot; depende do System Link
- `a5262a968a` | Não se aplica | Nomes de erro; cosmético (sozinho introduz bug)
- `a9fe61e036` | Talvez | MmLockAndMapSegmentArray (XeFu)
- `aac25ad0c2` | Portar | license_mask=1 passa a valer em pacotes
- `ac1ece3086` | Portar | XeCryptMd5* escrevia além do estado guest e vazava contexto FFmpeg
- `ad74dabc0d` | Portar | Erros de socket chegam ao WSAGetLastError
- `aebd98b620` | Não se aplica | Só log
- `afa25f8a01` | Já tem | Várias threads de despacho presentes
- `afd282d505` | Talvez | XMic: mais detalhes; microfone desligado
- `b09650b146` | Portar | NtCreateFile com nome nulo lia o endereço guest 0
- `b0a1f40b41` | Não se aplica | Cópia do canary de 6db5a346d2
- `b11458e49b` | Não se aplica | Refactor XContent do canary; o edge o descartou no merge
- `b2741d1baf` | Talvez | XMsg overlapped não conclui antes de retornar; muda timing
- `b2c51e0a09` | Não se aplica | Revert de 7803acc3df
- `b2d1e57eab` | Não se aplica | UI ImGui do desktop
- `b2f55aa103` | Já tem | xam_net.cc igual ao edge do sync (mais network_enabled)
- `b3d508d566` | Não se aplica | Build Qt e diálogo desktop
- `b42d204e0a` | Portar | Remove `\??\` dos caminhos de NtCreateFile/NtOpenFile/QueryFull
- `b5275a6cb9` | Já tem | Partes de kernel presentes; app desktop não se aplica
- `b5496e9ea5` | Não se aplica | App desktop: processo filho
- `b62d6f9edd` | Portar | accept sem timeout de SO_RCVTIMEO; connect em socket já conectado
- `b703570045` | Já tem | SO_REUSEADDR já ligado em xsocket.cc
- `b7336b5605` | Não se aplica | Cvar de dump de módulos (depuração)
- `b8001c4185` | Não se aplica | UI desktop de multi-disco
- `ba539b60c2` | Talvez | System Link: transportes XLink Kai e túnel UDP
- `bb798a0b84` | Já tem | Cherry-pick do XenDroid 4dde8412e
- `be780154b0` | Talvez | XamNavigateBack vai ao dashboard; nenhum jogo citado
- `beb3230fe9` | Talvez | NtReadFile postado concluído como no NT; tema I/O
- `bf300b65f8` | Não se aplica | Só contadores (pré-requisito de 83abd43576)
- `bfae7a7a12` | Já tem | Retornos de threading presentes
- `c097a6d319` | Não se aplica | Código morto (sole_waiter); facilita cherry-picks do escalonador
- `c50e07331d` | Já tem | Partes de kernel presentes; app desktop não se aplica
- `c51b5fb096` | Talvez | System Link: unicast no túnel em Wi-Fi (bom em celular)
- `c6298dd0e3` | Não se aplica | Diálogo ImGui; Xendroid+ já sai ao dashboard
- `c6cc761676` | Talvez | NtReadFile assíncrono no pool (PGR3 em corrida); série grande
- `ceb328035a` | Já tem | Mapa supported_socket_options equivalente em xsocket.cc
- `cf3dff81c1` | Talvez | Características 2 em volume de pacote; só o Internet Explorer
- `d275d8e30f` | Portar | Janela de fundo do console no escalonador (menu do Halo Reach)
- `d4b1ae936c` | Portar | TLS lido após troca de fiber derruba espera alertável
- `d633c5e784` | Talvez | XamContentSetThumbnailInternal e stubs
- `d7a868837c` | Já tem | Emulator::current_disc_number já acompanha o disco
- `d811c1ac4c` | Não se aplica | Revertido por 1210c26c81
- `d9679b7482` | Não se aplica | Diálogos Qt; Xendroid+ tem XamSwapDisc próprio
- `d9bf601e1c` | Talvez | XapipGetLocale, XamReadString etc.; nenhum jogo citado
- `db3cbc2c47` | Talvez | ObInsertObject para tipos do título; depende do header de 0x10
- `dbbaf2232b` | Não se aplica | Diálogo Qt
- `dc715b17f4` | Talvez | RSA via Mbed TLS (substitui a0afe67b93)
- `dcf2994ea1` | Não se aplica | Cópia do canary de 54fd4a3078
- `ddfe2b87f9` | Portar | XeCryptSha512Final com digest errado e contexto FFmpeg vazado
- `de6556b3c2` | Não se aplica | Biblioteca de jogos do desktop
- `dfac87cd24` | Já tem | Pulso de evento a waiters parados presente (com pulse_epoch)
- `e0e1cf7e31` | Portar | Separa repoll_preempt; pré-requisito de d275d8e30f e 086b4421df
- `e257c1437e` | Talvez | HMAC-SHA com chave longa; parte da troca para Mbed TLS
- `e31142bd79` | Já tem | AsioErrorToWSAError já trata would_block/try_again
- `e54a3658f0` | Não se aplica | UI
- `e60b90ac04` | Talvez | Regressões do NtReadFile assíncrono (Split/Second, Cars)
- `e69277661a` | Talvez | open/stat/rename fora da thread de despacho
- `e71a423883` | Talvez | XSession mínimo para System Link
- `e824ae6e37` | Não se aplica | UI
- `e9626150cf` | Não se aplica | UI
- `e987fd7f50` | Talvez | Ajuste do modelo de tempo de leitura (FM4)
- `ec197944a4` | Talvez | WSARecvFrom overlapped (host de System Link)
- `ec38c30cd2` | Portar | Perfil com idioma definido lia como controle parental
- `ed0e06112c` | Portar | Miniatura do pacote some da enumeração (saves do RR6)
- `ed4540a641` | Já tem | Sleeps de XMP já ausentes em xmp_app.cc
- `ef6cebf6a3` | Talvez | DNS via RunBlockingIo; depende de e69277661a
- `f035700edc` | Já tem | Assert de memória devkit já removido
- `f13a11c2e2` | Não se aplica | UI e processos do desktop
- `f16dcba84b` | Portar | getsockname com byte order errado; adiciona getpeername
- `f3b1a3b641` | Já tem | Substituído pelo relançamento in-process
- `f41ee57bc3` | Portar | Mutex no vetor de propriedades (corrida no Poker Smash)
- `f5c8af81a5` | Talvez | Monta pacotes STFS soltos nas pastas de conteúdo
- `f653561f7d` | Não se aplica | SHA-256 via Mbed TLS sem mudar comportamento
- `fb1c5bf08b` | Talvez | DPC com processador alvo roda nele; nenhum jogo citado
- `fb59484428` | Talvez | XNetQosLookup (System Link)
- `fbfffd9b84` | Não se aplica | UI
- `fc46978664` | Talvez | XamUserLogon slot a slot (XeFu, launcher XNA)
- `fdb8e4d593` | Não se aplica | UI Qt
- `fe40c0d68f` | Talvez | Corrige o tamanho do header (par de 889d93e134)
- `04f85416e0` | Não se aplica | Kinect (canary); tipos do título cobertos por db3cbc2c47
- `21faab895a` | Não se aplica | NUI/Kinect (canary)
- `5f55dc7ff9` | Talvez | XMP com ajustes por controle (canary)
- `c3ec0ba255` | Não se aplica | UI NUI/Kinect (canary)
