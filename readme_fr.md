# ESP32-P4 + GoPro USB CDC-NCM Live Streaming

## 🇫🇷 Français

Ce projet permet à une carte **ESP32-P4-Module-DEV-KIT** de contrôler automatiquement une **GoPro** en **USB Host High-Speed** afin de lancer un mode webcam USB et de retransmettre le flux vidéo vers un ordinateur via le **RJ45 intégré** de l’ESP32-P4.

La GoPro est vue par l’ESP32-P4 comme une interface réseau **USB CDC-NCM**. Une fois cette interface active, l’ESP32-P4 communique avec la GoPro en HTTP sur le réseau USB, démarre le mode webcam, reçoit le flux vidéo UDP, puis le renvoie vers le PC pour affichage dans VLC.

Cette version intègre aussi une recovery automatique complète pour relancer le live après une perte USB, un redémarrage de la GoPro ou une interruption du flux vidéo.

---

## 🧠 Fonctionnement global

```text
GoPro
  │
  │ USB-C / USB Host
  │ - détection USB
  │ - interface CDC-NCM
  │ - réseau USB 172.20.140.x
  │ - commandes HTTP GoPro
  │ - flux webcam UDP
  │
ESP32-P4
  │
  │ USB Host High-Speed
  │ - configure CDC-NCM
  │ - envoie ARP vers la GoPro
  │ - lance /gopro/webcam/start
  │ - reçoit UDP port 5000
  │
RJ45 intégré ESP32-P4
  │
  │ Ethernet
  │ - retransmission UDP vers le PC
  │
PC
  │
  │ VLC
  │ - reçoit le flux sur udp://@:5001
```

Résumé :

```text
USB CDC-NCM = communication réseau entre ESP32-P4 et GoPro
HTTP GoPro  = commandes stop / exit / start / Labs
UDP 5000    = flux webcam reçu depuis la GoPro
RJ45        = sortie réseau vers le PC
VLC         = affichage du live vidéo
```

Le flux vidéo ne passe pas par BLE ni par Wi-Fi.  
Tout passe par l’USB entre la GoPro et l’ESP32-P4.

---

## 🎥 Chemin du flux vidéo

```text
GoPro
  → USB CDC-NCM
  → ESP32-P4
  → RJ45 intégré
  → PC
  → VLC udp://@:5001
```

---

## 🌐 Partie importante : adresses réseau

Ce projet utilise deux réseaux différents :

1. le réseau USB entre l’ESP32-P4 et la GoPro ;
2. le réseau Ethernet entre l’ESP32-P4 et le PC.

---

### 1. Réseau USB GoPro

La GoPro utilise une interface réseau USB CDC-NCM.

Adresse ESP32-P4 côté USB :

```text
172.20.140.50
```

Adresse GoPro côté USB :

```text
172.20.140.51
```

Port HTTP GoPro :

```text
8080
```

Port UDP webcam GoPro :

```text
5000
```

Ces valeurs sont utilisées dans le code pour envoyer les commandes HTTP à la GoPro et recevoir le flux vidéo.

---

### 2. Réseau Ethernet vers le PC

L’ESP32-P4 utilise le RJ45 intégré pour envoyer le flux reçu vers le PC.

Adresse du PC dans le code actuel :

```text
10.5.159.148
```

Port UDP côté PC :

```text
5001
```

Dans le code :

```c
#define PC_STREAM_IP "10.5.159.148"
#define PC_STREAM_PORT 5001
```

Si l’adresse IPv4 du PC change, il faut modifier `PC_STREAM_IP`, recompiler et reflasher.

---

### 3. Comment vérifier l’adresse IPv4 du PC

Sur Windows, ouvrir PowerShell ou CMD :

```powershell
ipconfig
```

Chercher la ligne de l’interface réseau utilisée :

```text
Adresse IPv4. . . . . . . . . . . . . .: 10.5.159.xxx
```

Si l’adresse affichée est différente, par exemple :

```text
10.5.159.154
```

alors modifier dans le code :

```c
#define PC_STREAM_IP "10.5.159.154"
```

Puis recompiler :

```powershell
idf.py build
idf.py -p COMx flash monitor
```

---

## 📌 Tableau récapitulatif des adresses

