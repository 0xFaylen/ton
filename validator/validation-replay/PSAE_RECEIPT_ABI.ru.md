# PSAE worker receipt: контракт coordinator ↔ worker

Статус: реализованы commitment header, детерминированная проверка account chain,
hash-committed cell-usage journal и первая каноническая версия immutable
Transaction payload. Coordinator заново разбирает TL-B и вычисляет commitments,
а batch precommit не публикует частичный account frontier при ошибке. Global
effects, worker runtime и подключение к live collator ещё не реализованы.
Поэтому этот код пока не ускоряет блок и не меняет state.

## Зачем нужен receipt

TON-транзакция обрабатывает одно сообщение и изменяет один destination account.
PSAE использует это как заранее известную границу сериализации: разные accounts
можно вычислять независимо, а транзакции одного account образуют строгую цепочку.
Worker не получает validator keys и не применяет результат к глобальному state.
Он возвращает materialized immutable cell payload и commitment header. Payload
не может хранить `Transaction&` или ссылку на lane-local `Account`: после каждой
транзакции остаются только canonical cells и journals. Только coordinator может
проверить и последовательно зафиксировать готовый глобальный prefix.

## Реализованный header

`WorkerReceipt` содержит:

- canonical input key `(message_lt, message_hash)`;
- destination account и account-local sequence;
- hash состояния account до и после исполнения;
- transaction hash;
- commitments для effects и proof journal;
- начальный и конечный transaction LT;
- использованный gas.

`AccountCheckpoint` задаёт ожидаемые `state_hash`, `next_sequence` и
`last_transaction_end_lt` перед первой работой lane.

## Что coordinator уже проверяет

`validate_receipt_set()` не меняет block state и выполняет проверки в
каноническом порядке `OutputQueueMerger`:

1. число slots совпадает с числом входов;
2. ключи строго возрастают как `(lt, message_hash)`, без дубликатов;
3. transit/coordinator-only item не имеет worker receipt;
4. receipt ссылается ровно на свой input и destination account;
5. account sequence и pre-state совпадают с текущим checkpoint;
6. LT монотонен относительно предыдущей транзакции account;
7. после отсутствующего receipt нельзя принять более поздний receipt того же
   account, хотя независимый account может продолжить вычисление.

Пункт 7 решает только account-local causal chain. Глобальный commit всё равно
не перескакивает через дырку: `commit_ready_prefix()` фиксирует лишь непрерывный
prefix канонической очереди. `ProcessedUpto` обновляется только после такого
commit, а не после завершения worker.

## Реализованный proof journal

Разбор исходного пути показал точную границу мутаций:

- `Transaction` держит `const Account&`, а account меняется только в
  `Transaction::commit()`;
- но загрузки state cells во время TVM раньше commit уже отмечают общий
  `CellUsageTree` и меняют оценку collated proof bytes через callback;
- после исполнения coordinator ещё меняет `BlockLimitStatus`, account-dict
  estimator, storage stats, new-message heap, max LT и value flow.

`CellUsageJournal` изолирует первый из этих глобальных эффектов. Каждый local
worker использует собственный `CellUsageTree` и записывает для загруженной cell:

- hash immutable anchor root;
- путь из reference indexes от anchor до cell;
- ожидаемые cell hash и effective level.

Journal имеет domain-separated SHA-256 commitment. При commit coordinator не
доверяет присланной cell: он заново проходит путь от своей immutable pure root,
сверяет anchor/hash/level и только затем воспроизводит load в своём serial
`CellUsageTree`. Invalid path, wrong anchor, wrong cell и duplicate path
отклоняются до изменения coordinator tree. Тесты подтверждают, что полученный
Merkle proof равен serial proof, union двух worker journals не зависит от
arrival order, а malformed journal не оставляет частично применённый proof.

Это ещё не полная изоляция: отдельные journals для account storage proof должны
быть привязаны к своим roots. `CanonicalTransactionPayload` уже принимает
строго отсортированный набор anchored journals и вычисляет общий commitment, но
offline transaction replay пока передаёт пустой набор, а live collator продолжает
использовать старый serial callback.

