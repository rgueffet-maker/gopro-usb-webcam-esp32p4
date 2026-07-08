#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <errno.h>

/*
 * FreeRTOS :
 * - task.h permet de créer des tâches et d'utiliser vTaskDelay()
 * - event_groups.h permet d'attendre les événements du lien Ethernet
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_system.h"

/*
 * ESP-IDF : réseau Ethernet W5500 + événements + NVS.
 */
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "nvs_flash.h"

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "lwip/ip4_addr.h"
#include "esp_eth_driver.h"
#include "driver/gpio.h"
/*
 * Serveur HTTP :
 * Permet de créer des routes web comme :
 * - /
 * - /status
 */
#include "esp_http_server.h"

/*
 * USB Host :
 * Permet à l'ESP32-P4 d'agir comme hôte USB.
 */
#include "usb/usb_host.h"
#include "esp_intr_alloc.h"

/*
 * Déclaration explicite pour éviter toute erreur d'implicit declaration
 * selon la version exacte des headers ESP-IDF installés.
 */
extern esp_err_t usb_host_lib_set_root_port_power(bool enable);

#define ETH_LINK_UP_BIT BIT0
#define ETH_GOT_IP_BIT  BIT1

static const char *TAG = "GOPRO_USB_P4";

/*
 * Ethernet RJ45 intégré Waveshare ESP32-P4-Module-DEV-KIT.
 * PHY : IP101GRI / P101GRI
 */
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

static EventGroupHandle_t s_eth_event_group = NULL;
static esp_netif_t *s_eth_netif = NULL;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static bool s_eth_link_up = false;
static uint32_t s_eth_ip_addr = 0;
/*
 * Évite d'initialiser plusieurs fois le W5500.
 */
static bool s_ethernet_initialized = false;

static void ethernet_event_handler(void *arg,
                                   esp_event_base_t event_base,
                                   int32_t event_id,
                                   void *event_data)
{
    (void)arg;
    (void)event_base;

    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED: {
        uint8_t mac_addr[6] = {0};

        s_eth_link_up = true;
        xEventGroupSetBits(s_eth_event_group, ETH_LINK_UP_BIT);

        ESP_ERROR_CHECK(esp_eth_ioctl(
            eth_handle,
            ETH_CMD_G_MAC_ADDR,
            mac_addr
        ));

        ESP_LOGI(TAG, "[ETH] Câble connecté, lien Ethernet actif");
        ESP_LOGI(
            TAG,
            "[ETH] MAC %02X:%02X:%02X:%02X:%02X:%02X",
            mac_addr[0], mac_addr[1], mac_addr[2],
            mac_addr[3], mac_addr[4], mac_addr[5]
        );
        break;
    }

    case ETHERNET_EVENT_DISCONNECTED:
        s_eth_link_up = false;
        xEventGroupClearBits(
            s_eth_event_group,
            ETH_LINK_UP_BIT | ETH_GOT_IP_BIT
        );

        s_eth_ip_addr = 0;
        break;

    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "[ETH] Pilote Ethernet démarré");
        break;

    case ETHERNET_EVENT_STOP:
        s_eth_link_up = false;
        xEventGroupClearBits(s_eth_event_group, ETH_LINK_UP_BIT);
        ESP_LOGI(TAG, "[ETH] Pilote Ethernet arrêté");
        break;

    default:
        break;
    }
}

static void ethernet_got_ip_handler(void *arg,
                                    esp_event_base_t event_base,
                                    int32_t event_id,
                                    void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;

    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    s_eth_ip_addr = event->ip_info.ip.addr;

    xEventGroupSetBits(
        s_eth_event_group,
        ETH_GOT_IP_BIT
    );

    ESP_LOGI(TAG, "[ETH] Adresse IP Ethernet prête");
    ESP_LOGI(TAG, "[ETH] IP      : " IPSTR, IP2STR(&event->ip_info.ip));
    ESP_LOGI(TAG, "[ETH] Masque  : " IPSTR, IP2STR(&event->ip_info.netmask));
    ESP_LOGI(TAG, "[ETH] Gateway : " IPSTR, IP2STR(&event->ip_info.gw));
}

static void ethernet_init_p4_internal(void)
{
    if (s_ethernet_initialized) {
        ESP_LOGW(TAG, "[ETH] Ethernet déjà initialisé");
        return;
    }

    ESP_LOGI(TAG, "[ETH] Initialisation RJ45 interne ESP32-P4 / IP101GRI");

    s_eth_event_group = xEventGroupCreate();

    if (s_eth_event_group == NULL) {
        ESP_LOGE(TAG, "[ETH] Impossible de créer EventGroup Ethernet");
        return;
    }

    /*
     * Interface réseau Ethernet côté RJ45.
     *
     * DHCP :
     * - le routeur/switch donne une IP à l'ESP32-P4
     * - le PC peut rester en Wi-Fi sur le même réseau labo
     */
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);

    if (s_eth_netif == NULL) {
        ESP_LOGE(TAG, "[ETH] esp_netif_new ETH échoué");
        return;
    }

    ESP_LOGI(TAG, "[ETH] Mode DHCP : attente d'une IP du réseau");

    /*
     * Configuration MAC interne ESP32-P4.
     */
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    mac_config.rx_task_stack_size = 4096;

    eth_esp32_emac_config_t emac_config =
        ETH_ESP32_EMAC_DEFAULT_CONFIG();

    emac_config.smi_gpio.mdc_num = P4_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = P4_ETH_MDIO_GPIO;

    /*
     * Le PHY fournit le clock RMII vers l'ESP32-P4.
     */
    emac_config.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_config.clock_config.rmii.clock_gpio = P4_ETH_RMII_CLK;

    emac_config.emac_dataif_gpio.rmii.tx_en_num  = P4_ETH_RMII_TX_EN;
    emac_config.emac_dataif_gpio.rmii.txd0_num   = P4_ETH_RMII_TXD0;
    emac_config.emac_dataif_gpio.rmii.txd1_num   = P4_ETH_RMII_TXD1;
    emac_config.emac_dataif_gpio.rmii.crs_dv_num = P4_ETH_RMII_CRS_DV;
    emac_config.emac_dataif_gpio.rmii.rxd0_num   = P4_ETH_RMII_RXD0;
    emac_config.emac_dataif_gpio.rmii.rxd1_num   = P4_ETH_RMII_RXD1;

    esp_eth_mac_t *mac =
        esp_eth_mac_new_esp32(
            &emac_config,
            &mac_config
        );

    if (mac == NULL) {
        ESP_LOGE(TAG, "[ETH] Création MAC ESP32-P4 échouée");
        return;
    }

    /*
     * Configuration PHY IP101GRI.
     */
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = -1;
    phy_config.reset_gpio_num = P4_ETH_PHY_RST_GPIO;
    phy_config.reset_timeout_ms = 100;

    esp_eth_phy_t *phy =
        esp_eth_phy_new_ip101(
            &phy_config
        );

    if (phy == NULL) {
        ESP_LOGE(TAG, "[ETH] Création PHY IP101 échouée");
        return;
    }

    esp_eth_config_t eth_config =
        ETH_DEFAULT_CONFIG(
            mac,
            phy
        );

    ESP_ERROR_CHECK(
        esp_eth_driver_install(
            &eth_config,
            &s_eth_handle
        )
    );

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);

    if (s_eth_glue == NULL) {
        ESP_LOGE(TAG, "[ETH] Glue netif Ethernet échouée");
        return;
    }

    ESP_ERROR_CHECK(
        esp_netif_attach(
            s_eth_netif,
            s_eth_glue
        )
    );

    /*
     * IMPORTANT :
     * Il faut enregistrer ETH_EVENT ET IP_EVENT.
     * ETH_EVENT donne le lien câble.
     * IP_EVENT donne l'adresse DHCP.
     */
    ESP_ERROR_CHECK(
        esp_event_handler_register(
            ETH_EVENT,
            ESP_EVENT_ANY_ID,
            &ethernet_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_ETH_GOT_IP,
            &ethernet_got_ip_handler,
            NULL
        )
    );

    /*
     * Active le client DHCP.
     */
    esp_err_t dhcp_err = esp_netif_dhcpc_start(s_eth_netif);

    if (dhcp_err != ESP_OK &&
        dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {

        ESP_LOGE(
            TAG,
            "[ETH] Démarrage DHCP impossible : %s",
            esp_err_to_name(dhcp_err)
        );

        return;
    }

    ESP_LOGI(TAG, "[ETH] DHCP démarré, lancement du pilote Ethernet");

    /*
     * Démarre le pilote Ethernet UNE SEULE FOIS.
     */
    ESP_ERROR_CHECK(
        esp_eth_start(
            s_eth_handle
        )
    );

    s_ethernet_initialized = true;

    /*
     * Attend :
     * - lien Ethernet actif
     * - adresse IP DHCP reçue
     */
    EventBits_t bits = xEventGroupWaitBits(
        s_eth_event_group,
        ETH_LINK_UP_BIT | ETH_GOT_IP_BIT,
        pdFALSE,
        pdTRUE,
        pdMS_TO_TICKS(20000)
    );

    if ((bits & ETH_LINK_UP_BIT) &&
        (bits & ETH_GOT_IP_BIT)) {

        ESP_LOGI(TAG, "[ETH] Lien actif et adresse DHCP obtenue");
    }
    else if (bits & ETH_LINK_UP_BIT) {

        ESP_LOGW(
            TAG,
            "[ETH] Câble connecté, mais aucune adresse DHCP après 20 secondes"
        );
    }
    else {

        ESP_LOGW(
            TAG,
            "[ETH] Aucun lien Ethernet après 20 secondes"
        );
    }

    ESP_LOGI(TAG, "[ETH] RJ45 interne démarré en DHCP");
}

//* Handle du serveur HTTP.
static httpd_handle_t s_http_server = NULL;
//variables globales

static bool s_usb_gopro_detected = false;   // État de la GoPro détectée en USB.
static bool s_usb_ncm_detected = false;  // Vrai si la GoPro expose une interface réseau USB CDC-NCM.
static uint16_t s_usb_vid = 0;      // Identifiants USB de la GoPro.
static uint16_t s_usb_pid = 0;
static uint8_t s_usb_num_interfaces = 0;  // Nombre d'interfaces USB trouvées sur la GoPro.

static usb_device_handle_t s_gopro_dev_hdl = NULL;  // Handle USB gardé ouvert pour pouvoir continuer à parler avec la GoPro.
static bool s_gopro_open = false;
static bool s_gopro_if0_claimed = false;  // Vrai si les interfaces USB utiles ont été réservées.
static bool s_gopro_if1_claimed = false;


// Taille maximale lue sur l'endpoint de notification.
#define GOPRO_NOTIFICATION_SIZE 16
// Transfert USB utilisé pour recevoir les notifications.
static usb_transfer_t *s_notification_transfer = NULL;
// Vrai si l'écoute des notifications est lancée.
static bool s_notification_started = false;

// Endpoints USB trouvés sur la GoPro.
static uint8_t s_gopro_ep_notify = 0;
static uint8_t s_gopro_ep_bulk_in = 0;
static uint8_t s_gopro_ep_bulk_out = 0;

// Taille utilisée pour envoyer des données réseau USB.
#define GOPRO_BULK_OUT_SIZE 2048
// Transfert USB pour envoyer des données ESP32 -> GoPro.
static usb_transfer_t *s_bulk_out_transfer = NULL;
// Vrai si un envoi bulk OUT a été tenté.
static bool s_bulk_out_sent = false;
// Numéro de séquence des blocs NTB envoyés.
static uint16_t s_ntb_sequence = 0;

// Indique si les endpoints réseau ont été trouvés.
static bool s_gopro_endpoints_ready = false;
// Taille de lecture bulk IN basée sur le maximum annoncé par la GoPro.
#define GOPRO_BULK_IN_SIZE 16384
// Transfert USB pour lire les données GoPro -> ESP32.
#define GOPRO_BULK_IN_TRANSFER_COUNT 2
static usb_transfer_t *s_bulk_in_transfers[GOPRO_BULK_IN_TRANSFER_COUNT] = {0};
// Vrai si la lecture bulk IN est lancée.
static bool s_bulk_in_started = false;

// Requête CDC-NCM pour lire les paramètres NTB.
#define CDC_NCM_GET_NTB_PARAMETERS 0x80
// Interface de contrôle CDC-NCM.
#define CDC_NCM_CONTROL_INTERFACE 0
// Taille attendue de la réponse NTB Parameters.
#define CDC_NCM_NTB_PARAMETERS_SIZE 28
// Transfert USB utilisé pour la requête de contrôle NCM.
static usb_transfer_t *s_ncm_ctrl_transfer = NULL;
// Vrai si les paramètres NCM ont été reçus.
static bool s_ncm_params_received = false;
// Requête CDC-NCM pour régler la taille maximale des blocs NTB reçus.
#define CDC_NCM_SET_NTB_INPUT_SIZE 0x86
// Taille maximale annoncée par la GoPro.
#define CDC_NCM_INPUT_SIZE 16384

//choicir le format NTB
#define CDC_NCM_SET_NTB_FORMAT 0x84
#define CDC_NCM_NTB_FORMAT_16 0x0000
// Vrai si le format NTB a été choisi.
static bool s_ncm_format_set = false;
// Transfert USB utilisé pour choisir le format NTB.
static usb_transfer_t *s_ncm_format_transfer = NULL;
// Transfert USB utilisé pour SET_NTB_INPUT_SIZE.
static usb_transfer_t *s_ncm_input_size_transfer = NULL;
// Vrai si SET_NTB_INPUT_SIZE a réussi.
static bool s_ncm_input_size_set = false;

// Requête CDC pour choisir quels paquets réseau accepter.
#define CDC_SET_ETHERNET_PACKET_FILTER 0x43
// Accepte les paquets destinés à la GoPro.
#define CDC_PACKET_TYPE_DIRECTED 0x0004
// Accepte les paquets broadcast, utile pour ARP.
#define CDC_PACKET_TYPE_BROADCAST 0x0008
// Filtre réseau utilisé au démarrage.
#define CDC_PACKET_FILTER_DEFAULT (CDC_PACKET_TYPE_DIRECTED | CDC_PACKET_TYPE_BROADCAST)

// Transfert USB utilisé pour activer le filtre réseau.
static usb_transfer_t *s_packet_filter_transfer = NULL;
// Vrai si le filtre réseau a été activé.
static bool s_packet_filter_set = false;

// Vrai si la GoPro annonce que le réseau CDC-NCM est connecté.
static bool s_cdc_network_connected = false;
// Dernière notification CDC reçue.
static uint8_t s_last_cdc_notification = 0;
// Dernière valeur CDC reçue.
static uint16_t s_last_cdc_value = 0;

// Demande de démarrage du test réseau après NETWORK_CONNECTION = 1.
static bool s_network_test_pending = false;

// Vrai si le test réseau USB/NCM a déjà été lancé pour la session actuelle.
static bool s_network_test_started = false;

// Vrai quand une tentative de démarrage live est en cours.
static bool s_live_start_requested = false;

/*
 * État du live.
 * true seulement après réception du premier paquet UDP vidéo.
 */
static volatile bool s_live_running = false;

/*
 * Devient vrai après le premier vrai paquet vidéo reçu.
 * Sert à distinguer :
 * - premier démarrage normal : on tente START normalement ;
 * - reconnexion USB après un live déjà lancé : on évite le START inutile qui donne error 4,
 *   et on lance directement la recovery forte qui a été validée dans les tests.
 */
static volatile bool s_live_has_ever_run = false;

/*
 * Anti-spam recovery :
 * - ARP retry : si la GoPro ne répond pas tout de suite.
 * - Live retry : si START échoue ou si le flux UDP s'arrête.
 */
static uint32_t s_last_arp_retry_ms = 0;
static uint32_t s_next_live_attempt_ms = 0;

