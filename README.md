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

Create `~/.sirio/models.json` before configuring credentials, inspecting the
catalog, or starting an agent. Sirio does not generate or migrate this file. A
complete starting configuration is:

```json
{
  "interface": {
    "models": [
      "opencode-go/deepseek-v4-flash",
      "opencode-go/deepseek-v4-pro"
    ],
    "last_used": {"model": null}
  },
  "deepseek": [
    {"id": "deepseek-v4-flash", "last_effort": "high", "active": true},
    {"id": "deepseek-v4-pro", "last_effort": "high", "active": true}
  ],
  "openai": [
    {"id": "gpt-5.6-sol", "last_effort": "low", "active": true},
    {"id": "gpt-5.6-terra", "last_effort": "medium", "active": true},
    {"id": "gpt-5.6-luna", "last_effort": "low", "active": true}
  ],
  "opencode-go": [
    {"id": "deepseek-v4-flash", "last_effort": "none", "active": true},
    {"id": "deepseek-v4-pro", "last_effort": "high", "active": true},
    {"id": "glm-5.3", "last_effort": "high", "active": true}
  ],
  "kimi": [
    {"id": "k3", "last_effort": "high", "active": true},
    {"id": "k3-256k", "last_effort": "high", "active": true},
    {"id": "kimi-for-coding", "last_effort": "high", "active": true}
  ]
}
```

Create the directory and keep the configuration private:

```sh
mkdir -p ~/.sirio
chmod 700 ~/.sirio
$EDITOR ~/.sirio/models.json
chmod 600 ~/.sirio/models.json
```

Only models listed in a provider array are configured. `interface.models` is
an ordered subset, using full `provider/model` identifiers, available to the
base interface. Active configured models outside that list are available only
through `subprocess`; `active: false` disables a model everywhere. Provider
arrays may be omitted when none of their models are needed.

`interface.last_used` has exactly one field, `model`, whose value is either
`null` or one full identifier from `interface.models`. Sirio updates that field,
together with the selected model's `last_effort`, after a successful interface
selection. There are no model aliases; use the model ID, qualifying it with the
provider whenever the same ID exists under more than one provider.

Then save credentials for the providers you configured:

```sh
./sirio auth --api-key opencode-go
./sirio auth --api-key deepseek
./sirio auth --login openai
./sirio auth --api-key kimi
```

Without `--model`, Sirio reuses the active `interface.last_used` model when its
credentials are available, then falls back through `interface.models` in
order. Use `./sirio catalog --models` to inspect `interface` and `subprocess`
scope.

Then start the interface, or run one prompt:

```sh
./sirio
./sirio -C /path/to/project
./sirio --non-interactive -p "Explain this repository"
```

`/model` selects any active model in `interface.models`, including a model from
another provider. Alt-k and Alt-l traverse the configured interface order.

List or resume saved sessions with `./sirio sessions --list` and
`./sirio sessions --resume <id>`. See `./sirio sessions --help` for deletion
and canonical rewriting.

`./sirio --help` lists the available options. `make test` runs the tests and
`make sanitize` repeats them with ASan and UBSan.

Agents can use the `subprocess` tool to delegate focused work to another
host-side agent process with inherited workspace, configuration and
authentication. The optional `model` argument selects any active catalog model
using `provider/model`; omitting it inherits the current model.

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