## Реализованный canonical Transaction payload

`CanonicalTransactionPayload` переносит:

- canonical Transaction root;
- post-transaction Account cell;
- канонически упорядоченный набор anchored proof/usage journals.

`inspect_transaction_payload()` не доверяет header и заново:

1. разбирает `Transaction` и `HASH_UPDATE Account`;
2. проверяет TL-B post-account и совпадение его hash с `new_hash`;
3. выводит account, pre/post state hashes и transaction hash;
4. выводит `start_lt`, `end_lt = start_lt + outmsg_cnt + 1` и gas из
   `TransactionDescr/TrComputePhase`;
5. требует точные последовательные индексы `0..outmsg_cnt-1`, валидный
   `Message Any` и сохраняет порядок/логический time всех out-messages;
6. пересчитывает domain-separated effects и proof-journal commitments.

`validate_precommit_set()` сначала проверяет каждый payload против receipt,
затем всю account chain. На любой ошибке он возвращает исходные checkpoints, а
не частично продвинутый frontier. Unit tests подменяют каждое выводимое поле,
ломают cells и journal order, а также проверяют атомарность batch reject.

На copied mainnet corpus block `87341675` offline replay повторно проверил 51/51
canonical payloads, включая 42 упорядоченных out-messages, при прежнем точном
совпадении transaction hash и account-state hash. Proof journals в этом прогоне
пусты, global effects не применяются.

## Payload/effects, которые ещё обязательны до P2

Текущий payload подтверждает account-local consensus-visible output, но пока не
переносит и не применяет:

- изменения InMsg/OutMsg descriptors и new-message heap;
- точные вклады в `BlockLimitStatus`, value flow, max LT и storage estimators;
- account/storage dictionary deltas и полный state merge;
- фактические state/account-storage journals из worker execution;
- wire encoding и bounded transport для отдельного worker process.

Coordinator обязан материализовать payload, заново вычислить все hashes,
проверить текущий `BlockLimitStatus`, а затем применить его в serial canonical
order. Worker-provided `gas_used`, hashes или limit deltas не являются
доверенными значениями.

## Failure model

- missing/delayed worker: готовые независимые accounts можно сохранить в памяти,
  но commit останавливается на первой глобальной дырке;
- malformed/tampered result: discard этого receipt, retry либо serial fallback
  с исходного predecessor state;
- worker crash: не влияет на safety; liveness текущей попытки сохраняется только
  при bounded timeout и рабочем serial fallback;
- duplicate/reordered result: определяется по input key, account sequence и
  checkpoint, поэтому arrival order не меняет commit order.

## Совместимость

Начальный PSAE — локальная оптимизация collator/validator implementation. Он не
меняет TVM semantics, TL-B block format, gas schedule, message order или
consensus validation rules. Поэтому при точном совпадении roots ему не нужны TEP,
network config vote или обновление контрактов. Remote scale-out допустим только
после доказательства той же receipt ABI на local workers.

## Обязательные gates до включения

1. Зафиксировать wire encoding текущего canonical payload и bounded transport;
   in-process TL-B recomputation уже реализован.
2. Подключение уже проверенного isolated `CellUsageJournal` к state и account
   storage contexts каждой lane; full-block deterministic union.
3. Полное равенство Transaction, account, shard-state, Merkle-update и block
   roots против serial oracle на mainnet-derived copied state.
4. Равенство `ProcessedUpto`, descriptors, gas/value flow и поведения на всех
   block-limit boundaries.
5. Fault injection: crash, delay, duplicate, reorder, corrupted cell/hash,
   timeout и serial fallback.
6. Full-wall benchmark на скопированной validator DB; shadow overhead ≤ 2% и
   положительный выигрыш после merge/commit costs.

До прохождения этих gates любые коэффициенты остаются потолком модели, не TPS и
не измеренным ускорением collator.