/*
 * Version test propre :
 * on garde le nettoyage USB de ton code qui reconnectait mieux,
 * mais on évite de relancer le START webcam trop vite après reboot GoPro.
 */
static bool s_after_usb_reconnect = false;

#define USB_STD_SET_INTERFACE 0x0B
#define GOPRO_DATA_INTERFACE 1
#define GOPRO_DATA_ALT_SETTING 1
static usb_transfer_t *s_set_interface_transfer = NULL;
static bool s_data_alt_setting_set = false;
static void gopro_set_data_interface_alt1(void);

// Vrai si on a reçu une réponse ARP de la GoPro.
static bool s_gopro_arp_ok = false;
// Adresse MAC de la GoPro côté USB.
static uint8_t s_gopro_mac[6] = {0};
// Adresse IP de la GoPro côté USB.
static uint8_t s_gopro_ip[4] = {0};

//interface iwIP nécessaires pour créer une interface réseau lwIP.
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "lwip/ip4_addr.h"
#include "lwip/err.h"
#include "netif/ethernet.h"
#include <stdlib.h>
// Interface lwIP utilisée pour le réseau USB GoPro.
static struct netif s_usb_netif;
// Vrai si l'interface lwIP USB est prête.
static bool s_usb_netif_ready = false;
// MAC locale utilisée par l'ESP32 côté USB.
static uint8_t s_esp_usb_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
// Vrai si un envoi lwIP -> USB est déjà en cours.
static volatile bool s_usb_tx_busy = false;
static void gopro_usb_netif_start(void);
static void gopro_usb_netif_receive_frame(const uint8_t *eth, int eth_len);
static int build_ntb16_from_ethernet_frame(uint8_t *buf, int max_len, const uint8_t *eth, int eth_len);
static err_t gopro_usb_netif_init(struct netif *netif);

// Includes nécessaires pour utiliser les sockets TCP lwIP.
#include "lwip/sockets.h"
#include "lwip/inet.h"

// Port UDP utilisé par le flux webcam GoPro.
#define GOPRO_WEBCAM_UDP_PORT 5000
// Adresse du PC qui affichera la vidéo dans VLC.
#define PC_STREAM_IP "10.5.159.148"
// Port UDP écouté par VLC sur le PC.
#define PC_STREAM_PORT 5001

// ---- Ajouter juste après "#define PC_STREAM_PORT 5001" ----

typedef struct {
    uint8_t last_cc[8192];
    uint8_t pid_seen[8192];
    uint32_t datagrams;
    uint32_t cc_errors;
    uint32_t jumps;
    uint32_t duplicates;
    uint32_t sync_errors;
    uint32_t tei_errors;
    uint32_t bad_sizes;
} ts_diag_t;

static ts_diag_t s_ts_usb_diag;    // Analyse en sortie USB
static ts_diag_t s_ts_socket_diag; // Analyse après socket UDP
/*
 * Diagnostic vidéo :
 * 1 = active les compteurs MPEG-TS pour tester la qualité
 * 0 = désactive pour le vrai live, moins de charge CPU
 */
#define VIDEO_DIAG_ENABLE   1
#define VIDEO_DIAG_INTERVAL 5000

static void ts_diag_reset(ts_diag_t *diag) {
    if (diag) memset(diag, 0, sizeof(*diag));
}

static void check_mpeg_ts_continuity(ts_diag_t *diag, const uint8_t *data, int len) {
    if (!diag || !data || len <= 0) return;
    diag->datagrams++;
    if ((len % 188) != 0) diag->bad_sizes++;

    for (int off = 0; off + 188 <= len; off += 188) {
        const uint8_t *ts = data + off;
        if (ts[0] != 0x47) { diag->sync_errors++; continue; }
        if (ts[1] & 0x80) diag->tei_errors++;

        uint16_t pid = ((uint16_t)(ts[1] & 0x1F) << 8) | ts[2];
        if (pid == 0x1FFF) continue;

        uint8_t afc = (ts[3] >> 4) & 0x03;
        if (afc == 0) { diag->sync_errors++; continue; }
        bool has_payload = (afc == 1 || afc == 3);
        if (!has_payload) continue;

        uint8_t cc = ts[3] & 0x0F;
        if (diag->pid_seen[pid]) {
            uint8_t expected = (diag->last_cc[pid] + 1) & 0x0F;
            if (cc != expected) {
                diag->cc_errors++;
                if (cc == diag->last_cc[pid]) diag->duplicates++;
                else diag->jumps++;
            }
        }
        diag->last_cc[pid] = cc;
        diag->pid_seen[pid] = 1;
    }
}

static void check_usb_video_ethernet_frame(const uint8_t *eth, int eth_len) {
    if (eth_len < 14) return;
    uint16_t etype = (eth[12] << 8) | eth[13];
    int ip_off = 14;
    if (etype == 0x8100) { // VLAN
        if (eth_len < 18) return;
        etype = (eth[16] << 8) | eth[17];
        ip_off = 18;
    }
    if (etype != 0x0800) return;
    if (eth_len < ip_off + 20) return;
    const uint8_t *ip = eth + ip_off;
    if ((ip[0] >> 4) != 4) return;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || eth_len < ip_off + ihl + 8) return;
    uint16_t total = (ip[2] << 8) | ip[3];
    if (total < ihl + 8 || ip_off + total > eth_len) return;
    if (ip[9] != 17) return; // UDP seulement
    uint16_t frag = (ip[6] << 8) | ip[7];
    if (frag & 0x3FFF) return; // pas de fragments
    const uint8_t *udp = ip + ihl;
    uint16_t dport = (udp[2] << 8) | udp[3];
    if (dport != 5000) return; // port vidéo
    uint16_t udplen = (udp[4] << 8) | udp[5];
    if (udplen < 8 || udplen > total - ihl) return;
    check_mpeg_ts_continuity(&s_ts_usb_diag, udp + 8, udplen - 8);
}

// Empêche de lancer deux fois la tâche webcam.
static bool s_gopro_webcam_task_started = false;
static void gopro_start_webcam_once(void);
static void gopro_webcam_task(void *arg);

/*
 * Recovery 100 % code, sans modifier le câble USB.
 * Niveau 1 : tentative Labs !OR via HTTP USB.
 * Niveau 2 : reset logique du root port USB Host ESP32-P4.
 */
static volatile bool s_gopro_code_recovery_running = false;
static uint32_t s_live_fail_count = 0;
static void gopro_schedule_code_only_recovery(const char *reason);

static void gopro_usb_allow_live_retry(const char *reason)
{
    uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

    /*
     * On libère immédiatement la possibilité de recréer une tâche live,
     * mais on évite de spammer START : la recovery prend la main.
     */
    s_gopro_webcam_task_started = false;
    s_live_running = false;
    s_live_start_requested = false;

    s_live_fail_count++;

    ESP_LOGW(
        TAG,
        "[RECOVERY] Échec live USB n°%" PRIu32 " : %s",
        s_live_fail_count,
        reason
    );

    if (s_gopro_code_recovery_running) {
        ESP_LOGW(TAG, "[RECOVERY] Recovery déjà en cours, pas de nouvelle action");
        s_next_live_attempt_ms = now_ms + 120000;
        return;
    }

    if (!s_gopro_open || !s_usb_gopro_detected || !s_cdc_network_connected) {
        ESP_LOGW(
            TAG,
            "[RECOVERY] %s -> GoPro USB absente, attente reconnexion USB",
            reason
        );
        s_next_live_attempt_ms = now_ms + 30000;
        return;
    }

    /*
     * Ici on ne se contente plus de refaire webcam/start.
     * On lance une vraie recovery code-only : !OR si accepté, puis root-port OFF/ON.
     */
    gopro_schedule_code_only_recovery(reason);

    /* Sécurité : aucune tentative START pendant la recovery. */
    s_next_live_attempt_ms = now_ms + 120000;
}

// * Handler de la page racine
static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char *resp = "ESP32-P4 OK - USB Host test";
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// Envoie l'état de la carte en JSON quand on ouvre /status. handler de statut
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char resp[1024];

    char gopro_ip_str[16] = "0.0.0.0";
    char gopro_mac_str[18] = "00:00:00:00:00:00";

    // Si l'ARP a répondu, on prépare l'IP et la MAC de la GoPro.
    if (s_gopro_arp_ok) {
        snprintf(gopro_ip_str, sizeof(gopro_ip_str),
                 "%u.%u.%u.%u",
                 s_gopro_ip[0], s_gopro_ip[1], s_gopro_ip[2], s_gopro_ip[3]);

        snprintf(gopro_mac_str, sizeof(gopro_mac_str),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 s_gopro_mac[0], s_gopro_mac[1], s_gopro_mac[2],
                 s_gopro_mac[3], s_gopro_mac[4], s_gopro_mac[5]);
    }

    snprintf(resp, sizeof(resp),
         "{"
         "\"ok\":true,"
         "\"device\":\"ESP32-P4\","
         "\"ethernet_link\":%s,"
         "\"usb\":{"
            "\"gopro_detected\":%s,"
            "\"vid\":\"0x%04x\","
            "\"pid\":\"0x%04x\","
            "\"interfaces\":%d,"
            "\"ncm_detected\":%s,"
            "\"if0_claimed\":%s,"
            "\"if1_claimed\":%s,"
            "\"ep_notify\":\"0x%02x\","
            "\"ep_bulk_in\":\"0x%02x\","
            "\"ep_bulk_out\":\"0x%02x\","
            "\"endpoints_ready\":%s,"
            "\"ncm_params_received\":%s,"
            "\"ncm_format_set\":%s,"
            "\"packet_filter_set\":%s,"
            "\"cdc_network_connected\":%s,"
            "\"last_cdc_notification\":\"0x%02x\","
            "\"last_cdc_value\":\"0x%04x\","
            "\"gopro_arp_ok\":%s,"
            "\"gopro_usb_ip\":\"%s\","
            "\"gopro_usb_mac\":\"%s\""
         "}"
         "}",
         s_eth_link_up ? "true" : "false",
         s_usb_gopro_detected ? "true" : "false",
         s_usb_vid,
         s_usb_pid,
         s_usb_num_interfaces,
         s_usb_ncm_detected ? "true" : "false",
         s_gopro_if0_claimed ? "true" : "false",
         s_gopro_if1_claimed ? "true" : "false",
         s_gopro_ep_notify,
         s_gopro_ep_bulk_in,
         s_gopro_ep_bulk_out,
         s_gopro_endpoints_ready ? "true" : "false",
         s_ncm_params_received ? "true" : "false",
         s_ncm_format_set ? "true" : "false",
         s_packet_filter_set ? "true" : "false",
         s_cdc_network_connected ? "true" : "false",
         s_last_cdc_notification,
         s_last_cdc_value,
         s_gopro_arp_ok ? "true" : "false",
         gopro_ip_str,
         gopro_mac_str);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

//* Démarre le serveur web HTTP sur le port 80.
static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "[HTTP] Erreur démarrage serveur");
        return NULL;
    }

    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };

    httpd_uri_t status_uri = {
        .uri = "/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL
    };

    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &status_uri);

    ESP_LOGI(TAG, "[HTTP] Serveur web démarré sur le port 80");

    return server;
}

//* Handle du client USB Host.
static usb_host_client_handle_t s_usb_client_hdl = NULL;

// Affiche les données reçues en hexadécimal.
static void print_usb_data(const uint8_t *data, int len)
{
    char line[256];
    int pos = 0;

    int max_print = len;
    if (max_print > 96) {
        max_print = 96;
    }

    for (int i = 0; i < max_print && pos < sizeof(line) - 4; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02X ", data[i]);
    }

    if (len > max_print) {
        ESP_LOGI(TAG, "[USB] Données reçues (%d octets, début): %s...", len, line);
    } else {
        ESP_LOGI(TAG, "[USB] Données reçues (%d octets): %s", len, line);
    }
}
//petites fonctions de lectures
// Lit un entier 16 bits en little-endian.
static uint16_t read_le16(const uint8_t *p)
{
    return p[0] | (p[1] << 8);
}

// Lit un entier 32 bits en little-endian.
static uint32_t read_le32(const uint8_t *p)
{
    return p[0] |
           (p[1] << 8) |
           (p[2] << 16) |
           (p[3] << 24);
}

// Écrit un entier 16 bits en little-endian.
static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

// Écrit un entier 32 bits en little-endian.
static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = (v >> 24) & 0xFF;
}

// Écrit un entier 16 bits en big-endian pour Ethernet/IP/ARP.
static void write_be16(uint8_t *p, uint16_t v)
{
    p[0] = (v >> 8) & 0xFF;
    p[1] = v & 0xFF;
}

// Écrit un entier 32 bits en big-endian pour les adresses IP.
static void write_be32(uint8_t *p, uint32_t v)
{
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

//interface réseau iwIP
// Construit un bloc NTB-16 contenant une trame Ethernet déjà prête.
static int build_ntb16_from_ethernet_frame(uint8_t *buf, int max_len, const uint8_t *eth_frame, int eth_len)
{
    const int nth_offset = 0;
    const int ndp_offset = 12;
    const int eth_offset = 28;

    // Ethernet impose une taille minimale de 60 octets.
    int eth_len_padded = eth_len;
    if (eth_len_padded < 60) {
        eth_len_padded = 60;
    }

    // On aligne la taille totale sur 4 octets pour respecter le NCM.
    int ntb_len = eth_offset + eth_len_padded;
    ntb_len = (ntb_len + 3) & ~3;

    if (max_len < ntb_len) {
        ESP_LOGE(TAG, "[NCM] Buffer trop petit pour NTB Ethernet");
        return -1;
    }

    memset(buf, 0, ntb_len);

    // NTH16 : en-tête principal du bloc NCM.
    write_le32(buf + nth_offset + 0, 0x484D434E);       // "NCMH"
    write_le16(buf + nth_offset + 4, 12);               // Taille NTH16
    write_le16(buf + nth_offset + 6, s_ntb_sequence++); // Numéro de séquence
    write_le16(buf + nth_offset + 8, ntb_len);          // Taille totale NTB
    write_le16(buf + nth_offset + 10, ndp_offset);      // Position du NDP16

    // NDP16 : table qui décrit la position de la trame Ethernet.
    write_le32(buf + ndp_offset + 0, 0x304D434E);       // "NCM0"
    write_le16(buf + ndp_offset + 4, 16);               // Taille NDP16
    write_le16(buf + ndp_offset + 6, 0);                // Pas de NDP suivant

    // Première entrée : position et taille de la trame Ethernet.
    write_le16(buf + ndp_offset + 8, eth_offset);
    write_le16(buf + ndp_offset + 10, eth_len_padded);

    // Entrée finale vide.
    write_le16(buf + ndp_offset + 12, 0);
    write_le16(buf + ndp_offset + 14, 0);

    // Copie la trame Ethernet dans le bloc NTB.
    memcpy(buf + eth_offset, eth_frame, eth_len);

    return ntb_len;
}

// Callback appelé quand lwIP a fini d'envoyer une trame vers l'USB.
static void gopro_usb_tx_done_cb(usb_transfer_t *transfer)
{
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE(
            TAG,
            "[LWIP-USB] TX USB erreur status=%d",
            transfer->status
        );
    }

    s_usb_tx_busy = false;
    usb_host_transfer_free(transfer);
}

