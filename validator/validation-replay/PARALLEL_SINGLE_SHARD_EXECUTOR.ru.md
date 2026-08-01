# Prefix-Safe Account Executor для одного shardchain

Дата среза: 2026-07-31.

Статус: архитектура и исполняемая модель планировщика; live collator пока не
изменён. Это не результат TPS и не готовая consensus-фича.

## Решение

Большой проект — не JIT и не перенос Block-STM. Для TON нужен
`Prefix-Safe Account Executor` (PSAE): выполнение входящих сообщений разными
account-lanes параллельно, при этом все общие структуры блока изменяет один
детерминированный commit в точном порядке `(message_lt, message_hash)`.

Это использует свойство TON, которого нет у обычной синхронной EVM-модели:
транзакция обрабатывает одно сообщение и изменяет состояние одного destination
account. Она не может во время TVM синхронно прочитать или изменить другой
контракт. Исходящие сообщения лишь ставят будущую асинхронную работу в очередь.

Текущий collator уже отделяет фазы подходящим образом:

1. `OutputQueueMerger` объединяет сообщения всех соседей и сортирует их по
   `(lt, hash)`.
2. Collator обрабатывает весь доступный prefix входящих internal messages.
3. Только после этого он запускает созданные в этих транзакциях сообщения.
4. В конце `ProcessedUpto` получает последний обработанный `(lt, hash)`.

Следовательно, во входящей фазе заранее известен канонический порядок, а
динамически созданные зависимости не возвращаются в исполняемый batch. Один
account остаётся последовательным, разные accounts можно исполнять параллельно.

## Что меняется и что не меняется

Меняется только внутренняя реализация collator/validator:

- serial `create_ordinary_transaction` разделяется на account-local execute и
  global commit;
- account-lane владеет копией account state и последовательно исполняет все
  сообщения этого account;
- worker возвращает immutable receipt: transaction root, post-account state,
  outgoing messages, gas/LT, storage updates, loaded-cell journal и timing;
- coordinator применяет receipts только непрерывным каноническим prefix;
- proof/state merge и словари блока сначала остаются на coordinator, затем
  распараллеливаются как отдельная фаза.

Не меняются:

- TVM semantics, gas, exit codes и контрактный ABI;
- block TLB, `ProcessedUpto`, message routing и logical time;
- правила ValidateQuery и consensus;
- формат state и совместимость кошельков/DEX;
- block/collated-data limits;
- #2485 dedicated-collator wire protocol.

Первый production-вариант является node-local оптимизацией. Если serial и
parallel режимы строят одинаковые block/state roots, голосование и TEP не нужны:
валидатор проверяет валидность кандидата, а не выбранное число worker threads.
Удалённые workers позже используют тот же receipt ABI внутри доверенной границы
одного валидатора и также не требуют изменения блокчейн-протокола.

## Закрытие ProcessedUpto

`ProcessedUpto` нельзя двигать по завершившимся аккаунтам. Он описывает prefix
глобального порядка `(lt, hash)`. Поэтому правило commit жёсткое:

```text
input: A, B, C, D
ready: A, -, C, D
commit: A
ProcessedUpto: A
discard/retry: B, C, D
```

C и D разрешено вычислить спекулятивно, но запрещено включать в block поверх
дыры B. Иначе следующий блок сочтёт C/D ещё не обработанными и повторит их.

Это не требует нового bitmap или TLB. Исполняемая модель находится в
`validator/impl/parallel-inbound-scheduler.*`. Тесты фиксируют:

- строгий входной порядок `(lt, hash)`;
- один account всегда остаётся в одной последовательной lane;
- завершённый suffix не продвигает frontier через pending/failed item;
- порядок прихода worker results не меняет committed prefix;
- проверка block limit выполняется до следующего item, как в serial collator;
- worker failure оставляет полезный prefix, если хотя бы первые items готовы.

Для liveness item на текущем frontier получает высший приоритет. При worker
crash он повторяется на coordinator; после retry budget collator переходит на
существующий serial path. Если сама транзакция не укладывается в deadline, это
не новая проблема PSAE: serial collator также не способен закончить её вовремя.

## Глобальные лимиты

Workers не принимают решение о включении. Они только вычисляют receipts.
Coordinator перед каждым commit вызывает настоящий
`BlockLimitStatus::fits(cl_normal)`, затем применяет receipt через те же операции,
которые сейчас вызывает serial collator. Это сохраняет важную семантику: item,
начатый пока block ещё fits, может перевести его через normal limit; следующий
item уже не начинается.

Gas складывается просто, но bytes/proofs не являются суммой независимых
оценок из-за дедупликации cells. Поэтому workers не должны самостоятельно
резервировать «байты транзакции». Exact byte/collated-data accounting остаётся
на serial commit до появления проверенного union-алгоритма для cell journals.

## Главная техническая работа

Планировщик — не самый трудный кусок. Реальный blocker находится в proof/state
контексте текущего collator:

