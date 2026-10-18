# Varredura upstream: CPU, frontend PPC e backend A64

Lista: 235 commits do has207/xenia-edge.
Base comparada: `emulator-core/src/main/cpp/xenia` (Xendroid+).
Revisão só de leitura: nada foi compilado nem executado; cada veredito vem do diff upstream comparado com o código atual do Xendroid+.

## Resumo

Contagem por veredito: **Portar 53** · **Talvez 64** · **Já tem 37** · **Não se aplica 81** (total 235).

O que muda a leitura do worklist:

- **51 commits são do backend A64 de Will Martin (oaknut)**, que o próprio edge reverteu (f4e36a673b, também no worklist). O A64 do Xendroid+ vem do backend xbyak_aarch64 do Herman, então esse código não existe aqui. Onde a correção semântica importava (half Xenos, offsets grandes de locals, wrap de 32 bits no endereço, shifts mascarados), o Xendroid+ já tem o equivalente.
- **O Xendroid+ desligou os fixups de NaN do PPC em 2026-10-07** (6da558bc0: `a64_fpu_nan_fixup` e `a64_vmx_nan_fixup` = false, regra de NaN do host). Por isso a série de NaN/FPSCR do Herman e da XeniOS fica em Talvez, salvo onde o caminho padrão diverge do x64. Com o fixup ligado, o Xendroid+ ainda devolve o QNaN padrão negativo (0xFFC00000/0xFFF8…); o hardware e o ARM usam o positivo.
- **Falso "presente" no filtro do sweep:** de4d24493 (`[CPU] Derive the FPSCR invalid-operation summary for CR1`) saiu do worklist com razão 0,99, inflada pelas linhas de `skip.txt`, mas o código não está no Xendroid+ (`PPCHIRBuilder::UpdateFPSCR` ainda é o stub que zera FX/VX). A cadeia FPSCR (32920009d9, 378c95215e, 8b19ee756f) depende dele.
- **Cuidados de port que valem para quase todo commit A64:** (1) nas sequências do Xendroid+, rótulos são `e.NewCachedLabel()`, não `Xbyak_aarch64::Label` na pilha (XenDroid c1915cdd0), e vários commits upstream usam rótulo de pilha; (2) `A64BackendContext` vive nos 256 B que `thread_state.cc` reserva antes do `PPCContext` (`static_assert`, ~224 B em uso), então commits que acrescentam campos (b416d19675, 6fc16d118d, fb225d9756) pedem aumentar `PackedContext::backend_data`; (3) há duplicatas edge/canary: 45827cc2ba = 5ce7c336df, d39b10a2ab = 63ea6e2725, 46e8fd8dcc = 577fb8ee17, 850f45610b = ca9e079051.

Temas de maior valor, em ordem:

1. **Instruções PPC com resultado errado**: mcrf, mtfsfi, mtfsb0/1, mcrfs, decodificação de vupkhsh128/vupklsh128, vcmpbfp e o CA das formas OE estão errados no Xendroid+. São patches pequenos, a maioria aplica limpo.
2. **Fold de constantes do HIR**: o fold de `Extract` usa outro lane que o backend e lê fora do vetor; `VectorCompareEQ` float e o merge de compares tratam floats por bits. Esse é o bug que o upstream ligou à promoção de VEC128 (FIFA, State of Decay); vale portar e testar de novo `context_promote_vec128` no Halo 4, que levou o Xendroid+ a desligá-la (no upstream, a cena limitada por CPU do Lost Odyssey foi de ~31 para ~37 fps com ela).
3. **MMIO no A64**: um load MMIO de 32 bits que passa pelo SIGSEGV chega com os bytes invertidos (detecção LDR+REV), e a gravação das faltas MMIO nunca é registrada, então todo acesso MMIO paga um sinal.
4. **Correções de codegen A64**: largura I8/I16, MAX/MIN com duas constantes, dot product com infinito e o modo FPCR nas idas e voltas ao host.
5. **Robustez em ARM64**: corrida na publicação de entradas do JIT (memória fraca), EntryTable O(n) sob o lock global, longjmp na primeira instrução, `bl` sem módulo dono e UB de `assert` com NDEBUG.
6. **Passe de stores de CR mortos + fusão compare/branch**: −41,6% de stores de CR no Halo 3 upstream, e o `b.cond` direto só aparece depois que o store morre.
7. **Codegen A64 mais enxuto**: imediatos, CR6 vetorial, vsldoi/vperm/vsel, stvlx/stvrx, vrsqrtefp e rematerialização de spill (o A64 tem só 7 GPRs alocáveis).
8. **Chamadas mais baratas no A64**: no Android a indireção é sempre a encoded, que monta imediatos de 64 bits em cada chamada indireta, e o `CallExtern` salva q4-q31 a cada export do kernel.
9. **HLE, XEX e boot**: `TranslateVirtual` faz duas chamadas fora de linha (`allocation_granularity()` → `getpagesize()` da libc) em todo acesso de export, antes mesmo de testar o endereço; patches XEX com sobreposição (NG2+TU) e com mudança de base (XNA Indie Player); inicializadores estáticos compilados antes do boot (Hydrophobia).

## Temas recomendados