| Élément | Adresse / valeur | Rôle |
| --- | --- | --- |
| ESP32-P4 USB | `172.20.140.50` | Adresse de l’ESP32 côté USB GoPro |
| GoPro USB | `172.20.140.51` | Adresse de la GoPro côté USB |
| HTTP GoPro | `172.20.140.51:8080` | Commandes GoPro par HTTP |
| UDP GoPro | `5000` | Port webcam reçu par l’ESP32-P4 |
| PC VLC | `10.5.159.148` | Ordinateur qui affiche le live |
| Port VLC | `5001` | Port UDP écouté par VLC |
| VLC | `udp://@:5001` | Adresse à ouvrir dans VLC |
| Ethernet ESP32-P4 | DHCP | Adresse donnée par le réseau labo |
| GoPro USB VID | `0x2672` | Identifiant constructeur GoPro |
| GoPro USB PID | `0x0059` | Identifiant périphérique GoPro |

---

## 🔁 Schéma réseau complet

```text
GoPro
USB IP : 172.20.140.51
HTTP   : 172.20.140.51:8080
UDP    : port 5000
        │
        │ USB-C / CDC-NCM
        ▼
ESP32-P4 USB Host
USB IP : 172.20.140.50
        │
        │ réception UDP 5000
        │ retransmission UDP
        ▼
ESP32-P4 RJ45 intégré
IP Ethernet : DHCP 10.5.159.x
        │
        │ Ethernet
        ▼
PC
IP   : 10.5.159.148
Port : 5001
VLC  : udp://@:5001
```

---

## ⚙️ Pourquoi cette version fonctionne

Cette version fonctionne parce qu’elle traite séparément trois cas différents :

```text
1. Premier démarrage normal
2. Live déjà lancé puis perte de flux
3. Reconnexion USB après redémarrage ou débranchement GoPro
```

Au premier démarrage, le code attend que tout soit prêt :

```text
1. Démarrage de la GoPro
2. Initialisation USB Host
3. Détection USB GoPro
4. Configuration CDC-NCM
5. Attente NETWORK_CONNECTION = 1
6. Création interface lwIP USB
7. ARP vers 172.20.140.51
8. Initialisation Ethernet RJ45
9. Ouverture UDP 5000
10. Envoi /gopro/webcam/start
11. Réception du premier paquet UDP
12. Retransmission vers le PC
```

Quand le premier paquet UDP est reçu, le code considère que le live est réellement lancé :

```c
s_live_running = true;
s_live_has_ever_run = true;
```

Ce flag est important. Il permet ensuite de savoir que si la GoPro se reconnecte, ce n’est plus un premier boot classique, mais une récupération après un live déjà lancé.

---

## 🔁 Recovery automatique après perte du live

Quand le live s’arrête ou quand la GoPro disparaît du bus USB, le code nettoie la session puis attend la reconnexion.

Quand l’ARP répond après une reconnexion USB post-live, le code ne tente plus directement `webcam/start`.

Pourquoi ? Parce que les tests ont montré que la GoPro répond souvent :

```text
HTTP/1.1 500 Internal Server Error
"error": 4
```

Cela veut dire que :

```text
USB OK
ARP OK
HTTP joignable
mais service webcam GoPro pas encore prêt
```

Donc cette version évite ce START inutile et lance directement la recovery forte.

---

## 🧩 Séquence de recovery forte

La séquence utilisée est :

```text
1. /gopro/webcam/stop
2. /gopro/webcam/exit
3. appel Labs !OR via HTTP USB
4. reset de la session USB locale
5. root-port USB OFF pendant 5 secondes
6. root-port USB ON demandé
7. esp_restart volontaire
8. nouveau boot complet ESP32-P4
9. nouvelle énumération USB GoPro
10. nouveau START webcam
```

Cette séquence est volontairement conservée car c’est celle qui a fonctionné pendant les tests.

---

## ❗ Pourquoi garder `!OR` même s’il est refusé

La commande envoyée est :

```text
/gopro/qrcode?labs=1&code=%21OR
```

Dans les logs, la GoPro peut répondre :

```text
404 Not Found
Command is not recognized
```

Cela signifie que l’endpoint HTTP Labs ne valide pas officiellement la commande.

Cependant, les tests ont montré que :

```text
version avec !OR    → le live repart correctement après recovery
version sans !OR    → la GoPro peut rester bloquée en CDC-NCM non connecté
```

Donc dans ce projet, `!OR` est gardé comme une étape de stabilisation/timing.

