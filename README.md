# Sirio

Sirio is a small terminal coding agent written in C.

## Build

It requires a C11 compiler, libcurl, POSIX threads and Make:

```sh
make sirio
make install
```

`make install` installs the binary in `/usr/local/bin` and asks for superuser
permissions; set `PREFIX` to choose another system directory.

Agent runs also require `podman`, an existing image named `cma`, and a compatible
tool runner inside that image. Build the repository image locally with:

```sh
./container/build.sh
```

The script compiles the runner inside Linux and tags the result `cma`. The Sirio
executable never builds or pulls it; `--raw-prompt` does not start a container.

Podman can show untagged `<none>` images after a build. They may be reusable
build cache; pruning them frees disk space but can make the next build slower.

## Use

Save credentials once:

```sh
./sirio auth --api-key deepseek
./sirio auth --login openai
./sirio auth --api-key opencode-go
./sirio auth --api-key kimi
```

Use `./sirio catalog --models` to inspect the current catalog. Without an
explicit model, Sirio reuses the last one selected for that provider; without
a provider, it reuses the last global selection.

Set `active` in `~/.sirio/models.json` to enable or disable catalog models.

Then start the interface, or run one prompt:

```sh
./sirio
./sirio -C /path/to/project
./sirio --non-interactive -p "Explain this repository"
```

A provider is fixed for the lifetime of a conversation. `/model` selects only
models from that provider; loading a saved session with `/switch` restores the
provider saved with that session.

List or resume saved sessions with `./sirio sessions --list` and
`./sirio sessions --resume <id>`. See `./sirio sessions --help` for deletion
and canonical rewriting.

`./sirio --help` lists the available options. `make test` runs the tests and
`make sanitize` repeats them with ASan and UBSan.

Agents can delegate work to another host-side Sirio process with inherited
workspace, configuration and authentication.

The selected workspace is mounted read-write in the tool container. Content
included in prompts or tool results is sent to the configured model provider.

## Known problems

Session history shown by `sessions --resume`, `/switch`, or `/history` is not
yet rendered exactly like live output; some Markdown or terminal formatting
may appear as plain text.

`google_search` can repeatedly hit Google's automated-traffic block, even when
Chromium is installed and later calls reuse the same browser process. For a
known URL, use `visit_page` or `curl` through the `bash` tool.

## Origin and license

Sirio started from the agent layer of [DS4 Agent](https://github.com/antirez/ds4)
by Salvatore Sanfilippo and uses linenoise. See [LICENSE](LICENSE).
