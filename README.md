# ESP32-P4 GoPro USB CDC-NCM Webcam Live Streaming

ligne 300 (en francais)

---

# 🇬🇧 English

This project allows a **Waveshare ESP32-P4-Module-DEV-KIT** board to automatically control a **GoPro** through **USB CDC-NCM** and forward the GoPro webcam video stream to a computer over the integrated **RJ45 Ethernet** port.

The ESP32-P4 works as a **USB Host**. The GoPro exposes a CDC-NCM network interface over USB. The ESP32-P4 communicates with the GoPro HTTP API through this USB network, starts the webcam mode, receives the UDP video stream, and forwards it to the PC.

---

## General Operation

```text
GoPro
  → USB CDC-NCM network
  → ESP32-P4 USB Host
  → ESP32-P4 internal RJ45 Ethernet
  → PC
  → VLC / video player
```

Simple summary:

```text
USB CDC-NCM = control + video network between GoPro and ESP32-P4
HTTP API    = start / stop GoPro webcam mode
UDP 5000    = video stream from GoPro to ESP32-P4
UDP 5001    = forwarded stream from ESP32-P4 to PC
RJ45        = output network to the PC
```

The video stream does not go through BLE or Wi-Fi.
Everything is done through USB and Ethernet.

---

## Important Network Addresses

### GoPro USB network

The GoPro USB network uses fixed addresses:

```text
ESP32-P4 USB IP : 172.20.140.50
GoPro USB IP    : 172.20.140.51
GoPro HTTP port : 8080
```

The ESP32-P4 sends HTTP requests to:

```text
http://172.20.140.51:8080
```

Important webcam commands:

```text
/gopro/webcam/start?res=7&fov=0&port=5000
/gopro/webcam/stop
/gopro/webcam/exit
```

---

### PC video address

The PC receives the forwarded video stream from the ESP32-P4.

Current PC IPv4 address:

```text
10.5.159.148
```

This address is configured in `main.c`:

```c
#define PC_STREAM_IP "10.5.159.148"
#define PC_STREAM_PORT 5001
```

If the PC IPv4 address changes, update `PC_STREAM_IP`.

---

### How to check the PC IPv4 address

On Windows PowerShell or CMD:

```powershell
ipconfig
```

Look for:

```text
IPv4 Address: 10.5.159.148
```

If the address is different, update:

```c
#define PC_STREAM_IP "NEW_PC_IP"
```

Then rebuild and flash:

```powershell
idf.py build
idf.py -p COMx flash monitor
```

---

## Address Table

| Element | Address / value | Role |
| ------ | --------------- | ---- |
| ESP32-P4 USB | `172.20.140.50` | ESP32-P4 address on GoPro USB network |
| GoPro USB | `172.20.140.51` | GoPro address on USB CDC-NCM network |
| GoPro HTTP port | `8080` | GoPro HTTP API port |
| GoPro webcam UDP | `5000` | UDP stream received by ESP32-P4 |
| PC UDP port | `5001` | UDP stream received by VLC on PC |
| PC IP | `10.5.159.148` | Computer receiving the video stream |
| ESP32-P4 Ethernet | DHCP | RJ45 network address from the lab network |
| Ethernet PHY | IP101GRI / P101GRI | Integrated RJ45 PHY on ESP32-P4 board |

---

## Network Diagram

```text
GoPro
USB IP: 172.20.140.51
HTTP  : 172.20.140.51:8080
UDP   : sends video to port 5000
        │
        │ USB CDC-NCM
        ▼
ESP32-P4 USB Host
USB IP: 172.20.140.50
        │
        │ UDP receive on port 5000
        │ HTTP control to GoPro
        ▼
ESP32-P4 RJ45 Ethernet
DHCP IP from lab network
        │
        │ UDP forward to 10.5.159.148:5001
        ▼
PC
IP   : 10.5.159.148
VLC  : udp://@:5001
```

---

## Why this version works

The main difficulty is not ARP or Ethernet.
The GoPro can be detected over USB, the CDC-NCM network can connect, and ARP can reply correctly.

