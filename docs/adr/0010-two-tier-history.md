# ADR-0010: Two-tier history — an mmap'd FlatBuffers blob under an append log

**Status:** Accepted
**Date:** 2026-08-27

This is the spec index for the history tickets. It is the owner's design prompt
(fish 3.7.1's `history.cpp` protocol, two known holes fixed) as amended in the
2026-08-27 grilling; where this document and the original prompt disagree, this
document wins. Task agents read this and their ticket and nothing else.

## Context

`runtime::history_store` (#113) is an escaped-line file at `~/.lesh_history`
with two verbs, no cache, no timestamps, no dedup, no lock: every autosuggest
miss re-opens and re-reads the whole file on a worker thread. It lives in
`src/runtime/` for no reason the runtime has — nothing non-interactive calls it
(#101); its callers are `main.cpp` and `ui/session.cpp`. CONTEXT.md classifies
history as KNOWLEDGE, which is `src/ui/`.

fish's design is the reference because it is battle-tested: append-only file,
periodic vacuum rewritten to a temp and `rename()`d in, mmap under a transient
lock, flock with a 0.25 s give-up. Two of its weaknesses are fixed here: the
O(n) startup scan that builds an offset index (an index baked into the file
replaces it), and read-only sessions never noticing a vacuum (fish #3565; a
directory watch replaces the `loaded_old` latch). Its text-delimiter scanner's
fragility class (#1581, #10483, #10782) is designed out by binary framing.

## Decision

### Placement

- New code in **`src/ui/history/`**, target `lesh_ui`. `src/runtime/history_store.*`
  and `tests/unit/history_store_tests.cpp` stay in the tree, compiling, as the
  historical implementation; `main.cpp` wires only the new store; nothing
  writes `~/.lesh_history` any more and nothing imports it. No runtime switch.
- `leshper` stays blind: history reaches the editor only as
  `LESH_PROPOSAL_HISTORY_MATCH` / `LESH_PROPOSAL_AUTOSUGGESTION` bytes. `lesh_leshper`
  still links `lesh_substrate` and nothing else.
- Files: `$XDG_DATA_HOME/lesh/` (default `~/.local/share/lesh/`):
  `history.data` (Tier 1) and `history.new.log` (Tier 2). `0700` dir, `0600` files.
- FlatBuffers is a **header-only vendored runtime** under `third_party/flatbuffers/`
  plus the `flatc`-generated `history_generated.h` **committed** next to
  `history.fbs`. No build-time codegen (ADR-0005 holds: no runtime shared
  libraries, and no new toolchain dependency). A test regenerates and diffs
  when `flatc` is on `PATH`, and skips otherwise.

### Tier 1 — `history.data`

One FlatBuffers blob, `file_identifier "SHH1"`, records **newest-first**
(`records[0]` is the most recent). Opened read-only, `mmap`'d, verified with the
flatbuffers Verifier **once per (re)map**. Reads are pointer arithmetic over the
mapping; records are never copied out — views/spans go upward.

```fbs
file_identifier "SHH1";
table Record {
  cmd:        [ubyte] (required);  // raw bytes, not string: command lines are not UTF-8
  when:       uint64;              // unix seconds
  cwd:        [ubyte];             // logical $PWD bytes at add time
  exit_code:  int32;
  session_id: uint64;              // low 64 bits of the session's uuidv7
}
table HistoryFile { records: [Record]; }
root_type HistoryFile;
```

`table` not `struct` (schema evolution beats one vtable hop at 10^4–10^5
records); `[ubyte]` not `string`; newest-first because the dominant query is
"scan recent, prefix match, stop at first hit". Decided; revisit only with a
profile.

**Unknown `file_identifier`** (a future format, or garbage): never destroy it.
Refuse to vacuum, run the session on Tier 2 + memory, warn once on stderr.

### Tier 2 — `history.new.log`

```
repeated: [u32 payload_len LE] [u32 crc32(payload)] [u8 format_version] [payload]
payload = one Record as a standalone finished FlatBuffer
```

- Each frame is written with **one** `write(2)` (or `writev`) — header and payload
  together — so concurrent unlocked appenders interleave whole frames, not bytes.
- Reader: `offset + 9 + len > size` → torn tail, stop, keep what parsed. CRC
  mismatch → **resync**: advance one byte at a time until a header whose
  `len` fits the file and whose `crc` validates the bytes it frames, then
  continue. Payload bytes are never interpreted as delimiters — the resync is
  framing-only. Unknown `format_version` → skip that frame (its length is
  known). CRC32 is a hand-rolled table; no zlib.

### In memory

- `std::deque<HistoryItem> new_items`, newest at the back, this session's items;
  never discarded on save (they are how own items are told from foreign ones).
  `first_unwritten_new_item_index` is the write cursor.
- `HistoryItem { cmd bytes, when, cwd bytes, exit_code, session_id, persist_mode,
  pending }`. `persist_mode` ∈ `disk`, `memory`, `ephemeral`. **Leading space =
  ephemeral**, unconditionally (fish's rule): retrievable until the next add,
  never written. An option to switch this off is phase 2.
- Empty and whitespace-only commands are never added (fish #6032). Adjacent
  duplicate adds merge: keep the max timestamp.

### Recording a command (Q2/Q17)

`session::execute` calls `add(item, pending=true)` before running, with `cmd`
and the logical `$PWD`; after the wait returns it calls
`resolve_pending(exit_code)`, which is the point that **appends the frame**.
Pending items are excluded from reads. A session that dies with an unresolved
pending item loses it — no sentinel write.

### Read path — the `history_source` seam (Q13/Q14)

`History` **implements the existing `lesh::ui::history_source`**
(`for_each_newest_first(bool-returning callback)`, `ui/history_search.h`). The
prompt's `HistorySearch` class is **not built**: `lesh::ui::history_search`
(line/prefix/token, ranges, cancel poll, zero-alloc scratch) is the searcher
and is untouched. The walk merges `new_items` (resolved, newest first) then the
mapped vector, deduplicating across the seam with a per-walk seen-set of `cmd`
bytes. Same-`cmd` items: the newest wins; on equal `when`, own `session_id` wins.

**Threading (ADR-0009):** `add`/`resolve_pending`/vacuum/reload run on the loop
thread; the walk runs on stateless helpers. `History` hands each compute an
**immutable snapshot view** — `shared_ptr` to {frozen copy of resolved
`new_items`, refcounted mmap handle}; mutation builds a new view and swaps the
pointer. Workers never see a mutation, a stale view keeps its old mapping alive,
and the compute path stays heap-free per `UiAutosuggest`'s zero-heap tests.
`new_items` is bounded by the vacuum cadence, so the copy is small.

Never re-check file identity per read: the watch and the write-path check make
it redundant, and a `stat` per keystroke is a syscall on the autosuggest path.

### Locking and staleness

- Appends and vacuums take `flock(LOCK_EX)`; mapping Tier 1 takes `LOCK_SH`
  only long enough to get a consistent size. If any lock takes > 0.25 s, set
  `abandoned_locking` and never lock again this process (fish
  `maybe_lock_file`). Never lock and never mmap when the data dir is remote
  (`statfs`: `f_fstypename` on macOS, `f_type` magic list on Linux, fish's
  `path_remoteness`); read Tier 1 into a heap buffer instead (fish PR #5097).
- `file_id_t = {dev, inode, size}` cached per opened file. On every append:
  open, lock, verify `file_id_for_path == file_id_for_fd` (TOCTOU; retry up to
  1024). If the fd's id differs from the *cached* id, another session vacuumed →
  `reload_needed`.
- **Directory watch** on the data dir — inotify `IN_MOVED_TO|IN_CREATE` (Linux),
  kqueue `EVFILT_VNODE NOTE_WRITE` on the directory (macOS; a rename-over never
  fires on the *new* file, so the file itself cannot be watched). The watch fd
  is **one more topic in the ui loop's `poll`** (`ui/loop.h` §8's fd hook) —
  no thread, no atomic: the drain re-`stat`s `history.data` and sets
  `reload_needed` on the loop thread. The next mutation or the next request
  builds a fresh view.
- `boundary_timestamp` and fish's `timestamp_now` +1 s hack are **not ported**;
  there is no `history merge` command. Foreign items appear as soon as a reload
  produces a new view. `session_id` exists only for the dedup tie-break.

### Vacuum (fish `save_internal_via_rewrite`)

Countdown starts random in `[0, 25)`, then every 25 appends (`kVacuumFrequency`,
one named constant with the cost arithmetic in a comment — at the 256 Ki cap it
rewrites ~25 MB, accepted for now).

1. Open target `O_RDONLY|O_CREAT`, snapshot its `file_id`.
2. Build the new blob into a `mkstemp` temp **in the same directory**: re-read
   the old blob via the fd (not the cached mmap), merge all log frames and
   unwritten `new_items`, dedup in an LRU map keyed on `cmd` bytes keeping the
   max timestamp (`history_lru_cache_t::add_item`), cap at
   `HISTORY_SAVE_MAX = 256 * 1024`, sort newest-first, serialize, `fsync`
   (`F_FULLFSYNC` on macOS). This is the only fsync in the subsystem.
3. Reopen target, `LOCK_EX`, re-stat the **path**: if its `file_id` changed since
   step 1 → `ftruncate(tmp, 0)`, retry from 1, bounded by 1024. On give-up, do
   not drop data: fall back to plain append of unwritten items.
4. `fstat` the original; `fchown`/`fchmod` the temp to match (fish #2355).
5. `rename(tmp, target)`. Only after success: truncate the log, advance the
   write cursor, drop the mmap state (`reload_needed`).
6. `unlink` the temp on every exit path.

Writers never truncate or modify `history.data` in place — so a stale mapping
goes stale, never invalid, and cannot SIGBUS on a local fs.

`save()` on interactive exit flushes unwritten items to the log and does not
vacuum. `~History` munmaps and closes everything (ADR-0007: the gate is "count
zero at exit", not "no long-lived descriptors").

### Phase 2 (not this feature)

`remove`, `clear`, `clear_session` and the vacuum's deletion set; a
`history` builtin; an option to disable leading-space privacy; cross-machine
sync. No SQL-style queries ever — filtering by cwd/exit code is a linear scan.

## Consequences

- Autosuggest stops doing file I/O per keystroke: a walk is pointer arithmetic
  over a mapping plus a small deque.
- Startup is `open`+`mmap`+one Verifier pass; no scan.
- A vacuum in another terminal is visible here at the next request without any
  write on our side.
- `src/ui/` changes, so the sweep exemption does not apply: every ticket runs
  `--gtest_filter='UiHistory*:UiAutosuggest*:UiHistorySearch*:UiSession*'`, then
  the sanitized gate, then the conformance sweeps (the corpus is non-interactive
  and should not move; the measurement is the proof).
- Two files instead of one, and a vendored header-only dependency.

## Tickets

Milestones in dependency order, each an Opus-class task: schema + round-trip;
log framing + torn-tail/resync; `History` + merge read path + session wiring;
vacuum + crash injection; staleness (file_id, watch topic, remote fallback);
property tests on merge/dedup.

## References

fish 3.7.1 `src/history.h`, `src/history.cpp`, `src/history_file.{h,cpp}`:
`history_impl_t` state, `maybe_lock_file`, `save_internal_via_appending`,
`save_internal_via_rewrite`, `rewrite_to_temporary_file`, `history_lru_cache_t`,
`item_at_index`. fish issues #3565, #5097, #1581, #10483, #4931, #2355, #6032.
