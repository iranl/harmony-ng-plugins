# Android TV / Google TV Remote (v2) Plugin for Harmony-NG

Native C daemon for local network control of Android TV, Google TV, NVIDIA Shield, and Chromecast with Google TV devices via the official **Google TV Remote Protocol v2** (TLS).

Designed for **100% protocol and feature parity** with [`androidtvremote2`](https://github.com/tronikos/androidtvremote2) protocol engine.

---

## Highlights

- **Zero ADB Dependency**: Connects directly to Google's official Android TV Remote Service on TLS ports `6466` and `6467`. No ADB debugging, no developer options, and no ADB server background processes.
- **Direct Wire Protobufs**: Nanosecond C-level Protocol Buffer packet serialization for immediate tactile response on Harmony remotes.
- **The Hub's BearSSL**: TLS 1.2 through the firmware's shared `/data/harmony/lib/libbearssl.so.0` (`requires_libs`: the Hub refuses the install on firmware without it, update the Hub first). The plugin is 34 KB instead of 100 KB with its own copy. Each Hub makes its own RSA key and certificate on the plugin's first start (one time, can take a few minutes on the Hub) and keeps them in `/data/harmony/plugin-data/android-tv/`.
- **Bi-Directional Pairing**: Interactive pairing flow with on-screen 6-character PIN code exchange.

---

## Ports & Protocol

| Port | Transport | Purpose |
|---|---|---|
| `6467` | TLS (Polo protocol) | Initial pairing handshake, `OuterMessage` negotiation, and PIN verification. |
| `6466` | Authenticated TLS | Runtime command injection (`RemoteKeyInject`), text input (`RemoteImeBatchEdit`), app launches (`RemoteAppLinkLaunchRequest`), and session heartbeats. |

---

## Pairing Instructions

1. Ensure your Android TV / Google TV is powered on and connected to the same Wi-Fi / local network as your Harmony Hub.
2. In the Harmony-NG WebUI (**Devices Studio**), click **Add Device** and select **Android TV / Google TV (Plugin)**.
3. Enter your TV's host name or local IP address (e.g. `shield.home.lan` or `192.168.1.60`).
4. Click **Pair with TV** (or send the `Pair` command):
   - The plugin initiates the pairing handshake on port `6467`.
   - Your TV screen displays a **6-character PIN code** (e.g. `B83F1A`).
   - Enter the PIN code when prompted in the WebUI.
5. Once verified, the credentials are saved on the Hub in `/data/harmony/plugin-data/android-tv/state.txt`. All future commands route through authenticated TLS port `6466`.

---

## Command Syntax

### 1. Direction Prefixes (Press, Hold, Release)
Supports explicit direction modifiers:
- `SHORT:<command>` — Standard momentary key tap (default).
- `START_LONG:<command>` — Key down / start hold (used for continuous scrolling, fast-forwarding, or turning on TV).
- `END_LONG:<command>` — Key up / release hold.

*Example*: `START_LONG:DPAD_RIGHT` followed by `END_LONG:DPAD_RIGHT`.

### 2. Text Input (`text:<string>`)
Send text directly into search boxes, text fields, or URL inputs on Android TV without typing letter by letter:
- `text:Stranger Things`
- `text:The Matrix`

### 3. App Launching & Deep Linking (`app:<link>` or `launch:<link>`)
Directly launch streaming applications, URLs, or specific app activities:

| Command | Action |
|---|---|
| `app:youtube` or `app:https://www.youtube.com` | Opens YouTube (`https://www.youtube.com`) |
| `app:netflix` or `app:com.netflix.ninja` | Opens Netflix (`https://www.netflix.com/title`) |
| `app:prime` or `app:com.amazon.amazonvideo.livingroom` | Opens Amazon Prime Video (`https://app.primevideo.com`) |
| `app:disney` or `app:com.disney.disneyplus` | Opens Disney+ (`https://www.disneyplus.com`) |
| `app:spotify` or `app:com.spotify.tv.android` | Opens Spotify (`spotify://`) |
| `app:plex` or `app:com.plexapp.android` | Opens Plex (`plex://`) |
| `app:kodi` or `app:org.xbmc.kodi` | Opens Kodi where `market://launch` still works (no deep link) |
| `app:vlc` or `app:org.videolan.vlc` | Opens VLC where `market://launch` still works (no deep link) |
| `app:twitch` or `app:tv.twitch.android.app` | Opens Twitch (`twitch://home`) |
| `app:https://example.com/video` | Opens Web Browser / Deep link |

#### Any Custom Package, URL, or Deep Link
You are not limited to built-in presets. Any Android package ID, intent URI, or HTTP(S) link is supported:
- **Any Android app package**: Simply use `app:<package_name>`. The presets above go out as their deep link. Any other package goes out as `market://launch?id=<package_name>`, as Home Assistant sends it (`android-app://<package_name>` is converted the same way). Links with another scheme (`https://`, `market://`, …) are sent unchanged.
  - A Google Play change broke `market://launch` for many apps: the TV answers it with an error, or opens the store page. Use the app's deep link when it has one (see the [Home Assistant Android TV Remote docs](https://www.home-assistant.io/integrations/androidtv_remote/)). Without a deep link (Kodi, for example), the app cannot be started this way on such a TV.
  - Apple TV+: `app:com.apple.atve.androidtv.appletv`
  - Max (HBO): `app:com.max.stream`
  - Hulu: `app:com.hulu.livingroomplus`
  - Peacock: `app:com.peacocktv.peacockandroid`
  - BBC iPlayer: `app:uk.co.bbc.iplayer`
- **Direct web URLs or video deep links**:
  - `app:https://www.youtube.com/watch?v=dQw4w9WgXcQ`
  - `app:https://open.spotify.com/album/4LH4d3cOWNNXdsqFd4Bw1s`

#### Adding Custom App Commands to Harmony-NG
1. Open the Harmony-NG WebUI and navigate to **Devices Studio**.
2. Select your **Android TV** device.
3. In the **Saved Commands** section, enter a descriptive Command Name (e.g., `Watch Apple TV+`).
4. In **Plugin Keycode / Action**, enter:
   ```
   PLUGIN:android-tv:app:com.apple.atve.androidtv.appletv
   ```
5. Click **Add Command**. The custom app launcher is now saved to the device!

#### Using App Launchers in Harmony Activities
You can bind any custom app launcher to a Harmony Activity (such as a "Watch Apple TV+" or "Play YouTube" activity):
1. In the WebUI, go to **Activities** -> edit or create an Activity.
2. In the **Startup Sequence / Macro**, add your Android TV device.
3. Select your saved custom app command (e.g. `Watch Apple TV+`).
4. Whenever you select the Activity on your Harmony Elite remote or the WebUI, the TV powers on, switches inputs, and automatically opens your designated app!


### 4. Connection (`Connect`, `Disconnect`)
- `Connect` opens the control session and sends no key: the next key goes out at once, without a TLS handshake first. Already connected: nothing changes.
- `Disconnect` closes the session. Nothing reconnects until the next command.

### 5. Wake-on-LAN (`PowerOn`, `WakeOnLan`)
- `PowerOn` sends the wake key when the TV answers. When it does not (switched off, or a standby that turns its network off), the plugin sends a Wake-on-LAN packet instead.
- The MAC address is the device's **MAC Address** field, else the one the plugin learned from the Hub's ARP table the last time it connected to the TV (kept in `/data/harmony/plugin-data/android-tv/mac-<ip>`). Turn the TV on once while the plugin runs, and the field can stay empty.
- `WakeOnLan` sends the packet only.
- The TV must have Wake-on-LAN (or "Turn on with Wi-Fi / network") enabled in its settings.

### 6. Sequential Channel Switching (`channel:<digits>`)
- `channel:104` — Automatically sends `1`, `0`, `4` keycodes sequentially with a 100ms inter-digit delay.

---

## Full Keycode Reference Table

All commands accept either standard friendly names, formal Android `TvKeys` / `KEYCODE_*` constants, or raw integer values:

### Navigation & D-Pad
| Friendly Name | Home Assistant Name | Android Constant | Keycode |
|---|---|---|---|
| `Up`, `DirectionUp` | `DPAD_UP` | `KEYCODE_DPAD_UP` | `19` |
| `Down`, `DirectionDown` | `DPAD_DOWN` | `KEYCODE_DPAD_DOWN` | `20` |
| `Left`, `DirectionLeft` | `DPAD_LEFT` | `KEYCODE_DPAD_LEFT` | `21` |
| `Right`, `DirectionRight` | `DPAD_RIGHT` | `KEYCODE_DPAD_RIGHT` | `22` |
| `Select`, `OK`, `Enter` | `DPAD_CENTER`, `ENTER` | `KEYCODE_DPAD_CENTER` | `23` |
| `Back`, `Return` | `BACK` | `KEYCODE_BACK` | `4` |
| `Home` | `HOME` | `KEYCODE_HOME` | `3` |
| `Menu` | `MENU` | `KEYCODE_MENU` | `82` |
| `Settings` | `SETTINGS` | `KEYCODE_SETTINGS` | `176` |
| `Search` | `SEARCH` | `KEYCODE_SEARCH` | `84` |
| `Assist`, `VoiceAssist` | `ASSIST`, `VOICE_ASSIST` | `KEYCODE_ASSIST` | `219` |
| `Info` | `INFO` | `KEYCODE_INFO` | `165` |
| `Guide` | `GUIDE` | `KEYCODE_GUIDE` | `172` |
| `Captions`, `Subtitles` | `CAPTIONS` | `KEYCODE_CAPTIONS` | `175` |
| `Notifications` | `NOTIFICATION` | `KEYCODE_NOTIFICATION` | `83` |
| `AppSwitcher` | `APP_SWITCH` | `KEYCODE_APP_SWITCH` | `187` |
| `AllApps` | `ALL_APPS` | `KEYCODE_ALL_APPS` | `284` |

### Power
| Friendly Name | Home Assistant Name | Android Constant | Keycode |
|---|---|---|---|
| `PowerToggle`, `Power` | `POWER` | `KEYCODE_POWER` | `26` |
| `PowerOn`, `Wakeup` | `WAKEUP` | `KEYCODE_WAKEUP` | `224` |
| `PowerOff`, `Sleep` | `SLEEP` | `KEYCODE_SLEEP` | `223` |
| `TvPower` | `TV_POWER` | `KEYCODE_TV_POWER` | `177` |

### Volume & Audio
| Friendly Name | Home Assistant Name | Android Constant | Keycode |
|---|---|---|---|
| `VolumeUp` | `VOLUME_UP` | `KEYCODE_VOLUME_UP` | `24` |
| `VolumeDown` | `VOLUME_DOWN` | `KEYCODE_VOLUME_DOWN` | `25` |
| `VolumeMute`, `Mute` | `VOLUME_MUTE`, `MUTE` | `KEYCODE_VOLUME_MUTE` | `164` |
| `AudioTrack` | `MEDIA_AUDIO_TRACK` | `KEYCODE_MEDIA_AUDIO_TRACK` | `222` |

### Media Playback
| Friendly Name | Home Assistant Name | Android Constant | Keycode |
|---|---|---|---|
| `Play` | `MEDIA_PLAY` | `KEYCODE_MEDIA_PLAY` | `126` |
| `Pause` | `MEDIA_PAUSE` | `KEYCODE_MEDIA_PAUSE` | `127` |
| `PlayPause` | `MEDIA_PLAY_PAUSE` | `KEYCODE_MEDIA_PLAY_PAUSE` | `85` |
| `Stop` | `MEDIA_STOP` | `KEYCODE_MEDIA_STOP` | `86` |
| `Next` | `MEDIA_NEXT` | `KEYCODE_MEDIA_NEXT` | `87` |
| `Previous`, `Prev` | `MEDIA_PREVIOUS` | `KEYCODE_MEDIA_PREVIOUS` | `88` |
| `FastForward`, `Fwd` | `MEDIA_FAST_FORWARD` | `KEYCODE_MEDIA_FAST_FORWARD` | `90` |
| `Rewind`, `Rev` | `MEDIA_REWIND` | `KEYCODE_MEDIA_REWIND` | `89` |
| `Record` | `MEDIA_RECORD` | `KEYCODE_MEDIA_RECORD` | `130` |
| `TopMenu` | `MEDIA_TOP_MENU` | `KEYCODE_MEDIA_TOP_MENU` | `226` |
| `SkipForward` | `MEDIA_SKIP_FORWARD` | `KEYCODE_MEDIA_SKIP_FORWARD` | `272` |
| `SkipBackward` | `MEDIA_SKIP_BACKWARD` | `KEYCODE_MEDIA_SKIP_BACKWARD` | `273` |

### Channels & Numbers
| Friendly Name | Home Assistant Name | Android Constant | Keycode |
|---|---|---|---|
| `0` to `9` | `0` to `9` | `KEYCODE_0` to `KEYCODE_9` | `7` to `16` |
| `ChannelUp` | `CHANNEL_UP` | `KEYCODE_CHANNEL_UP` | `166` |
| `ChannelDown` | `CHANNEL_DOWN` | `KEYCODE_CHANNEL_DOWN` | `167` |
| `LastChannel`, `PrevChannel` | `LAST_CHANNEL` | `KEYCODE_LAST_CHANNEL` | `229` |

### Color Keys
| Friendly Name | Home Assistant Name | Android Constant | Keycode |
|---|---|---|---|
| `Red` | `PROG_RED` | `KEYCODE_PROG_RED` | `183` |
| `Green` | `PROG_GREEN` | `KEYCODE_PROG_GREEN` | `184` |
| `Yellow` | `PROG_YELLOW` | `KEYCODE_PROG_YELLOW` | `185` |
| `Blue` | `PROG_BLUE` | `KEYCODE_PROG_BLUE` | `186` |

### TV Inputs
| Friendly Name | Home Assistant Name | Android Constant | Keycode |
|---|---|---|---|
| `Input`, `TvInput` | `TV_INPUT` | `KEYCODE_TV_INPUT` | `178` |
| `HDMI 1` | `TV_INPUT_HDMI_1` | `KEYCODE_TV_INPUT_HDMI_1` | `243` |
| `HDMI 2` | `TV_INPUT_HDMI_2` | `KEYCODE_TV_INPUT_HDMI_2` | `244` |
| `HDMI 3` | `TV_INPUT_HDMI_3` | `KEYCODE_TV_INPUT_HDMI_3` | `245` |
| `HDMI 4` | `TV_INPUT_HDMI_4` | `KEYCODE_TV_INPUT_HDMI_4` | `246` |
| `Composite 1` | `TV_INPUT_COMPOSITE_1` | `KEYCODE_TV_INPUT_COMPOSITE_1` | `247` |
| `Component 1` | `TV_INPUT_COMPONENT_1` | `KEYCODE_TV_INPUT_COMPONENT_1` | `249` |

### Dedicated Hardware App Buttons
| Friendly Name | Home Assistant Name | Android Constant | Keycode |
|---|---|---|---|
| `Netflix` | `VIDEO_APP_1` | `KEYCODE_VIDEO_APP_1` | `289` |
| `YouTube` | `VIDEO_APP_2` | `KEYCODE_VIDEO_APP_2` | `290` |
| `PrimeVideo`, `Prime` | `VIDEO_APP_3` | `KEYCODE_VIDEO_APP_3` | `291` |
| `Disney`, `Disney+` | `VIDEO_APP_4` | `KEYCODE_VIDEO_APP_4` | `292` |
| `Button1` to `Button4` | `BUTTON_1` to `BUTTON_4` | `KEYCODE_BUTTON_1` to `4` | `188` to `191` |

---

## Troubleshooting

### Android TV Not Responding to Pairing
If your TV does not display the 6-character PIN prompt or refuses connections:
1. On your Android TV, navigate to:
   `Settings` > `Apps` > `See all apps` > `Show system apps`.
2. Locate **Android TV Remote Service**.
3. Select **Clear Data / Clear Storage** and restart your TV.
4. Retry the pairing flow in Harmony-NG.

### Pair Again After Updating to 1.2.0
Versions before 1.2.0 used one RSA key built into the plugin and published with its source, the same for every Hub. Version 1.2.0 makes a key for this Hub, so a TV paired before must be paired again (the plugin marks it unpaired). On the TV you may also remove the old "Harmony Hub" entry in the Remote Service's paired devices.

### TV Refused After a Reset ("different key")
The plugin remembers the key of each TV on the first session after pairing and refuses a TV at that address that shows another key (the log says "different key"). A TV that was reset, or a new TV at the same address, needs pairing again.

### Commands Not Arriving / Debug Log
The plugin logs to `/tmp/plugins/android-tv.log`. Lines starting with `[-]` explain a dropped or failed command: TV unreachable, TV error for the last command (the plugin reconnects), session silent for 15 s (the plugin reconnects before the next send), key released after a lost release. Start the plugin with `ATV_DEBUG=1` in its environment to also log every key sent and every message the TV sends (off by default: the log is kept in RAM).

### Firewall & VLAN Configuration
Ensure local network firewalls permit traffic between your Harmony Hub and Android TV on:
- TCP Port `6466` (TLS Commands)
- TCP Port `6467` (TLS Pairing)