# Commandes pour compiler et lancer le projet ESP32-P4 GoPro USB

Ce fichier résume les commandes à utiliser dans le terminal ESP-IDF pour compiler, configurer, flasher et lancer le projet.

Projet concerné :  ESP32-P4 + GoPro USB CDC-NCM + RJ45 interne

---

## . Ouvrir le bon terminal

Il faut ouvrir le terminal ESP-IDF : ESP-IDF 5.5 PowerShell

Ne pas utiliser un terminal Windows classique si l’environnement ESP-IDF n’est pas chargé.


## . Choisir la bonne carte / target

Pour notre carte **ESP32-P4**, il faut mettre :

```powershell
idf.py set-target esp32p4
```

Cette commande est importante.

Si le projet était avant configuré pour une autre carte, par exemple ESP32-S3, il faut refaire cette commande.

---

## . Nettoyer le projet si besoin

Si on change de carte, de configuration ou si la compilation bug, faire :

```powershell
idf.py fullclean
---

## 5. Ouvrir menuconfig

Lancer :

```powershell
idf.py menuconfig
```

---

## . Réglages importants dans menuconfig

### Serial flasher config

Vérifier :

```text
Flash size = 16 MB
Flash mode = DIO
Flash frequency = 80 MHz
```

Si la carte boote mal, tester :

```text
Flash frequency = 40 MHz
```

---

### Partition Table

Dans :

```text
Partition Table
```

Choisir :

```text
Single factory app, no OTA
```

Dans la version actuelle du projet, les logs montrent une table simple :

```text
nvs
phy_init
factory
```

Donc il n’est pas obligatoire d’utiliser une table OTA pour ce projet.

---

### Ethernet

Le projet utilise le **RJ45 interne de la carte ESP32-P4**

Vérifier que l’Ethernet est activé :

```text
Component config
  → Ethernet
```

Options importantes :

```text
Ethernet enabled
ESP32 internal EMAC enabled
```

Le code configure ensuite les pins RMII directement :

```c
#define P4_ETH_MDC_GPIO       GPIO_NUM_31
#define P4_ETH_MDIO_GPIO      GPIO_NUM_52
#define P4_ETH_PHY_RST_GPIO   GPIO_NUM_51

#define P4_ETH_RMII_TX_EN     GPIO_NUM_49
#define P4_ETH_RMII_TXD0      GPIO_NUM_34
#define P4_ETH_RMII_TXD1      GPIO_NUM_35
#define P4_ETH_RMII_CRS_DV    GPIO_NUM_28
#define P4_ETH_RMII_RXD0      GPIO_NUM_29
#define P4_ETH_RMII_RXD1      GPIO_NUM_30
#define P4_ETH_RMII_CLK       GPIO_NUM_50
```

---

### USB Host

Le projet utilise l’ESP32-P4 comme **USB Host** pour parler avec la GoPro.

Vérifier que l’USB Host est activé si l’option apparaît dans menuconfig :

```text
Component config
  → USB
  → USB Host
```

Le code utilise :

```c
#include "usb/usb_host.h"
```

et démarre l’USB Host avec :

```c
usb_host_install(&host_config);
```

---

### Wi-Fi / Bluetooth

Pour cette version ESP32-P4 USB, le live ne passe pas par Wi-Fi ou BLE.

Donc ce n’est pas la partie importante du projet.

Le contrôle principal est fait par :

```text
USB CDC-NCM + HTTP GoPro + UDP vidéo
```

---

## 7. Vérifier l’adresse IP du PC

Le flux vidéo est renvoyé vers le PC sur le port UDP 5001.

Dans le code, il y a une ligne de ce type :

```c
#define PC_STREAM_IP "10.5.159.148"
```

il faut remplacer  :

```c
#define PC_STREAM_IP "[remplacaer par la vraie adresse du pc"]
```

---

### Trouver l’adresse IP du PC

Dans PowerShell ou CMD :

```powershell
ipconfig
```
Chercher la carte réseau utilisée, puis la ligne :

```text
Adresse IPv4
``
Exemple :

```text
Adresse IPv4. . . . . . . . . . . . . .: 10.5.159.148
```

Dans ce cas, dans le code il faut mettre :

```c
#define PC_STREAM_IP "10.5.159.148"
```

Si l’adresse change, il faut modifier cette ligne, puis recompiler et reflasher.

---

## 8. Compiler le projet

Commande normale :

```powershell
idf.py build
```

Si le projet a déjà eu des erreurs bizarres, faire plutôt :

```powershell
idf.py fullclean
idf.py build
```

## . Lancer le monitor série



Ou tout faire en une seule commande :

```powershell
idf.py -p COM8 flash monitor

---

## . Commande complète la plus utilisée

Quand tout est déjà configuré :

```powershell
idf.py -p COM8 build flash monitor
```

Si ça bug après beaucoup de modifications :

```powershell
idf.py fullclean
idf.py set-target esp32p4
idf.py build
idf.py -p COM8 flash monitor
```

---

## 12. Lancer VLC sur le PC

Le code envoie le flux vidéo vers le PC sur le port :

```text
5001
```

Dans VLC, ouvrir un flux réseau :

```text
udp://@:5001
```

---

## . Logs attendus si tout fonctionne

USB GoPro :

```text
[USB] GoPro détectée
[USB] Interface CDC-NCM détectée
[USB] Réseau CDC-NCM connecté
```

ARP GoPro :

```text
[NET] ARP reply reçue
[NET] IP GoPro = 172.20.140.51
```

Ethernet RJ45 :

```text
[ETH] Câble connecté, lien Ethernet actif
[ETH] Adresse IP Ethernet prête
```

Live GoPro :

```text
[WEBCAM] Commande START acceptée
[WEBCAM] LIVE OK : premier paquet UDP reçu
```

Envoi vers PC :

```text
[FORWARD] Destination PC : 10.5.159.xxx:5001
[FORWARD] Envoyés=..., perdus=...
```

---
