#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/lora.h>
#include <zephyr/shell/shell.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(reverse_bridge, LOG_LEVEL_INF);

#define LORA_NODE DT_ALIAS(lora0)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(LORA_NODE), "SX1262 lora0 is not enabled");
#define MAX_PAYLOAD 18
#define FRAME_HEADER_SIZE 9
#define MAX_FRAME (FRAME_HEADER_SIZE + MAX_PAYLOAD)
#define COMPANY_ID 0xffff
#define MAGIC0 'R'
#define MAGIC1 'B'
#define MAGIC2 '2'
#define TYPE_REQUEST 1
#define TYPE_RESPONSE 2

enum app_role { ROLE_INITIATOR, ROLE_RESPONDER };

static const struct device *lora_dev = DEVICE_DT_GET(LORA_NODE);
static enum app_role role = ROLE_INITIATOR;
static bool lora_ready;
static bool ble_ready;
volatile uint32_t boot_cookie;
static bool scan_running;
static uint32_t next_id = 1;
static uint32_t outstanding_id;
static char outstanding_payload[MAX_PAYLOAD + 1];
static size_t outstanding_len;
static uint32_t lora_tx_count, lora_rx_count, lora_drop_count;
static uint32_t ble_adv_count, ble_match_count, ble_drop_count;
static uint8_t adv_buf[2 + MAX_FRAME];
static size_t adv_len;
static struct bt_data adv_data;
static struct k_mutex radio_lock;
static struct bt_le_scan_cb scan_callbacks;
static void lora_rx_cb(const struct device *dev, uint8_t *data, uint16_t size,
			       int16_t rssi, int8_t snr, void *user_data);

static int radio_configure(bool tx)
{
	struct lora_modem_config cfg = {
		.frequency = 915000000,
		.bandwidth = BW_125_KHZ,
		.datarate = SF_9,
		.preamble_len = 8,
		.coding_rate = CR_4_5,
		.tx_power = 14,
		.tx = tx,
		.iq_inverted = false,
		.public_network = false,
	};
	return lora_config(lora_dev, &cfg);
}

static size_t make_frame(uint8_t type, uint32_t id, const char *payload, size_t len,
				 uint8_t *out)
{
	if (len > MAX_PAYLOAD) {
		len = MAX_PAYLOAD;
	}
	out[0] = MAGIC0;
	out[1] = MAGIC1;
	out[2] = MAGIC2;
	out[3] = type;
	sys_put_le32(id, &out[4]);
	out[8] = (uint8_t)len;
	memcpy(&out[9], payload, len);
	return FRAME_HEADER_SIZE + len;
}

static bool parse_frame(const uint8_t *in, size_t len, uint8_t expected_type,
				uint32_t *id, const uint8_t **payload, size_t *payload_len)
{
	if (len < FRAME_HEADER_SIZE || in[0] != MAGIC0 || in[1] != MAGIC1 || in[2] != MAGIC2 ||
	    in[3] != expected_type || in[8] > MAX_PAYLOAD || 9 + in[8] != len) {
		return false;
	}
	*id = sys_get_le32(&in[4]);
	*payload = &in[9];
	*payload_len = in[8];
	return true;
}

static int send_lora_frame(uint8_t *frame, size_t len)
{
	int tx_rc;
	int rx_rc;
	k_mutex_lock(&radio_lock, K_FOREVER);
	/* Stop continuous async RX before taking the modem for TX. */
	(void)lora_recv_async(lora_dev, NULL, NULL);
	tx_rc = radio_configure(true);
	if (tx_rc == 0) {
		tx_rc = lora_send(lora_dev, frame, len);
	}
	if (tx_rc == 0) {
		lora_tx_count++;
	}

	/* TX owns the modem only for this operation. Restore continuous RX once,
	 * here in the ordinary path; the Basics Modem callback already keeps RX
	 * alive and must not reacquire it. */
	rx_rc = radio_configure(false);
	if (rx_rc == 0) {
		rx_rc = lora_recv_async(lora_dev, lora_rx_cb, NULL);
	}
	if (rx_rc != 0) {
		printk("LORA_RX_RESTORE_FAIL rc=%d\n", rx_rc);
	}
	k_mutex_unlock(&radio_lock);
	return tx_rc != 0 ? tx_rc : rx_rc;
}

static void start_scan(void)
{
	int rc = bt_le_scan_start(BT_LE_SCAN_PASSIVE, NULL);
	if (rc == 0 || rc == -EALREADY) {
		scan_running = true;
		printk("BLE_SCAN_READY\n");
	} else {
		printk("BLE_SCAN_FAIL rc=%d\n", rc);
	}
}