| Tema | Commits (sha curto) | O que traz para o Xendroid+ | Evidência (arquivo/função) | Dependências | Risco de conflito | Esforço | Veredito |
|---|---|---|---|---|---|---|---|
| Instruções PPC com resultado errado | 46e8fd8dcc (= 577fb8ee17), e1e4851f7a, 7d7557f2f7, a5532a0d19, 504cbee7eb, e4b13738c3, 10da45ea40 → 4d83110dcd | mcrf copia o campo (hoje eq vira gt e SO some); mtfsfi grava o RN certo (hoje "truncar" vira "+inf"); mtfsb0/1 deixam de ser NOP; mcrfs não zera RN/NI; vupkhsh128/vupklsh128 deixam de passar pela gambiarra de vupkhsb128; vcmpbfp marca os dois bits com NaN e não liga CR6[0]; addco/subfco/subfeo voltam a gravar CA | `ppc_emit_control.cc` `InstrEmit_mcrf` (LoadCR→StoreCR→UpdateCR); `ppc_emit_fpu.cc` `InstrEmit_mtfsfix` (`i.X.RB <<`), `InstrEmit_mtfsb0x/1x` (XEINSTRNOTIMPLEMENTED), `InstrEmit_mcrfs` (`~(0xF << shift)`); `ppc_opcode_lookup_gen.cc` sem `0b1111010`/`0b1111110`; `ppc_emit_altivec.cc` `InstrEmit_vcmpbfp_` (FIXME de CR6[0]); `ppc_emit_alu.cc` `InstrEmit_addcx`/`subfcx`/`subfex` (OE sem `StoreCA`) | Nenhuma; 10da45ea40 antes de 4d83110dcd, e ele toca `ppc_emit_alu.cc` junto com b4a5072505 | Baixo (Médio em 10da45ea40) | P (M nas formas OE) | Portar |
| Fold de constantes do HIR (e reavaliar `context_promote_vec128`) | 45827cc2ba (= 5ce7c336df), 920f35c938, eddcd8f31e | Fold de `Extract` usa o mesmo lane que o backend (flip `^3`/`^1`) e não lê `u8[idx & 0x1F]` fora do vetor; `VectorCompareEQ` float compara valores (0/−0, NaN); merge de compares deixa de achar iguais dois floats constantes. Depois, testar Halo 4 com `context_promote_vec128=true` | `hir/value.cc` `Value::Extract` vs `a64_seq_vector.cc` `EXTRACT_I8` (`VEC128_B`); `Value::VectorCompareEQ` (casos int e float juntos); `simplification_pass.cc` `SameValueOrEqualConstant` (`AsUint64` dá 0 para float); cvar em `context_promotion_pass.cc` cita o Halo 4 | 920f35c938 é o mesmo hunk de `value.cc` que e4b13738c3 (portar um) | Baixo | P | Portar |
| MMIO no A64 | 2cdfb45cbf, 10ac2f5eff | Load MMIO de 32 bits pelo caminho de sinal deixa de chegar com bytes invertidos; faltas MMIO passam a marcar `accessed_mmio` no info cache (persistido em disco), e compilações seguintes chamam `MMIOAwareLoad/Store` em vez de pagar um SIGSEGV por acesso; `MMIOAware*` para de somar 0x1000 em host de páginas 4K | `mmio_handler.cc` `TryDecodeLoadStore` (detecção LDR+REV, `byte_swap = true`); `memory.cc` `emit_inline_mmio_checks=false`; `a64_backend.cc` sem `SetMMIOExceptionRecordingCallback`; `a64_seq_memory.cc` `MMIOAwareStore/Load` (`guestaddr >= 0xE0000000`) | Portar 10ac2f5eff inteiro: registrar sem o gate ativa o bug de +0x1000 | Baixo | P | Portar |
| Correções de codegen A64 | 565aad3186, fcd8e58a30, e6a86116f3, a5f59a48ae, 7b778e2a3e, fa57fbdb5f, 35d5c81d71, 95b14f55fa (parte A64) | I8/I16 mantidos com extensão por zero (ADD/SUB/NEG/NOT/SHL/SHA/MIN); MAX/MIN escalar com duas constantes; constante em v0/v1 não é mais sobrescrita antes da última leitura (ROTATE_LEFT, DENORMFLUSH, SHL/SHR_V128); lvrx com endereço alinhado não lê a memória (podia cair na página seguinte, fim de memcpy); dot product preserva infinito de entrada como o x64; op VMX após `CallNativeSafe` volta a ter FZ; código host roda com FPCR padrão, não com o arredondamento/FZ do guest; o host recupera o próprio FPCR após callback guest | `a64_sequences.cc` `ADD_I8`, `NOT_I8` (`mvn`), `NEG_I8`, `SIGN_EXTEND_I16_I8`, `MAX_F32` (`i.src2.is_constant ? e.s1`), `SHL_V128`/`SHR_V128`, `DOT_PRODUCT_3/4` (todo inf → QNaN); `a64_seq_vector.cc` `VECTOR_ROTATE_LEFT_V128` (`movi v0` antes de reler s1), `LVR_V128` (`ldr` incondicional); `a64_emitter.cc` `CallNativeSafe` sem `ForgetFpcrMode`; `a64_backend.cc` `EmitGuestToHostThunk`/`EmitHostToGuestThunk` | 95b14f55fa muda `THUNK_STACK_SIZE` (ajustar unwind POSIX); 565aad3186 tem contexto de af082b6d0d; a5f59a48ae usa o literal pool de 8642d8eff9, mas o early-out do LVR sai sozinho | Médio | M | Portar |
| Robustez: publicação no JIT, longjmp, NDEBUG | 8ffe24e372 → a77db1ed3b, 6fe1231561, df72008d9a, 6e304ab866, c87c034153, 619170cc7a | `entry->function/status` publicados sob o lock (corrida real em ARM64); EntryTable em hash map (insert O(1), `FindWithAddress` limitado); `OnThreadExit` de thread desconhecida sem deref de `end()`; `bl` sem módulo vira call indireta em vez de deref nulo no `A64Emitter::Call`; longjmp nunca cai na 1ª instrução (pularia o prólogo); checagem de sync de pilha antes do epílogo e após `COMMENT` | `processor.cc` `ResolveFunction` (escrita sem lock); `entry_table.h` `split_map`; `assert.h` (NDEBUG → no-op) + `processor.cc` `OnThreadExit`; `ppc_emit_control.cc` `InstrEmit_branch` (`f.Call(function, …)` sem checar nulo); `a64_backend.cc` `ResolveFunction` (`machine_code() != host_address` nunca rejeita); `a64_emitter.cc` `Emit` | 8ffe24e372 antes de a77db1ed3b | Baixo-Médio | P-M | Portar |
| Stores de CR mortos + fusão compare/branch | 689590ba3c + f6f9a03768, aba5703fa0 | Passe de liveness tira stores de CR que nenhum caminho lê (upstream: −41,6% stores de CR, −6,3% HIR no Halo 3 menu); com o store morto, compare+branch vira `b.cond` direto, sem `cset`+`cbnz` | `ppc_translator.cc` (pipeline sem passe equivalente); `a64_seq_control.cc` `BRANCH_TRUE_I8` (`cbnz` sobre o booleano) | f6f9a03768 é correção obrigatória; aba5703fa0 usa a infra de handoff de 7a5d9713a3 (adaptar sem 004ece768c); validar a ordem com `SpinLoopBackoffPass`/`MemoryPollParkPass` | Médio | M | Portar |
| Codegen A64 mais enxuto | af082b6d0d (só as partes fora de desvios), fb1c3dec3f, 8642d8eff9, 42087f44bf, 968f1ef459, ef4b3a4669, 882df3330f, e9582aca75, f1173df88d, a3a4cd4683, b4a5072505, 70ac6a3574, 038050012a | Imediatos add/sub/cmp/lógicos de 64 bits e MUL potência de 2 → `lsl`; CR6 de `vcmp*.` via `uminv/umaxv`; constante v128 sem forma imediata vem de literal pool (1 `ldr` em vez de até 10 instruções e um GPR); vsldoi alinhado vira um `ext`; vperm com `rev32` e vmrg via `zip`; vsel com `bit/bif`; `mov wN, wN` de TRUNCATE omitido; stvlx/stvrx sem laço de bytes (68→16 instr.); vrsqrtefp/frsqrte em helpers JIT (1 chamada no lugar de 4 `blr` crus para C); menos HIR em subfc/adde/srawx; spill rematerializado do contexto | `a64_sequences.cc` `ADD_I32` (`imm <= 4095` senão `mov`+`add`), `AND_I64` (`mov`+`and`), `RSQRT_V128`, `TRUNCATE_I32_I64`; `ppc_hir_builder.cc` `UpdateCR6` (4 extracts + ORs); `ppc_emit_altivec.cc` `InstrEmit_vsldoi_`; `a64_seq_util.h` `LoadV128Const` (fallback `mov`/`fmov`/`mov`/`ins`); `a64_seq_vector.cc` `PERMUTE_V128::EmitByInt8` (`eor 3` em runtime), `STVL_V128`/`STVR_V128`; `register_allocation_pass.cc` | Independentes; trocar rótulos de pilha por `NewCachedLabel`; o literal pool precisa conviver com a reemissão de `a64_near_branches` (o pool é zerado em `ResetPerFunctionState`); 746e5ac649 só se quiser a tabela compartilhada | Médio | M (vários P) | Portar |
| Chamadas mais baratas no A64 | b416d19675 + 7a97888962, 6fc16d118d | No Android a indireção é sempre a encoded (`execute_address_high() != 0`): cada `CallIndirect` monta bias, base do cache e tabela externa como imediatos de 64 bits, e passa a ler do contexto com 1 `ldr`; `CallExtern` deixa de salvar q4-q31 (56 acessos a menos por export do kernel); helpers com endereço fixo via contexto (−2,2% instr. upstream) | `code_cache_base.h` `Initialize` (`if (kExecuteAddrHigh != 0) try_fast_indirection = false`); `a64_emitter.cc` `CallIndirect` (`mov x14, bias/base`); `a64_backend.cc` `EmitGuestToHostThunk` (salva q4-q31) | 7a97888962 é a guarda de liveness obrigatória; b416d19675 pede 35d5c81d71 (e no upstream fc0ed6e81d; adaptar); aumentar `PackedContext::backend_data` | Médio-Alto | M-G | Portar |
| HLE, XEX e boot | 44e05e470a, 7dab7132b5, 2fe7b7d635, e011d1eeab | `TranslateVirtual` deixa de chamar `allocation_granularity()`→`getpagesize()` em todo acesso de export (o teste de endereço vem antes e `page_size()` fica em cache); patch XEX com regiões sobrepostas usa `memmove` (crash do NG2+TU em aarch64); patch que muda a base da imagem (atualização do XNA Indie Player); inicializadores estáticos da CRT (e o que chamam) compilados antes de a thread principal rodar, para compile preguiçoso não atrasar threads que o título cria (Hydrophobia não saía da tela título); independe de `enable_early_precompilation` | `ppc_context.h` `TranslateVirtual`; `memory_posix.cc` `page_size()`; `lzx.cc` `lzxdelta_apply_patch` e `xex_module.cc` `ApplyPatch` (`memcpy`); `kernel_state.cc` `LaunchModule` (`thread->Resume()` logo após criar) | e011d1eeab: no Xendroid+ a thread principal é retomada dentro de `LaunchModule`, então a chamada entra antes desse `Resume()` (o edge a move para depois dos plugins) | Baixo | P (M em e011d1eeab) | Portar |