The real problem appears after a GoPro reboot or USB reconnection:

```text
USB CDC-NCM connected
ARP OK
but webcam/start can return error 4
```

This means the USB network is alive, but the internal GoPro webcam service is not ready or is blocked.

The stable solution is therefore not only to retry `webcam/start`.
The firmware performs a stronger recovery sequence.

---

## Stable Recovery Sequence

When the live has already worked once and the GoPro reconnects over USB, the firmware avoids the useless first `START` attempt.

Instead, it directly runs the recovery sequence:

```text
1. /gopro/webcam/stop
2. /gopro/webcam/exit
3. Labs !OR HTTP stabilization step
4. local USB session cleanup
5. root port USB OFF for 5 seconds
6. root port USB ON request
7. voluntary ESP32-P4 reboot with esp_restart()
8. full USB enumeration again
9. CDC-NCM connection
10. ARP reply
11. webcam/start
12. LIVE OK
```

Important detail:

```text
The Labs !OR request can be refused by the GoPro HTTP endpoint.
It is still kept because tests showed that the version without this step did not reconnect CDC-NCM reliably.
```

So the `!OR` step is treated as a stabilization / timing step.
The real recovery is done by the root-port USB reset and the ESP32-P4 reboot.

---

## Why the direct START after reconnection was removed

Before this version, after USB reconnection the firmware did:

```text
USB reconnect
ARP OK
wait 30 s
webcam/start
error 4
recovery
```

This was not optimal because the first `START` was known to fail after a GoPro reboot.

The new behavior is:

```text
USB reconnect after previous live
ARP OK
no useless START attempt
stable recovery directly
ESP32-P4 reboot
clean START after reboot
```

This avoids wasting time and makes the recovery clearer.

---

## Why `!WRESET` was not kept

A previous test used:

```text
/gopro/qrcode?labs=1&code=%21WRESET
```

The GoPro HTTP endpoint answered that the command was not recognized.

So this solution was not reliable for automatic recovery.

The final version uses:

```text
/gopro/qrcode?labs=1&code=%21OR
```

Even if the endpoint can refuse it, the tested sequence with `!OR` followed by root-port recovery works better than the version without it.

---

## Hardware

* Waveshare ESP32-P4-Module-DEV-KIT
* GoPro with USB webcam / Open GoPro support
* USB cable between GoPro and ESP32-P4 USB Host port
* RJ45 Ethernet cable
* PC with VLC
* Stable power supply

---

## ESP32-P4 USB Host

The ESP32-P4 must be configured as USB Host.

The GoPro is detected with:

```text
VID = 0x2672
PID = 0x0059
```

The useful USB interfaces are:

```text
Interface 0 : CDC-NCM control
Interface 1 : CDC-DATA alt setting 1
```

The useful endpoints are:

```text
Notification endpoint : 0x82
Bulk IN endpoint      : 0x81
Bulk OUT endpoint     : 0x01
```

The firmware configures CDC-NCM with:

```text
GET_NTB_PARAMETERS
SET_NTB_FORMAT NTB-16
SET_NTB_INPUT_SIZE 16384
SET_ETHERNET_PACKET_FILTER
SET_INTERFACE interface 1 alt 1
```

---

## Ethernet RJ45

The ESP32-P4 uses the integrated RJ45 Ethernet interface.

PHY:

```text
IP101GRI / P101GRI
```

The firmware uses DHCP:

```text
ESP32-P4 Ethernet IP = assigned by the network
```

The PC and ESP32-P4 must be reachable on the same network.

---

## ESP-IDF Configuration

ESP-IDF version:

```text
ESP-IDF 5.5.2
```

Target:

```powershell
idf.py set-target esp32p4
```

---

## Menuconfig

Run:

```powershell
idf.py menuconfig
```

### Serial flasher config

```text
Flash size = 16 MB
Flash mode = DIO
Flash frequency = 80 MHz
```

### Ethernet

Enable Ethernet support for the internal ESP32-P4 Ethernet MAC and IP101 PHY.

### USB Host

