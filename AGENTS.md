# Repository Guidelines

## Project Structure & Module Organization

Sirio is a C11 terminal coding agent. Sources and headers live at the repository
root. `sirio.c` is the entry point; `sirio_provider.*` handles model providers,
`sirio_core.*` and `sirio_worker.*` implement the
agent runtime, and `sirio_container.*` owns the Podman/runner boundary.
`linenoise.*` provides terminal input. Standalone C tests live under `tests/`.
`container/` contains the Linux tool runner, multistage `Containerfile`, and
image build script.

## Build, Test, and Development Commands

- `make` prints the available targets.
- `make sirio` builds `./sirio` with the configured C compiler and libcurl.
- `make install` installs an already-built binary in `/usr/local/bin`; it does
  not compile and requests superuser permission only for installation.
- `make test` builds Sirio and runs every standalone test executable.
- `make sanitize` repeats the suite with AddressSanitizer and UBSan, then
  removes generated files.
- `make clean` removes binaries and object files.
- `./container/build.sh` builds the `cma` image. The runner is compiled inside
  Linux; do not add a host-built runner artifact.

## Coding Style & Naming Conventions

Use four-space indentation in C; Make recipes require tabs. Follow the existing
brace placement. Use `snake_case` and the `sirio_` prefix for public symbols,
uppercase names for macros, and `static` for
translation-unit internals. Keep interfaces in matching `.h` files and avoid
merging unrelated provider, worker, or container responsibilities. No automatic
formatter or linter is configured; builds must remain warning-free under the
Makefile's `-Wall -Wextra`.

## Testing Guidelines

Tests are plain C programs named `tests/<module>_test.c`, including the
container runner contract test. Add new binaries to
`TEST_BINS` and invoke them from the `test` recipe. Run both `make test` and
`make sanitize` before submission. There is no numeric coverage requirement or
hosted CI; cover changed contracts and failure paths.

## Commit & Pull Request Guidelines

Existing history uses short imperative subjects. Continue with focused commits
in that style. Pull requests should explain the behavior change, list
verification commands, and call out changes to provider schemas, the runner
protocol, or container permissions. Include terminal output only when it
clarifies user-visible CLI behavior; screenshots are generally unnecessary.

## Security & Configuration

Never commit credentials, session data, or traces. The selected workspace is
mounted read-write into `cma`, and prompt or tool content may be sent to the
configured model provider. Keep trace files private and preserve the ephemeral
container and browser-profile lifecycle.