// Envoie une trame Ethernet lwIP vers la GoPro en NTB-16.
static err_t gopro_usb_send_ethernet_frame(const uint8_t *eth_frame, int eth_len)
{
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGW(TAG, "[LWIP-USB] Envoi impossible : GoPro non ouverte");
        return ERR_IF;
    }

    if (s_gopro_ep_bulk_out == 0) {
        ESP_LOGW(TAG, "[LWIP-USB] Envoi impossible : endpoint bulk OUT absent");
        return ERR_IF;
    }

    // Attend que l'envoi USB précédent soit terminé.
    // Important : pour TCP, il ne faut pas perdre de paquets ACK/DATA.
    int wait_count = 0;

    while (s_usb_tx_busy && wait_count < 1000) {
        vTaskDelay(pdMS_TO_TICKS(1));
        wait_count++;
    }

    if (s_usb_tx_busy) {
        ESP_LOGE(TAG, "[LWIP-USB] TX bloqué depuis 1000 ms, paquet non envoyé");
        return ERR_IF;
    }

    usb_transfer_t *transfer = NULL;

    esp_err_t err = usb_host_transfer_alloc(
        GOPRO_BULK_OUT_SIZE,
        0,
        &transfer
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[LWIP-USB] Allocation TX impossible: %s", esp_err_to_name(err));
        return ERR_MEM;
    }

    int ntb_len = build_ntb16_from_ethernet_frame(
        transfer->data_buffer,
        GOPRO_BULK_OUT_SIZE,
        eth_frame,
        eth_len
    );

    if (ntb_len < 0) {
        usb_host_transfer_free(transfer);
        return ERR_BUF;
    }

    transfer->device_handle = s_gopro_dev_hdl;
    transfer->bEndpointAddress = s_gopro_ep_bulk_out;
    transfer->callback = gopro_usb_tx_done_cb;
    transfer->context = NULL;
    transfer->num_bytes = ntb_len;

    s_usb_tx_busy = true;

    err = usb_host_transfer_submit(transfer);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[LWIP-USB] Submit TX impossible: %s", esp_err_to_name(err));
        s_usb_tx_busy = false;
        usb_host_transfer_free(transfer);
        return ERR_IF;
    }

    return ERR_OK;
}

// Fonction appelée par lwIP quand il veut envoyer une trame Ethernet.
static err_t gopro_usb_linkoutput(struct netif *netif, struct pbuf *p)
{
    if (p == NULL) {
        ESP_LOGE(TAG, "[LWIP-USB] pbuf NULL");
        return ERR_ARG;
    }

    if (p->tot_len == 0 || p->tot_len > 1520) {
        ESP_LOGE(TAG, "[LWIP-USB] Trame trop grande ou vide: %d", p->tot_len);
        return ERR_BUF;
    }

    // On évite un gros tableau local pour ne pas remplir la pile lwIP.
    uint8_t *eth_frame = malloc(p->tot_len);

    if (eth_frame == NULL) {
        ESP_LOGE(TAG, "[LWIP-USB] malloc eth_frame impossible");
        return ERR_MEM;
    }

    // Copie la trame lwIP dans un buffer continu.
    uint16_t copied = pbuf_copy_partial(p, eth_frame, p->tot_len, 0);

    if (copied != p->tot_len) {
        ESP_LOGE(TAG, "[LWIP-USB] Copie pbuf incomplète");
        free(eth_frame);
        return ERR_BUF;
    }


    // Envoie la trame Ethernet vers la GoPro en USB CDC-NCM.
    err_t result = gopro_usb_send_ethernet_frame(eth_frame, p->tot_len);

    free(eth_frame);

    return result;
}

// Initialise les paramètres bas niveau de l'interface lwIP USB.
static err_t gopro_usb_netif_init(struct netif *netif)
{
    netif->name[0] = 'u';
    netif->name[1] = 's';

    // Fonction lwIP utilisée pour envoyer des paquets IPv4.
    netif->output = etharp_output;

    // Fonction appelée quand lwIP veut sortir une trame Ethernet.
    netif->linkoutput = gopro_usb_linkoutput;

    // MTU Ethernet classique.
    netif->mtu = 1500;

    // Adresse MAC de l'ESP32 côté USB.
    netif->hwaddr_len = 6;
    memcpy(netif->hwaddr, s_esp_usb_mac, 6);

    // Interface Ethernet avec ARP.
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;

#ifdef NETIF_FLAG_ETHERNET
    netif->flags |= NETIF_FLAG_ETHERNET;
#endif

    ESP_LOGI(TAG, "[LWIP-USB] Init interface lwIP USB OK");

    return ERR_OK;
}

// Crée l'interface réseau USB dans le thread TCP/IP lwIP.
static void gopro_usb_netif_start_cb(void *arg)
{
    if (s_usb_netif_ready) {
        return;
    }

    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gateway;

    // IP locale de l'ESP32 sur le lien USB GoPro.
    IP4_ADDR(&ipaddr, 172, 20, 140, 50);

    // Réseau USB GoPro.
    IP4_ADDR(&netmask, 255, 255, 255, 0);

    // Pas de vraie passerelle, on met la GoPro.
    IP4_ADDR(&gateway, 172, 20, 140, 51);

    struct netif *netif = netif_add(
        &s_usb_netif,
        &ipaddr,
        &netmask,
        &gateway,
        NULL,
        gopro_usb_netif_init,
        tcpip_input
    );

    if (netif == NULL) {
        ESP_LOGE(TAG, "[LWIP-USB] Création interface USB échouée");
        return;
    }

    netif_set_up(&s_usb_netif);
    netif_set_link_up(&s_usb_netif);

    s_usb_netif_ready = true;

    ESP_LOGI(TAG, "[LWIP-USB] Interface us%d prête : 172.20.140.50/24", s_usb_netif.num);
}

// Demande la création de l'interface réseau USB lwIP.
static void gopro_usb_netif_start(void)
{
    if (s_usb_netif_ready) {
        return;
    }

    tcpip_callback(gopro_usb_netif_start_cb, NULL);
}

// Injecte une trame Ethernet reçue par USB dans lwIP.
static void gopro_usb_netif_receive_frame(const uint8_t *eth, int eth_len)
{
    if (!s_usb_netif_ready) {
        return;
    }

    if (eth_len <= 0 || eth_len > 1520) {
        ESP_LOGW(TAG, "[LWIP-USB] RX ignoré, taille invalide=%d", eth_len);
        return;
    }

    struct pbuf *p = pbuf_alloc(PBUF_RAW, eth_len, PBUF_POOL);

    if (p == NULL) {
        ESP_LOGE(TAG, "[LWIP-USB] pbuf_alloc impossible");
        return;
    }

    if (pbuf_take(p, eth, eth_len) != ERR_OK) {
        ESP_LOGE(TAG, "[LWIP-USB] pbuf_take impossible");
        pbuf_free(p);
        return;
    }

    // On donne la trame Ethernet complète à lwIP via le thread TCP/IP.
    err_t err = tcpip_inpkt(p, &s_usb_netif, ethernet_input);

    if (err != ERR_OK) {
        ESP_LOGE(TAG, "[LWIP-USB] Injection lwIP impossible, err=%d", err);
        pbuf_free(p);
        return;
    }
}

// Lance une seule fois la tâche qui démarre la webcam et reçoit le flux UDP.
static void gopro_start_webcam_once(void)
{
    if (s_gopro_webcam_task_started) {
        return;
    }

    s_gopro_webcam_task_started = true;

    BaseType_t created = xTaskCreatePinnedToCore(
        gopro_webcam_task,
        "gopro_webcam",
        10240,   // plus de pile pour les logs
        NULL,
        18,      // priorité élevée
        NULL,
        0        // cœur 0, libère le cœur 1 pour l'USB
    );

    if (created != pdPASS) {
        s_gopro_webcam_task_started = false;
        ESP_LOGE(TAG, "[WEBCAM] Impossible de créer la tâche webcam");
    }
}

// Démarre le mode webcam par HTTP puis reçoit directement le flux UDP sur le port choisi.
static void gopro_webcam_task(void *arg)
{
    (void)arg;

    // Laisse à lwIP et à l'ARP le temps d'être complètement prêts.
    vTaskDelay(pdMS_TO_TICKS(1000));

    if (!s_usb_netif_ready ||
        !s_cdc_network_connected ||
        !s_eth_link_up ||
        s_eth_ip_addr == 0) {

        ESP_LOGE(
            TAG,
            "[WEBCAM] Réseau non prêt : "
            "USB=%d CDC=%d Ethernet=%d IP=%s",
            s_usb_netif_ready,
            s_cdc_network_connected,
            s_eth_link_up,
            s_eth_ip_addr != 0 ? "OK" : "ABSENTE"
        );
        gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
        vTaskDelete(NULL);
        return;
    }

    // 1) Ouvre d'abord le port UDP qui recevra la vidéo.
    int udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    /*
    * Augmente la quantité de vidéo que lwIP peut garder
    * temporairement lorsque la tâche ou le W5500 prend du retard.
    */
    int udp_rcvbuf = 128 * 1024;

    if (setsockopt(
            udp_sock,
            SOL_SOCKET,
            SO_RCVBUF,
            &udp_rcvbuf,
            sizeof(udp_rcvbuf)
        ) < 0) {

        ESP_LOGW(
            TAG,
            "[WEBCAM] SO_RCVBUF non appliqué, errno=%d",
            errno
        );
    }
    else {
        ESP_LOGI(
            TAG,
            "[WEBCAM] Buffer UDP RX demandé : %d octets",
            udp_rcvbuf
        );
    }

    if (udp_sock < 0) {
        ESP_LOGE(TAG, "[WEBCAM] socket UDP impossible, errno=%d", errno);
        gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in udp_addr;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = htons(GOPRO_WEBCAM_UDP_PORT);
    udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(udp_sock, (struct sockaddr *)&udp_addr, sizeof(udp_addr)) < 0) {
        ESP_LOGE(TAG, "[WEBCAM] bind UDP 0.0.0.0:%d impossible, errno=%d",
                 GOPRO_WEBCAM_UDP_PORT, errno);
        close(udp_sock);
        gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
        vTaskDelete(NULL);
        return;
    }

    struct timeval udp_timeout = {
        .tv_sec = 5,
        .tv_usec = 0
    };
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &udp_timeout, sizeof(udp_timeout));

    ESP_LOGI(TAG, "[WEBCAM] Écoute UDP prête sur 0.0.0.0:%d",
             GOPRO_WEBCAM_UDP_PORT);

    /*
    * Socket utilisée pour retransmettre le flux
    * reçu de la GoPro vers le PC par Ethernet.
    */
    int pc_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (pc_sock < 0) {
        ESP_LOGE(
            TAG,
            "[FORWARD] Création socket vers PC impossible, errno=%d",
            errno
        );

        close(udp_sock);
        gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
        vTaskDelete(NULL);
        return;
    }

    /*
 * Force la retransmission à passer par
 * l'interface Ethernet W5500 et non par l'interface USB GoPro.
 */
struct sockaddr_in pc_local_addr;
memset(&pc_local_addr, 0, sizeof(pc_local_addr));

pc_local_addr.sin_family = AF_INET;
pc_local_addr.sin_port = htons(0);
pc_local_addr.sin_addr.s_addr = s_eth_ip_addr;

if (bind(
        pc_sock,
        (struct sockaddr *)&pc_local_addr,
        sizeof(pc_local_addr)
    ) < 0) {

    ESP_LOGE(
        TAG,
        "[FORWARD] bind du socket sur Ethernet échoué, errno=%d",
        errno
    );

    close(pc_sock);
    close(udp_sock);

    gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
    vTaskDelete(NULL);
    return;
}

