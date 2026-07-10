# Neuracoust RT Plugin SDK

This SDK is the server-side DSP ABI for Neuracoust-owned plugins. Third-party
VST3/AU plugins remain local-only in the DAW unless a separate Linux host path is
added later.

## Module Contract

A server plugin is a shared library exporting:

```c
const NaRtPlugin *na_rt_get_plugin(void);
```

The plugin receives channel-major float buffers through `NaRtAudioBlock`.
Processing must be realtime-safe:

- no heap allocation inside `process`
- no locks inside `process`
- no file or network I/O inside `process`
- bounded CPU cost per frame
- parameter changes must complete without blocking the audio packet path

## Build

```sh
make modules
make check-4001e
```

The first production-facing module is:

```text
build/na_4001e.so
```

## Loading

Run a self-test with a module:

```sh
./build/neuracoust-rt-engine --module ./build/na_4001e.so --self-test
```

Run the UDP engine with a module:

```sh
sudo ./build/neuracoust-rt-engine --module ./build/na_4001e.so
```

The current UDP audio port is `20000` by default and can be changed with
`NA_RT_AUDIO_PORT` or `--port`.
