# Samsung Smart TV (Tizen) Plugin

Controls Samsung Smart TVs with Tizen (2016 and later models) over the TV's remote control
WebSocket. Every remote key, held keys, apps, text, power state, Wake-on-LAN.

## Setup

1. Device Studio > Add Device > **Samsung Smart TV**. Enter the TV's host name or IP address (give the
   TV a fixed address or a DNS name in your router).
2. Press **Pair with TV**, then choose **Allow** on the TV. The TV gives the Hub a token, kept in
   `/data/harmony/plugin-data/samsung-tv/token-<ip>`. Without pairing the TV asks on the first
   command instead.
3. Port: `8002` (TLS, 2018 and later models: the default), or `8001` for 2016 and 2017 models.

The TV lists the Hub under the Hub's name (**Hub Name** in the Hub's settings; `Harmony-NG Hub`
on firmware that does not send it). After a rename the TV asks once more to allow it, as a new
device. Keep **Access Notification** at "First Time Only": the TV then asks once per name.

A TV that was allowed once is listed in its **Device Connection Manager** (Settings > General >
External Device Manager); remove the Hub there to pair again.

## Commands

| Command | Key or action |
|---|---|
| `Up` `Down` `Left` `Right` `Select` `Back` `Exit` `Home` `Menu` `Tools` `Info` `Guide` `Captions` | `KEY_UP` ... `KEY_CAPTION` |
| `VolumeUp` `VolumeDown` `VolumeMute` `ChannelUp` `ChannelDown` `LastChannel` `ChannelList` | volume and channels |
| `Play` `Pause` `PlayPause` `Stop` `FastForward` `Rewind` `Record` | media keys |
| `0`-`9` `Red` `Green` `Yellow` `Blue` `Input` `HDMI` `HDMI 1`-`HDMI 4` | numbers, colours, sources |
| `KEY_<name>` | any Samsung key, as written (`KEY_AMBIENT`, `KEY_PMODE`, ...) |
| `app:<name or id>` | launch an app: `youtube`, `netflix`, `prime`, `disney`, `spotify`, `plex`, `appletv`, `max`, `twitch`, `browser`, or a Samsung app id (`app:3201907018807`) |
| `text:<text>` | type into the text field on screen |
| `PowerOn` | `PowerState` from `http://<tv>:8001/api/v2/`: standby: power key; off the network: Wake-on-LAN |
| `PowerOff` | power key while the TV is on (it toggles, so not in standby) |
| `PowerToggle` | power key |
| `Connect` / `Disconnect` | open or close the connection, no key sent |
| `WakeOnLan` | the magic packet only |

A held remote button holds the key on the TV (`Press`, then `Release`): the TV repeats it
itself, and the plugin releases it when no repeat or release came for 600 ms.

## HDMI inputs on 2018 and later TVs

Samsung TVs from 2018 on ignore the HDMI 1-4 keys over the network (seen on a 2019 QE65Q90R; the
other keys work, `HDMI` steps to the next input). For these the Hub sends HDMI 1-4 as infrared,
Samsung's discrete codes, from its own IR blaster: tick **HDMI via IR** on the device. **Pair
with TV** ticks it by itself for a TV whose info page reports a 2018 or later model, and leaves
it off for older ones; save the device afterwards. Needs Hub firmware that sends a plugin
device's IR alternatives (`irKeyCode`, `irWhen`); older firmware sends HDMI over the network as
before. The Hub's IR blaster must reach the TV.

## Wake-on-LAN

Turn on **Settings > General > Network > Expert Settings > Power On with Mobile** (name varies
by year). The plugin learns the TV's MAC while it is on; or set **MAC Address** on the device.

## Notes

- TLS: the TV's self-signed certificate is accepted on the first connection and its key is
  pinned (`tv-<ip>.pin`); a different key later is refused (delete the file to trust a new one).
- Needs Hub firmware with the shared BearSSL (`libbearssl.so.0`): the Hub refuses the install
  on older firmware.
- Protocol as implemented by [samsungtvws](https://github.com/xchwarze/samsung-tv-ws-api) (MIT).
  Pre-2016 models (Orsay, port 55000) are not supported.