ESP_LOGI(
    TAG,
    "[FORWARD] Socket lié à l'interface RJ45 ESP32-P4"
);

    struct sockaddr_in pc_addr;
    memset(&pc_addr, 0, sizeof(pc_addr));

    pc_addr.sin_family = AF_INET;
    pc_addr.sin_port = htons(PC_STREAM_PORT);
    pc_addr.sin_addr.s_addr = inet_addr(PC_STREAM_IP);

    ESP_LOGI(
        TAG,
        "[FORWARD] Destination PC : %s:%d",
        PC_STREAM_IP,
        PC_STREAM_PORT
    );

    /*
    * Prépare le chemin Ethernet avant le début réel de la vidéo.
    *
    * Auparavant, le premier paquet vidéo était perdu pendant
    * la résolution ARP vers le PC.
    *
    * On envoie ici des paquets MPEG-TS nuls et valides.
    * VLC peut les recevoir sans afficher d'erreur.
    */
    uint8_t warmup_ts[188 * 7];

    memset(warmup_ts, 0xFF, sizeof(warmup_ts));

    for (int packet = 0; packet < 7; packet++) {
        int offset = packet * 188;

        // Octet de synchronisation MPEG-TS.
        warmup_ts[offset + 0] = 0x47;

        // PID 0x1FFF = paquet nul MPEG-TS.
        warmup_ts[offset + 1] = 0x1F;
        warmup_ts[offset + 2] = 0xFF;

        // Payload présent + compteur de continuité.
        warmup_ts[offset + 3] =
            0x10 | (packet & 0x0F);
    }

    /*
    * Plusieurs envois permettent :
    * - de résoudre l'adresse ARP du PC ;
    * - de préparer les buffers Ethernet ;
    * - de vérifier le chemin vers le PC avant la vraie vidéo.
    */
    for (int attempt = 0; attempt < 20; attempt++) {
        int warmup_sent = sendto(
            pc_sock,
            warmup_ts,
            sizeof(warmup_ts),
            0,
            (struct sockaddr *)&pc_addr,
            sizeof(pc_addr)
        );

        if (warmup_sent < 0 && errno != ENOMEM) {
            ESP_LOGW(
                TAG,
                "[FORWARD] Échec préparation Ethernet, errno=%d",
                errno
            );
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /*
    * Laisse au pilote le temps de terminer
    * la préparation de la liaison radio.
    */
    vTaskDelay(pdMS_TO_TICKS(300));

    ESP_LOGI(
        TAG,
        "[FORWARD] Chemin Ethernet préparé avant démarrage webcam"
    );

    // ******************** AJOUT : Arrêt propre de toute session webcam précédente ********************
    {
        int stop_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
        if (stop_sock >= 0) {
            struct timeval stop_timeout = { .tv_sec = 2, .tv_usec = 0 };
            setsockopt(stop_sock, SOL_SOCKET, SO_RCVTIMEO, &stop_timeout, sizeof(stop_timeout));
            setsockopt(stop_sock, SOL_SOCKET, SO_SNDTIMEO, &stop_timeout, sizeof(stop_timeout));

            // Bind sur l'interface USB 172.20.140.50
            struct sockaddr_in stop_local;
            memset(&stop_local, 0, sizeof(stop_local));
            stop_local.sin_family = AF_INET;
            stop_local.sin_port = htons(0);
            stop_local.sin_addr.s_addr = inet_addr("172.20.140.50");
            if (bind(stop_sock, (struct sockaddr *)&stop_local, sizeof(stop_local)) == 0) {
                struct sockaddr_in stop_gopro;
                memset(&stop_gopro, 0, sizeof(stop_gopro));
                stop_gopro.sin_family = AF_INET;
                stop_gopro.sin_port = htons(8080);
                stop_gopro.sin_addr.s_addr = inet_addr("172.20.140.51");

                if (connect(stop_sock, (struct sockaddr *)&stop_gopro, sizeof(stop_gopro)) == 0) {
                    const char *stop_req =
                        "GET /gopro/webcam/stop HTTP/1.1\r\n"
                        "Host: 172.20.140.51:8080\r\n"
                        "Connection: close\r\n"
                        "\r\n";
                    send(stop_sock, stop_req, strlen(stop_req), 0);

                    char dummy[64];
                    recv(stop_sock, dummy, sizeof(dummy)-1, 0); // on ignore la réponse

                    ESP_LOGI(TAG, "[WEBCAM] Stop envoyé pour nettoyer l'ancienne session");
                } else {
                    ESP_LOGW(TAG, "[WEBCAM] Stop impossible, connexion refusée");
                }
            }
            close(stop_sock);
        }
    }

    /*
    * Envoie directement la commande de démarrage webcam.
    *
    * On ne fait plus :
    * - status
    * - stop
    * - exit
    *
    * On revient à la commande qui avait déjà réussi
    * à lancer le live dans l'ancienne version.
    */
    int http_sock = socket(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_IP
    );

    if (http_sock < 0) {
        ESP_LOGE(
            TAG,
            "[WEBCAM] socket HTTP impossible, errno=%d",
            errno
        );

        close(pc_sock);
        close(udp_sock);

        gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
        vTaskDelete(NULL);
        return;
    }

    /*
    * Timeout pour éviter de rester bloqué
    * si la GoPro ne répond pas.
    */
    struct timeval http_timeout = {
        .tv_sec = 5,
        .tv_usec = 0
    };

    setsockopt(
        http_sock,
        SOL_SOCKET,
        SO_RCVTIMEO,
        &http_timeout,
        sizeof(http_timeout)
    );

    setsockopt(
        http_sock,
        SOL_SOCKET,
        SO_SNDTIMEO,
        &http_timeout,
        sizeof(http_timeout)
    );

    /*
    * Force cette connexion HTTP à sortir par
    * l'interface USB GoPro 172.20.140.50,
    * et non par le W5500.
    */
    struct sockaddr_in http_local_addr;
    memset(
        &http_local_addr,
        0,
        sizeof(http_local_addr)
    );

    http_local_addr.sin_family = AF_INET;
    http_local_addr.sin_port = htons(0);
    http_local_addr.sin_addr.s_addr =
        inet_addr("172.20.140.50");

    if (bind(
            http_sock,
            (struct sockaddr *)&http_local_addr,
            sizeof(http_local_addr)
        ) < 0) {

        ESP_LOGE(
            TAG,
            "[WEBCAM] bind HTTP USB échoué, errno=%d",
            errno
        );

        close(http_sock);
        close(pc_sock);
        close(udp_sock);

        gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
        vTaskDelete(NULL);
        return;
    }

    /*
    * Serveur HTTP de la GoPro.
    */
    struct sockaddr_in gopro_addr;
    memset(
        &gopro_addr,
        0,
        sizeof(gopro_addr)
    );

    gopro_addr.sin_family = AF_INET;
    gopro_addr.sin_port = htons(8080);
    gopro_addr.sin_addr.s_addr =
        inet_addr("172.20.140.51");

    ESP_LOGI(
        TAG,
        "[WEBCAM] Connexion HTTP vers 172.20.140.51:8080"
    );

    if (connect(
            http_sock,
            (struct sockaddr *)&gopro_addr,
            sizeof(gopro_addr)
        ) < 0) {

        ESP_LOGE(
            TAG,
            "[WEBCAM] Connexion HTTP GoPro impossible, errno=%d",
            errno
        );

        close(http_sock);
        close(pc_sock);
        close(udp_sock);

        gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
        vTaskDelete(NULL);
        return;
    }

    /*
    * Reprise exacte de la commande qui avait déjà
    * permis de lancer le live :
    *
    * res=7  : résolution demandée
    * fov=2  : champ de vision
    * port=5000 : port UDP écouté par l'ESP32
    */
    const char *request =
        "GET /gopro/webcam/start?res=7&fov=0&port=5000 HTTP/1.1\r\n"
        "Host: 172.20.140.51:8080\r\n"
        "Connection: close\r\n"
        "\r\n";


    ESP_LOGW(
        TAG,
        "[WEBCAM] START webcam simple : res=7, after_reconnect=%d",
        s_after_usb_reconnect
    );

    ESP_LOGI(
        TAG,
        "[WEBCAM] Envoi direct de la commande START"
    );

    int request_length = strlen(request);
    int sent_total = 0;
    ts_diag_reset(&s_ts_usb_diag);
    ts_diag_reset(&s_ts_socket_diag);

    while (sent_total < request_length) {
        int sent = send(
            http_sock,
            request + sent_total,
            request_length - sent_total,
            0
        );

        if (sent <= 0) {
            ESP_LOGE(
                TAG,
                "[WEBCAM] Envoi START impossible, errno=%d",
                errno
            );

            close(http_sock);
            close(pc_sock);
            close(udp_sock);

            gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
            vTaskDelete(NULL);
            return;
        }

        sent_total += sent;
    }

    /*
    * Récupération de la réponse HTTP complète.
    */
    char http_response[512];
    int response_len = 0;

    while (response_len < (int)sizeof(http_response) - 1) {
        int received = recv(
            http_sock,
            http_response + response_len,
            sizeof(http_response) - 1 - response_len,
            0
        );

        if (received > 0) {
            response_len += received;
            continue;
        }

        break;
    }

    http_response[response_len] = '\0';

    close(http_sock);

    ESP_LOGI(
        TAG,
        "[WEBCAM] Réponse HTTP GoPro:\n%s",
        http_response
    );

    /*
    * Vérifie si la GoPro a accepté START.
    */
    bool http_ok =
        strstr(
            http_response,
            "HTTP/1.1 200 OK"
        ) != NULL;

    bool gopro_ok =
        strstr(
            http_response,
            "\"error\": 0"
        ) != NULL ||
        strstr(
            http_response,
            "\"error\":0"
        ) != NULL;

    if (response_len == 0) {
        /*
        * Certaines fois, la réponse HTTP peut arriver
        * en retard alors que le flux UDP démarre.
        *
        * On continue donc vers la réception UDP.
        */
        ESP_LOGW(
            TAG,
            "[WEBCAM] Réponse HTTP vide, "
            "vérification du flux UDP"
        );
    }
    else if (http_ok && gopro_ok) {
        ESP_LOGI(
            TAG,
            "[WEBCAM] Commande START acceptée"
        );

        /* Le START a réussi : la session après reboot est redevenue saine. */
        s_after_usb_reconnect = false;
    }
    else {
        ESP_LOGE(
            TAG,
            "[WEBCAM] Commande START refusée"
        );

        close(pc_sock);
        close(udp_sock);

        gopro_usb_allow_live_retry("erreur pendant démarrage webcam");
        vTaskDelete(NULL);
        return;
    }

    // 3) Le live est réellement démarré quand les premiers datagrammes UDP arrivent.
    ESP_LOGI(TAG, "[WEBCAM] Attente des paquets vidéo UDP sur le port %d...",
             GOPRO_WEBCAM_UDP_PORT);

    uint8_t udp_buffer[2048];
    uint32_t packet_count = 0;
    uint64_t total_bytes = 0;
    int timeout_count = 0;

    // Compteurs de retransmission vers le PC.
    uint32_t forwarded_packets = 0;
    uint32_t dropped_packets = 0;

    while (s_cdc_network_connected && s_gopro_open) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);

        int len = recvfrom(udp_sock,
                           udp_buffer,
                           sizeof(udp_buffer),
                           0,
                           (struct sockaddr *)&from_addr,
                           &from_len);

        if (len > 0) {
            if (VIDEO_DIAG_ENABLE) {
                check_mpeg_ts_continuity(&s_ts_socket_diag, udp_buffer, len);
            }
            packet_count++;
            total_bytes += (uint32_t)len;
            timeout_count = 0;

            /*
            * Envoie le paquet vidéo vers le PC.
            */
            int forwarded = -1;
            int send_error = 0;

            /*
            * Plusieurs tentatives courtes :
            * un paquet MPEG-TS perdu peut rendre une grande partie
            * de l'image pixélisée jusqu'à la prochaine image clé.
            */
            for (int attempt = 0; attempt < 5; attempt++) {

                forwarded = sendto(
                    pc_sock,
                    udp_buffer,
                    len,
                    0,
                    (struct sockaddr *)&pc_addr,
                    sizeof(pc_addr)
                );

                if (forwarded == len) {
                    break;
                }

                send_error = errno;

                /*
                * Si ce n'est pas un manque temporaire de mémoire,
                * recommencer ne servirait à rien.
                */
                if (send_error != ENOMEM &&
                    send_error != EAGAIN &&
                    send_error != EWOULDBLOCK) {

                    break;
                }

                /*
                * Laisse les tâches Ethernet traiter les buffers,
                * sans attendre un délai de plusieurs millisecondes.
                */
                taskYIELD();
            }

            if (forwarded == len) {
                forwarded_packets++;
            }
            else {
                int saved_errno = send_error;
                dropped_packets++;

                if (dropped_packets == 1 ||
                    (dropped_packets % 100) == 0) {

                    ESP_LOGW(
                        TAG,
                        "[FORWARD] Paquets perdus=%" PRIu32
                        ", envoyés=%" PRIu32
                        ", erreur=%d",
                        dropped_packets,
                        forwarded_packets,
                        saved_errno
                    );
                }
            }

            if (packet_count == 1) {
                s_live_running = true;
                s_live_has_ever_run = true;

                ESP_LOGI(
                    TAG,
                    "[WEBCAM] LIVE OK : premier paquet UDP reçu (%d octets)",
                    len
                );

                print_usb_data(
                    udp_buffer,
                    len > 32 ? 32 : len
                );
            }
            else if ((packet_count % 5000) == 0) {
                ESP_LOGI(
                    TAG,
                    "[WEBCAM] Flux actif : %" PRIu32 " paquets, %" PRIu64 " octets",
                    packet_count,
                    total_bytes
                );

                ESP_LOGI(
                    TAG,
                    "[FORWARD] Envoyés=%" PRIu32 ", perdus=%" PRIu32,
                    forwarded_packets,
                    dropped_packets
                );

                if (VIDEO_DIAG_ENABLE) {
                    uint32_t cc_per_10000 = 0;

                    if (s_ts_socket_diag.datagrams > 0) {
                        cc_per_10000 =
                            (uint32_t)(((uint64_t)s_ts_socket_diag.cc_errors * 10000ULL) /
                                    s_ts_socket_diag.datagrams);
                    }

                    ESP_LOGI(
                        TAG,
                        "[TS-SOCKET] udp=%" PRIu32
                        " cc=%" PRIu32
                        " jumps=%" PRIu32
                        " dup=%" PRIu32
                        " sync=%" PRIu32
                        " bad=%" PRIu32
                        " cc_x10000=%" PRIu32,
                        s_ts_socket_diag.datagrams,
                        s_ts_socket_diag.cc_errors,
                        s_ts_socket_diag.jumps,
                        s_ts_socket_diag.duplicates,
                        s_ts_socket_diag.sync_errors,
                        s_ts_socket_diag.bad_sizes,
                        cc_per_10000
                    );
                }
            }

            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            timeout_count++;
            ESP_LOGW(TAG, "[WEBCAM] Aucun paquet UDP reçu depuis 5 s (%d/3)", timeout_count);

            if (timeout_count >= 3) {
                if (packet_count == 0) {
                    ESP_LOGE(
                        TAG,
                        "[WEBCAM] Aucun paquet UDP reçu"
                    );
                } else {
                    ESP_LOGW(
                        TAG,
                        "[WEBCAM] Flux interrompu après %" PRIu32
                        " paquets et %" PRIu64 " octets",
                        packet_count,
                        total_bytes
                    );
                }

                break;
            }

            continue;
        }

        ESP_LOGE(TAG, "[WEBCAM] Erreur recvfrom, errno=%d", errno);
        break;
    }

        close(udp_sock);
        close(pc_sock);

        ESP_LOGW(TAG, "[WEBCAM] Réception UDP arrêtée");

        /*
        * Ancien projet BLE/Wi-Fi :
        * quand le live est perdu, on remet les flags à zéro.
        *
        * Ici en USB :
        * si la GoPro redémarre, si le flux UDP coupe,
        * ou si aucun paquet vidéo n'arrive, on autorise app_main()
        * à relancer une nouvelle tentative.
        */
        gopro_usb_allow_live_retry("flux UDP arrêté");

        vTaskDelete(NULL);
}

static void gopro_test_bulk_out(void);
// Décode une notification USB CDC simple.
static void decode_cdc_notification(const uint8_t *data, int len)
{
    if (len < 8) {
        ESP_LOGW(TAG, "[USB] Notification trop courte");
        return;
    }

    uint8_t bmRequestType = data[0];
    uint8_t bNotification = data[1];
    uint16_t wValue = data[2] | (data[3] << 8);
    uint16_t wIndex = data[4] | (data[5] << 8);
    uint16_t wLength = data[6] | (data[7] << 8);
    s_last_cdc_notification = bNotification;
    s_last_cdc_value = wValue;

    ESP_LOGI(TAG, "[USB] Notification CDC:");
    ESP_LOGI(TAG, "[USB]  bmRequestType=0x%02x", bmRequestType);
    ESP_LOGI(TAG, "[USB]  bNotification=0x%02x", bNotification);
    ESP_LOGI(TAG, "[USB]  wValue=0x%04x", wValue);
    ESP_LOGI(TAG, "[USB]  wIndex=0x%04x", wIndex);
    ESP_LOGI(TAG, "[USB]  wLength=%d", wLength);

    if (bNotification == 0x00) {
        ESP_LOGI(TAG, "[USB]  Type: NETWORK_CONNECTION");

        if (wValue == 1) {
            s_cdc_network_connected = true;
            ESP_LOGI(TAG, "[USB]  Réseau CDC-NCM connecté");

            // On ne lance pas le bulk OUT directement dans le callback.
            // On demande à la boucle principale de le faire après un petit délai.
            if (!s_network_test_started) {
                s_network_test_pending = true;
            }
        } else {
            s_cdc_network_connected = false;
            ESP_LOGW(TAG, "[USB]  Réseau CDC-NCM non connecté");
        }
    } else if (bNotification == 0x2A) {
        ESP_LOGI(TAG, "[USB]  Type: CONNECTION_SPEED_CHANGE");
    } else {
        ESP_LOGI(TAG, "[USB]  Type: notification inconnue");
    }
}

//Décode la réponse GET_NTB_PARAMETERS.  //fonction de décodage des paramètres NCM
static void decode_ncm_ntb_parameters(const uint8_t *data, int len)
{
    if (len < CDC_NCM_NTB_PARAMETERS_SIZE) {
        ESP_LOGW(TAG, "[USB] Réponse NTB trop courte: %d", len);
        return;
    }

    uint16_t wLength = read_le16(data + 0);
    uint16_t bmNtbFormatsSupported = read_le16(data + 2);
    uint32_t dwNtbInMaxSize = read_le32(data + 4);
    uint16_t wNdpInDivisor = read_le16(data + 8);
    uint16_t wNdpInPayloadRemainder = read_le16(data + 10);
    uint16_t wNdpInAlignment = read_le16(data + 12);
    uint32_t dwNtbOutMaxSize = read_le32(data + 16);
    uint16_t wNdpOutDivisor = read_le16(data + 20);
    uint16_t wNdpOutPayloadRemainder = read_le16(data + 22);
    uint16_t wNdpOutAlignment = read_le16(data + 24);
    uint16_t wNtbOutMaxDatagrams = read_le16(data + 26);

    ESP_LOGI(TAG, "[USB] Paramètres NCM reçus:");
    ESP_LOGI(TAG, "[USB]  wLength=%d", wLength);
    ESP_LOGI(TAG, "[USB]  formats=0x%04x", bmNtbFormatsSupported);
    ESP_LOGI(TAG, "[USB]  IN max size=%lu", dwNtbInMaxSize);
    ESP_LOGI(TAG, "[USB]  IN divisor=%d", wNdpInDivisor);
    ESP_LOGI(TAG, "[USB]  IN remainder=%d", wNdpInPayloadRemainder);
    ESP_LOGI(TAG, "[USB]  IN alignment=%d", wNdpInAlignment);
    ESP_LOGI(TAG, "[USB]  OUT max size=%lu", dwNtbOutMaxSize);
    ESP_LOGI(TAG, "[USB]  OUT divisor=%d", wNdpOutDivisor);
    ESP_LOGI(TAG, "[USB]  OUT remainder=%d", wNdpOutPayloadRemainder);
    ESP_LOGI(TAG, "[USB]  OUT alignment=%d", wNdpOutAlignment);
    ESP_LOGI(TAG, "[USB]  OUT max datagrams=%d", wNtbOutMaxDatagrams);

    s_ncm_params_received = true;
}

