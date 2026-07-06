# ESP32-P4 GoPro USB Webcam Streaming

## 🇫🇷 Français

Ce projet permet à une carte **ESP32-P4-Module-DEV-KIT** de communiquer avec une **GoPro** directement en **USB Host**.

Contrairement au projet BLE/Wi-Fi, cette version n’utilise pas le Wi-Fi de la GoPro pour transporter le flux vidéo.  
La GoPro est reliée à l’ESP32-P4 par câble USB-C, et l’ESP32-P4 agit comme hôte USB.

La GoPro expose une interface réseau **USB CDC-NCM**.  
L’ESP32-P4 crée alors une interface réseau USB locale, envoie les commandes HTTP OpenGoPro à la caméra, démarre le mode webcam, reçoit le flux vidéo UDP, puis le retransmet vers un PC par Ethernet.

---

## 🧠 Fonctionnement global

```text
GoPro
  │
  │ USB-C
  │ - détection USB
  │ - configuration CDC-NCM
  │ - réseau USB 172.20.140.x
  │ - commandes HTTP OpenGoPro
  │ - flux webcam UDP
  │
ESP32-P4
  │
  │ Ethernet RJ45 interne
  │ - transfert UDP vers le PC
  │
PC
  │
  │ VLC
  │ - reçoit le flux vidéo UDP
```

Résumé :

```text
USB CDC-NCM = communication réseau entre ESP32-P4 et GoPro
HTTP USB = commandes OpenGoPro envoyées à la caméra
UDP USB = flux webcam reçu depuis la GoPro
Ethernet RJ45 = transfert du flux vers le PC
VLC = affichage du live vidéo
```

Le flux vidéo ne passe pas par le BLE.  
Le flux vidéo ne passe pas par le Wi-Fi.  
Le projet est basé sur l’USB.

---

## 🎥 Chemin du flux vidéo

```text
GoPro
  → USB CDC-NCM
  → ESP32-P4
  → Ethernet RJ45
  → PC
  → VLC
```

---

## 🌐 Partie importante : adresses réseau

Ce projet utilise deux réseaux différents :

```text
1. Réseau USB entre ESP32-P4 et GoPro
2. Réseau Ethernet entre ESP32-P4 et PC
```

---

### 1. Réseau USB GoPro

L’ESP32-P4 crée une interface réseau USB locale :

```text
ESP32-P4 côté USB : 172.20.140.50
GoPro côté USB    : 172.20.140.51
```

L’API HTTP de la GoPro est accessible à cette adresse :

```text
http://172.20.140.51:8080
```

Commandes utilisées :

```text
/gopro/webcam/start
/gopro/webcam/stop
/gopro/webcam/exit
/gopro/webcam/status
/gopro/camera/control/wired_usb
```

---

### 2. Adresse IPv4 du PC

Le PC reçoit le flux vidéo dans VLC.

Adresse IPv4 actuelle du PC :

```text
10.5.159.150
```

Dans le code, cette adresse est définie ici :

```c
#define PC_STREAM_IP "10.5.159.150"
```

Si l’adresse IPv4 du PC change, il faut modifier cette ligne.

---

### 3. Comment vérifier l’adresse IPv4 du PC

Sur Windows, ouvrir PowerShell ou CMD :

```powershell
ipconfig
```

Chercher la ligne de la carte Wi-Fi ou Ethernet utilisée :

```text
Adresse IPv4. . . . . . . . . . . . . .: 10.5.159.150
```

Si l’adresse affichée est différente, par exemple :

```text
10.5.159.144
```

alors il faut modifier dans le code :

```c
#define PC_STREAM_IP "10.5.159.144"
```

Puis recompiler et reflasher :

```powershell
idf.py build
idf.py -p COMx flash monitor
```

---

### 4. Ports utilisés

| Élément | Port | Rôle |
|---|---:|---|
| GoPro HTTP API | `8080` | Commandes OpenGoPro |
| Flux webcam GoPro vers ESP32-P4 | `5000` | UDP reçu par l’ESP32-P4 |
| Flux ESP32-P4 vers PC | `5001` | UDP reçu par VLC |

---

## 📌 Tableau récapitulatif des adresses

| Élément | Adresse / valeur | Rôle |
|---|---|---|
| ESP32-P4 USB | `172.20.140.50` | Interface USB côté ESP32-P4 |
| GoPro USB | `172.20.140.51` | Interface USB côté GoPro |
| GoPro HTTP | `172.20.140.51:8080` | API HTTP OpenGoPro par USB |
| Port UDP GoPro | `5000` | Flux webcam reçu par l’ESP32-P4 |
| PC VLC | `10.5.159.150` | Ordinateur qui reçoit le flux |
| Port UDP VLC | `5001` | Port écouté par VLC |
| VLC URL | `udp://@:5001` | Adresse à ouvrir dans VLC |

