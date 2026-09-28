# Add-ons

An add-on is an optional cpio overlay the sandbox can boot with, beside the base images
(`initramfs.cpio`, `python.cpio`, `tools.cpio`). Each one lives in a folder of its own here and is
off unless the app asks for it.

| Add-on | What it adds |
|---|---|
| [`claude-code`](claude-code) | `claude`: Claude Code in the guest (tools, permissions, hooks, sessions, MCP, sub-agents, the terminal UI), with the Anthropic API or an OpenAI-compatible server such as a local LLM |

## Layout of an add-on

```
addons/<name>/
  build.sh      builds the add-on; writes out/<name>.cpio (and out/licenses/*, if it has any)
  addon.json    its description, copied to out/<name>.json (see below)
  README.md
```

`./build.sh addons` (or `addons/build.sh [NAME...]`) runs every `build.sh` here. `scripts/build-engine.sh`
collects `out/<name>.cpio` and `<name>.json` into `dist/engine/images/addons/`, and
`scripts/package-runtime.sh` ships them as `app/images/addons/` and adds `--addon-dir` to the
manifest's `entry`.

An add-on image is an overlay (`userspace/mkinitramfs.py --overlay`): it holds only its own files
and is unpacked after the base images, so it can add to `/usr/bin` without repeating what is there.

## Turning one on

The app, in `start.config`:

```json
"addons": {"claude-code": {"provider": "openai", "baseUrl": "http://localhost:11434/v1", "model": "qwen3-coder"}}
```

(or a list of names, or `{"name": true}`, for no settings). On the command line:

```sh
bin/collabo-core-engine ... --addon-dir app/images/addons \
  --addon claude-code --addon-config claude-code:provider=openai --addon-config claude-code:model=qwen3-coder
```

For each add-on it turns on, the engine appends `<name>.cpio` to the initramfs and writes the
settings to `/etc/collabo/addons/<name>.json` in the guest — except `apiKey`, which becomes a
network secret the host adds to https requests for the host of `baseUrl`, so the guest never sees
it (a plain-http `baseUrl`, such as a model server on this computer, gets the key in the guest's
settings instead: the host adds secrets to https requests only).

## addon.json

```json
{
  "name": "claude-code",
  "description": "…",
  "extraAllowedHeaders": ["anthropic-version"],
  "apiKey": {
    "selector": "provider", "default": "anthropic",
    "variants": {
      "anthropic": {"baseUrl": "https://api.anthropic.com", "header": "x-api-key", "value": "{key}"},
      "openai": {"baseUrl": "https://api.openai.com/v1", "header": "authorization", "value": "Bearer {key}"}
    }
  }
}
```

- `extraAllowedHeaders`: request headers the guest may set through the request API (port 1080)
  beyond the defaults (accept, content-type, authorization, x-api-key), while the add-on is on.
- `apiKey`: how an `apiKey` setting becomes a secret. `selector` names the setting that picks the
  variant; `value` is the header's value, with `{key}` for the key.

## Writing one

Guest programs are wasm32 Linux (musl) binaries. C builds with `userspace/bin/wasm-cc`; Rust with
the guest toolchain `python/build-rust-toolchain.sh` makes (see `claude-code/build.sh`). Both need
the `userspace` step first (musl, compiler-rt).