## Talvez

| Tema | Commits (sha curto) | O que traz para o Xendroid+ | Evidência (arquivo/função) | Dependências | Risco de conflito | Esforço | Veredito |
|---|---|---|---|---|---|---|---|
| FMA vetorial com fontes diretas | 497bec2bbe (ideia) | `MUL_ADD_V128` ainda grava e relê s3 na pilha mesmo com o fixup desligado e o FZ do host descartando denormais; vmaddfp é a op VMX mais comum | `a64_sequences.cc` `MUL_ADD_V128` (`str`/`ldr` em GUEST_SCRATCH+32) | Adaptar ao caminho sem fixup; o commit em si supõe fixup ligado | Médio | P-M | Talvez |
| Pré-compilação paralela e por padrão | fc22bceae6, 510a9553be, 62140dea88, f453ede2e0, a0d11bec9d | Menos travadas de JIT em jogo; corrige timing de NG2 (intro) e Def Jam Icon (logo mudo). Custa tempo de boot e memória de code cache (Madden 25 ~325 MB no a64) | `xex_module.cc` `PrecompileDiscoveredFunctions` (serial, `enable_early_precompilation` off) | a77db1ed3b antes; medir boot e RSS no celular | Médio | M-G | Talvez |
| Rastreio do modo FPCR entre blocos | fc0ed6e81d, df7d6d95d0, e0e1e58775, 8f21542469, c5b3c02e9b | Menos `msr FPCR` (serializante) em fronteiras de bloco e thunks (Reach: 79% do thunk era o msr). A parte barata e isolada é o read-back no thunk (`mrs`+`cmp` antes do `msr`) | `a64_emitter.cc` `ForgetFpcrMode` em toda entrada de bloco; `a64_backend.cc` thunks | 35d5c81d71; e0e1e58775 supõe opcodes upstream de lfs/stfs (o Xendroid+ tem os seus) | Alto | G | Talvez |
| Endereçamento guest e handoffs | 004ece768c, 1bb7afe782, 7a5d9713a3, 6bc0aa54f5 | `[membase, Wn, UXTW]` sem o `mov` de extensão por acesso; remap 0xE0000000 decidido no endereço efetivo (correto em páginas de 16K); máscara de lvx/stvx e endereço de retorno reaproveitados | `a64_seq_util.h` `ComputeMemoryAddressOffset` (divergência anotada no próprio comentário) | Reescreve `a64_seq_memory.cc`, que no Xendroid+ tem reservas nativas, spin park e MMIO próprios | Alto | G | Talvez |
| NaN e FPSCR exatos do PPC | 9804846f42, cf43c4c522, 9900f7cebb, b2d6a41401, 089473ccc4, 36a7bb57f7, bef83c66d5, 6cc7c18358, ed9bfc9a40, 6544934138, 32920009d9, 28f38affee, 378c95215e, 8b19ee756f, 078a07b530, 92bf74906f, 39aa0bfc7d | Exatidão de NaN (ordem A,B,C, QNaN positivo), FPSCR das formas Rc=1, quirk do Xenon com double denormal em op single | `a64_backend.cc` `a64_fpu_nan_fixup`/`a64_vmx_nan_fixup` = false desde 2026-10-07; `a64_seq_util.h` `FixupVmxMaxMinNan` (`src1|src2`) | A cadeia FPSCR depende de de4d24493 (falso "presente") | Alto | M-G | Talvez |
| vexptefp/vlogefp inline, LOAD_CLOCK inline, tabela vrsqrte | fb225d9756, c7a3f1a671, 746e5ac649 | Sem `CallNativeSafe` em vexptefp/vlogefp (grade 2^-11 do hardware); mftb inline só com cvars opt-in; tabela de vrsqrtefp compartilhada entre x64 e a64 (refatoração de a3a4cd4683) | `a64_sequences.cc` `POW2_V128`/`LOG2_V128`, `a64_seq_memory.cc` `LOAD_CLOCK` | fb225d9756 põe 384 B de constantes no `A64BackendContext` (limite de 256 B) | Médio | M | Talvez |
| Modo usuário e código dinâmico (.NET CF/XNA, XeFu) | 52188fafea, f2583e9def, 32a95fff0d, 63c95eca4e, efabe1d6ac, 1e640ba491, cebb1d1abc, b5cc59e854, afbf8cf118, 0492b38b2c, 492e1cc67b, 2ea595d6c4, 0788c561e3, 2317b55473, 723f542129, 123209cb03, edf01c1e6b, fad70b50f0, c5777398f1, d3f7ca3e04, 19706836b8 | Base para títulos XNA (o runtime .NET CF entra em modo usuário e gera código) e para o emulador de Xbox original do 360 (XeFu); invalidação de código em `KeSweepIcacheRange` | `processor.cc`, `memory.cc`, `xboxkrnl_usermode.cc` (inexistentes ou sem esse suporte no Xendroid+) | Série longa; o lado A64 está em 1e640ba491 e 123209cb03; entrega de page fault só existe no win32 (7873b1ed94) | Alto | G | Talvez |
| Formatos XEX de protótipo | 131503f68d, cdd60494d7 | Chave XEX1 devkit e formato XEX% (dash 1746); só protótipos | `xex_module.cc` `Load` | cdd60494d7 depende de 131503f68d | Baixo | P | Talvez |
| Profiler de cobertura guest | 229fec07c2, 0f55d6cb50 | Contagem de execução por função e por sequência do backend | Remove `function_trace_data.h`, usado pelo `A64Emitter` do Xendroid+ | — | Alto | M | Talvez |
| Mbed TLS | 89f90684f5, abb96c9504 | Troca SHA-1/AES por Mbed TLS; corrige leitura além do buffer em bloco AES parcial | `xex_module.cc` `aes_decrypt_buffer` (ignora `output_size`) | Adicionar mbedtls ao `third_party` | Médio | M | Talvez |
| Depuração e unwind | ff681ddf48, 59d00d4baf | Unwind correto para frame guest de 224 B; SIGTRAP no POSIX; dentro de 59d00d4baf há uma correção de 2 linhas no write-back de registradores vetoriais (`regs[v]`) que vale isolar | `a64_code_cache_posix.cc` (thunk inferido pelo tamanho da pilha); `exception_handler_posix.cc` (`mcontext.regs[modified_register_index]` no laço de vetores) | — | Baixo | P-M | Talvez |

