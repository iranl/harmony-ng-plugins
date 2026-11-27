# Plugin Template

A working plugin to copy: `my_plugin.c` sends one HTTP POST per key to a device, with the three
kinds of command a plugin has (one-shot, repeating, Wake-on-LAN fallback) and a pairing button.
CI builds and tests it with the other plugins, so it stays correct.

## Make your own plugin

1. Copy the folder: `cp -r template my-device`. Rename `my_plugin.c`, and in it set `ID` to the
   folder name (the plugin id: letters, digits, `-`, `_`).
2. Rename `manifest.template.json` to `manifest.json` (named so that the release workflow and
   `repository.json` skip the template) and edit it: `id`, `name`, `entrypoint` (`bin/<binary>`), `files`, the device's
   `config_fields` and `default_commands` (keyCode `PLUGIN:<id>:<command>`). Remove `pairing`
   when the device needs none. A command that takes a number gets `"range": [min, max]` and
   reads `m.value`.
3. Replace `send_key()` with your device's protocol. The shared headers in `../common/`:

   | Header | What it has |
   |---|---|
   | `plugin.h` | socket (`plg_open`, `plg_recv`), datagram fields (`plg_parse`, `plg_json_str`), one-shot check (`plg_once`), pairing answer (`plg_status`), state dir (`plg_data_dir`), files, TCP, plain HTTP, base64 |
   | `wol.h` | Wake-on-LAN (`wol_wake`), the device's MAC from the ARP table (`wol_learn` while it is on) |
   | `tls.h` | TLS client with the device's key pinned on first use (needs `"requires_libs": ["libbearssl.so.0"]`) |
   | `ws.h` | WebSocket client over TCP or TLS, ping check of an idle link (`ws_alive`) |

   Examples: `sony-bravia` (HTTP), `samsung-tv` (WebSocket over TLS, held keys, confirm pairing),
   `lg-webos` (two WebSockets), `wol` (listen pairing with a host).
4. Add it to `build_plugins.sh`: `build_c my-device my_device.c my_device` (TLS: add
   `"$BEARSSL_LINK"`), and to the packaging list. Add its test to `scripts/run_host_tests.sh`:
   `build_tv my-device my_device.c test_my_device.py`.
5. Test against a fake device: `test_my_plugin.py` (Python, `scripts/fake_ws.py` has the Plugin
   helper and a fake WebSocket/TLS server).
6. Open a pull request. Bump `version` to release a change.

## Rules of the Hub

- Stay in the foreground: the Hub starts the binary, restarts it when it exits, stops it with
  SIGTERM. No fork into the background, no pid file.
- One `poll()` loop is the smallest design (threads work on the Linux 6.18 Hub system, but
  every thread costs RAM). Network calls with short timeouts: the socket queue fills while the
  plugin waits.
- Set `"min_harmony_version": "4.0.0"` in `manifest.json`: the plugins link the Linux 6.18
  system's shared C library, and an older Hub refuses them instead of installing a plugin that
  cannot start.
- State that must survive an update in `plg_data_dir()` (`/data/harmony/plugin-data/<id>`):
  it is flash, write only what changed (`plg_write_file` does). Runtime files in `/tmp/plugins`.
- A remote button sends press, repeats and release: act once for one-shot commands, release a
  key held on the device when no repeat or release came for 600 ms (`PLG_HOLD_DEADLINE_MS`).

The full guide: [docs/PLUGINS.md](https://github.com/iranl/harmony-ng/blob/main/docs/PLUGINS.md).
