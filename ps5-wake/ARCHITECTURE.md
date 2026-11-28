# PlayStation 5 Bluetooth Cold-Wake Architecture & Implementation Specification

## 1. Executive Summary

Sony PlayStation 5 (PS5) consoles support waking over the network via UDP LAN broadcast **only when in Rest Mode**. When completely powered off (plugged into AC power, shut down), network adapters are unpowered, and the console can **only** be woken wirelessly via a Bluetooth baseband page request impersonating an official paired controller (Sony DualSense).

On Logitech Harmony Hub, the Bluetooth subsystem is driven by a Cambridge Silicon Radio (CSR) CSR8811 transceiver communicating over a physical UART interface (`/dev/ttyS0`) via the BCSP (BlueCore Serial Protocol) line discipline. 

This document details the reverse-engineered hardware mechanics, the baseband connection parameters required to wake the PS5 from cold power-off, and the architectural design that encapsulates this capability entirely inside the `ps5-wake` plugin with minimal impact on `hub_btstack` and zero core code bloat.

---

## 2. PS5 Baseband Wake Requirements

Waking a PlayStation 5 from cold power-off over Bluetooth is strictly guarded by the console's Southbridge baseband hardware:

1. **BD_ADDR Authentication**:
   - The PS5 Southbridge maintains a hardware whitelist of paired controller addresses in standby NVRAM.
   - Paging from an unknown address (e.g., Harmony Hub's genuine address `C8:DB:26:09:BB:85`) results in immediate hardware rejection:
     `HCI Connection Complete status: 0x0F (ERROR_CODE_CONNECTION_REJECTED_DUE_TO_UNACCEPTABLE_BD_ADDR)`.
   - Paging from the BD_ADDR of a paired DualSense controller (e.g. `24:A6:FA:A0:1C:E5`) passes hardware MAC filtering.

2. **Page Scan Repetition Mode R2 (`0x02`)**:
   - In deep standby / cold power-off, the PS5 conserves power by cycling its Bluetooth radio in **R2 mode** (listening for an 11.25 ms window every 2.56 seconds).
   - Standard host connection tools (like BlueZ `hcitool cc`) default to R1 (1.28 s) or R0, causing frequency-hopping train mismatches that time out before hitting the 11.25 ms window.
   - Paging **must** specify `Page_Scan_Repetition_Mode = 0x02` (R2).

3. **Mandatory Role Switch (`0x01`)**:
   - In the Bluetooth topology, a PlayStation console **must always be Master**, and peripherals (controllers) **must be Slave**.
   - When initiating `HCI_Create_Connection` (`0x0405`), the initiator is default Master. 
   - `Allow_Role_Switch` **must** be set to `0x01`. If set to `0x00` (role switch disallowed), the PS5 baseband drops or ignores the connection request because it cannot operate as a Slave peripheral.

4. **Link Packet Types (`0xCC18`)**:
   - Standard BR/EDR packet masks enabling DM1, DH1, DM3, DH3, DM5, and DH5 packets.

5. **Completion Handshake**:
   - As soon as the PS5 baseband acknowledges the page and link setup begins, the console powers on its main power rails and triggers boot.
   - The baseband emits `HCI_EVENT_CONNECTION_COMPLETE` (`0x03`) with status `0x00` (Success, handle `0x004d`) or status `0x13` (`Remote User Terminated Connection`). Both confirm successful wake.

---

## 3. CSR8811 Hardware Constraints & BCSP Radio Spoofing

On Atheros AR9344 / CSR8811 UART hardware, the Bluetooth controller does **not** support on-the-fly BD_ADDR modification via raw HCI vendor commands (such as Broadcom `0xFC01` or standard HCI `0xFC00`). Over UART BCSP, runtime HCI commands to change MAC are either rejected or silently ignored.

### Proven Working Spoof Sequence:
1. **BCSP Persistent Store (.psr) Patching**:
   - The CSR8811 stores its active MAC in PSKey `&0x0001` (NAP, UAP, LAP).
   - BlueZ / CSR word mapping:
     `&0x0001 = 0x(mac[3]) 0x(mac[4])(mac[5]) 0x(mac[2]) 0x(mac[0])(mac[1])`
     *Example (`24:A6:FA:A0:1C:E5`)*: `&0x0001 = 0xa0 0x1ce5 0xfa 0x24a6`
2. **Warm Reset via HCI**:
   - `/usr/sbin/bccmd -t hci -d hci0 psload -r <psr>` writes the whole PS key file (patch bundle included) over the attached `hci0` and warm-resets the CSR8811, in about 0.3 seconds.
   - Always the whole file: a reload of `&0x0001` alone left the chip unreachable until a reboot.
   - With no `hci0` the helper falls back to `bccmd -t bcsp -d /dev/ttyS0 -b 115200` on the bare UART (after `killall -9 hciattach`). That path takes 3-5 seconds and on Linux 6.18 often failed, leaving the chip unreachable until a reboot.
3. **Re-attaching Kernel BCSP**:
   - The warm reset ends the kernel's BCSP link ("card has reset"): `killall -9 hciattach`, then `/usr/sbin/hciattach ttyS0 bcsp` starts a new one and returns once `hci0` exists.
   - `hciconfig hci0 up && hciconfig hci0 sspmode 1` brings up `hci0` with the spoofed MAC actively transmitting on air.
4. **Restoration**:
   - The same steps with `/tmp/pskeys.psr` restore the Hub's own MAC address; the supervisor then starts `hub_btstack` again.

---

## 4. Minimal-Core Architecture

To respect core maintainability rules:
- **No PS5-specific logic in Harmony core (`hub_btstack` or `hub_daemon`)**.
- **No radio work in the resident daemon**: the small `ps5_wake` daemon only receives commands; the spoof sequence runs in a one-off helper process.
- **Minimal interruption to standard Bluetooth functioning**.

### Core Changes: Exactly 1 Generic Lock Check
In `payload/scripts/init.sh` (the Hub background supervisor loop):
```sh
# If locked by an exclusive radio task (e.g. ps5-wake spoof sequence), pause supervisor
if [ -f /tmp/hub_btstack.lock ]; then
  sleep 2
  continue
fi
```
**Why this is needed**:
When `hub_btstack` is stopped to allow `bccmd` exclusive UART access to `/dev/ttyS0`, the `init.sh` supervisor would normally detect the missing process within 5 seconds and restart it, issuing an `HCI_Reset` that aborts outgoing paging.
The `/tmp/hub_btstack.lock` flag tells the supervisor to pause gracefully while an exclusive radio task runs.

### Plugin Boundaries (`ps5-wake`):
1. `hub_daemon` routes activity or button commands generically to `/tmp/plugins/ps5-wake.sock`.
2. `ps5_wake` plugin receives:
   ```json
   {
     "command": "wake",
     "host_mac": "84:E6:57:C2:0D:97",
     "controller_mac": "24:A6:FA:A0:1C:E5"
   }
   ```
3. If `controller_mac` is omitted:
   - `ps5_wake` dispatches a standard `bt_page <host_mac>` request to `hub_btstack.sock` (paging from the Hub's own address, without spoofing).
4. If `controller_mac` is present:
   - `ps5_wake` executes the isolated helper `bin/ps5_wake_helper.sh` to perform the BCSP spoof -> page -> restore cycle.
5. Both addresses must have the form `AA:BB:CC:DD:EE:FF`: they go into the helper's command line, so anything else is refused.
6. Only `Wake`/`PowerOn` on `tap` or `press` start a wake; `repeat` and `release` are ignored.

### Radio lock
The helper creates `/tmp/hub_btstack.lock` with noclobber (`set -C`), so creation fails when the file exists. One file does both jobs:
- a second wake (or any other exclusive radio task) is refused while one runs;
- the `init.sh` supervisor, which tests `[ -f /tmp/hub_btstack.lock ]`, does not restart `hub_btstack`.

An `EXIT` trap restores the Hub's PS keys, re-attaches `hci0` and removes the lock on every exit after the lock is taken, including a failed `bccmd` step and `SIGTERM`. The helper does not start `hub_btstack` itself: the supervisor does once the lock is gone. A `SIGKILL` of the helper skips the trap and leaves the lock: remove `/tmp/hub_btstack.lock` by hand or reboot.

### Note: `bt_page` spoof argument in `hub_btstack`
`hub_btstack` still accepts `bt_page <target> <spoof_bdaddr>` (`payload/source/hub_btstack/bt_cmd.c`), which changes the BD_ADDR with a runtime vendor command. Section 3 found that runtime changes are ignored or rejected over BCSP on the CSR8811, so this plugin never passes the spoof argument and uses the PS key reload instead.

---

## 5. Sequence Diagram

```mermaid
sequenceDiagram
    autonumber
    actor User as Remote / WebUI
    participant Daemon as hub_daemon
    participant Plugin as ps5_wake (Plugin)
    participant Supervisor as init.sh (Supervisor)
    participant Radio as CSR8811 (Radio)
    participant PS5 as PlayStation 5

    User->>Daemon: Trigger PS5 Activity
    Daemon->>Plugin: Forward JSON via /tmp/plugins/ps5-wake.sock
    Note over Plugin: Detects controller_mac is specified
    Plugin->>Plugin: Create /tmp/hub_btstack.lock (noclobber)
    Plugin->>Supervisor: killall -9 hub_btstack
    Note over Supervisor: Sees lockfile, pauses respawn loop
    Plugin->>Radio: bccmd -t hci psload -r /tmp/ps5_spoof.psr
    Note over Radio: Reboots with DualSense MAC (24:A6:FA:A0:1C:E5)
    Plugin->>Radio: killall -9 hciattach; hciattach ttyS0 bcsp && hciconfig hci0 up
    Plugin->>Radio: HCI_Create_Connection (psrm=R2, role=Slave, target=PS5)
    Radio->>PS5: BR/EDR Baseband Page (Whitelisted DualSense MAC)
    PS5-->>Radio: Page Response & ACL Handshake
    PS5->>PS5: POWER ON (Cold Wake Complete!)
    Radio-->>Plugin: HCI_EVENT_CONNECTION_COMPLETE (Status 0x00, handle 0x004d)
    Plugin->>Radio: bccmd -t hci psload -r /tmp/pskeys.psr
    Note over Radio: Reboots with genuine Hub MAC (C8:DB:26:09:BB:85)
    Plugin->>Radio: killall -9 hciattach; hciattach ttyS0 bcsp && hciconfig hci0 up
    Plugin->>Plugin: rm -f /tmp/hub_btstack.lock
    Supervisor->>Supervisor: restarts /data/harmony/bin/hub_btstack
    Note over Supervisor: Resumes normal supervision
```

---

## 6. Impact on Harmony Hub System Functioning

1. **RF Remote Unaffected**:
   - Harmony Elite and Smart Control remotes operate over the **CC2544 RF transceiver** on SPI (`/dev/rfspi`) handled by `hub_rf`.
   - The CSR8811 UART reload does **not** touch SPI, nor does it affect remote button responsiveness.
2. **Transient BTstack Pause**:
   - `hub_btstack` is stopped for the length of the helper run (about 7 seconds plus the page, which takes up to about 10 seconds when it times out), plus the supervisor's restart delay (5 seconds).
   - Once restored, BTstack reconnects to any active peripheral hosts via `start_host_reconnect()`.
3. **Fail-Safe Restoration**:
   - The helper's `EXIT` trap restores the genuine address after any failed step. If the helper is killed with `SIGKILL`, `/tmp/pskeys.psr` is preserved on `/tmp`, and the boot sequence (`/etc/init.d/rcS.local`) automatically restores genuine firmware keys on any warm or cold reboot.