- `CellUsageTree` изменяемый и не thread-safe;
- `current_tx_storage_dict_` — один общий указатель callback-контекста;
- `collated_data_stat`, `BlockLimitStatus`, account dictionary estimator,
  `value_flow_`, `new_msgs` и In/Out descriptors сейчас общие и изменяемые.

Поэтому TVM нельзя просто завернуть в thread pool. Нужны:

1. per-lane `CellUsageTree` и per-lane loaded-cell/proof journal;
2. deterministic union загруженных путей в master usage tree;
3. account checkpoint после каждого receipt, чтобы безопасно отбросить suffix;
4. serial application всех global deltas;
5. exact serial-vs-parallel root comparison в offline replay.

Эта граница и отличает проект от «добавить async». Без неё возможны гонки,
ошибочная оценка collated data и несовпадающий Merkle update.

## Этапы реализации

### P0. Исполняемые инварианты — выполнено

- чистая deterministic lane plan;
- prefix-only commit;
- hole/failure/limit tests;
- никаких изменений live path.

Gate: 18 model tests проходят, включая промежуточные completion permutations,
worker/commit failure, limit boundary, account affinity и full-wall projection.

### P1. Phase-aware shadow full-wall planner — инструментация готова, corpus run ожидается

В offline replay serial collator продолжает строить настоящий block. В режиме
`--exact-tvm-hotpaths` успешная ordinary transaction записывает полный
account-local creation wall (`TVM + storage/action/serialization`), но не меняет
исполнение. Outer timer `ValidationReplayer` даёт полный collation wall. JSON
`vrp hotpaths ... --source collate --metric wall` для 1/2/4/8/16 workers считает:

- `account_serial_seconds` и число destination accounts;
- теоретический account critical path через greedy LPT;
- неизменяемый serial residue `full_collation_wall - account_serial`;
- `projected_full_ideal_speedup = full_wall / (serial_residue + account_critical_path)`;
- флаг несогласованного измерения, если вложенные account timers неожиданно
  превышают outer wall.

Метрика теперь разделена на `inbound_internal`, `external`, `new_or_deferred`,
`special` и `all_ordinary`. Для `all_ordinary` время одного account суммируется
между фазами до планирования lane. Поэтому `inbound_internal` — честный потолок
ограниченного P2, а `all_ordinary` — потолок полной архитектуры после P4.

Default online mode не хранит per-account timing map. Профилирование запускается
только на отдельной копии validator DB, не на нашей production/mainnet ноде.
Ещё не измерены canonical frontier stalls, speculative waste и доля transit;
они остаются следующей частью P1, а не выдаются за готовый результат.

Gate: shadow не меняет ни один root; overhead не более 2% на Linux replay.
Текущий код компилируется и покрыт unit tests, но этот gate ещё не пройден без
полного скопированного validator DB corpus.

### P2. Parallel execute, serial commit

Только inbound internal phase, только basechain, без split/merge boundary.
Workers исполняют account lanes на изолированных state/proof contexts.
Coordinator применяет готовый prefix. На любом расхождении или timeout — serial
fallback с исходного predecessor state.

Исполняемый заголовок worker receipt уже фиксирует canonical input key,
destination account, account-local sequence, pre/post state commitments,
transaction/effects/proof-journal commitments, LT interval и gas. Coordinator
детерминированно отклоняет неканонический input, разрыв account chain,
неверный predecessor и receipt для coordinator-only work. Это пока только
commitment header: payload с ячейками и global deltas ещё не реализован, поэтому
receipt не имеет права менять block state. Полный контракт и stop gates описаны
в [`PSAE_RECEIPT_ABI.ru.md`](PSAE_RECEIPT_ABI.ru.md).

Первый компонент payload уже реализован отдельно: anchored `CellUsageJournal`
записывает worker-local cell paths, а coordinator повторно разрешает их от своей
pure state root и сверяет cell hash/level. Synthetic-tree tests доказали равный
serial Merkle proof, arrival-order-independent union и reject для подменённых
anchor/path/cell. Этот primitive ещё не подключён к live execution и не закрывает
account-storage journal, Transaction/effects payload или global limit merge.

Gate на каждом block:

- одинаковый serialized Transaction для каждого committed item;
- одинаковые account-state, shard-state, Merkle-update и block roots;
- одинаковые InMsg/OutMsg descriptors, `ProcessedUpto`, gas, value flow;
- одинаковое поведение у границы всех четырёх limits;
- fault injection: crash, delay, duplicate и reordered result.

### P3. Existing parallel ValidateQuery

ValidateQuery уже умеет запускать `CheckAccountTxs` отдельно для разных
AccountBlocks. Нужно не изобретать второй validator, а измерить и усилить этот
путь: bounded worker pool, proof-context accounting, wall critical path и
fallback. Serial validation остаётся oracle.

Gate: parallel и serial validation принимают/отклоняют один и тот же corpus и
получают одинаковые roots; p99 укладывается в consensus deadline.

### P4. New-message waves

Новые сообщения нельзя брать произвольным большим batch: их execution создаёт
новые heap entries. Безопасная wave содержит текущий канонический слой, для
которого доказано, что ни один output scheduled item не может появиться раньше
следующего input. Первая реализация может ограничиться одинаковым minimum LT;
расширение окна разрешается только доказанным lower bound output LT.