Il ne faut pas considérer `!OR` comme la vraie commande principale de reboot.  
La vraie recovery est le reset USB complet :

```text
root-port OFF / ON + esp_restart
```

Mais l’appel `!OR` garde un timing qui rend la séquence plus fiable avec cette GoPro.

---

## ❌ Pourquoi `!WRESET` n’est pas utilisé

`!WRESET` fonctionne lorsqu’il est scanné en QR code physique par la GoPro.

Mais dans ce projet, l’envoi par HTTP USB via :

```text
/gopro/qrcode?labs=1&code=%21WRESET
```

n’a pas donné le même résultat fiable.

Le problème n’était pas seulement de redémarrer la GoPro. Le vrai problème était de retrouver une session USB CDC-NCM saine entre l’ESP32-P4 et la GoPro.

La solution retenue est donc :

```text
nettoyage HTTP webcam
+ stabilisation Labs !OR
+ reset logique root-port USB
+ redémarrage volontaire ESP32-P4
```

---

## ✅ État actuel du projet

Comportement observé :

* détection USB GoPro fonctionnelle ;
* configuration CDC-NCM fonctionnelle ;
* ARP vers la GoPro fonctionnel ;
* commandes HTTP vers la GoPro fonctionnelles ;
* démarrage webcam fonctionnel ;
* réception UDP depuis la GoPro fonctionnelle ;
* retransmission UDP vers le PC fonctionnelle ;
* live VLC fonctionnel ;
* recovery après perte de flux fonctionnelle ;
* recovery après reconnexion USB fonctionnelle ;
* pas besoin de débrancher/rebrancher le câble manuellement.

---

## 📦 Matériel utilisé

* ESP32-P4-Module-DEV-KIT
* GoPro compatible USB webcam / OpenGoPro
* Câble USB-C data entre GoPro et ESP32-P4
* RJ45 intégré de l’ESP32-P4
* Câble Ethernet
* PC avec VLC
* Alimentation stable

---

## 🔌 Connexions

| Élément | Connexion |
| --- | --- |
| GoPro | USB-C vers port USB Host ESP32-P4 |
| ESP32-P4 | RJ45 vers réseau / PC |
| PC | VLC en écoute UDP |

VLC doit écouter :

```text
udp://@:5001
```

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
Flash size = 16 MB
Flash mode = DIO
Flash frequency = 80 MHz
```

Si la carte boote mal, tester :

```text
Flash frequency = 40 MHz
```

---

### 2. USB Host

Le projet utilise l’USB Host natif de l’ESP32-P4.

Vérifier que l’USB Host est activé dans ESP-IDF.

Le code installe la pile USB Host avec :

```c
usb_host_install(&host_config);
```

Le root port est configuré avec :

```c
.root_port_unpowered = false
```

---

### 3. Ethernet interne ESP32-P4

Le projet utilise le RJ45 intégré avec PHY IP101GRI.

Pins utilisées :

| Signal | GPIO |
| --- | --- |
| MDC | GPIO 31 |
| MDIO | GPIO 52 |
| PHY RESET | GPIO 51 |
| TX EN | GPIO 49 |
| TXD0 | GPIO 34 |
| TXD1 | GPIO 35 |
| CRS DV | GPIO 28 |
| RXD0 | GPIO 29 |
| RXD1 | GPIO 30 |
| RMII CLK | GPIO 50 |

---

## 🚀 Compilation

```powershell
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

## 🎥 Utilisation du live

1. Vérifier l’adresse IPv4 du PC avec `ipconfig`.
2. Modifier `PC_STREAM_IP` si l’adresse du PC a changé.
3. Ouvrir VLC.
4. Ouvrir un flux réseau :

```text
udp://@:5001
```

5. Brancher la GoPro en USB sur l’ESP32-P4.
6. Brancher le RJ45 de l’ESP32-P4.
7. Flasher ou redémarrer l’ESP32-P4.
8. Attendre la détection USB CDC-NCM.
9. Attendre le démarrage automatique du live.
10. La vidéo doit arriver dans VLC.

---

## ✅ Logs attendus

Détection USB GoPro :

```text
[USB] Nouveau périphérique détecté
[USB] VID=0x2672 PID=0x0059
[USB] GoPro détectée
[USB] Interface CDC-NCM détectée
[USB] Endpoint bulk IN trouvé: 0x81
[USB] Endpoint bulk OUT trouvé: 0x01
```