USB Host must be enabled.

The project uses the ESP-IDF USB Host API:

```c
#include "usb/usb_host.h"
```

The recovery also uses:

```c
usb_host_lib_set_root_port_power(false);
usb_host_lib_set_root_port_power(true);
```

---

## Build

```powershell
idf.py fullclean
idf.py build
```

---

## Flash

Replace `COMx` with the serial port of the board:

```powershell
idf.py -p COMx flash monitor
```

Example:

```powershell
idf.py -p COM5 flash monitor
```

Exit monitor:

```text
Ctrl + ]
```

---

## VLC

On the PC, open VLC with:

```text
udp://@:5001
```

The ESP32-P4 receives the GoPro UDP stream on port `5000` and forwards it to the PC on port `5001`.

---

## Expected Logs

USB detection:

```text
[USB] Nouveau périphérique détecté
[USB] VID=0x2672 PID=0x0059
[USB] GoPro détectée
[USB] Interface CDC-NCM détectée
[USB] Tous les endpoints utiles sont prêts
```

CDC-NCM connection:

```text
[USB] Type: NETWORK_CONNECTION
[USB] Réseau CDC-NCM connecté
[USB-P4] CDC-NCM GoPro connecté
```

ARP:

```text
[NET] ARP reply reçue
[NET] IP GoPro = 172.20.140.51
```

First live start:

```text
[LIVE] ARP OK premier boot -> attente 10 s avant START webcam
[WEBCAM] Commande START acceptée
[WEBCAM] LIVE OK : premier paquet UDP reçu
```

Recovery after GoPro reboot:

```text
[LIVE] ARP OK apres reconnexion USB post-live -> pas de START inutile, recovery forte directe
[RECOVERY] Début recovery STABLE
[RECOVERY] Etape de stabilisation GoPro: appel Labs !OR via USB HTTP
[RECOVERY] Reponse Labs !OR refusee/ignoree -> suite normale: reset root-port USB
[RECOVERY] Root port USB OFF pendant 5 s
[USB] Périphérique USB débranché
[RECOVERY] Root port USB ON demande puis reboot volontaire
[RECOVERY] esp_restart volontaire apres sequence OFF/ON
```

After reboot:

```text
[USB] Nouveau périphérique détecté
[USB] Réseau CDC-NCM connecté
[NET] ARP reply reçue
[WEBCAM] Commande START acceptée
[WEBCAM] LIVE OK
```

---

## Frequent Problems

### `webcam/start` returns error 4

Meaning:

```text
The GoPro USB network is alive, but the internal webcam service is blocked or not ready.
```

Solution used by this firmware:

```text
stable recovery sequence with stop/exit + !OR stabilization + root-port OFF/ON + esp_restart
```

---

### CDC-NCM stays disconnected

Log example:

```text
[USB] wValue=0x0000
[USB] Réseau CDC-NCM non connecté
```

Possible causes:

* GoPro not ready after USB power cycle
* USB root-port state not fully reset
* recovery sequence changed too much

The tested version must keep the `!OR` stabilization step because the version without it was less reliable.

---

### VLC receives no video

Check:

* PC IP in `PC_STREAM_IP`
* VLC opened on `udp://@:5001`
* PC firewall
* Ethernet link
* ESP32-P4 got an Ethernet IP
* GoPro START accepted

---

## Current Project Status

Observed behavior with this stable version:

* first boot live start works;
* GoPro USB CDC-NCM detection works;
* ARP with GoPro works;
* webcam START works after clean boot;
* UDP stream is received on port 5000;
* UDP stream is forwarded to PC port 5001;
* recovery after GoPro USB reconnection works;
* direct useless START after post-live reconnection is avoided;
* root-port USB OFF/ON + ESP32-P4 reboot provides the clean recovery.

---

## Git Commands

```powershell
git init
git add .
git commit -m "Stable ESP32-P4 GoPro USB CDC-NCM webcam live streaming"
git branch -M main
git remote add origin https://github.com/USERNAME/REPOSITORY_NAME.git
git push -u origin main
```
