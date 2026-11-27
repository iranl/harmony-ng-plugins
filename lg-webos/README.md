# LG webOS TV Plugin

Controls LG TVs with webOS (2014 and later) over the TV's SSAP WebSocket: remote buttons,
volume, channels, inputs, apps, text, power, Wake-on-LAN.

## Setup

1. Device Studio > Add Device > **LG webOS TV**. Enter the TV's host name or IP address (give the TV
   a fixed address or a DNS name in your router).
2. Press **Pair with TV**, then choose **Accept** on the TV. The TV gives the Hub a client key,
   kept in `/data/harmony/plugin-data/lg-webos/key-<ip>`.

The plugin connects with TLS on port 3001 and falls back to port 3000 (older firmware). It sends
LG's signed app manifest and, when newer firmware refuses it, the unsigned one.

## Commands

| Command | Action |
|---|---|
| `Up` `Down` `Left` `Right` `Select` `Back` `Exit` `Home` `Menu` `QuickSettings` `Info` `Guide` `Captions` | remote buttons (pointer input socket) |
| `VolumeUp` `VolumeDown` `VolumeMute` `ChannelUp` `ChannelDown` | buttons; volume, channels and arrows repeat while held |
| `Play` `Pause` `Stop` `FastForward` `Rewind` `Record` `0`-`9` `Red` `Green` `Yellow` `Blue` `Input` `Netflix` `Amazon` | buttons |
| `<BUTTON>` | any webOS button name in capitals (`ASPECT_RATIO`, `SCREEN_REMOTE`, ...) |
| `Volume` | set the volume to the command's value (0-100; the command has `range` [0, 100]) |
| `HDMI 1`-`HDMI 4` | switch input |
| `app:<name or id>` | `youtube`, `netflix`, `prime`, `disney`, `spotify`, `plex`, `appletv`, `livetv`, `browser`, `hdmi1`-`hdmi4`, or a webOS app id |
| `text:<text>` | type into the text field on screen |
| `channel:<number>` | open a channel |
| `toast:<text>` | show a notification on the TV |
| `ssap:<uri> [<json>]` | any SSAP request, e.g. `ssap:audio/setMute {"mute":true}` |
| `PowerOn` | the TV answers (screen off): screen on; off the network: Wake-on-LAN |
| `PowerOff` | `system/turnOff` |
| `PowerToggle` | on: turn off; off the network: Wake-on-LAN |
| `Connect` / `Disconnect` | open or close the connection, nothing sent |
| `WakeOnLan` | the magic packet only |

## Wake-on-LAN

Turn on **Settings > General > Devices > TV Management > Turn on via Wi-Fi** (or **Mobile TV
On**; the name varies by year). The plugin learns the TV's MAC while it is on; or set **MAC
Address** on the device. Most models wake from Ethernet; some from Wi-Fi.

## Notes

- TLS: the TV's self-signed certificate is accepted on the first connection and its key is
  pinned (`tv-<ip>.pin`); a different key later is refused (delete the file to trust a new one).
- Needs Hub firmware with the shared BearSSL (`libbearssl.so.0`).
- Protocol and manifest as in [aiowebostv](https://github.com/home-assistant-libs/aiowebostv)
  (Apache-2.0); `lg_manifest.h` is generated from its `handshake.py`.
