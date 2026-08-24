# Write-Ahead Log

Durability for NeuralKV: every SET/DELETE is appended to a log and
fsync'd before it's applied to memory, so an acknowledged write survives
a crash. No snapshots yet; concurrent writers' fsyncs are batched (group
commit — see below). `term` in the record format is what lets this same
on-disk log double as Raft's replicated log (see raft-design.md's WAL
integration section) without a separate format for single-node vs.
clustered mode.

## Record format

`data_dir/wal/wal.log` is an append-only sequence of records:

| Field | Size | Notes |
|---|---|---|
| crc32 | 4 B | big-endian; covers every field below |
| term | 8 B | big-endian; always 0 until Raft |
| index | 8 B | big-endian; monotonic, assigned by `WalWriter` |
| op | 1 B | `1` = SET, `2` = DELETE |
| key_len | 4 B | big-endian |
| key | `key_len` B | never empty |
| value_len | 4 B | big-endian |
| value | `value_len` B | empty for DELETE; never empty for SET |

All multi-byte integers are big-endian. The CRC covers term through value
— everything after the CRC field itself.

## Write path

`DurableStorage::Set`/`Delete`:

1. Append the record to the WAL (buffered, not yet durable).
2. `Sync` the WAL up to that record's index — see Group commit below.
3. Apply the mutation to the in-memory `ShardedKV`, gated by a small
   apply-order barrier so applies land in the same order as WAL indices
   even when step 2 completes out of order across concurrent callers.

If step 2 fails, step 3 never runs and the error goes back to the client
— an unacknowledged write is allowed to be missing after a crash, but an
acknowledged one is not. GET bypasses the log entirely and reads straight
from memory, since recovery has already replayed everything the log
fsync'd by the time a server accepts connections.

## Group commit

`WalWriter::Sync` batches concurrent callers into as few real `fsync`
calls as possible instead of one per caller: it waits for more pending
appends up to a record cap (16) or a latency cap (1ms), whichever comes
first, then fsyncs everything accumulated in one call. No caller waits
longer than the latency cap for its own durability. The wait polls in
short ticks and flushes early once nothing new arrives, so an
uncontended write — Raft's own per-node log, already serialized by
`RaftNode`'s mutex — only pays a small bounded delay rather than the
full cap. See the write path under fsync in
docs/benchmark-methodology.md for what this costs and saves under
concurrency.

## Recovery

On startup, `RecoverFromWal` reads `wal.log` sequentially and applies each
record to the KV in order:

- A missing or empty file is not an error — a fresh node has nothing to
  recover, and reports `last_applied_index = 0`.
- A **truncated tail record** — fewer bytes present than the record
  declares — is not an error. This is the exact shape of a crash mid
  `write()`: the previous process died after a partial append, and
  nothing has appended past that point since. Recovery stops there and
  starts the node with everything before it applied.
- A **CRC mismatch, unknown op, empty key, or SET with an empty value** on
  a *complete* record is a fatal error: recovery refuses to start the
  node rather than guess at corrupted data. The distinction from a
  truncated tail is exactly whether enough bytes were present to read the
  record at all — a short read only happens at the true end of the file
  for a regular file, so it can't be confused with a fully-present record
  with a wrong CRC.
- Record field lengths (`key_len`, `value_len`) are capped at 64 MiB.
  Without this, a garbage length decoded from a corrupted record could
  turn a real "not enough bytes remain" corruption case into a
  false-negative "truncated tail" if it happened to land near the true
  end of the file. The cap forces that case to fail loudly instead.

`WalWriter` does its own lightweight scan of the log on open (same
truncated-tail/CRC rules, but only to learn the highest index already
written) so new appends continue the index sequence. This is a second
pass over the log distinct from `RecoverFromWal`'s replay into the KV;
for the log sizes expected at this stage, reading the file twice at
startup isn't worth avoiding.

### Known limitation

Recovery assumes a truncated tail only ever occurs once, at the very end
of the file, immediately before the next successful append. If a node
crashes mid-write, restarts, and appends more records without anything
else touching the file, that assumption holds: `WalWriter` opens in
`O_APPEND` mode and writes always land after the existing bytes,
truncated tail included. There is no compaction or repair tool to fix a
WAL that ends up with garbage anywhere other than at the true end of the
file — that class of corruption is deliberately out of scope and would
surface as a fatal CRC error on the next restart rather than being
silently repaired.

## SET requires a non-empty value

`DurableStorage::Set` rejects an empty value before touching the WAL,
matching the same rule the protocol codec already enforces on the wire
(`EncodeClientRequest`/`DecodeClientRequest` both reject a SET with an
empty value). This isn't a new restriction — it keeps every SET record
recovery will ever see already inside the set of records recovery
considers valid, rather than writing something durability would later
refuse to replay.
