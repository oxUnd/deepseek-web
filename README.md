# deepseek-web

Web search and page fetching for Morph, powered by DeepSeek's Search / Fetch API.
Registers the native tool `deepseek_web` through `ext_run_v2`.

## Install

In Morph:

```text
/ext install github:oxUnd/deepseek-web@main --yes
```

Restart Morph to load the plugin. Requires ext SDK 1.1 `get_config_string`
support; Morph v0.5.0 is not supported.

## Build

Requires a C11 compiler, make, curl, tar, CMake >= 3.20, and libcurl >= 7.85
with asynchronous DNS support.

```sh
make
make test
make install
```

Installs to `~/.morph/exts/deepseek_web`. Override with
`make install EXT_INSTALL_DIR=/absolute/path`.

The build uses the bundled Morph SDK 1.1.0 and fetches `nothing` commit
`fe8cd34e7bf0b401f553eb908794dbb5f2c6c7a3`, verified by SHA-256. Its public
`<nb/xxx.h>` headers and static library come from the same source. Dependencies
are cached in `.deps/`; no Morph source checkout is needed.

`make clean` removes build outputs; `make distclean` also removes dependencies.

## Configuration

The active text model must use the `deepseek` adapter. API keys are resolved from
`model.text.api_key` / `api_key_env`, then `model.vision`, then `DEEPSEEK_API_KEY`.

The default endpoint is `https://api.deepseek.com`. Override it with
`DEEPSEEK_API_BASE`.

## Tests

`make test` runs local mock tests for search/fetch, configuration, errors,
redirects, response limits, cancellation, and deadlines.

To test the live API with a valid key:

```sh
python3 test_deepseek_web.py --lib ./deepseek_test.so --real
```
