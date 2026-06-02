# protoc 3.15.x toolchain

The vendored protobuf runtime in `include/google/protobuf/` declares
`GOOGLE_PROTOBUF_VERSION 3015003` and the prebuilt static lib at
`lib/libprotobuf-lite.a` is the matching 3.15.x build. Generated
`.pb.h` / `.pb.cc` files must come from a `protoc` in the same minor
line; system `protoc` 4.x emits headers that fail the
`PROTOBUF_MIN_PROTOC_VERSION` check baked into the runtime headers.

This directory holds a pinned `protoc 3.15.8` (Linux x86_64 release
from `protocolbuffers/protobuf`) used for code generation only —
nothing at runtime depends on it. The downloaded `bin/` and
`include/` are gitignored; only `fetch.sh` and this README are
tracked.

## Setup

```sh
tools/protoc-3.15/fetch.sh
```

Idempotent: skips the download if the right version is already
present. Override the version with `PROTOC_VERSION=3.15.x` if a
matching runtime is ever vendored in.

## Usage

Generate `.pb.h` and `.pb.cc` from a `.proto` in
`src/sdk/protobufs/`, then rename `.pb.cc` to `.pb.cpp` so the
Makefile glob (`find src/ -iname '*.cpp'`) picks it up:

```sh
tools/protoc-3.15/bin/protoc \
    --proto_path=src/sdk/protobufs \
    --cpp_out=src/sdk/protobufs \
    src/sdk/protobufs/steammessages_contentserverdirectory.proto
mv src/sdk/protobufs/steammessages_contentserverdirectory.pb.cc \
   src/sdk/protobufs/steammessages_contentserverdirectory.pb.cpp
```

## .proto requirements

Every `.proto` in `src/sdk/protobufs/` must declare:

```proto
option optimize_for = LITE_RUNTIME;
```

The shipped `libprotobuf-lite.a` only provides the lite runtime
symbols. Without this option, generated code references the full
`Message` base class (descriptor pool, reflection, `UnknownFieldSet`),
none of which is in lite, and the link fails with undefined
references to `AssignDescriptors`, `UnknownFieldParse`, etc.

## Refresh

To bump the pinned protoc version, edit `fetch.sh` (`version=...`)
or set `PROTOC_VERSION` in the environment when running it. Stay
inside the 3.15.x line until the runtime in
`include/google/protobuf/` is also bumped.
