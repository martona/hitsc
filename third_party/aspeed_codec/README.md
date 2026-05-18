# aspeed_codec provenance

This directory contains a trimmed vendored subset of the upstream
`AspeedTech-BMC/aspeed_codec` project:

https://github.com/AspeedTech-BMC/aspeed_codec

The upstream project is licensed under MPL-2.0; see `LICENSE` in this
directory.

## Retained files

- `src/decoder.c`
- `src/jpegdec.h`
- `src/jtable.h`
- `LICENSE`

`hitsc` builds the retained C decoder directly through CMake and wraps it from
`src/aspeed_decoder.cpp`.

## Omitted files

The upstream JavaScript package metadata and browser decoder artifacts are not
used by `hitsc` and are intentionally not vendored here:

- `package.json`
- `lib/*.js`
- `lib/*.wasm`
