# FlatBuffers (vendored header-only runtime)

Upstream: <https://github.com/google/flatbuffers>
Version: **25.12.19** — release tag `v25.12.19`.
Tarball: <https://github.com/google/flatbuffers/archive/refs/tags/v25.12.19.tar.gz>
SHA-256 of that tarball: `f81c3162b1046fe8b84b9a0dbdd383e24fdbcf88583b9cb6028f90d04d90696a`
Licence: Apache License 2.0 — the upstream `LICENSE`, copied here verbatim.

Segregated under `third_party/` because these files are Google's, not lesh's.
Nothing here is modified: each header is the published one, byte for byte.

## Why a vendored copy, and why only headers

ADR-0010 puts Tier 1 of the history — `history.data` — in one FlatBuffers blob:
`mmap` it, run the Verifier once, and read records as pointer arithmetic over
the mapping. That needs the FlatBuffers *runtime*, which is header-only: a
template library with no out-of-line definitions and no object code of its own.

ADR-0005 forbids runtime shared libraries. A header-only dependency adds none —
it compiles into `lesh_ui` and disappears. Vendoring rather than
`find_package(Flatbuffers)` also keeps the build free of a new toolchain
dependency: a checkout builds with the compiler and CMake it already had.

## What is vendored

`include/flatbuffers/*.h` — the 30 published headers of the include directory,
whole. The `pch/` subdirectory is not copied (nothing in the set includes it),
and neither is `src/`, `tests/`, or any of the per-language runtimes.

Only the runtime half of that set is reachable from lesh:
`flatbuffers.h` and its closure — `allocator.h`, `array.h`, `base.h`,
`buffer.h`, `buffer_ref.h`, `default_allocator.h`, `detached_buffer.h`,
`flatbuffer_builder.h`, `stl_emulation.h`, `string.h`, `struct.h`, `table.h`,
`vector.h`, `vector_downward.h`, `verifier.h`.

The rest — `idl.h`, `flatc.h`, `code_generator.h`, `code_generators.h`,
`file_manager.h`, `minireflect.h`, `reflection.h`, `reflection_generated.h`,
`registry.h`, `util.h`, `hash.h`, `grpc.h`, `flexbuffers.h`,
`flex_flat_util.h` — belong to the **schema compiler**, not the runtime.
Several of them declare functions whose definitions live in the upstream
`src/*.cpp` that is deliberately *not* here, so including one of those from
lesh code compiles and then fails to link. Don't. They are kept only so that
this directory is the published header set rather than a subset somebody has
to re-derive on the next re-vendor.

## The generated header is committed, not generated at build time

`src/ui/history/history.fbs` is the schema; `src/ui/history/history_generated.h`
is what `flatc --cpp` emits from it, and it is **committed**. The build never
runs `flatc`, so `flatc` is not a build dependency (ADR-0010 §Placement).

`UiHistoryBlobSchema.GeneratedHeaderIsCurrent` in
`tests/unit/ui_history_blob_tests.cpp` regenerates the header with whatever
`flatc` is on `PATH` and diffs it against the committed one, and `GTEST_SKIP`s
when there is no `flatc` — the same shape as `unicode_tables_current`, and for
the same reason: a hand-edited generated file is a wrong answer that still
compiles.

The generated header `static_assert`s on `FLATBUFFERS_VERSION_*`, so **the
`flatc` that regenerates it must be the same version as these headers**. That
is the pin above: 25.12.19 (`brew install flatbuffers`, at time of writing).

## Re-vendoring

```sh
V=<new version>
curl -sSL -o fb.tar.gz \
  "https://github.com/google/flatbuffers/archive/refs/tags/v$V.tar.gz"
shasum -a 256 fb.tar.gz            # record it above
tar xzf fb.tar.gz
rm -f third_party/flatbuffers/include/flatbuffers/*.h
cp "flatbuffers-$V"/include/flatbuffers/*.h \
   third_party/flatbuffers/include/flatbuffers/
cp "flatbuffers-$V"/LICENSE third_party/flatbuffers/LICENSE
```

Then install the matching `flatc`, regenerate `history_generated.h` —

```sh
flatc --cpp -o src/ui/history src/ui/history/history.fbs
```

— and update the version, URL and SHA-256 in this file.
