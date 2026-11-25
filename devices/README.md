# Device templates

Ready-made devices for Harmony-NG. The Hub's web UI lists `devices/index.json` under
**Create New Appliance / Device > From template**. The user picks a template, ticks the
transports to use, installs the plugin if one is needed, and fills in the setup fields. The UI
then saves one combined device (harmony-ng `docs/API.md` 3.10).

`scripts/device_templates.py . --write` checks every template and writes `index.json`. CI runs it
without `--write` (from `scripts/run_host_tests.sh`) and fails when a template would be cut or
refused by the Hub, or when the index is out of date.

## Format (`devices/<id>.json`, schema 1)

```json
{
  "schema": 1,
  "id": "denon-avr",
  "name": "Denon / Marantz AV receiver",
  "manufacturer": "Denon",
  "model": "AVR-X / SR / NR (2016 and later)",
  "category": "receiver",
  "icon": "receiver",
  "description": "One line for the template list.",
  "transports": {
    "tcp":  { "label": "Telnet (port 23)", "default": true,
              "connection": { "netHost": "{{host}}", "netPort": 23 } },
    "http": { "label": "HTTP (port 8080)",
              "connection": { "netHost": "{{host}}", "netPort": 8080 } },
    "plugin": { "label": "...", "id": "sony-bravia", "config": { "host": "{{host}}" },
                "default_commands": true },
    "ir":   { "label": "Infrared (learn the codes later)" }
  },
  "setup": {
    "message": "Shown above the fields: what to set on the device first.",
    "fields": [ { "key": "host", "label": "IP address", "type": "string", "required": true } ]
  },
  "commands": [
    { "name": "PowerOn", "transport": "tcp", "tcpPayload": "PWON\\r" },
    { "name": "PowerOn", "transport": "http", "httpPath": "/goform/formiPhoneAppDirect.xml?PWON" }
  ]
}
```

- `transports` uses the keys `ir`, `bt`, `http`, `tcp`, `mqtt` and `plugin`. **Their order is the
  priority**: when two ticked transports have a command of the same name, the first transport's
  command is kept. Transports with `"default": true` start ticked, and at least one must have it.
- `connection` (http, tcp) holds the fields of an HTTP or TCP device: `netHost`, `netPort`,
  `netTls`, `netTlsVerify`, `netTimeoutMs`, `httpHeaders`, `httpAuthType`, `httpUser`,
  `httpPassword`, `httpToken`.
- `plugin.id` must be in `repository.json`. The UI offers to install it. `config` becomes the
  device's plugin settings, and `"default_commands": true` adds the plugin's own commands.
- `setup.fields` have the types `string`, `password` and `number`, plus optional `default`,
  `placeholder` and `required`.
- `{{key}}` in any string is replaced with the field's value. A string that is only `{{key}}`
  takes the value as typed, so `"netPort": "{{port}}"` with a number field gives a number. Note
  that `{device}` and `{command}` (single braces) are left for the Hub to fill in at send time.
- A command has the fields of its transport (harmony-ng `docs/API.md` 3.9):
  - http: `httpMethod`, `httpPath`, `httpBody` (at most 2048 bytes), `httpContentType`, and
    `httpHeaders` of its own (at most 8 together with the connection's);
  - tcp: `tcpPayload`, and `tcpFormat` `text` (escapes `\r \n \t \xHH`) or `hex`;
  - plugin: `keyCode` `PLUGIN:<id>:<command>`.

  IR, BT and MQTT commands are usually added afterwards in the studio: learned, chosen from the
  key presets, or named for MQTT.