Connexion CDC-NCM :

```text
[USB] Type: NETWORK_CONNECTION
[USB] wValue=0x0001
[USB] Réseau CDC-NCM connecté
```

ARP GoPro :

```text
[NET] ARP reply reçue
[NET] MAC GoPro = 06:57:47:84:92:EC
[NET] IP GoPro = 172.20.140.51
```

Démarrage live :

```text
[LIVE] Conditions OK : démarrage webcam GoPro
[WEBCAM] START webcam simple
[WEBCAM] Commande START acceptée
[WEBCAM] LIVE OK : premier paquet UDP reçu
```

Flux actif :

```text
[WEBCAM] Flux actif : 5000 paquets
[FORWARD] Envoyés=5000, perdus=0
```

Recovery après reconnexion post-live :

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

---

## ⚠️ Problèmes fréquents

### VLC ne reçoit rien

Vérifier :

* VLC écoute bien `udp://@:5001` ;
* `PC_STREAM_IP` correspond bien à l’IPv4 du PC ;
* le pare-feu Windows ne bloque pas VLC ;
* l’ESP32-P4 a bien obtenu une IP Ethernet ;
* les logs indiquent `LIVE OK`.

---

### Le live fonctionnait hier mais plus aujourd’hui

Cause probable :

```text
l’adresse IPv4 du PC a changé
```

Refaire :

```powershell
ipconfig
```

Puis modifier :

```c
#define PC_STREAM_IP "NOUVELLE_IP"
```

---

### La GoPro est détectée mais CDC-NCM reste non connecté

Log typique :

```text
[USB] Type: NETWORK_CONNECTION
[USB] wValue=0x0000
[USB] Réseau CDC-NCM non connecté
```

Cela signifie que la GoPro est visible en USB, mais que l’interface réseau CDC-NCM n’est pas encore prête.

La recovery forte sert justement à retrouver une session complète.

---

### START renvoie error 4

Log typique :

```text
HTTP/1.1 500 Internal Server Error
"error": 4
```

Cela signifie que le réseau USB fonctionne, mais que le service webcam interne de la GoPro n’est pas prêt.

Après un live déjà lancé, le code évite maintenant de refaire ce START inutile et déclenche directement la recovery forte.

---

### `!OR` est refusé dans les logs

C’est attendu.

Le log peut afficher :

```text
Command is not recognized
```

Dans cette version, `!OR` est gardé comme étape de stabilisation, pas comme commande principale.

La recovery principale reste :

```text
root-port OFF / ON + esp_restart
```

---

## 🧪 Différences principales avec l’ancien code

L’ancien code essayait surtout de relancer le live après erreur, mais il ne faisait pas une vraie recovery USB complète.

Le nouveau code ajoute :

```text
1. détection du premier vrai live avec s_live_has_ever_run
2. distinction entre premier boot et reconnexion post-live
3. suppression du START inutile après reconnexion post-live
4. recovery forte dédiée dans une tâche séparée
5. stop/exit HTTP avant recovery
6. étape !OR conservée comme stabilisation
7. reset complet de la session USB locale
8. root-port USB OFF pendant 5 secondes
9. tentative root-port ON
10. esp_restart volontaire
```

C’est cette séquence complète qui rend la relance fiable.

---

## 📌 Commandes importantes utilisées

Stop webcam :

```text
/gopro/webcam/stop
```

Exit webcam :

```text
/gopro/webcam/exit
```

Start webcam :

```text
/gopro/webcam/start?res=7&fov=0&port=5000
```

Étape Labs conservée :

```text
/gopro/qrcode?labs=1&code=%21OR
```

---

## ✅ Conclusion

Cette version est la version stable actuelle du projet.

Elle permet :

```text
GoPro USB CDC-NCM
→ ESP32-P4 USB Host
→ lancement webcam GoPro
→ réception UDP
→ retransmission RJ45
→ VLC sur PC
```

La partie la plus importante est la recovery automatique :

```text
pas de START inutile après reconnexion post-live
+ stop / exit
+ !OR stabilisation
+ reset session USB
+ root-port OFF / ON
+ esp_restart volontaire
```

C’est cette logique qui permet de retrouver le live sans intervention manuelle après une perte de flux ou une reconnexion USB.