## Apêndice: todos os commits

Ordem por data do autor; `sha | veredito | motivo curto`.

- 0401029f70 | Não se aplica | GDBStub: tipo de exceção para o depurador
- 19f1dc79f8 | Não se aplica | GDBStub: escrita de registradores com --debug
- 5987130616 | Não se aplica | Servidor GDBStub do app desktop; fix de BP é do JIT x64
- c4cc075bdb | Não se aplica | Breakpoints do depurador desktop (x64)
- 6c8d3adcde | Não se aplica | Infra de testes Linux/x64; trechos de código (xbdm, guarda MMIO) já presentes
- 06c3e339ad | Não se aplica | Diálogo Qt de conquistas (UI desktop)
- 6a92e325ba | Não se aplica | Testes cpu-ppc com intel-sde (x86)
- 91d1349b79 | Não se aplica | Limpeza de shm nos testes
- 03335d56e8 | Não se aplica | Default de indireção 64-bit do oaknut
- 0cb9dfba6b | Já tem | ~CodeCacheBase já libera kIndirectionTableSize (code_cache_base.h)
- 107a148460 | Não se aplica | MAP_JIT/macOS e fallback de mapeamento do backend oaknut; Xendroid+ usa execute_address_high
- 129462f326 | Não se aplica | Ordem de pack só para Windows ARM64 no backend oaknut
- 1de36083c8 | Não se aplica | PERMUTE int16 do oaknut; Xendroid+ tem implementação própria em TBL
- 24db01fe75 | Não se aplica | Nome fixo do mapeamento (debug), backend oaknut revertido
- 283f3d7db7 | Já tem | PACK::EmitD3DCOLOR já usa fmaxnm/fminnm (a64_seq_vector.cc)
- 284024165b | Já tem | Saturação 0x7FFF do half Xenos em PACK::EmitFloatToXenosHalf4 (a64_seq_vector.cc)
- 2e82935dcc | Não se aplica | Diagnóstico de ResolveFunction do oaknut
- 310a2e8bea | Não se aplica | Capturas de lambda não usadas, cosmético
- 3afa85c4c5 | Não se aplica | Xendroid+ já usa fmaxnm/fminnm no PACK SHORT; BIC de zero diverge do x64
- 4df1d22cd1 | Não se aplica | Loads da indireção 64-bit oaknut
- 4f3f14a3d9 | Não se aplica | Aviso de variável não usada, backend oaknut revertido
- 5520cda456 | Não se aplica | SPLAT oaknut; Xendroid+ usa LoadV128Const com movi/mvni/fmov
- 5c1d4b6851 | Não se aplica | Máscara de ConstantFitsIn32Reg no oaknut; função inexistente no Xendroid+
- 5f1da79eb0 | Já tem | VECTOR_CONVERT_I2F/F2I usam SrcVReg para constantes
- 652eeac197 | Não se aplica | Indireção 64-bit do oaknut; Xendroid+ usa modo encoded do CodeCacheBase
- 683cc221c8 | Já tem | SequenceTable() singleton em a64_sequences.cc/h
- 74180887ee | Não se aplica | Brace-init cosmético no backend oaknut revertido
- 745581d4bd | Já tem | PrepareLocalBase trata offsets grandes de LOAD/STORE_LOCAL (a64_sequences.cc)
- 7bc540d07a | Não se aplica | Cast em assert, backend oaknut revertido
- 82061a5ffd | Não se aplica | Troca oaknut::List por List, cosmético
- 8a915746ab | Não se aplica | Contadores não usados, cosmético
- 9b6af46744 | Já tem | Fallback de endereço da tabela: CodeCacheBase::Initialize (modo encoded) em code_cache_base.h
- 9d331d99af | Não se aplica | Só comentário no backend oaknut revertido
- 9fe9f2ab2a | Já tem | MUL_ADD_V128 do Xendroid+ usa v2/v3 scratch, sem aliasing
- a05dfe70f2 | Não se aplica | Log de extern indefinido (XeKeys) só diagnóstico
- a12dddcf27 | Já tem | VECTOR_SHR_V128 mascara o shamt por lane (a64_seq_vector.cc)
- a6bacef4d3 | Não se aplica | Casts de enum no a64_op.h oaknut, só avisos
- a83bd019a4 | Não se aplica | Casts nos helpers C do oaknut; Xendroid+ faz shifts/pow2/log2 inline em NEON
- a8b39dc20a | Já tem | 0x7FFF->131008.0f via EmitXenosHalfToFloat4 (UNPACK FLOAT16, a64_seq_vector.cc)
- ab942708ff | Já tem | CONVERT_F64_F32 trata fonte constante (a64_sequences.cc)
- ad7a10edc5 | Não se aplica | Log de access violation do backend oaknut
- b18442e197 | Não se aplica | Init de JIT por thread no macOS (pthread_jit_write_protect_np)
- b6b45f9977 | Não se aplica | BTI/ajuste de SP do oaknut; Xendroid+ usa prólogo xbyak próprio
- b8ae35085c | Não se aplica | Capstone cosmético no backend oaknut revertido
- bdf24adeb7 | Não se aplica | Log de trap TraceOnInstruction (diagnóstico)
- c8487d641a | Não se aplica | const/maybe_unused no a64_op.h oaknut
- db039b1295 | Não se aplica | Config/log da indireção oaknut revertida; Xendroid+ usa code_cache_base.h
- dba51ae0b2 | Não se aplica | Thunk oaknut revertido; RET em alvo nulo só mascara falha de resolução
- ddd36bbab6 | Não se aplica | Tabela de indireção 64-bit oaknut; Xendroid+ tem CodeCacheBase fast/encoded
- e3a2b1f9de | Não se aplica | PlaceConstData para PAGEZERO do macOS
- eb15c34234 | Não se aplica | Include NEON do backend oaknut revertido
- ed319bccec | Não se aplica | Move helpers fimm8 entre arquivos do oaknut
- f0b87fa1c7 | Não se aplica | Cast de aviso no backend oaknut (Will Martin), revertido no edge; Xendroid+ usa xbyak_aarch64
- f5d0288631 | Não se aplica | Mensagem de erro do capstone, backend oaknut revertido
- fc38ebb985 | Não se aplica | Code cache POSIX do oaknut; Xendroid+ tem a64_code_cache_posix.cc próprio
- 62e5238ad1 | Não se aplica | Tracing/diagnóstico do app macOS
- 6a5acb4666 | Já tem | EmitGuestOffsetAdd faz soma em W (wrap 32-bit) em a64_seq_util.h
- 8dbb4edadd | Não se aplica | Delta xenia-mac do backend oaknut, revertido no edge
- d54d2666b2 | Já tem | Constantes vetoriais via SrcVReg/LoadV128Const em todo o backend
- 1d780bd98d | Já tem | DeallocFixed POSIX já retorna false em views (memory_posix.cc); resto é x64
- 3dbd58b640 | Já tem | mmio_handler.cc pula QueryProtect em XE_PLATFORM_LINUX (Android define LINUX)
- f4e36a673b | Não se aplica | Revert do merge oaknut; o backend xbyak do Xendroid+ não é afetado
- 3a1ca2dedd | Já tem | Substituído pelo CodeCacheBase fast/encoded (code_cache_base.h)
- 6970da7b78 | Já tem | Substituído pelo CodeCacheBase relocável (code_cache_base.h)
- 9f0ef3123f | Já tem | Superado: lwarx/stwcx. nativo com geração ABA-safe (a64_seq_memory.cc)
- ba05ea73d6 | Já tem | ResolveFunction com retomada de longjmp (a64_backend.cc:734)
- 3fc3597550 | Já tem | preempt_check_injection_pass só em entrada/back-edge, com melhoria local
- e268e113be | Já tem | Safepoint por flag preempt_requested (A64Emitter::EmitPreemptCheck)
- c0eaaedb48 | Já tem | Comentário já atualizado em a64_emitter.h
- 131503f68d | Talvez | Chave XEX1 devkit e formatos XEX antigos; só protótipos/devkit
- ec5c875122 | Não se aplica | Otimizações VMX só no backend x64
- 565aad3186 | Portar | ADD/SUB/NEG/NOT/SHL/SHA/MIN I8/I16 não mascaram largura; Xendroid+ tem o bug
- 6e81825dfb | Não se aplica | Só teste de unwind
- 6f2e59bb13 | Já tem | Inline de __savegprlr/__restgprlr no HIR (ppc_emit_control.cc TryInlineGprlrSaverest)
- 6fe1231561 | Portar | Com NDEBUG o assert some e OnThreadExit desreferencia end(); Xendroid+ tem o padrão
- 7b778e2a3e | Portar | DOT_PRODUCT do Xendroid+ troca todo inf por QNaN; x64 (padrão) preserva inf de entrada
- 7c59fca0a2 | Já tem | Xendroid+ já emite lwarx/stwcx. inline (a64_native_reserved_ops), versão mais robusta
- 92b0b28ec1 | Não se aplica | Só teste de integração
- a6c8b1450b | Não se aplica | Move cvars do app desktop para xenia-core
- af082b6d0d | Portar | Imediatos add/sub/cmp/lógicos, MUL pot2->lsl, SWIZZLE ext/dup; desvios já cobertos (a64_near_branches)
- b6d4c0de75 | Já tem | Restauração inline já usa CallIndirect CALL_TAIL|POSSIBLE_RETURN no HIR
- c7a3f1a671 | Talvez | LOAD_CLOCK inline só com cvars opt-in (inline_loadclock ou clock_no_scaling+raw)
- ef0d200c87 | Não se aplica | Só teste de integração
- 19fb3979dd | Não se aplica | Forma FMA x64; no A64 a ordem vem do fixup (desligado)
- 28f38affee | Talvez | Quirk do Xenon: double denormal em op single dá QNaN; raro, custa por op
- 32920009d9 | Talvez | FPSCR via FPSR do host só em formas Rc=1; depende de de4d24493 (ausente no Xendroid+)
- 36a7bb57f7 | Talvez | Flush exato p/ vmaddfp/vmsum com VSCR.NJ=0, opt-in e caro; raro
- 42087f44bf | Portar | vsldoi alinhado a palavra vira PERMUTE_I32 -> um EXT no A64
- 6cc7c18358 | Talvez | vmaxfp/vminfp NaN por posição; Xendroid+ hoje devolve src1|src2 se ambos NaN
- 6de9c21ecb | Não se aplica | x86 gera QNaN negativo; ARM já gera o QNaN positivo do PPC em sqrt(-x)
- 746e5ac649 | Talvez | Só compartilha a tabela vrsqrte entre x64 e a64 (refatoração); depende de a3a4cd4683
- 9804846f42 | Talvez | Ordem A,B,C e QNaN positivo no FMA; só afeta a64_fpu/vmx_nan_fixup (desligados)
- 9900f7cebb | Talvez | Fixup de NaN FMA sem desvio; só com a64_vmx_nan_fixup ligado
- a3a4cd4683 | Portar | vrsqrtefp/frsqrte como helpers JIT: 1 chamada vs 4 blr cruas p/ C; honra NJM
- b2d6a41401 | Talvez | Fixup de NaN VMX de 2 operandos sem desvio; só com a64_vmx_nan_fixup
- bca15a9214 | Não se aplica | Só skip.txt de testes
- cf43c4c522 | Talvez | QNaN positivo em operação inválida; Xendroid+ usa 0xFFC00000 só no fixup desligado
- e4b13738c3 | Portar | vcmpbfp: NaN marca os dois bits e CR6[0] fica 0; inclui fold float de VectorCompareEQ
- e9582aca75 | Portar | stvlx/stvrx com stores sobrepostos (68->16 instr.); usar NewCachedLabel no port
- ed9bfc9a40 | Talvez | Aquieta o NaN de vmaxfp/vminfp; par de 6cc7c18358
- f1173df88d | Portar | PACK FLOAT16 guarda o sinal em registrador em vez de spill na pilha (aplica limpo)
- fb225d9756 | Talvez | vexptefp/vlogefp inline (sem CallNativeSafe); exige mover consts (A64BackendContext <=256 B)
- 01efb80ed2 | Não se aplica | Ordem de NaN no vmaddfp x64; commit diz que o A64 já acertava
- 378c95215e | Talvez | FPSCR em fcti*; NaN de fctiw com palavra alta estendida; depende da cadeia FPSCR
- 8b19ee756f | Talvez | Pula arredondamento single p/ operando denormal; cadeia FPSCR/quirk
- 8ffe24e372 | Portar | ResolveFunction publica entry->function/status sem lock; corrida real em ARM64 (memória fraca)
- ff6bb1c953 | Não se aplica | Só skip.txt de testes
- 229fec07c2 | Talvez | Profiler de cobertura guest por função/sequência; ferramenta, conflita com trace_data
- 8642d8eff9 | Portar | Constantes v128 sem imediato via literal pool (1 ldr em vez de até 10 instr.)
- bb4da59dac | Já tem | Promoção de contexto por faixa já em context_promotion_pass.cc (cvar context_promote_vec128)
- cdd60494d7 | Talvez | Formato XEX% (dash 1746, protótipo); depende de 131503f68d
- e3d399e5dc | Já tem | Só texto do cvar; Xendroid+ tem descrição própria
- e6a86116f3 | Portar | Constante em v0/v1 clobberada em ROTATE_LEFT, DENORMFLUSH e SHL/SHR_V128 (dot já corrigido)
- fb1c3dec3f | Portar | CR6 por uminv/umaxv (VECTOR_ALL/NONE_SET) em vez de 4 extracts+ORs; aplica limpo
- 0c9dbf8cf9 | Não se aplica | Só testes (nomes de casos vupkd3d128)
- 0f55d6cb50 | Talvez | Chave de amostra 64-bit do profiler; depende de 229fec07c2
- 70ac6a3574 | Portar | RA rematerializa spill do contexto (só 7 GPRs alocáveis no A64); aplica limpo
- a5f59a48ae | Portar | LVR lê memória com endereço alinhado (pode cair na página seguinte); usa o pool
- d39b10a2ab | Já tem | Fold INT16 de vperm já corrigido (d434ef516, Value::Permute)
- 3212e5d38b | Não se aplica | Só teste (SELECT_F64)
- f88d285dc6 | Não se aplica | Só testes de operandos não opacos
- 1383888834 | Já tem | Coalescência de DELAY_EXECUTION já existe (com isb) em a64_seq_memory.cc
- 8a8fe1d32a | Não se aplica | Safepoint PC só no x64; A64 do Xendroid+ já grava (log_safepoint_pc)
- 45827cc2ba | Portar | Value::Extract constante ignora flip ^3/^1 e lê u8[idx&0x1F] fora do vetor
- 850f45610b | Não se aplica | EXTRACT/F2I com constante no x64; A64 usa SrcVReg
- d3094f5e67 | Não se aplica | QNaN positivo nos binops VMX x64; ARM já gera o positivo
- 2cdfb45cbf | Portar | Detecção LDR+REV troca bytes duas vezes em load MMIO 32-bit pelo caminho de sinal (mmio_handler.cc)
- 44e05e470a | Portar | TranslateVirtual chama allocation_granularity()->getpagesize() a cada acesso HLE
- 46e8fd8dcc | Portar | mcrf vira compare com zero: eq copiado vira gt e SO some (ppc_emit_control.cc)
- 59d00d4baf | Talvez | SIGTRAP no POSIX e traps por tipo; inclui fix de regs[v] no write-back de vetores
- ff681ddf48 | Talvez | Unwind do thunk por flag; frame guest de 224 B recebe unwind errado
- 619170cc7a | Portar | Checagem de sync de pilha deve pular COMMENT (com debug info/dumps)
- c87c034153 | Portar | Função que termina em call nunca emite a checagem de sync de pilha
- 038050012a | Portar | Numera blocos antes do RA; bug latente que passes próprios podem expor
- b4a5072505 | Portar | Menos HIR em subfc/subfic/adde*/srawx (srawx 61->51 instr.)
- 63ea6e2725 | Já tem | Duplicata (canary) de d39b10a2ab; já corrigido em Value::Permute
- 01dc16a604 | Já tem | Revertido por 0e1307bd2e; Xendroid+ tem o cvar (padrão off)
- 577fb8ee17 | Portar | Duplicata (canary) de 46e8fd8dcc; portar um só
- 0e1307bd2e | Já tem | Estado final = VEC128 fora da promoção, igual ao padrão do Xendroid+
- 10ac2f5eff | Portar | A64 não registra faltas MMIO (sempre SIGSEGV); MMIOAware* soma 0x1000 em host 4K
- 7dab7132b5 | Portar | memcpy com sobreposição ao aplicar patch XEX (crash NG2+TU em aarch64); usar memmove
- 4b95b068f6 | Não se aplica | MAP_JIT do macOS
- 5ce7c336df | Portar | Duplicata (canary) de 45827cc2ba; portar um só
- 920f35c938 | Portar | Fold de VectorCompareEQ float por bits (0/-0, NaN); mesmo hunk de e4b13738c3
- a33ee3117a | Não se aplica | Constantes em shift/rotate/pack/minmax no x64; A64 usa SrcVReg
- ca9e079051 | Não se aplica | Duplicata (canary) de 850f45610b; só x64
- e011d1eeab | Portar | Compila inicializadores estáticos antes de soltar a thread principal (Hydrophobia travava)
- 4e80f1ea83 | Não se aplica | Só teste (zera memória)
- 504cbee7eb | Portar | vupkhsh128/vupklsh128 decodificados como vupkhsb128 via gambiarra; tabela correta
- 510a9553be | Talvez | Pré-compilação antecipada ligada por padrão (NG2 intro); medir tempo/memória no celular
- 62140dea88 | Talvez | Pré-compila fora da thread de UI (fluxo de launch desktop; adaptar ao JNI)
- 8682ca78a3 | Não se aplica | Só testes VMX128
- a77db1ed3b | Portar | EntryTable em split_map: inserção O(n) sob lock global e FindWithAddress linear
- e25eaffffa | Não se aplica | Só parser de testes
- fc22bceae6 | Talvez | Pré-compilação em threads de trabalho
- 52188fafea | Talvez | Lê instruções pelo módulo dono; pré-requisito de modo usuário/código dinâmico
- 2fe7b7d635 | Portar | Patch XEX que muda a base da imagem (atualização do XNA Indie Player)
- 32a95fff0d | Talvez | Espaço de endereço de modo usuário (.NET CF/XNA, XeFu)
- 63c95eca4e | Talvez | Kernel trata sc do modo usuário
- 69821e7280 | Não se aplica | Stackpoints por pilha host no x64; A64 em 1e640ba491
- b539853698 | Não se aplica | Metade x64 do código dinâmico; metade A64 é 1e640ba491
- efabe1d6ac | Talvez | KeGetImagePageTableEntry com proteção da seção (runtime .NET CF/XNA)
- 1e640ba491 | Talvez | Metade A64 do modo usuário (cache de chamadas dinâmicas, stackpoints por pilha)
- 7914407563 | Não se aplica | Registro de stackpoint no x64
- 9afccb2c82 | Não se aplica | cvars log_lr_* só para depuração/log
- a0d11bec9d | Talvez | Code cache de 1 GB p/ pré-compilação; Xendroid+ já falha alto com 256 MB
- f2583e9def | Talvez | Extensão de função lia 1 instrução além do limite; depende de 52188fafea
- f453ede2e0 | Talvez | Pré-compila funções só referenciadas por endereço (Def Jam Icon)
- 004ece768c | Talvez | Endereço guest via [membase,Wn,UXTW] e remap correto em páginas 16K; reescreve a64_seq_memory
- 078a07b530 | Talvez | Otimiza o quirk de denormal single, que o Xendroid+ não tem (ver 28f38affee)
- 089473ccc4 | Talvez | Elimina fixup escalar onde ARM já concorda; só com a64_fpu_nan_fixup ligado
- 0ffb0c2b2d | Já tem | Desvio condicional direto já coberto por a64_near_branches (reemite se não alcança)
- 10c8ae7952 | Não se aplica | MXCSR padrão ao entrar no host (x64); equivalente A64 é 35d5c81d71
- 10da45ea40 | Portar | Formas OE (addco/subfco/subfeo) não atualizam CA no Xendroid+; implementa OV/SO
- 1bb7afe782 | Talvez | Mantém o limite do remap 0xE0000000 entre blocos; depende de 004ece768c
- 35d5c81d71 | Portar | GuestToHostThunk entra no host com FPCR do guest (arredondamento/FZ); aplica limpo
- 39aa0bfc7d | Talvez | Ajuste da prova de 92bf74906f
- 497bec2bbe | Talvez | Ideia: com FZ do host, FMA vetorial usa fontes direto; Xendroid+ faz spill de s3
- 5f36967c5a | Já tem | Xendroid+ já tira o fixup escalar de NaN do caminho quente (a0619c7de)
- 6544934138 | Talvez | PACK com SNaN empacota lixo (fmaxnm não aquieta SNaN); 1 instrução por PACK
- 689590ba3c | Portar | Passe de liveness elimina stores de CR mortos (-41% stores de CR em Halo 3)
- 6bc0aa54f5 | Talvez | Reusa x0 do endereço de retorno no STORE_CONTEXT do LR; requer infra de handoff
- 6e5846d639 | Não se aplica | Constantes em compares/MAX/MIN no x64; a parte A64 é fcd8e58a30
- 6fc16d118d | Portar | Endereços fixos de helpers via ldr do backend context (-2,2% instr.)
- 75051d36df | Não se aplica | Só testes de chamada ao host
- 7a5d9713a3 | Talvez | Funde máscara de alinhamento de lvx/stvx no endereço; requer 004ece768c
- 7a97888962 | Portar | Checagem de vetores vivos em CALL_EXTERN; obrigatória junto com b416d19675
- 7d8c5c7d35 | Não se aplica | Só teste (matriz de NaN escalar)
- 882df3330f | Portar | Pula mov wN,wN de TRUNCATE lido só por compares (-1,2% instr.); aplica limpo
- 8f21542469 | Talvez | Retorno em modo Fpu exigido pelo rastreador de fc0ed6e81d
- 92bf74906f | Talvez | Prova de FPR não-denormal entre blocos; só útil com o quirk 28f38affee
- 95b14f55fa | Portar | HostToGuestThunk devolve ao host o FPCR do guest; salvar/restaurar FPCR do host
- 968f1ef459 | Portar | vperm via REV32 em vez de EOR 3; vmrg de bytes/meias via ZIP
- 9a8aaaf883 | Não se aplica | Xendroid+ aloca o contexto com AlignedAlloc, sem reserva em endereço fixo
- aba5703fa0 | Portar | Compare+branch sem cset/cbnz (b.cond direto); adaptar infra de handoff
- b416d19675 | Portar | Endereços de thunk/tabela via ldr do contexto e CallExtern sem salvar q4-q31
- b5cc59e854 | Talvez | Tradução de endereços de modo usuário pela tabela de páginas do guest
- bef83c66d5 | Talvez | Flush de denormal em software segue VSCR.NJ; só hosts sem kA64FZFlushesInputs
- c496db01fe | Já tem | lfs/stfs com fixup fora do caminho quente: SINGLE_BITS_TO_DOUBLE/DOUBLE_TO_SINGLE_BITS
- c5b3c02e9b | Talvez | Tabela FPCR única e static_assert DN/AH; refatoração ligada ao rastreador
- cebb1d1abc | Talvez | Invalida código compilado em KeSweepIcacheRange (código gerado em runtime)
- d88e3eaec4 | Já tem | Fixup escalar só olhando o resultado já existe (a64_fpu_nan_fixup_result_check)
- df72008d9a | Portar | bl direto sem módulo dono passa Function nula para Call e o A64 derefere
- df7d6d95d0 | Talvez | Assume Fpu em função sem VEC128; depende de fc0ed6e81d
- e0e1e58775 | Talvez | Ops escalares exatas sem trocar modo; depende de fc0ed6e81d e opcodes lfs/stfs upstream
- eddcd8f31e | Portar | SameValueOrEqualConstant trata dois floats constantes como iguais (AsUint64=0)
- ef4b3a4669 | Portar | vsel com destino aliasado usa BIT/BIF (3->1 instr.); aplica limpo
- f6f9a03768 | Portar | Correção do 689590ba3c: chamada guest lê todo o CR
- fa57fbdb5f | Portar | CallNativeSafe não esquece modo VMX; op VMX seguinte roda sem FZ
- fc0ed6e81d | Talvez | Rastreio do modo FPCR entre blocos; corta msr no thunk (79%->37%) mas é invasivo
- fcd8e58a30 | Portar | MAX/MIN_F32/F64 com 2 constantes lê src2 de s1/d1 lixo; Xendroid+ tem o bug
- 0492b38b2c | Talvez | Limita funções de código dinâmico pela faixa varrida
- 2ea595d6c4 | Talvez | Watches de escrita da GPU nas views de modo usuário
- 492e1cc67b | Talvez | Lê código dinâmico pela view física
- 7873b1ed94 | Não se aplica | Entrega de page fault de modo usuário só no win32
- afbf8cf118 | Talvez | Tabela de indireção comitada sob demanda (código sem módulo)
- 0788c561e3 | Talvez | Limite por faixa varrida terminando em qualquer desvio incondicional (XeFu)
- 7d7557f2f7 | Portar | mtfsb0/mtfsb1 não implementados no Xendroid+ (viram NOP com log)
- c5777398f1 | Talvez | PPCContext::set_cr, usado na entrada do modo usuário
- 123209cb03 | Talvez | Falta de busca de instrução no alvo de chamada de modo usuário (XeFu)
- 2317b55473 | Talvez | Mantém faixa varrida quando um patch toca uma instrução (XeFu)
- 4d83110dcd | Portar | OV julgado nos 32 bits baixos; ajuste de 10da45ea40
- 6e304ab866 | Portar | longjmp na 1ª instrução pula o prólogo; teste machine_code() do Xendroid+ nunca rejeita
- 723f542129 | Talvez | Descarta código dinâmico compilado sob outro alias
- edf01c1e6b | Talvez | Impede chamadas a código dinâmico descartado (sobretudo x64)
- 19706836b8 | Talvez | LDIDecompress com stream LZX persistente (XeFu e títulos que usam LDI*)
- 853e933277 | Não se aplica | Título do XeFu na barra de título (UI desktop)
- a5532a0d19 | Portar | mcrfs zera bits de controle (RN/NI) do campo copiado
- e1e4851f7a | Portar | mtfsfi grava o dobro do imediato: RN errado (truncar vira +inf)
- fad70b50f0 | Talvez | Recompila código dinâmico varrido durante a compilação
- 7ba916c010 | Não se aplica | Só amplia log de módulo de usuário
- 9f26a1b0e0 | Já tem | Overflow do trace de registradores já limitado (kMaxTraceDests, XenDroid 40f193b23)
- d3f7ca3e04 | Talvez | Carrega dados de título do XeFu (retrocompatibilidade do Xbox original)
- 89f90684f5 | Talvez | SHA-1 via Mbed TLS; exige adicionar mbedtls ao third_party
- abb96c9504 | Talvez | AES da XEX via Mbed TLS; corrige leitura além do buffer em bloco parcial
