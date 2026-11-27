# Roku ECP Plugin for Harmony-NG

Native C daemon for local Wi-Fi control of Roku streaming devices (Roku Streaming Stick, Roku Ultra, Roku Express) and Roku TVs (TCL, Hisense, Sharp, etc.) via the official **External Control Protocol (ECP)**.

## Features
- **Zero Cloud Dependency**: Operates entirely over the local network via HTTP port `8060`.
- **Ultra Low Latency**: Native C socket client executes keypresses in under 10 ms.
- **Bi-Directional Action Support**: Supports tap (`/keypress`), press/hold (`/keydown`), and release (`/keyup`). A held key with no repeat or release for 600 ms is released.
- **Apps and Text**: `Launch:<app id>` (or `App:`) and `Text:<text>` act once per button press.
- **Complete Command Catalog**: Pre-configured with navigation, media playback, volume, power, and app navigation commands.

## Requirements & Roku Setup
1. Connect your Roku device to the same local network / Wi-Fi subnet as the Harmony Hub.
2. Ensure **Control by mobile apps** is enabled on your Roku:
   - Go to **Settings** -> **System** -> **Advanced system settings** -> **Control by mobile apps** -> Set to **Network access: Enabled** (or **Default**).
3. Check your Roku IP address:
   - Go to **Settings** -> **Network** -> **About** (e.g. `192.168.1.50`).

## Configuration Fields
| Field | Type | Default | Description |
|---|---|---|---|
| `ip_address` | String | - | Host name or IP address of the Roku player or TV (required). |
| `port` | Number | `8060` | Roku ECP port (standard: 8060). |

## Available Commands
- **Navigation**: `Home`, `Back`, `Select`, `DirectionUp`, `DirectionDown`, `DirectionLeft`, `DirectionRight`
- **Playback**: `Play`, `Rev`, `Fwd`, `InstantReplay`
- **TV / Audio**: `VolumeUp`, `VolumeDown`, `VolumeMute`, `PowerOn`, `PowerOff`