// Fonction appelée quand la GoPro répond à la requête NCM.//callback du transfert control
static void ncm_control_transfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "[USB] Réponse control NCM reçue, taille=%d", transfer->actual_num_bytes);

        int payload_len = transfer->actual_num_bytes - USB_SETUP_PACKET_SIZE;

        if (payload_len > 0) {
            const uint8_t *payload = transfer->data_buffer + USB_SETUP_PACKET_SIZE;

            // Affiche la réponse brute.
            print_usb_data(payload, payload_len);

            // Décode la réponse NCM.
            decode_ncm_ntb_parameters(payload, payload_len);
        }
    } else {
        ESP_LOGW(TAG, "[USB] Réponse control NCM erreur status=%d", transfer->status);
    }
}
//déclaration de fonction car on va appeler gopro_start_bulk_in_read() dans le callback du packet filter, alors que la fonction est écrite plus bas dans ton fichier.
static void gopro_start_bulk_in_read(void);
static void gopro_test_bulk_out(void);
static void gopro_set_ntb_input_size(void);
static void gopro_set_packet_filter(void);
static void gopro_set_ntb_input_size(void);
// callback appelée après SET_ETHERNET_PACKET_FILTER.//// Fonction appelée quand la commande packet filter est terminée.
static void packet_filter_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_packet_filter_set = true;
        ESP_LOGI(TAG, "[USB] SET_ETHERNET_PACKET_FILTER OK");

        // On teste d'abord l'envoi vers la GoPro.
        // gopro_start_bulk_in_read();
        // On attend d'abord de voir si NETWORK_CONNECTION passe à 1.
        // gopro_test_bulk_out();
    } else {
        ESP_LOGW(TAG, "[USB] SET_ETHERNET_PACKET_FILTER erreur status=%d", transfer->status);
    }
}

static void gopro_set_packet_filter(void);  //la fonction doit etre appelé acant la fonction ncm_set_format

// Fonction appelée après SET_NTB_FORMAT. callback
static void ncm_set_format_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_ncm_format_set = true;
        ESP_LOGI(TAG, "[USB] SET_NTB_FORMAT OK : format NTB-16 sélectionné");

        // Après le choix du format, on configure la taille NTB.
        gopro_set_ntb_input_size();

    } else {
        ESP_LOGW(TAG, "[USB] SET_NTB_FORMAT erreur status=%d", transfer->status);
    }
}

// Fonction appelée après SET_NTB_INPUT_SIZE.
static void ncm_set_input_size_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_ncm_input_size_set = true;
        ESP_LOGI(TAG, "[USB] SET_NTB_INPUT_SIZE OK");

        // Après SET_NTB_INPUT_SIZE, on active le filtre réseau.
        gopro_set_packet_filter();
    } else {
        ESP_LOGW(TAG, "[USB] SET_NTB_INPUT_SIZE erreur status=%d", transfer->status);
    }
}
// Configure la taille maximale des blocs NCM reçus.
static void gopro_set_ntb_input_size(void)
{
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGE(TAG, "[USB] Impossible SET_NTB_INPUT_SIZE : GoPro pas ouverte");
        return;
    }

    if (!s_gopro_if0_claimed) {
        ESP_LOGE(TAG, "[USB] Impossible SET_NTB_INPUT_SIZE : interface 0 non claim");
        return;
    }

    if (s_ncm_input_size_transfer != NULL) {
        ESP_LOGW(TAG, "[USB] SET_NTB_INPUT_SIZE déjà envoyé");
        return;
    }

    esp_err_t err = usb_host_transfer_alloc(
        USB_SETUP_PACKET_SIZE + 4,
        0,
        &s_ncm_input_size_transfer
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[USB] Allocation SET_NTB_INPUT_SIZE échouée: %s", esp_err_to_name(err));
        return;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ncm_input_size_transfer->data_buffer;

    // Requête OUT, classe CDC-NCM, vers interface.
    setup->bmRequestType = 0x21;

    // Commande CDC-NCM : SET_NTB_INPUT_SIZE.
    setup->bRequest = CDC_NCM_SET_NTB_INPUT_SIZE;

    setup->wValue = 0;
    setup->wIndex = CDC_NCM_CONTROL_INTERFACE;

    // 4 octets de données après le setup.
    setup->wLength = 4;

    // Donnée envoyée : 16384 en little-endian.
    uint8_t *payload = s_ncm_input_size_transfer->data_buffer + USB_SETUP_PACKET_SIZE;
    write_le32(payload, CDC_NCM_INPUT_SIZE);

    s_ncm_input_size_transfer->device_handle = s_gopro_dev_hdl;
    s_ncm_input_size_transfer->bEndpointAddress = 0;
    s_ncm_input_size_transfer->callback = ncm_set_input_size_cb;
    s_ncm_input_size_transfer->context = NULL;
    s_ncm_input_size_transfer->num_bytes = USB_SETUP_PACKET_SIZE + 4;

    err = usb_host_transfer_submit_control(s_usb_client_hdl, s_ncm_input_size_transfer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[USB] SET_NTB_INPUT_SIZE envoyé");
    } else {
        ESP_LOGE(TAG, "[USB] SET_NTB_INPUT_SIZE échec: %s", esp_err_to_name(err));
    }
}

// Active le filtre réseau de la GoPro.
static void gopro_set_packet_filter(void)
{
    // Vérifie que la GoPro est ouverte.
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGE(TAG, "[USB] Impossible PACKET_FILTER : GoPro pas ouverte");
        return;
    }

    // Vérifie que l'interface de contrôle est réservée.
    if (!s_gopro_if0_claimed) {
        ESP_LOGE(TAG, "[USB] Impossible PACKET_FILTER : interface 0 non claim");
        return;
    }

    // Évite d'envoyer deux fois la commande.
    if (s_packet_filter_transfer != NULL) {
        ESP_LOGW(TAG, "[USB] PACKET_FILTER déjà envoyé");
        return;
    }

    // Crée un transfert control sans données.
    esp_err_t err = usb_host_transfer_alloc(
        USB_SETUP_PACKET_SIZE,
        0,
        &s_packet_filter_transfer
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[USB] Allocation PACKET_FILTER échouée: %s", esp_err_to_name(err));
        return;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_packet_filter_transfer->data_buffer;

    // Requête OUT, classe CDC, vers interface.
    setup->bmRequestType = 0x21;

    // Commande CDC : SET_ETHERNET_PACKET_FILTER.
    setup->bRequest = CDC_SET_ETHERNET_PACKET_FILTER;

    // Paquets acceptés : directed + broadcast.
    setup->wValue = CDC_PACKET_FILTER_DEFAULT;

    // Interface 0 = interface de contrôle CDC-NCM.
    setup->wIndex = CDC_NCM_CONTROL_INTERFACE;

    // Pas de données après le setup.
    setup->wLength = 0;

    s_packet_filter_transfer->device_handle = s_gopro_dev_hdl;
    s_packet_filter_transfer->bEndpointAddress = 0;
    s_packet_filter_transfer->callback = packet_filter_cb; //appel d'un callback
    s_packet_filter_transfer->context = NULL;

    // Taille totale = seulement le setup USB.
    s_packet_filter_transfer->num_bytes = USB_SETUP_PACKET_SIZE;

    // Envoie la commande control.
    err = usb_host_transfer_submit_control(s_usb_client_hdl, s_packet_filter_transfer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[USB] SET_ETHERNET_PACKET_FILTER envoyé");
    } else {
        ESP_LOGE(TAG, "[USB] SET_ETHERNET_PACKET_FILTER échec: %s", esp_err_to_name(err));
    }
}

// Choisit le format NTB utilisé pour les paquets NCM.
static void gopro_set_ntb_format_16(void)
{
    // Vérifie que la GoPro est ouverte.
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGE(TAG, "[USB] Impossible SET_NTB_FORMAT : GoPro pas ouverte");
        return;
    }

    // Vérifie que l'interface de contrôle est réservée.
    if (!s_gopro_if0_claimed) {
        ESP_LOGE(TAG, "[USB] Impossible SET_NTB_FORMAT : interface 0 non claim");
        return;
    }

    // Évite d'envoyer la commande plusieurs fois.
    if (s_ncm_format_transfer != NULL) {
        ESP_LOGW(TAG, "[USB] SET_NTB_FORMAT déjà envoyé");
        return;
    }

    // Crée un transfert control sans données, seulement le setup USB.
    esp_err_t err = usb_host_transfer_alloc(
        USB_SETUP_PACKET_SIZE,
        0,
        &s_ncm_format_transfer
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[USB] Allocation SET_NTB_FORMAT échouée: %s", esp_err_to_name(err));
        return;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ncm_format_transfer->data_buffer;

    // Requête OUT, classe CDC, vers interface.
    setup->bmRequestType = 0x21;

    // Commande CDC-NCM : SET_NTB_FORMAT.
    setup->bRequest = CDC_NCM_SET_NTB_FORMAT;

    // 0 = format NTB-16.
    setup->wValue = CDC_NCM_NTB_FORMAT_16;

    // Interface 0 = interface de contrôle CDC-NCM.
    setup->wIndex = CDC_NCM_CONTROL_INTERFACE;

    // Pas de données après le setup.
    setup->wLength = 0;

    s_ncm_format_transfer->device_handle = s_gopro_dev_hdl;
    s_ncm_format_transfer->bEndpointAddress = 0;
    s_ncm_format_transfer->callback = ncm_set_format_cb;
    s_ncm_format_transfer->context = NULL;

    // Taille totale = seulement le setup USB.
    s_ncm_format_transfer->num_bytes = USB_SETUP_PACKET_SIZE;

    // Envoie la commande control.
    err = usb_host_transfer_submit_control(s_usb_client_hdl, s_ncm_format_transfer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[USB] SET_NTB_FORMAT envoyé");
    } else {
        ESP_LOGE(TAG, "[USB] SET_NTB_FORMAT échec: %s", esp_err_to_name(err));
    }
}

// Demande les paramètres NCM à la GoPro.//fonction qui envoie GET_NTB_PARAMETERS
static void gopro_get_ntb_parameters(void)
{
    // Vérifie que la GoPro est ouverte.
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGE(TAG, "[USB] Impossible GET_NTB_PARAMETERS : GoPro pas ouverte");
        return;
    }

    // Vérifie que l'interface de contrôle est réservée.
    if (!s_gopro_if0_claimed) {
        ESP_LOGE(TAG, "[USB] Impossible GET_NTB_PARAMETERS : interface 0 non claim");
        return;
    }

    // Évite d'envoyer la commande plusieurs fois.
    if (s_ncm_ctrl_transfer != NULL) {
        ESP_LOGW(TAG, "[USB] Requête NCM déjà envoyée");
        return;
    }

    // Crée un transfert control : 8 octets setup + 28 octets de réponse.
    esp_err_t err = usb_host_transfer_alloc(
        USB_SETUP_PACKET_SIZE + CDC_NCM_NTB_PARAMETERS_SIZE,
        0,
        &s_ncm_ctrl_transfer
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[USB] Allocation control NCM échouée: %s", esp_err_to_name(err));
        return;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_ncm_ctrl_transfer->data_buffer;

    // Requête IN, classe CDC, vers interface.
    setup->bmRequestType = 0xA1;

    // Commande CDC-NCM : GET_NTB_PARAMETERS.
    setup->bRequest = CDC_NCM_GET_NTB_PARAMETERS;

    // Pas de valeur spéciale.
    setup->wValue = 0;

    // Interface 0 = interface de contrôle CDC-NCM.
    setup->wIndex = CDC_NCM_CONTROL_INTERFACE;

    // Nombre d'octets demandés à la GoPro.
    setup->wLength = CDC_NCM_NTB_PARAMETERS_SIZE;

    s_ncm_ctrl_transfer->device_handle = s_gopro_dev_hdl;
    s_ncm_ctrl_transfer->bEndpointAddress = 0;
    s_ncm_ctrl_transfer->callback = ncm_control_transfer_cb;
    s_ncm_ctrl_transfer->context = NULL;

    // Taille totale = setup USB + réponse attendue.
    s_ncm_ctrl_transfer->num_bytes = USB_SETUP_PACKET_SIZE + CDC_NCM_NTB_PARAMETERS_SIZE;

    // Envoie la requête control sur endpoint 0.
    err = usb_host_transfer_submit_control(s_usb_client_hdl, s_ncm_ctrl_transfer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[USB] GET_NTB_PARAMETERS envoyé");
    } else {
        ESP_LOGE(TAG, "[USB] GET_NTB_PARAMETERS échec: %s", esp_err_to_name(err));
    }
}

static void set_interface_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        s_data_alt_setting_set = true;
        ESP_LOGI(TAG, "[USB] SET_INTERFACE interface 1 alt 1 OK");

        // Après activation explicite de l'alt setting data,
        // on attendra NETWORK_CONNECTION puis on pourra tester ARP.
    } else {
        ESP_LOGW(TAG, "[USB] SET_INTERFACE interface 1 alt 1 erreur status=%d", transfer->status);
    }
}