static void start_lora_receive(void)
{
	int rc;
	k_mutex_lock(&radio_lock, K_FOREVER);
	rc = radio_configure(false);
	if (rc == 0) {
		rc = lora_recv_async(lora_dev, lora_rx_cb, NULL);
	}
	k_mutex_unlock(&radio_lock);
	if (rc != 0) {
		printk("LORA_RX_START_FAIL rc=%d\n", rc);
	}
}

static int advertise_response(uint32_t id, const char *payload, size_t len)
{
	int rc;
	uint8_t frame[MAX_FRAME];
	size_t frame_len = make_frame(TYPE_RESPONSE, id, payload, len, frame);
	adv_buf[0] = COMPANY_ID & 0xff;
	adv_buf[1] = COMPANY_ID >> 8;
	memcpy(&adv_buf[2], frame, frame_len);
	adv_len = frame_len + 2;
	adv_data = (struct bt_data)BT_DATA(BT_DATA_MANUFACTURER_DATA, adv_buf, adv_len);
	(void)bt_le_adv_stop();
	rc = bt_le_adv_start(BT_LE_ADV_NCONN_IDENTITY, &adv_data, 1, NULL, 0);
	if (rc == 0) {
		ble_adv_count++;
		printk("BLE_RESPONSE id=%08x payload=%.*s\n", id, (int)len, payload);
	}
	return rc;
}

static bool scan_data_cb(struct bt_data *data, void *user_data)
{
	uint32_t id;
	const uint8_t *payload;
	size_t len;
	ARG_UNUSED(user_data);
	if (data->type != BT_DATA_MANUFACTURER_DATA || data->data_len < 11 ||
	    sys_get_le16(data->data) != COMPANY_ID) {
		return true;
	}
	if (!parse_frame(&data->data[2], data->data_len - 2, TYPE_RESPONSE, &id, &payload, &len)) {
		ble_drop_count++;
		return true;
	}
	printk("BLE_RX id=%08x payload=%.*s\n", id, (int)len, payload);
	if (outstanding_id != 0 && id == outstanding_id && len == outstanding_len &&
	    memcmp(payload, outstanding_payload, len) == 0) {
		ble_match_count++;
		printk("BRIDGE_COMPLETE id=%08x payload=%.*s\n", id, (int)len, payload);
		outstanding_id = 0;
		outstanding_len = 0;
		memset(outstanding_payload, 0, sizeof(outstanding_payload));
	} else {
		ble_drop_count++;
	}
	return true;
}

static void scan_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *ad)
{
	ARG_UNUSED(info);
	bt_data_parse(ad, scan_data_cb, NULL);
}

static void lora_rx_cb(const struct device *dev, uint8_t *data, uint16_t size,
			       int16_t rssi, int8_t snr, void *user_data)
{
	uint32_t id;
	const uint8_t *payload;
	size_t len;
	char text[MAX_PAYLOAD + 1];
	ARG_UNUSED(dev); ARG_UNUSED(rssi); ARG_UNUSED(snr); ARG_UNUSED(user_data);
	lora_rx_count++;
	if (!parse_frame(data, size, TYPE_REQUEST, &id, &payload, &len)) {
		lora_drop_count++;
		printk("LORA_RX_DROP size=%u\n", size);
		return;
	}
	printk("LORA_RX id=%08x payload=%.*s\n", id, (int)len, payload);
	if (role != ROLE_RESPONDER) {
		return;
	}
	memcpy(text, payload, len);
	text[len] = '\0';
	if (advertise_response(id, text, len) == 0) {
		printk("BRIDGE_RESPONDED id=%08x payload=%.*s\n", id, (int)len, text);
	}
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc); ARG_UNUSED(argv);
	shell_print(sh, "role=%s lora=%s ble=%s scan=%s", role == ROLE_INITIATOR ? "initiator" : "responder",
		    lora_ready ? "ready" : "not-ready", ble_ready ? "ready" : "not-ready",
		    scan_running ? "on" : "off");
	shell_print(sh, "freq=915000000 bw=125 sf=9 txp=14 lora_tx=%u lora_rx=%u lora_drop=%u ble_adv=%u ble_match=%u ble_drop=%u",
		    lora_tx_count, lora_rx_count, lora_drop_count, ble_adv_count, ble_match_count, ble_drop_count);
	return 0;
}

static int cmd_role(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2 || (strcmp(argv[1], "initiator") && strcmp(argv[1], "responder"))) {
		shell_error(sh, "usage: role initiator|responder");
		return -EINVAL;
	}
	role = !strcmp(argv[1], "responder") ? ROLE_RESPONDER : ROLE_INITIATOR;
	if (role == ROLE_RESPONDER) {
		if (scan_running) {
			(void)bt_le_scan_stop();
			scan_running = false;
		}
	} else if (ble_ready) {
		start_scan();
	}
	shell_print(sh, "ROLE_SET %s", argv[1]);
	return 0;
}