---

## 🔁 Schéma réseau complet

```text
GoPro
USB IP : 172.20.140.51
Port HTTP : 8080
UDP webcam : 5000
        │
        │ USB CDC-NCM
        ▼
ESP32-P4
USB IP : 172.20.140.50
        │
        │ Réception UDP 5000
        │ Forward UDP
        ▼
Ethernet RJ45 ESP32-P4
        │
        │ UDP vers PC
        ▼
PC
IP : 10.5.159.150
Port : 5001
VLC : udp://@:5001
```

---

## ⚙️ Pourquoi le lancement peut prendre du temps

Le lancement peut prendre plusieurs secondes car le firmware attend que toute la chaîne USB soit prête :

```text
1. Détection USB de la GoPro
2. Ouverture du périphérique USB
3. Lecture des descripteurs USB
4. Détection de l’interface CDC-NCM
5. Claim des interfaces USB
6. Passage de l’interface data en alternate setting 1
7. Lecture des paramètres NTB
8. Sélection du format NTB-16
9. Configuration de la taille NTB
10. Activation du filtre Ethernet CDC-NCM
11. Attente de NETWORK_CONNECTION = 1
12. Création de l’interface lwIP USB
13. Envoi ARP vers la GoPro
14. Réception ARP reply
15. Ouverture socket UDP 5000
16. Préparation du forwarding Ethernet vers le PC
17. Envoi de /gopro/webcam/start
18. Réception du premier paquet UDP vidéo
19. Forward vers VLC
```

Ce temps d’attente est normal.  
Il permet d’éviter de lancer le live trop tôt, avant que la GoPro et le réseau USB soient prêts.

---

## ✅ État actuel du projet

Comportement observé :

* détection USB de la GoPro fonctionnelle ;
* configuration CDC-NCM fonctionnelle ;
* endpoint bulk IN et bulk OUT fonctionnels ;
* ARP vers la GoPro fonctionnel ;
* communication HTTP avec la GoPro fonctionnelle ;
* premier lancement webcam fonctionnel ;
* réception UDP du flux GoPro fonctionnelle ;
* transfert UDP vers VLC fonctionnel ;
* Ethernet RJ45 interne ESP32-P4 fonctionnel.

---

## ⚠️ Limitation actuelle après reboot GoPro

Après un redémarrage de la GoPro, le lien USB revient correctement :

```text
CDC-NCM connecté
ARP reply reçue
IP GoPro = 172.20.140.51
HTTP API joignable
```

Les commandes suivantes répondent correctement :

```text
/gopro/camera/control/wired_usb?p=0
/gopro/webcam/exit
/gopro/webcam/stop
/gopro/webcam/status
```

Exemple observé :

```json
{
    "status": 0,
    "error": 0
}
```

Mais la commande de démarrage webcam peut ensuite retourner :

```text
HTTP/1.1 500 Internal Server Error
```

avec :

```json
{
    "status": 1,
    "error": 4
}
```

Interprétation :

```text
Le transport USB fonctionne.
L’ARP fonctionne.
L’HTTP fonctionne.
La GoPro répond aux commandes.
Mais le service webcam interne de la GoPro reste bloqué après reboot.
```

Un reset GoPro Labs `!WRESET` par QR physique débloque cet état, mais cette commande n’est pas reconnue lorsqu’elle est envoyée via l’API HTTP USB actuelle.

Conclusion :

```text
Le projet USB fonctionne pour le premier lancement live.
La limitation concerne la récupération automatique après reboot caméra.
```

---

## 📦 Matériel utilisé

* ESP32-P4-Module-DEV-KIT
* GoPro compatible OpenGoPro / webcam USB
* Câble USB-C data
* Câble Ethernet
* PC avec VLC
* Alimentation stable

---

## 🔌 Connexions

### USB

```text
GoPro USB-C
  ↔ câble USB-C data
  ↔ port USB Host ESP32-P4
```

Le câble doit être un vrai câble data.  
Un câble charge uniquement ne fonctionne pas.

---

### Ethernet

```text
ESP32-P4 RJ45
  ↔ réseau local / switch / PC
```

Le PC doit pouvoir recevoir les paquets UDP envoyés par l’ESP32-P4.

---

## 🧩 Ethernet ESP32-P4

Cette version utilise l’Ethernet RJ45 interne de la carte ESP32-P4-Module-DEV-KIT.