static void gopro_set_data_interface_alt1(void)
{
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGE(TAG, "[USB] Impossible SET_INTERFACE : GoPro pas ouverte");
        return;
    }

    if (s_set_interface_transfer != NULL) {
        ESP_LOGW(TAG, "[USB] SET_INTERFACE déjà envoyé");
        return;
    }

    esp_err_t err = usb_host_transfer_alloc(
        USB_SETUP_PACKET_SIZE,
        0,
        &s_set_interface_transfer
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[USB] Allocation SET_INTERFACE échouée: %s", esp_err_to_name(err));
        return;
    }

    usb_setup_packet_t *setup = (usb_setup_packet_t *)s_set_interface_transfer->data_buffer;

    setup->bmRequestType = 0x01;              // Host -> device, standard, interface
    setup->bRequest = USB_STD_SET_INTERFACE;  // SET_INTERFACE
    setup->wValue = GOPRO_DATA_ALT_SETTING;   // alt setting = 1
    setup->wIndex = GOPRO_DATA_INTERFACE;     // interface = 1
    setup->wLength = 0;

    s_set_interface_transfer->device_handle = s_gopro_dev_hdl;
    s_set_interface_transfer->bEndpointAddress = 0;
    s_set_interface_transfer->callback = set_interface_cb;
    s_set_interface_transfer->context = NULL;
    s_set_interface_transfer->num_bytes = USB_SETUP_PACKET_SIZE;

    err = usb_host_transfer_submit_control(s_usb_client_hdl, s_set_interface_transfer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[USB] SET_INTERFACE interface 1 alt 1 envoyé");
    } else {
        ESP_LOGE(TAG, "[USB] SET_INTERFACE interface 1 alt 1 échec: %s", esp_err_to_name(err));
    }
}
static void decode_ntb16(const uint8_t *buf, int len);
// Fonction appelée quand une notification USB est reçue.
static void notification_transfer_cb(usb_transfer_t *transfer) {
    if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE) {
        ESP_LOGW(TAG, "[USB] GoPro absente du bus USB, arrêt des notifications");
        s_notification_started = false;
        return;
    }
    if (transfer->status == USB_TRANSFER_STATUS_CANCELED) {
        ESP_LOGW(TAG, "[USB] Lecture notification annulée");
        s_notification_started = false;
        return;
    }

    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "[USB] Notification reçue, taille=%d",
                 transfer->actual_num_bytes);
        if (transfer->actual_num_bytes > 0) {
            print_usb_data(transfer->data_buffer, transfer->actual_num_bytes);
            decode_cdc_notification(transfer->data_buffer,
                                    transfer->actual_num_bytes);
        }
    } else {
        ESP_LOGW(TAG, "[USB] Notification erreur status=%d", transfer->status);
    }

    if (s_gopro_open && s_notification_started) {
        esp_err_t err = usb_host_transfer_submit(transfer);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[USB] Relance notification impossible: %s",
                     esp_err_to_name(err));
            s_notification_started = false;
        }
    }
}
// Décode une trame Ethernet reçue dans un paquet NCM.
static void decode_ethernet_frame(const uint8_t *eth, int len)
{
    // Une trame Ethernet doit faire au moins 14 octets.
    if (len < 14) {
        ESP_LOGW(TAG, "[NET] Trame Ethernet trop courte");
        return;
    }

    // Le type Ethernet est aux octets 12 et 13.
    uint16_t ether_type = (eth[12] << 8) | eth[13];


    // 0x0806 = ARP.
    if (ether_type == 0x0806 && len >= 42) {
        const uint8_t *arp = eth + 14;

        // Opcode ARP : 1 = request, 2 = reply.
        uint16_t opcode = (arp[6] << 8) | arp[7];

        ESP_LOGI(TAG, "[NET] ARP opcode=%d", opcode);

        if (opcode == 2) {
            ESP_LOGI(TAG, "[NET] ARP reply reçue");

            // On mémorise la réponse ARP de la GoPro.
            s_gopro_arp_ok = true;
            // On mémorise la MAC de la GoPro.
            memcpy(s_gopro_mac, arp + 8, 6);
            // On mémorise l'IP de la GoPro.
            memcpy(s_gopro_ip, arp + 14, 4);

            /*
             * IMPORTANT : après un reboot USB GoPro, ARP répond souvent avant
             * que le service webcam interne soit vraiment prêt.
             * Donc on attend 30 s après reconnexion USB, mais seulement 10 s au premier boot.
             */
            uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

            if (s_after_usb_reconnect && s_live_has_ever_run) {
                /*
                 * Cas observé dans tes logs : après redémarrage/reconnexion GoPro,
                 * l'USB CDC-NCM et ARP reviennent, mais le premier START webcam donne error 4.
                 * Donc on ne tente plus ce START inutile : on déclenche directement la
                 * recovery forte qui a été validée expérimentalement.
                 */
                s_next_live_attempt_ms = now_ms + 120000;
                ESP_LOGW(TAG, "[LIVE] ARP OK apres reconnexion USB post-live -> pas de START inutile, recovery forte directe");

                if (!s_gopro_code_recovery_running) {
                    gopro_schedule_code_only_recovery("reconnexion USB post-live : recovery préventive avant START");
                }
            } else if (s_after_usb_reconnect) {
                /*
                 * Reconnexion USB avant tout premier live : on reste prudent et on garde
                 * l'ancien comportement, car il ne faut pas déclencher une recovery au boot
                 * si aucun live n'a encore été prouvé.
                 */
                s_next_live_attempt_ms = now_ms + 30000;
                ESP_LOGW(TAG, "[LIVE] ARP OK après reconnexion USB avant premier live -> attente 30 s avant START webcam");
            } else {
                s_next_live_attempt_ms = now_ms + 10000;
                ESP_LOGW(TAG, "[LIVE] ARP OK premier boot -> attente 10 s avant START webcam");
            }

            // Adresse MAC de celui qui répond.
            ESP_LOGI(TAG, "[NET] MAC GoPro = %02X:%02X:%02X:%02X:%02X:%02X",
                     arp[8], arp[9], arp[10], arp[11], arp[12], arp[13]);

            // Adresse IP de celui qui répond.
            ESP_LOGI(TAG, "[NET] IP GoPro = %d.%d.%d.%d",
                     arp[14], arp[15], arp[16], arp[17]);
        }
    }

    // 0x0800 = IPv4.
    else if (ether_type == 0x0800) {
        /*
        * Paquet IPv4 vidéo.
        * Aucun affichage ici : la trame sera donnée à lwIP
        * juste après le retour de cette fonction.
        */
    }

    // 0x86DD = IPv6.
    else if (ether_type == 0x86DD) {
        ESP_LOGI(TAG, "[NET] Paquet IPv6 reçu");
    }
}

// Décode un bloc USB CDC-NCM au format NTB-16.
static void decode_ntb16(const uint8_t *buf, int len)
{
    // Un bloc NTB-16 doit au moins contenir le header NTH16.
    if (len < 12) {
        ESP_LOGW(TAG, "[NCM] NTB trop court: %d", len);
        return;
    }

    // Vérifie la signature "NCMH".
    uint32_t nth_sig = read_le32(buf + 0);

    /*
    * La GoPro peut envoyer :
    * - "NCMH" = 0x484D434E
    * - "ncmh" = 0x686D636E
    */
    if (nth_sig != 0x484D434E && nth_sig != 0x686D636E) {
        ESP_LOGW(TAG, "[NCM] Signature NTH16 invalide: 0x%08" PRIx32, nth_sig);
        return;
    }

    // Taille totale annoncée du bloc NCM.
    uint16_t block_len = read_le16(buf + 8);

    // Position de la table NDP.
    uint16_t ndp_index = read_le16(buf + 10);

    // Vérifie la taille totale annoncée.
    if (block_len < 12 || block_len > len) {
        ESP_LOGW(
            TAG,
            "[NCM] Taille NTB invalide : block_len=%u, reçu=%d",
            block_len,
            len
        );
        return;
    }

    // Vérifie que le début du NDP est présent.
    if ((uint32_t)ndp_index + 8U > block_len) {
        ESP_LOGW(TAG, "[NCM] NDP hors limites");
        return;
    }

    // Signature du NDP, normalement "NCM0".
    uint32_t ndp_sig = read_le32(buf + ndp_index);

    // Taille du NDP.
    uint16_t ndp_len = read_le16(buf + ndp_index + 4);

    // Vérifie la signature "NCM0".
    /*
    * La GoPro peut envoyer :
    * - "NCM0" = 0x304D434E
    * - "ncm0" = 0x306D636E
    */
    if (ndp_sig != 0x304D434E && ndp_sig != 0x306D636E) {
        ESP_LOGW(
            TAG,
            "[NCM] Signature NDP invalide : 0x%08" PRIx32,
            ndp_sig
        );
        return;
    }

    // Vérifie que toute la table NDP est dans le bloc.
    if (ndp_len < 16 ||
        (uint32_t)ndp_index + ndp_len > block_len) {

        ESP_LOGW(
            TAG,
            "[NCM] Taille NDP invalide : %u",
            ndp_len
        );
        return;
    }


    // Les entrées commencent après les 8 premiers octets du NDP.
    int entry_offset = ndp_index + 8;
    int ndp_end = ndp_index + ndp_len;

    while (entry_offset + 4 <= ndp_end) {
        // Position de la trame Ethernet dans le bloc.
        uint16_t frame_index = read_le16(buf + entry_offset);

        // Taille de la trame Ethernet.
        uint16_t frame_len = read_le16(buf + entry_offset + 2);

                // Entrée vide = fin de la liste.
        if (frame_index == 0 && frame_len == 0) {
            break;
        }

        // Si la trame est dans le buffer, on la décode.
        if ((uint32_t)frame_index + frame_len <= block_len) {
            const uint8_t *eth_frame = buf + frame_index;

            /*
            * Pour l'instant, on revient au fonctionnement stable :
            * decode_ethernet_frame détecte notamment l'ARP reply de la GoPro
            * et permet de mettre s_gopro_arp_ok à true.
            */
            // check_usb_video_ethernet_frame(eth_frame, frame_len);

            decode_ethernet_frame(eth_frame, frame_len);

            gopro_usb_netif_receive_frame(eth_frame, frame_len);
        } else {
            ESP_LOGW(TAG, "[NCM] Datagram hors limites");
        }

        entry_offset += 4;
    }
}

// Fonction appelée quand des données réseau USB sont reçues.
static void bulk_in_transfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {

        if (transfer->actual_num_bytes > 0) {
            decode_ntb16(
                transfer->data_buffer,
                transfer->actual_num_bytes
            );
        }

    } else {
        /*
         * Important :
         * si le périphérique USB est parti ou si le transfert est annulé,
         * il ne faut surtout pas resoumettre le même transfert.
         * Sinon on obtient ESP_ERR_INVALID_STATE.
         */
        ESP_LOGW(
            TAG,
            "[USB] Bulk IN arrêté, status=%d",
            transfer->status
        );

        if (transfer->status == USB_TRANSFER_STATUS_NO_DEVICE ||
            transfer->status == USB_TRANSFER_STATUS_CANCELED) {

            s_bulk_in_started = false;
            return;
        }

        /*
         * Pour le test actuel, on évite aussi de resoumettre après erreur.
         * On préfère un log propre plutôt qu'une boucle d'erreurs.
         */
        return;
    }

    if (s_gopro_open && s_bulk_in_started) {
        transfer->num_bytes = GOPRO_BULK_IN_SIZE;

        esp_err_t err = usb_host_transfer_submit(transfer);

        if (err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "[USB] Relance Bulk IN arrêtée : %s",
                esp_err_to_name(err)
            );

            s_bulk_in_started = false;
        }
    }
}
// Fonction appelée après un envoi bulk OUT.
static void bulk_out_transfer_cb(usb_transfer_t *transfer)
{
    if (transfer->status == USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGI(TAG, "[USB] Bulk OUT envoyé OK, taille=%d", transfer->actual_num_bytes);
    } else {
        ESP_LOGW(TAG, "[USB] Bulk OUT erreur status=%d", transfer->status);
    }
}

// Construit un bloc NTB-16 contenant une requête ARP.
static int build_ntb16_arp_request(uint8_t *buf, int max_len)
{
    const int nth_offset = 0;
    const int ndp_offset = 12;
    const int eth_offset = 28;

    const int eth_len = 60;          // Trame Ethernet minimale avec padding.
    const int ntb_len = eth_offset + eth_len;

    if (max_len < ntb_len) {
        return -1;
    }

    memset(buf, 0, max_len);

    /*
     * Adresse MAC locale inventée pour l'ESP32 côté USB.
     * 02 indique une adresse locale, non officielle.
     */
    uint8_t esp_mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};

    /*
     * Adresse IP USB choisie pour l'ESP32.
     * On prend 172.20.140.50 pour être proche de la GoPro.
     */
    uint32_t esp_ip = 0xAC148C32;     // 172.20.140.50

    /*
     * Adresse IP supposée de la GoPro en USB.
     */
    uint32_t gopro_ip = 0xAC148C33;   // 172.20.140.51

    /*
     * NTH16 : en-tête principal du bloc NCM.
     */
    write_le32(buf + nth_offset + 0, 0x484D434E);       // "NCMH"
    write_le16(buf + nth_offset + 4, 12);               // Taille NTH16
    write_le16(buf + nth_offset + 6, s_ntb_sequence++); // Numéro de séquence
    write_le16(buf + nth_offset + 8, ntb_len);          // Taille totale NTB
    write_le16(buf + nth_offset + 10, ndp_offset);      // Position du NDP16

    /*
     * NDP16 : indique où se trouve la trame Ethernet dans le bloc.
     */
    write_le32(buf + ndp_offset + 0, 0x304D434E);       // "NCM0"
    write_le16(buf + ndp_offset + 4, 16);               // Taille NDP16
    write_le16(buf + ndp_offset + 6, 0);                // Pas de NDP suivant

    /*
     * Première entrée : une trame Ethernet commence à eth_offset.
     */
    write_le16(buf + ndp_offset + 8, eth_offset);
    write_le16(buf + ndp_offset + 10, eth_len);

    /*
     * Entrée finale vide.
     */
    write_le16(buf + ndp_offset + 12, 0);
    write_le16(buf + ndp_offset + 14, 0);

    /*
     * Trame Ethernet ARP.
     */
    uint8_t *eth = buf + eth_offset;

    // Destination broadcast FF:FF:FF:FF:FF:FF.
    memset(eth + 0, 0xFF, 6);

    // Source = MAC ESP32 côté USB.
    memcpy(eth + 6, esp_mac, 6);

    // Type Ethernet = ARP.
    write_be16(eth + 12, 0x0806);

    /*
     * Paquet ARP request.
     */
    uint8_t *arp = eth + 14;

    write_be16(arp + 0, 0x0001);     // Hardware type Ethernet
    write_be16(arp + 2, 0x0800);     // Protocol type IPv4
    arp[4] = 6;                      // Taille MAC
    arp[5] = 4;                      // Taille IP
    write_be16(arp + 6, 0x0001);     // Opération ARP request

    // Sender MAC.
    memcpy(arp + 8, esp_mac, 6);

    // Sender IP = ESP32 USB.
    write_be32(arp + 14, esp_ip);

    // Target MAC inconnue = 00:00:00:00:00:00.
    memset(arp + 18, 0x00, 6);

    // Target IP = GoPro USB.
    write_be32(arp + 24, gopro_ip);

    return ntb_len;
}

// Teste un premier envoi sur l'endpoint bulk OUT.
static void gopro_test_bulk_out(void)
{
    // Vérifie que la GoPro est ouverte.
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGE(TAG, "[USB] Impossible bulk OUT : GoPro pas ouverte");
        return;
    }

    // Vérifie que l'endpoint bulk OUT a été trouvé.
    if (s_gopro_ep_bulk_out == 0) {
        ESP_LOGE(TAG, "[USB] Endpoint bulk OUT non trouvé");
        return;
    }

    // Évite d'envoyer plusieurs fois le test.
    if (s_bulk_out_sent) {
        ESP_LOGW(TAG, "[USB] Bulk OUT déjà testé");
        return;
    }

    // Crée un transfert USB pour envoyer des données.
    esp_err_t err = usb_host_transfer_alloc(
        GOPRO_BULK_OUT_SIZE,
        0,
        &s_bulk_out_transfer
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[USB] Allocation bulk OUT échouée: %s", esp_err_to_name(err));
        return;
    }


    s_bulk_out_transfer->device_handle = s_gopro_dev_hdl;
    s_bulk_out_transfer->bEndpointAddress = s_gopro_ep_bulk_out;
    s_bulk_out_transfer->callback = bulk_out_transfer_cb;
    s_bulk_out_transfer->context = NULL;

    // Construit un vrai bloc NTB-16 contenant une requête ARP.
    int ntb_len = build_ntb16_arp_request(
        s_bulk_out_transfer->data_buffer,
        GOPRO_BULK_OUT_SIZE
    );

    if (ntb_len < 0) {
        ESP_LOGE(TAG, "[USB] Construction NTB-16 ARP impossible");
        return;
    }

    // Affiche le bloc envoyé pour vérifier.
    ESP_LOGI(TAG, "[USB] NTB-16 ARP construit, taille=%d", ntb_len);
    print_usb_data(s_bulk_out_transfer->data_buffer, ntb_len);

    // On envoie seulement la taille réelle du bloc NTB.
    s_bulk_out_transfer->num_bytes = ntb_len;

    s_bulk_out_sent = true;

    // Envoie les données sur l'endpoint bulk OUT.
    err = usb_host_transfer_submit(s_bulk_out_transfer);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[USB] Test bulk OUT envoyé sur endpoint 0x%02x", s_gopro_ep_bulk_out);
    } else {
        ESP_LOGE(TAG, "[USB] Test bulk OUT échec: %s", esp_err_to_name(err));
        s_bulk_out_sent = false;
    }
}