static int cmd_bridge_start(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t frame[MAX_FRAME];
	size_t len;
	int rc;
	if (role != ROLE_INITIATOR || argc != 2 || strlen(argv[1]) == 0 || strlen(argv[1]) > MAX_PAYLOAD) {
		shell_error(sh, "usage: bridge start <1..18 chars> (initiator only)");
		return -EINVAL;
	}
	outstanding_id = next_id++;
	outstanding_len = strlen(argv[1]);
	memcpy(outstanding_payload, argv[1], outstanding_len + 1);
	len = make_frame(TYPE_REQUEST, outstanding_id, argv[1], outstanding_len, frame);
	rc = send_lora_frame(frame, len);
	if (rc) {
		outstanding_id = 0;
		outstanding_len = 0;
		memset(outstanding_payload, 0, sizeof(outstanding_payload));
		shell_error(sh, "LoRa request failed rc=%d", rc);
		return rc;
	}
	shell_print(sh, "BRIDGE_SENT id=%08x payload=%s", outstanding_id, argv[1]);
	return 0;
}

static int cmd_lora_send(const struct shell *sh, size_t argc, char **argv)
{
	uint8_t frame[MAX_FRAME];
	int rc;
	if (argc != 2 || strlen(argv[1]) > MAX_PAYLOAD) return -EINVAL;
	/* Manual LoRa uses the same framed production path with a reserved id. */
	(void)make_frame(TYPE_REQUEST, 0, argv[1], strlen(argv[1]), frame);
	rc = send_lora_frame(frame, FRAME_HEADER_SIZE + strlen(argv[1]));
	shell_print(sh, "LORA_SEND rc=%d payload=%s", rc, argv[1]);
	return rc;
}

static int cmd_ble_adv(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t id;
	char *end;
	int rc;
	if (argc != 3 || strlen(argv[2]) > MAX_PAYLOAD) return -EINVAL;
	id = strtoul(argv[1], &end, 16);
	if (*end) return -EINVAL;
	rc = advertise_response(id, argv[2], strlen(argv[2]));
	shell_print(sh, "BLE_ADV rc=%d id=%08x payload=%s", rc, id, argv[2]);
	return rc;
}

static int cmd_ble_scan(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh); ARG_UNUSED(argc); ARG_UNUSED(argv);
	start_scan();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(lora_cmds,
	SHELL_CMD_ARG(send, NULL, "send framed LoRa payload", cmd_lora_send, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(ble_cmds,
	SHELL_CMD_ARG(adv, NULL, "advertise id and payload", cmd_ble_adv, 3, 0),
	SHELL_CMD(scan, NULL, "start BLE scan", cmd_ble_scan),
	SHELL_SUBCMD_SET_END);
SHELL_STATIC_SUBCMD_SET_CREATE(bridge_cmds,
	SHELL_CMD_ARG(start, NULL, "send reverse bridge request", cmd_bridge_start, 2, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(status, NULL, "show bridge state", cmd_status);
SHELL_CMD_REGISTER(role, NULL, "set runtime role", cmd_role);
SHELL_CMD_REGISTER(lora, &lora_cmds, "manual LoRa", NULL);
SHELL_CMD_REGISTER(ble, &ble_cmds, "manual BLE", NULL);
SHELL_CMD_REGISTER(bridge, &bridge_cmds, "reverse LoRa-to-BLE bridge", NULL);

static void bt_ready(int err)
{
	if (err) {
		printk("BLE_INIT_FAIL rc=%d\n", err);
		return;
	}
	ble_ready = true;
	printk("BLE_READY frame=RB2/type/id/len/payload company=ffff\n");
	scan_callbacks.recv = scan_cb;
	bt_le_scan_cb_register(&scan_callbacks);
	if (role == ROLE_INITIATOR) {
		start_scan();
	}
}

int main(void)
{
	boot_cookie = 0xB0070001;
	k_mutex_init(&radio_lock);
	int rc;
	printk("RB2_BOOT role=initiator mcu=nRF52840-QIAA radio=SX1262 wiring=MOSI=P1.15,MISO=P1.14,SCK=P1.13,NSS=P0.04,BUSY=P0.29,RESET=P0.28,DIO1=P0.03,RF_SW=P0.05\n");
	if (!device_is_ready(lora_dev)) {
		printk("LORA_NOT_READY\n");
	} else if (radio_configure(false) == 0) {
		lora_ready = true;
		printk("LORA_READY freq=915000000 bw=125 sf=9\n");
		start_lora_receive();
	} else {
		printk("LORA_CONFIG_FAIL\n");
	}
	rc = bt_enable(bt_ready);
	if (rc) printk("BLE_ENABLE_FAIL rc=%d\n", rc);
	return 0;
}