Il ne s’agit pas du module W5500.

Pins utilisés dans le code :

| Fonction | GPIO |
|---|---:|
| MDC | GPIO 31 |
| MDIO | GPIO 52 |
| PHY reset | GPIO 51 |
| RMII TX EN | GPIO 49 |
| RMII TXD0 | GPIO 34 |
| RMII TXD1 | GPIO 35 |
| RMII CRS DV | GPIO 28 |
| RMII RXD0 | GPIO 29 |
| RMII RXD1 | GPIO 30 |
| RMII CLK | GPIO 50 |

---

## 🛠️ Configuration ESP-IDF

Version utilisée :

```text
ESP-IDF 5.5.2
```

Target :

```powershell
idf.py set-target esp32p4
```

---

## ⚙️ Menuconfig

Lancer :

```powershell
idf.py menuconfig
```

### 1. Serial flasher config

Vérifier :

```text
Flash size adaptée à la carte
Flash mode compatible avec la carte
Flash frequency compatible avec la carte
```

Si la carte boote mal, tester une fréquence plus basse.

---

### 2. USB Host

Le projet utilise l’ESP32-P4 comme hôte USB.

Vérifier que les composants USB Host sont activés dans l’ESP-IDF.

---

### 3. Ethernet

L’Ethernet interne ESP32-P4 doit être activé.

Le code utilise le PHY IP101 avec l’interface RMII.

---

## 🚀 Compilation

Depuis le dossier du projet :

```powershell
idf.py set-target esp32p4
idf.py fullclean
idf.py build
```

---

## ⚡ Flash

Remplacer `COMx` par le port série de la carte :

```powershell
idf.py -p COMx flash monitor
```

Exemple :

```powershell
idf.py -p COM5 flash monitor
```

Quitter le monitor :

```text
Ctrl + ]
```

---

## 🎥 Utilisation du live USB

1. Vérifier l’adresse IPv4 du PC avec `ipconfig`.
2. Modifier `PC_STREAM_IP` dans le code si l’adresse du PC a changé.
3. Ouvrir VLC sur le PC.
4. Dans VLC, ouvrir un flux réseau :

```text
udp://@:5001
```

5. Brancher la GoPro en USB sur l’ESP32-P4.
6. Brancher l’Ethernet de l’ESP32-P4.
7. Flasher ou redémarrer l’ESP32-P4.
8. Attendre la détection USB de la GoPro.
9. Attendre l’ARP GoPro.
10. Le live doit démarrer automatiquement.

---

## ✅ Logs attendus

Détection USB :

```text
[USB] GoPro détectée
[USB] VID=0x2672 PID=0x0059
[USB-P4] CDC-NCM GoPro connecté
```

Configuration CDC-NCM :

```text
[USB] SET_NTB_FORMAT OK : format NTB-16 sélectionné
[USB] SET_NTB_INPUT_SIZE OK
[USB] SET_ETHERNET_PACKET_FILTER OK
[USB] Type: NETWORK_CONNECTION
[USB] Réseau CDC-NCM connecté
```

ARP :

```text
[NET] ARP reply reçue
[NET] MAC GoPro = 06:57:47:84:92:EC
[NET] IP GoPro = 172.20.140.51
```

Démarrage live :

```text
[LIVE] Conditions OK : démarrage webcam GoPro
[WEBCAM] Écoute UDP prête sur 0.0.0.0:5000
[FORWARD] Destination PC : 10.5.159.150:5001
[WEBCAM] START webcam simple : res=7
[WEBCAM] Commande START acceptée
[WEBCAM] LIVE OK : premier paquet UDP reçu
```

VLC :

```text
udp://@:5001
```

---

## ⚠️ Problèmes fréquents

### La GoPro n’est pas détectée en USB

Vérifier :

* câble USB-C data ;
* GoPro allumée ;
* port USB Host utilisé sur l’ESP32-P4 ;
* alimentation suffisante ;
* mode USB Host correctement activé côté carte.

---

### Le réseau CDC-NCM reste non connecté

Symptôme :

```text
NETWORK_CONNECTION
wValue=0x0000
Réseau CDC-NCM non connecté
```

Causes possibles :

* la GoPro n’a pas encore activé son mode USB data ;
* câble USB non data ;
* GoPro pas prête ;
* timing de démarrage trop court ;
* problème d’alimentation VBUS 5 V.

Attendre une deuxième notification :

```text
wValue=0x0001
Réseau CDC-NCM connecté
```

---

### ARP ne répond pas

Symptôme :

```text
ARP envoyé mais pas de réponse
```