// Démarre la lecture des données réseau USB.
static void gopro_start_bulk_in_read(void) {
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGE(TAG, "[USB] Impossible bulk IN : GoPro pas ouverte");
        return;
    }
    if (s_bulk_in_started) {
        ESP_LOGW(TAG, "[USB] Bulk IN déjà lancé");
        return;
    }
    if (s_gopro_ep_bulk_in == 0) {
        ESP_LOGE(TAG, "[USB] Endpoint bulk IN non trouvé");
        return;
    }

    s_bulk_in_started = true;
    int submitted = 0;
    for (int i = 0; i < GOPRO_BULK_IN_TRANSFER_COUNT; i++) {
        esp_err_t err = usb_host_transfer_alloc(GOPRO_BULK_IN_SIZE, 0,
                                                &s_bulk_in_transfers[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[USB] Allocation Bulk IN %d impossible: %s",
                     i, esp_err_to_name(err));
            s_bulk_in_transfers[i] = NULL;
            continue;
        }
        usb_transfer_t *t = s_bulk_in_transfers[i];
        t->device_handle = s_gopro_dev_hdl;
        t->bEndpointAddress = s_gopro_ep_bulk_in;
        t->callback = bulk_in_transfer_cb;
        t->context = NULL;
        t->num_bytes = GOPRO_BULK_IN_SIZE;

        err = usb_host_transfer_submit(t);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[USB] Submit Bulk IN %d impossible: %s",
                     i, esp_err_to_name(err));
            usb_host_transfer_free(t);
            s_bulk_in_transfers[i] = NULL;
            continue;
        }
        submitted++;
    }
    if (submitted == 0) {
        s_bulk_in_started = false;
        ESP_LOGE(TAG, "[USB] Aucun transfert Bulk IN lancé");
    } else {
        ESP_LOGI(TAG, "[USB] %d transferts Bulk IN lancés", submitted);
    }
}

// Démarre l'écoute des notifications USB de la GoPro.  
static void gopro_start_notification_read(void)
{
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {//// Vérifie que la GoPro est bien ouverte.
        ESP_LOGE(TAG, "[USB] Impossible d'écouter : GoPro pas ouverte");
        return;
    }

    if (s_notification_started) {//// Évite de lancer deux fois l'écoute.
        ESP_LOGW(TAG, "[USB] Notifications déjà lancées");
        return;
    }

    // Vérifie que l'endpoint de notification a bien été trouvé.
    // Sans cet endpoint, on ne sait pas où écouter les notifications.
    if (s_gopro_ep_notify == 0) {
        ESP_LOGE(TAG, "[USB] Endpoint notification non trouvé");
        return;
    }
    // Crée un transfert USB pour lire les notifications.
    esp_err_t err = usb_host_transfer_alloc(
        GOPRO_NOTIFICATION_SIZE,
        0,
        &s_notification_transfer
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[USB] Allocation transfert notification échouée: %s", esp_err_to_name(err));
        return;
    }

    s_notification_transfer->device_handle = s_gopro_dev_hdl;
    // On écoute l'endpoint notification trouvé automatiquement.
    s_notification_transfer->bEndpointAddress = s_gopro_ep_notify;
    s_notification_transfer->callback = notification_transfer_cb;  // appel d'un callback 2nd quand les données arrivent
    s_notification_transfer->context = NULL;
    s_notification_transfer->num_bytes = GOPRO_NOTIFICATION_SIZE;

    s_notification_started = true;

    err = usb_host_transfer_submit(s_notification_transfer); // // Lance la première lecture USB.

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[USB] Écoute notifications lancée sur endpoint 0x82");
    } else {
        ESP_LOGE(TAG, "[USB] Lancement écoute notification échoué: %s", esp_err_to_name(err));
        s_notification_started = false;
    }
}
// Réserve les interfaces USB nécessaires pour le réseau USB de la GoPro.
static void gopro_claim_interfaces(void)
{
    if (!s_gopro_open || s_gopro_dev_hdl == NULL) {
        ESP_LOGE(TAG, "[USB] Impossible de claim : GoPro pas ouverte");
        return;
    }

    esp_err_t err;

    // Interface 0 : interface de contrôle CDC-NCM.
    err = usb_host_interface_claim(
        s_usb_client_hdl,
        s_gopro_dev_hdl,
        0,
        0
    );

    if (err == ESP_OK) {
        s_gopro_if0_claimed = true;
        ESP_LOGI(TAG, "[USB] Interface 0 CDC-NCM claim OK");
    } else {
        ESP_LOGE(TAG, "[USB] Interface 0 claim échoué: %s", esp_err_to_name(err));
    }

    // Interface 1 alt 1 : interface de données réseau.
    err = usb_host_interface_claim(
        s_usb_client_hdl,
        s_gopro_dev_hdl,
        1,
        1
    );

    if (err == ESP_OK) {
        s_gopro_if1_claimed = true;
        ESP_LOGI(TAG, "[USB] Interface 1 alt 1 CDC-DATA claim OK");
        // Force explicitement l'activation de l'alternate setting 1.
        gopro_set_data_interface_alt1();
    } else {
        ESP_LOGE(TAG, "[USB] Interface 1 alt 1 claim échoué: %s", esp_err_to_name(err));
    }
}

static void gopro_free_transfer(usb_transfer_t **transfer)
{
    if (transfer != NULL && *transfer != NULL) {
        usb_host_transfer_free(*transfer);
        *transfer = NULL;
    }
}

static void gopro_usb_reset_session_after_disconnect(void)
{
    usb_device_handle_t old_dev = s_gopro_dev_hdl;

    ESP_LOGW(TAG, "[USB] Nettoyage complet session GoPro USB");

    /*
     * 1) On arrête logiquement les flux en cours.
     */
    s_notification_started = false;
    s_bulk_in_started = false;
    s_usb_tx_busy = false;
    s_bulk_out_sent = false;

    /*
     * 2) Si on a encore le handle, on libère proprement
     * les interfaces avant de fermer le device.
     */
    if (old_dev != NULL) {
        if (s_gopro_if1_claimed) {
            esp_err_t err = usb_host_interface_release(
                s_usb_client_hdl,
                old_dev,
                1
            );

            if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "[USB] Release interface 1 erreur: %s",
                    esp_err_to_name(err)
                );
            }
        }

        if (s_gopro_if0_claimed) {
            esp_err_t err = usb_host_interface_release(
                s_usb_client_hdl,
                old_dev,
                0
            );

            if (err != ESP_OK) {
                ESP_LOGW(
                    TAG,
                    "[USB] Release interface 0 erreur: %s",
                    esp_err_to_name(err)
                );
            }
        }

        esp_err_t close_err = usb_host_device_close(
            s_usb_client_hdl,
            old_dev
        );

        if (close_err != ESP_OK) {
            ESP_LOGW(
                TAG,
                "[USB] Close device erreur: %s",
                esp_err_to_name(close_err)
            );
        }
    }

    /*
     * 3) On libère tous les anciens transferts.
     * Très important pour permettre une nouvelle session après reboot GoPro.
     */
    gopro_free_transfer(&s_notification_transfer);

    for (int i = 0; i < GOPRO_BULK_IN_TRANSFER_COUNT; i++) {
        gopro_free_transfer(&s_bulk_in_transfers[i]);
    }

    gopro_free_transfer(&s_bulk_out_transfer);
    gopro_free_transfer(&s_set_interface_transfer);
    gopro_free_transfer(&s_ncm_ctrl_transfer);
    gopro_free_transfer(&s_ncm_format_transfer);
    gopro_free_transfer(&s_ncm_input_size_transfer);
    gopro_free_transfer(&s_packet_filter_transfer);

    /*
     * 4) Reset complet des états USB device.
     */
    s_usb_gopro_detected = false;
    s_usb_ncm_detected = false;
    s_usb_vid = 0;
    s_usb_pid = 0;
    s_usb_num_interfaces = 0;

    s_gopro_dev_hdl = NULL;
    s_gopro_open = false;

    s_gopro_if0_claimed = false;
    s_gopro_if1_claimed = false;

    s_gopro_ep_notify = 0;
    s_gopro_ep_bulk_in = 0;
    s_gopro_ep_bulk_out = 0;
    s_gopro_endpoints_ready = false;

    /*
     * 5) Reset complet NCM.
     */
    s_ncm_params_received = false;
    s_ncm_format_set = false;
    s_ncm_input_size_set = false;
    s_packet_filter_set = false;
    s_data_alt_setting_set = false;

    s_cdc_network_connected = false;
    s_last_cdc_notification = 0;
    s_last_cdc_value = 0;

    /*
     * 6) Reset complet live / ARP.
     */
    s_network_test_pending = false;
    s_network_test_started = false;

    s_live_running = false;
    s_live_start_requested = false;
    s_gopro_webcam_task_started = false;

    s_gopro_arp_ok = false;
    memset(s_gopro_mac, 0, sizeof(s_gopro_mac));
    memset(s_gopro_ip, 0, sizeof(s_gopro_ip));

    s_last_arp_retry_ms = 0;
    s_next_live_attempt_ms = 0;

    /*
     * Important :
     * On ne remet PAS s_usb_netif_ready à false ici,
     * car l'interface lwIP us2 existe déjà.
     * Sinon il faudrait faire netif_remove(), ce que ton code ne fait pas.
     */
}

/*
 * Requête HTTP simple vers la GoPro en forçant la sortie par l'interface USB.
 * Utilisé pour tester !OR via l'endpoint Labs, puis pour stop/exit si possible.
 */
static bool gopro_http_usb_get_path(const char *path, char *response, size_t response_size)
{
    if (response != NULL && response_size > 0) {
        response[0] = '\0';
    }

    if (path == NULL || path[0] == '\0') {
        return false;
    }

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "[GOPRO-HTTP] socket impossible, errno=%d", errno);
        return false;
    }

    struct timeval timeout = {
        .tv_sec = 4,
        .tv_usec = 0
    };

    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(0);
    local_addr.sin_addr.s_addr = inet_addr("172.20.140.50");

    if (bind(sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
        ESP_LOGE(TAG, "[GOPRO-HTTP] bind USB 172.20.140.50 échoué, errno=%d", errno);
        close(sock);
        return false;
    }

    struct sockaddr_in gopro_addr;
    memset(&gopro_addr, 0, sizeof(gopro_addr));
    gopro_addr.sin_family = AF_INET;
    gopro_addr.sin_port = htons(8080);
    gopro_addr.sin_addr.s_addr = inet_addr("172.20.140.51");

    if (connect(sock, (struct sockaddr *)&gopro_addr, sizeof(gopro_addr)) < 0) {
        ESP_LOGE(TAG, "[GOPRO-HTTP] connect GoPro échoué, errno=%d", errno);
        close(sock);
        return false;
    }

    char request[384];
    snprintf(
        request,
        sizeof(request),
        "GET %s HTTP/1.1\r\n"
        "Host: 172.20.140.51:8080\r\n"
        "Connection: close\r\n"
        "\r\n",
        path
    );

    int request_len = strlen(request);
    int sent_total = 0;

    while (sent_total < request_len) {
        int sent = send(sock, request + sent_total, request_len - sent_total, 0);
        if (sent <= 0) {
            ESP_LOGE(TAG, "[GOPRO-HTTP] send échoué pour %s, errno=%d", path, errno);
            close(sock);
            return false;
        }
        sent_total += sent;
    }

    int total = 0;
    if (response != NULL && response_size > 1) {
        while (total < (int)response_size - 1) {
            int r = recv(sock, response + total, (int)response_size - 1 - total, 0);
            if (r > 0) {
                total += r;
                continue;
            }
            break;
        }
        response[total] = '\0';
    } else {
        char tmp[128];
        while (recv(sock, tmp, sizeof(tmp), 0) > 0) {
            ;
        }
    }

    close(sock);

    ESP_LOGI(
        TAG,
        "[GOPRO-HTTP] Réponse pour %s:\n%s",
        path,
        (response != NULL && response_size > 0) ? response : ""
    );

    return true;
}

static bool gopro_response_has_final_404_or_unrecognized(const char *response)
{
    if (response == NULL) {
        return true;
    }

    if (strstr(response, "404 Not Found") != NULL ||
        strstr(response, "Command is not recognized") != NULL ||
        strstr(response, "err_msg") != NULL) {
        return true;
    }

    return false;
}

static bool gopro_try_labs_or_over_usb(void)
{
    char response[768];

    ESP_LOGW(
        TAG,
        "[RECOVERY] Etape de stabilisation GoPro: appel Labs !OR via USB HTTP (peut etre refuse, on continue ensuite)"
    );

    bool sent = gopro_http_usb_get_path(
        "/gopro/qrcode?labs=1&code=%21OR",
        response,
        sizeof(response)
    );

    if (!sent) {
        ESP_LOGW(TAG, "[GOPRO-RECOVERY] !OR non envoyé : HTTP USB indisponible");
        return false;
    }

    if (gopro_response_has_final_404_or_unrecognized(response)) {
        ESP_LOGW(
            TAG,
            "[RECOVERY] Reponse Labs !OR refusee/ignoree -> suite normale: reset root-port USB"
        );
        return false;
    }

    ESP_LOGW(
        TAG,
        "[RECOVERY] !OR semble accepte -> attente reboot GoPro"
    );

    return true;
}

static void gopro_root_port_power(bool enabled)
{
    esp_err_t err = usb_host_lib_set_root_port_power(enabled);

    if (err == ESP_OK) {
        ESP_LOGW(
            TAG,
            "[USB-PORT] Root port USB %s demandé",
            enabled ? "ON" : "OFF"
        );
    } else {
        ESP_LOGE(
            TAG,
            "[USB-PORT] Root port USB %s erreur: %s",
            enabled ? "ON" : "OFF",
            esp_err_to_name(err)
        );
    }
}

