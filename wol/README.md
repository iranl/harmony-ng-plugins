# Wake-on-LAN Plugin

Turns on PCs, NAS, TVs and consoles with the Wake-on-LAN magic packet (UDP broadcast, port 9,
sent 3 times).

## Two ways to use it

**A Wake-on-LAN device.** Device Studio > Add Device > **Wake-on-LAN Device**. Enter the MAC
address, or the device's host name or IP address and press **Find MAC** (the device must be on: the Hub
reads its MAC from its ARP table). Its `PowerOn` command wakes it. Put it in an activity's
start sequence next to the device's own commands.

**Any device, no extra device.** Give any device (an IR TV, an HTTP device) a command with the
keyCode

```
PLUGIN:wol:AA:BB:CC:DD:EE:FF
PLUGIN:wol:AA:BB:CC:DD:EE:FF@192.168.1.255
```

The second form sends to that broadcast address (for a Hub with more than one network, or a
router that drops `255.255.255.255`).

## Config fields

| Field | Use |
|---|---|
| `mac` | MAC address (`AA:BB:CC:DD:EE:FF`, `AA-BB-...` or `AABBCC...`) |
| `host` | Host name or IP address, for Find MAC; also used to look up the MAC when `mac` is empty |
| `broadcast` | Broadcast address, default `255.255.255.255` |

A held remote button wakes once. The device must have Wake-on-LAN enabled (BIOS/UEFI, network
adapter, or the TV's "Turn on via Wi-Fi/LAN" setting), and most devices wake only over Ethernet.

The TV plugins (`android-tv`, `samsung-tv`, `lg-webos`, `sony-bravia`) do the same on their own
`PowerOn` when the TV does not answer: they learn its MAC while it is on.
