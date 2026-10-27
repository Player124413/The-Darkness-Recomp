# DarkRecomp XWC audio tool

A standalone, zero-dependency Rust tool for read-only inspection of the observed
Xbox 360 XWC2 bank layout, original-packet XMA2 RIFF export, and a header-derived
timeline diagnostic. It currently supports inspect/export/header-timeline work
only. It has no PCM import, replacement-pack generation, decoder, or playback.

## Build

From the repository root (Rust 1.74 or newer):

```text
cargo build --manifest-path tools/xwc_audio/Cargo.toml --target-dir <OUTSIDE_SOURCE_BUILD_DIR> --offline --locked --release
```

The binary is `darkrecomp-xwc-audio` (`.exe` on Windows). The crate is independent
of CMake and the native runtime. `--manifest-path` may also be an absolute path
when invoking Cargo from another directory.

## Commands

```text
darkrecomp-xwc-audio INPUT.xwc
darkrecomp-xwc-audio INPUT.xwc NEW_METADATA.json
darkrecomp-xwc-audio validate-xma INPUT.xwc
darkrecomp-xwc-audio export-xma INPUT.xwc EXACT_WAVE_NAME NEW_OUTPUT.xma.wav
darkrecomp-xwc-audio raw-timeline INPUT.xwc EXACT_WAVE_NAME
```

Inputs are opened read-only. Report and RIFF outputs use exclusive `create_new`
creation and must not already exist; source banks are never modified. Selected
wave names must identify exactly one record. Inspection bounds metadata reads.
Envelope validation and export check absolute wave ranges, wrapper lengths,
format consistency, and complete 2048-byte packet bounds. Export copies the
original XMA2 format and encoded packets with a fixed 64 KiB buffer; it does not
re-encode audio. The exporter retains support for original XMA2 versions 3 and 4,
one-stream mono/stereo, with matching positive wrapper/format sample rates.

`raw-timeline` first validates the selected wave's XMA envelope and then reads
exactly its encoded payload, one complete packet at a time. This diagnostic is
restricted to the observed TV profile: original XMA2 format version 4, one
stream, mono/stereo, at 44100 or 48000 Hz. It reads each packet's first 32-bit word
as big-endian, sums its high six `frameCount` bits, and multiplies by 512 to give
**header-derived frames per channel**. It rejects any nonzero low-eight-bit
packet skip, truncated packet (including the final body), unsupported format,
or arithmetic overflow. Channel count does not multiply this timeline.

The diagnostic prints JSON with `headerDerivedCodecFrames`,
`headerDerivedFramesPerChannel`, `metadataSamplesPerChannel`, their signed
`headerDerivedMinusMetadataSamplesPerChannel` difference, packet count, rate,
channels, and `audioDecoded: false`. These are header arithmetic and bank
metadata. They do not establish decoded PCM length, playback, delay, trimming,
seeking, looping, or video synchronization.

An independent, previously saved native audit used the unchanged production
`XmaRawDecoder`, pulling 128 frames at a time without EOF or flushing. Its raw
frames matched this header formula for all 52 actual TV streams, with raw minus
metadata differences of 896..1404 frames per channel and no input/source drift.
That audit is separate evidence, not a decoder bundled with this tool; it was
not rerun for this integration. It does not certify runtime replacement or sync.

The original 17 parser/export tests are retained, with four additional pure
synthetic tests for header aggregation, skip rejection, truncation, and format
restrictions. No game banks, extracted audio, private binaries, or probe logs
belong in this crate.

## Native source diagnostic

For developer tracing, set `DARK_XMA_ORIGIN_PROBE=1` before starting a fresh
native game process. The probe is off by default. It observes the original
streamed loader and emits at most 128 source allocation/lifecycle events to
stderr, without changing audio decoding or guest memory.

A successful diagnostic snapshot copies the retained bank pathname, local
wave index, slice extent and stream index into the decoder allocation record.
The host allocation generation changes on reuse; freeing a context clears its
record. Reset events retain that generation. Unsupported routes remain
unbound; numeric rejection values correspond to `XmaSourceIdentity::Rejection`.

The observed reference chain keeps the source objects alive, but concurrent
field stability and all runtime path variants still require validation. These
records are diagnostic evidence only. No PCM selection, replacement, seeking,
looping or synchronization is implemented by this probe.