Vérifier :

* CDC-NCM connecté ;
* endpoints bulk IN/OUT actifs ;
* interface lwIP USB créée ;
* IP ESP32-P4 USB = `172.20.140.50` ;
* IP GoPro USB attendue = `172.20.140.51`.

---

### VLC ne reçoit rien

Vérifier :

* VLC écoute bien :

```text
udp://@:5001
```

* l’adresse du PC dans le code est correcte :

```c
#define PC_STREAM_IP "10.5.159.150"
```

* le firewall Windows ne bloque pas VLC ;
* l’ESP32-P4 a bien une IP Ethernet ;
* le PC est sur le bon réseau.

---

### Le premier live fonctionne mais pas après reboot GoPro

Symptôme :

```text
/gopro/webcam/start
HTTP/1.1 500 Internal Server Error
{
    "status": 1,
    "error": 4
}
```

Ce cas est observé après reboot de la GoPro.

Le lien USB est pourtant revenu :

```text
CDC-NCM connecté
ARP OK
HTTP OK
```

Cela indique probablement un état interne webcam bloqué côté GoPro après reboot.

Le projet documente cette limitation.

---

## 🧪 Commandes utiles

Vérifier le PC :

```powershell
ipconfig
```

Compiler :

```powershell
idf.py build
```

Flasher :

```powershell
idf.py -p COMx flash monitor
```

Nettoyer le build :

```powershell
idf.py fullclean
```

---

## 🧾 Git Commands

```powershell
git init
git add .
git commit -m "Initial ESP32-P4 GoPro USB webcam streaming project"
git branch -M main
git remote add origin https://github.com/USERNAME/REPOSITORY_NAME.git
git push -u origin main
```

---

# 🇬🇧 English

This project uses an **ESP32-P4-Module-DEV-KIT** as a USB Host to communicate with a **GoPro** camera over **USB CDC-NCM**.

The ESP32-P4 sends OpenGoPro HTTP commands to the camera over USB, starts the webcam mode, receives the UDP MPEG-TS stream, and forwards it to a PC over Ethernet.

---

## General Operation

```text
GoPro
  → USB CDC-NCM
  → ESP32-P4
  → Ethernet RJ45
  → PC
  → VLC
```

Simple summary:

```text
USB CDC-NCM = network link between ESP32-P4 and GoPro
HTTP over USB = OpenGoPro camera commands
UDP over USB = webcam video stream from the GoPro
Ethernet = forwarding to the PC
VLC = video receiver
```

---

## Important Network Addresses

### USB network

| Device | IP |
|---|---|
| ESP32-P4 USB interface | `172.20.140.50` |
| GoPro USB interface | `172.20.140.51` |

The GoPro HTTP API is accessed at:

```text
http://172.20.140.51:8080
```

---

### PC UDP receiver

Current PC IPv4 address:

```text
10.5.159.150
```

In the code:

```c
#define PC_STREAM_IP "10.5.159.150"
#define PC_STREAM_PORT 5001
```

If the PC IPv4 address changes, update `PC_STREAM_IP`.

---

## Address Table

| Element | Address / value | Role |
|---|---|---|
| ESP32-P4 USB | `172.20.140.50` | ESP32 USB network interface |
| GoPro USB | `172.20.140.51` | GoPro USB network interface |
| GoPro HTTP API | `172.20.140.51:8080` | OpenGoPro HTTP commands |
| GoPro UDP webcam port | `5000` | UDP stream received by ESP32-P4 |
| PC IP | `10.5.159.150` | Computer receiving the stream |
| PC UDP port | `5001` | VLC listening port |
| VLC URL | `udp://@:5001` | VLC network stream URL |

---

## Hardware

* ESP32-P4-Module-DEV-KIT
* GoPro with USB webcam support
* USB-C data cable
* Ethernet cable
* PC running VLC
* Stable power supply

---

## Build

```powershell
idf.py set-target esp32p4
idf.py fullclean
idf.py build
```

---

## Flash

```powershell
idf.py -p COMx flash monitor
```

Exit monitor:

```text
Ctrl + ]
```

---

## VLC

Open VLC and use:

```text
udp://@:5001
```

---

## Current status

Working:

* USB GoPro detection
* CDC-NCM setup
* ARP reply from GoPro
* HTTP communication over USB
* first webcam start
* UDP video forwarding to VLC

Known limitation:

After a GoPro reboot, the USB link, ARP and HTTP API come back correctly, but `/gopro/webcam/start` may return:

```json
{
  "status": 1,
  "error": 4
}
```

This appears to be an internal GoPro webcam service state issue after reboot.