Gate: exact serial order и roots на message-chain/DEX workloads; отсутствие
starvation и empty-block loop.

### P5. Local scale-up, затем remote scale-out

Сначала 2/4/8/16 local workers. Только после доказательства receipt ABI и
стоимости merge — remote workers за dedicated collator из #2485. Coordinator
держит keys, global queues и final block; worker не получает validator keys и
не влияет на consensus напрямую.

Gate: потеря worker влияет только на latency/liveness текущей попытки; serial
fallback строит корректный block. Multi-host тест отдельно доказывает network
overhead и recovery, а не выводится из same-host benchmark.

## Почему не другие механизмы

- Block-STM v1/v2 нужен там, где транзакция читает/пишет много общих объектов и
  конфликты выясняются после исполнения. В TON destination account известен до
  TVM, синхронных cross-contract reads нет; optimistic rollback добавил бы
  работу без пользы.
- Pilotfish/Remora полезны как образец coordinator/workers, receipt/versioning и
  recovery. Их object fetching и distributed cross-object transaction protocol
  TON во входящей фазе не нужен.
- RapidLane меняет programming model конфликтных объектов. Это TEP/semantic
  feature, а не совместимый первый этап.
- Solana scheduler опирается на объявленные account locks. У TON write-owner
  уже однозначен: destination account; достаточно account affinity.
- Trace JIT ускоряет только часть TVM. Account parallelism ускоряет также
  Ed25519 и другие тяжёлые операции разных транзакций.

## Что реально ожидать по TPS

### Измеренный lane-ceiling, 2026-07-31

На сохранённом полном basechain-блоке `87341675` выполнено 10 offline replay-прогонов:

- `51` транзакция, `29` destination accounts, `scope=full_block`;
- каждый прогон повторно получил точные исторические transaction hash и account-state hash;
- медианный идеальный потолок по измеренному transaction wall: `1.998x` для 2 workers и
  `3.253x` для 4 workers;
- 8 и 16 workers на этой выборке также дали потолок `3.253x`: самый тяжёлый account
  занимал медианно `30.75%` измеренной работы и стал последовательным critical path.

Это **не измерение параллельного collator и не TPS-результат**. Replay собран в Debug из-за отдельной
проблемы Release-окружения с отсутствующим `absl/hash/hash.h`; абсолютное время не используется.
Показатель отвечает только на вопрос, достаточно ли в реальном блоке независимых account chains,
чтобы продолжать P1/P2. Из потолка исключены worker contention, serial commit, block limits,
`CellUsageTree`/proof merge, state merge, сеть и consensus. Следующий gate обязан измерить эти расходы
на полном collator wall path. На этой небольшой выборке масштабирование выше четырёх workers уже
запрещено обещать без отдельного более широкого corpus.

Метрика воспроизводится полем `account_lane_ceiling` у `tvm-replay-bundle`; исходные state/proof
артефакты читаются локально, живая mainnet-нода не используется.

Сейчас доказанного коэффициента TPS нет. Измерен только факт, важный для выбора
архитектуры: на тяжёлом account subset Ed25519 занял медианно 42,09% TVM wall,
а у Wallet V5/V4 — 71,75%/82,09%. JIT этот участок не убирает; account workers
могут выполнять независимые подписи одновременно.

Целевые, но пока не достигнутые acceptance thresholds:

- не менее `1.8x` collation wall speedup на 4 workers;
- не менее `2.5x` на 8 workers для compute-bound corpus с достаточным числом
  destination accounts;
- не менее `2x` included useful operations/s при прежних config limits и
  sub-second p99;
- exact roots и ноль consensus-visible semantic differences.

Максимальный speedup ограничен законом Амдала:

```text
S(N) <= 1 / ((1 - P) + P/N + merge_overhead)
```

где `P` — измеренная доля полного collation wall path, реально перенесённая в
account workers. DEX hotspot с одним destination pool остаётся последовательным.
Wallet/jetton workload с множеством destination accounts должен масштабироваться
лучше, но это надо измерить на полном block replay.

Если workload уже упирается в block bytes, executor сам по себе не увеличит
полезные операции на block. Он создаёт временной запас для более плотного блока,
который дают отдельные byte/cell optimizations. Поэтому network/DB/compression и
PSAE не конкурируют: они снимают разные ограничения.

## Stop conditions

Проект останавливается или сужается, если выполнено хотя бы одно:

- `P < 0.5` полного collation wall path на репрезентативном corpus;
- 4 workers дают менее `1.5x` после вычета profiling overhead;
- proof-journal merge требует повторного чтения большей части state;
- exact root equivalence не достигается без изменения TLB/TVM semantics;
- single-account hotspots доминируют workload настолько, что lane parallelism
  не влияет на p95/p99;
- ValidateQuery или сеть остаются медленнее нового collator и не укладываются в
  deadline при том же размере block.

До P2 запрещено называть PSAE mainnet-ready. До P5 запрещено называть локальный
thread-pool горизонтальным scale-out.
