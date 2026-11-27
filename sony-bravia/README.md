# Sony Bravia TV Plugin

Controls Sony Bravia TVs (2013 and later) over their IP control API: plain HTTP with a
pre-shared key. Every remote key, power on and off, HDMI inputs, volume, text, Wake-on-LAN.

## Setup

1. On the TV: **Settings > Network > Home network > IP control**: Authentication **Normal and
   Pre-Shared Key**, and a **Pre-Shared Key** (for example `0000`). Turn on **Remote start**
   (Settings > Network) so the TV can be turned on from standby.
2. Device Studio > Add Device > **Sony Bravia TV**. Enter the TV's host name or IP address and the same key.

No pairing: the key is the authentication. A wrong key is logged as refused.

## Commands

| Command | Action |
|---|---|
| `Up` `Down` `Left` `Right` `Select` `Back` `Exit` `Home` `Options` `ActionMenu` `Info` `Guide` `Captions` | IRCC remote codes |
| `VolumeUp` `VolumeDown` `VolumeMute` `ChannelUp` `ChannelDown` `LastChannel` | repeat while held |
| `Play` `Pause` `Stop` `FastForward` `Rewind` `Next` `Previous` `Record` `0`-`9` `Red` `Green` `Yellow` `Blue` `Input` `Netflix` | IRCC codes |
| `ircc:<code>` | any IRCC code (base64, from the TV's `getRemoteControllerInfo`) |
| `HDMI 1`-`HDMI 4` | switch input (`setPlayContent`) |
| `Volume` | set the volume to the command's value (0-100) |
| `app:<uri>` | `setActiveApp` with the app URI the TV lists |
| `text:<text>` | type into the text field on screen |
| `PowerOn` | on: nothing; standby: `setPowerStatus` true; off the network: Wake-on-LAN |
| `PowerOff` | `setPowerStatus` false while on |
| `PowerToggle` | by the TV's power status |
| `WakeOnLan` | the magic packet only |

Bravia models with Android TV or Google TV also work with the `android-tv` plugin (apps by name,
no key to set); this plugin needs no pairing and gives discrete power and inputs.