static void gopro_code_only_recovery_task(void *arg)
{
    const char *reason = (const char *)arg;

    s_gopro_code_recovery_running = true;

    ESP_LOGW(
        TAG,
        "[RECOVERY] Début recovery STABLE : %s",
        reason != NULL ? reason : "raison inconnue"
    );

    /*
     * IMPORTANT :
     * Cette recovery garde la séquence qui a marché dans tes logs.
     *
     * IMPORTANT POUR CETTE GOPRO :
     * L'appel Labs !OR peut etre refuse par l'endpoint HTTP Labs.
     * On le garde quand meme car les tests montrent que la version sans cette etape
     * ne relance pas correctement le live, alors que la version avec cette etape fonctionne.
     * Il faut donc considerer cette etape comme une phase de stabilisation/timing,
     * pas comme la commande principale de reboot.
     *
     * La vraie partie utile est :
     *   stop/exit -> reset session locale -> root-port OFF 5 s -> tentative root-port ON -> reboot volontaire.
     *
     * On ne laisse plus le heap crasher tout seul : on redémarre volontairement juste après.
     */
    char response[512];

    gopro_http_usb_get_path("/gopro/webcam/stop", response, sizeof(response));
    vTaskDelay(pdMS_TO_TICKS(500));

    gopro_http_usb_get_path("/gopro/webcam/exit", response, sizeof(response));
    vTaskDelay(pdMS_TO_TICKS(1000));

    /*
     * Cette commande est probablement refusée chez toi avec 404.
     * Ce n'est PAS elle qui répare le live.
     * On la garde seulement pour conserver le même comportement/timing que le code qui marchait.
     */
    bool or_ok = gopro_try_labs_or_over_usb();

    if (or_ok) {
        ESP_LOGW(TAG, "[RECOVERY] !OR accepté, attente reboot caméra avant reboot ESP32");
        vTaskDelay(pdMS_TO_TICKS(8000));
    } else {
        ESP_LOGW(TAG, "[RECOVERY] Etape !OR terminee/refusee -> on garde la sequence root-port qui fonctionne");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    /*
     * Nettoyage local AVANT de couper le root port.
     * Dans tes logs qui marchaient, cette étape avait lieu avant Root port OFF.
     */
    gopro_usb_reset_session_after_disconnect();

    /*
     * Coupe logique du root port.
     * Le signe attendu dans les logs :
     *   GoPro absente du bus USB
     *   Périphérique USB débranché
     */
    ESP_LOGW(TAG, "[RECOVERY] Root port USB OFF pendant 5 s");
    gopro_root_port_power(false);
    vTaskDelay(pdMS_TO_TICKS(5000));

    /*
     * Dans le code qui marchait, il y avait aussi cette tentative ON.
     * Même si elle renvoie ESP_ERR_INVALID_STATE, elle semble participer
     * à remettre le contrôleur USB/PHY dans l'état qui permet le reboot propre.
     *
     * La différence avec l'ancien code : on ne reste pas vivant après.
     * On redémarre volontairement tout de suite pour éviter le crash heap TLSF.
     */
    ESP_LOGW(TAG, "[RECOVERY] Root port USB ON demande puis reboot volontaire");
    gopro_root_port_power(true);

    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGW(TAG, "[RECOVERY] esp_restart volontaire apres sequence OFF/ON");
    esp_restart();

    /*
     * On ne doit jamais arriver ici.
     */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void gopro_schedule_code_only_recovery(const char *reason)
{
    if (s_gopro_code_recovery_running) {
        ESP_LOGW(TAG, "[RECOVERY] Recovery déjà planifiée");
        return;
    }

    /*
     * On met le flag avant xTaskCreate pour éviter qu'une deuxième notification
     * USB/ARP planifie deux recoveries en parallèle.
     */
    s_gopro_code_recovery_running = true;

    BaseType_t ok = xTaskCreatePinnedToCore(
        gopro_code_only_recovery_task,
        "gopro_recovery",
        8192,
        (void *)reason,
        17,
        NULL,
        0
    );

    if (ok != pdPASS) {
        s_gopro_code_recovery_running = false;
        ESP_LOGE(TAG, "[RECOVERY] Création tâche recovery impossible");
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        s_next_live_attempt_ms = now_ms + 60000;
    }
}

//* Callback princiaple appelé quand le client USB reçoit un événement (branché/débranché)
static void usb_client_event_cb(const usb_host_client_event_msg_t *event_msg,
                                void *arg)
{
    // Un nouveau périphérique USB vient d'être détecté.
    if (event_msg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        
        if (s_gopro_open || s_gopro_dev_hdl != NULL) {
            ESP_LOGW(
                TAG,
                "[USB] Nouveau périphérique alors qu'une ancienne session existe -> cleanup"
            );

            gopro_usb_reset_session_after_disconnect();
        }
        uint8_t dev_addr = event_msg->new_dev.address;

        ESP_LOGI(TAG, "[USB] Nouveau périphérique détecté, adresse=%d", dev_addr);

        usb_device_handle_t dev_hdl = NULL;

        // Ouvre le périphérique USB pour lire ses informations.
        esp_err_t err = usb_host_device_open(s_usb_client_hdl, dev_addr, &dev_hdl);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "[USB] Impossible d'ouvrir le périphérique: %s", esp_err_to_name(err));
            return;
        }


        const usb_device_desc_t *dev_desc = NULL;

        // Lit les informations générales du périphérique USB : VID, PID, classe USB.
        err = usb_host_get_device_descriptor(dev_hdl, &dev_desc);
        if (err == ESP_OK && dev_desc != NULL) {
            s_usb_gopro_detected = true;
            s_usb_vid = dev_desc->idVendor;
            s_usb_pid = dev_desc->idProduct;

            ESP_LOGI(TAG, "[USB] VID=0x%04x PID=0x%04x",
                     dev_desc->idVendor,
                     dev_desc->idProduct);

            ESP_LOGI(TAG, "[USB] Device class=0x%02x subclass=0x%02x protocol=0x%02x",
                     dev_desc->bDeviceClass,
                     dev_desc->bDeviceSubClass,
                     dev_desc->bDeviceProtocol);

            if (dev_desc->idVendor == 0x2672) {
                ESP_LOGI(TAG, "[USB] GoPro détectée");
            }
        } else {
            ESP_LOGE(TAG, "[USB] Lecture device descriptor impossible: %s", esp_err_to_name(err));
            usb_host_device_close(s_usb_client_hdl, dev_hdl);
            return;
        }

        const usb_config_desc_t *config_desc = NULL;


        //fonction USB pour connaître les interfaces et endpoints.
        err = usb_host_get_active_config_descriptor(dev_hdl, &config_desc);
        if (err == ESP_OK && config_desc != NULL) {
            s_usb_num_interfaces = config_desc->bNumInterfaces;

            ESP_LOGI(TAG, "[USB] Config: interfaces=%d total_length=%d",
                     config_desc->bNumInterfaces,
                     config_desc->wTotalLength);

            const uint8_t *p = (const uint8_t *)config_desc;
            int offset = 0;
            // Ces variables servent à savoir dans quelle interface on se trouve.
            uint8_t current_interface = 0;
            uint8_t current_alt = 0;
            uint8_t current_class = 0;

            // Parcourt tous les descripteurs USB de la GoPro.
            while (offset < config_desc->wTotalLength) {
                uint8_t bLength = p[offset];
                uint8_t bDescriptorType = p[offset + 1];

                if (bLength == 0) {
                    ESP_LOGE(TAG, "[USB] Descriptor length 0, stop parsing");
                    break;
                }
            

                if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
                    const usb_intf_desc_t *intf = (const usb_intf_desc_t *)(p + offset);

                    ESP_LOGI(TAG,
                             "[USB] INTERFACE num=%d alt=%d class=0x%02x subclass=0x%02x protocol=0x%02x endpoints=%d",
                             intf->bInterfaceNumber,
                             intf->bAlternateSetting,
                             intf->bInterfaceClass,
                             intf->bInterfaceSubClass,
                             intf->bInterfaceProtocol,
                             intf->bNumEndpoints);
                    // On mémorise l'interface actuelle.
                    current_interface = intf->bInterfaceNumber;
                    current_alt = intf->bAlternateSetting;
                    current_class = intf->bInterfaceClass;         
                    // Classe 0x02 et sous-classe 0x0D = interface CDC-NCM, donc réseau USB.
                    if (intf->bInterfaceClass == 0x02 && intf->bInterfaceSubClass == 0x0D) {
                        s_usb_ncm_detected = true;
                        ESP_LOGI(TAG, "[USB] Interface CDC-NCM détectée");
                    }
                }

                else if (bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
                    const usb_ep_desc_t *ep = (const usb_ep_desc_t *)(p + offset);

                    ESP_LOGI(TAG,
                             "[USB] ENDPOINT addr=0x%02x attr=0x%02x maxPacket=%d interval=%d",
                             ep->bEndpointAddress,
                             ep->bmAttributes,
                             ep->wMaxPacketSize,
                             ep->bInterval);
                    // Type de transfert USB.
                    uint8_t ep_type = ep->bmAttributes & 0x03;

                    // Endpoint interrupt de notification.
                    if (ep_type == 0x03 && current_interface == 0) {
                        s_gopro_ep_notify = ep->bEndpointAddress;
                        ESP_LOGI(TAG, "[USB] Endpoint notification trouvé: 0x%02x", s_gopro_ep_notify);
                    }

                    // Endpoints bulk de l'interface data.
                    if (ep_type == 0x02 && current_interface == 1 && current_alt == 1 && current_class == 0x0A) {
                        if (ep->bEndpointAddress & 0x80) {
                            s_gopro_ep_bulk_in = ep->bEndpointAddress;
                            ESP_LOGI(TAG, "[USB] Endpoint bulk IN trouvé: 0x%02x", s_gopro_ep_bulk_in);
                        } else {
                            s_gopro_ep_bulk_out = ep->bEndpointAddress;
                            ESP_LOGI(TAG, "[USB] Endpoint bulk OUT trouvé: 0x%02x", s_gopro_ep_bulk_out);
                        }
                    }
                }

                offset += bLength;
            }
            // On vérifie que les trois endpoints importants ont été trouvés.
                if (s_gopro_ep_notify != 0 &&
                    s_gopro_ep_bulk_in != 0 &&
                    s_gopro_ep_bulk_out != 0) {

                    s_gopro_endpoints_ready = true;

                    ESP_LOGI(TAG, "[USB] Tous les endpoints utiles sont prêts");
                } else {
                    ESP_LOGW(TAG, "[USB] Endpoints incomplets");
                }

        
        } else {
            ESP_LOGE(TAG, "[USB] Lecture config descriptor impossible: %s", esp_err_to_name(err));
        }
        // On garde la GoPro ouverte pour pouvoir continuer à l'utiliser ensuite.
        s_gopro_dev_hdl = dev_hdl;
        s_gopro_open = true;
        // On réserve les interfaces USB nécessaires à la communication réseau.
        gopro_claim_interfaces();
        // Après le claim, on écoute les notifications USB de la GoPro.
        //focntion qui appel un autre callback
        gopro_start_notification_read();
        // Après les notifications, on écoute aussi les données réseau.
        // Pas encore utilisé : le réseau NCM n'est pas initialisé.
        //gopro_start_bulk_in_read();
        // Demande les paramètres NCM à la GoPro.
        gopro_get_ntb_parameters();
        // Choisit le format NCM le plus simple.
        gopro_set_ntb_format_16();
    }

    else if (event_msg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        ESP_LOGW(TAG, "[USB] Périphérique USB débranché");

        /* On garde le nettoyage USB qui reconnectait mieux, mais on marque la prochaine session. */
        s_after_usb_reconnect = true;

        gopro_usb_reset_session_after_disconnect();

        ESP_LOGW(
            TAG,
            "[USB] Session GoPro nettoyée, attente nouvelle énumération USB"
        );
    }
        
    
} 
// Tâche qui reçoit les événements USB côté client.
static void usb_client_task(void *arg)
{
    usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 5,
        .async = {
            .client_event_callback = usb_client_event_cb,
            .callback_arg = NULL,
        },
    };

    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &s_usb_client_hdl));

    ESP_LOGI(TAG, "[USB] Client USB enregistré");

    while (1) {
        // Attend les événements USB : branchement, débranchement, etc.
        usb_host_client_handle_events(s_usb_client_hdl, portMAX_DELAY);
    }
}
// Tâche qui fait tourner la bibliothèque USB Host en arrière-plan.
static void usb_host_daemon_task(void *arg)
{
    ESP_LOGI(TAG, "[USB] Daemon USB Host démarré");

    while (1) {
        uint32_t event_flags = 0;

        // gère les événements internes de la bibliothèque USB Host.
        esp_err_t err = usb_host_lib_handle_events(
            portMAX_DELAY,
            &event_flags
        );

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[USB] usb_host_lib_handle_events: %s", esp_err_to_name(err));
        }

        if (event_flags != 0) {
            ESP_LOGI(TAG, "[USB] Library event flags: 0x%lx", event_flags);
        }
    }
}

/*
 * Initialise la partie USB Host.
 *
 * Étapes :
 * 1. Installation de la bibliothèque USB Host
 * 2. Création de la tâche daemon
 * 3. Création de la tâche client
 *
 * Après cette fonction, l'ESP32-S3 attend qu'un périphérique USB soit branché.
 */// Initialise l'USB Host et lance les tâches USB.
static void usb_probe_init(void)
{
    ESP_LOGI(TAG, "[USB] Installation USB Host Library");

    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .root_port_unpowered = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
        .peripheral_map = 0,
    };

    // Active la bibliothèque USB Host de l'ESP32-S3.

    ESP_ERROR_CHECK(usb_host_install(&host_config));

    /* S'assure que le root port USB Host est alimenté au démarrage. */
    esp_err_t pwr_err = usb_host_lib_set_root_port_power(true);
    if (pwr_err == ESP_OK) {
        ESP_LOGI(TAG, "[USB-PORT] Root port USB ON au démarrage");
    } else {
        ESP_LOGW(TAG, "[USB-PORT] Root port ON non appliqué: %s", esp_err_to_name(pwr_err));
    }

    // Lance la tâche interne USB Host.
    xTaskCreate(
        usb_host_daemon_task,
        "usb_daemon",
        4096,
        NULL,
        20,
        NULL
    );
    // Lance la tâche qui reçoit les événements de périphériques USB.
    xTaskCreate(
        usb_client_task,
        "usb_client",
        4096,
        NULL,
        19,
        NULL
    );

    ESP_LOGI(TAG, "[USB] USB Host prêt, branche la GoPro sur le port USB natif");
}

void app_main(void)
{
    ESP_LOGI(TAG, "Boot ESP32-P4 OK");

    /*
     * Initialisation NVS.
     */
    esp_err_t ret = nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    /*
    * Initialise la pile réseau générale une seule fois.
    * Cela ne lance pas encore le W5500.
    */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /*
    * La GoPro et l'ESP32 sont probablement alimentés simultanément.
    * On laisse quatre secondes à la GoPro pour démarrer avant
    * d'activer le contrôleur USB Host.
    */
    ESP_LOGI(
        TAG,
        "[INIT] Attente du démarrage GoPro pendant 4 secondes"
    );

    vTaskDelay(pdMS_TO_TICKS(4000));

    usb_probe_init();

    ESP_LOGI(
        TAG,
        "[INIT] USB démarré, attente de la connexion CDC-NCM GoPro"
    );
    /*
    * Démarrage du RJ45 intégré ESP32-P4.
    * Premier test : Ethernet seul avec IP statique.
    */
    ethernet_init_p4_internal();

    /*
    * Serveur HTTP de diagnostic.
    * Accessible ensuite depuis le PC sur :
    * http://192.168.50.2/status
    */
    s_http_server = start_webserver();

        uint32_t heartbeat_counter = 0;

    while (1) {
        /*
         * Affichage environ toutes les 5 secondes.
         * La boucle tourne toutes les 500 ms.
         */
        heartbeat_counter++;

        if (heartbeat_counter >= 10) {
            heartbeat_counter = 0;

            if (s_usb_gopro_detected) {
                ESP_LOGI(
                    TAG,
                    "Programme vivant - GoPro USB détectée"
                );
            }
            else {
                ESP_LOGI(
                    TAG,
                    "Programme vivant - attente GoPro USB"
                );
            }
        }

        /*
         * Premier test ESP32-P4 :
         * on teste uniquement USB Host High-Speed + CDC-NCM GoPro.
         * Pas encore d'Ethernet, pas encore de VLC.
         */
        if (s_network_test_pending &&
            !s_network_test_started &&
            s_cdc_network_connected &&
            s_gopro_open) {

            s_network_test_pending = false;
            s_network_test_started = true;

            ESP_LOGI(
                TAG,
                "[USB-P4] CDC-NCM GoPro connecté"
            );

            ESP_LOGI(
                TAG,
                "[USB-P4] Test USB High-Speed sans W5500 et sans VLC"
            );

            vTaskDelay(pdMS_TO_TICKS(300));

            /*
             * Démarre les lectures USB venant de la GoPro.
             */
            gopro_start_bulk_in_read();

            /*
             * Crée l'interface réseau USB :
             * ESP32-P4 : 172.20.140.50
             * GoPro    : 172.20.140.51
             */
            gopro_usb_netif_start();

            vTaskDelay(pdMS_TO_TICKS(500));

            /*
             * Test ARP vers la GoPro.
             */
            gopro_test_bulk_out();

            ESP_LOGI(
                TAG,
                "[USB-P4] Test ARP GoPro lancé"
            );
        }

        vTaskDelay(pdMS_TO_TICKS(500));

        /*
        * Démarrage automatique du live.
        * Une seule tentative tant que la GoPro ne se déconnecte pas.
        */
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);

        if (!s_live_start_requested &&
            !s_gopro_webcam_task_started &&
            now_ms >= s_next_live_attempt_ms &&
            s_network_test_started &&
            s_usb_netif_ready &&
            s_cdc_network_connected &&
            s_gopro_open &&
            s_gopro_arp_ok &&
            s_eth_link_up &&
            s_eth_ip_addr != 0) {

            s_live_start_requested = true;

            ESP_LOGI(
                TAG,
                "[LIVE] Conditions OK : démarrage webcam GoPro"
            );

            gopro_start_webcam_once();
        }
    }

}