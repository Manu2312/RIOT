/*
 * Copyright (C) 2016 Unwired Devices
 *
 * This file is subject to the terms and conditions of the GNU Lesser
 * General Public License v2.1. See the file LICENSE in the top level
 * directory for more details.
 */

/**
 * @defgroup    
 * @ingroup     
 * @brief       
 * @{
 * @file		ls_end_device.c
 * @brief       Implementation of LoRa-Star stack for end-device
 * @author      Eugene Ponomarev
 */

#ifdef __cplusplus
extern "C" {
#endif

#include "random.h"
#include "assert.h"
#include "thread.h"

#include "ls-mac-types.h"
#include "ls-mac.h"
#include "ls-gate.h"

#include <stdint.h>

/**
 * Channels table. Frequency in Hz
 */
static uint32_t channels_table[3] = {
		868100000,
		868300000,
		868500000
};

/**
 * Data rates table.
 */
static uint8_t datarate_table[7][3] = {
    { SF12, BW_125_KHZ, CR_4_5 },       /* DR0 */
    { SF11, BW_125_KHZ, CR_4_5 },       /* DR1 */
    { SF10, BW_125_KHZ, CR_4_5 },       /* DR2 */
    { SF9, BW_125_KHZ, CR_4_5 },        /* DR3 */
    { SF8, BW_125_KHZ, CR_4_5 },        /* DR4 */
    { SF7, BW_125_KHZ, CR_4_5 },        /* DR5 */
    { SF7, BW_250_KHZ, CR_4_5 },        /* DR6 */
};

static msg_t msg_ping;

static void prepare_sx1276(ls_gate_channel_t *ch)
{
	/* Choose data rate */
    uint8_t *datarate = datarate_table[ch->dr];

    /* Setup transceiver settings according to datarate */
    sx1276_lora_settings_t settings;

    settings.datarate = datarate[0];
    settings.bandwidth = datarate[1];
    settings.coderate = datarate[2];

    settings.crc_on = true;
    settings.freq_hop_on = false;
    settings.hop_period = 0;
    settings.implicit_header = false;
    settings.iq_inverted = false;
    settings.low_datarate_optimize = false;
    settings.payload_len = 0;
    settings.power = TX_OUTPUT_POWER;
    settings.preamble_len = LORA_PREAMBLE_LENGTH;
    settings.rx_continuous = true;
    settings.tx_timeout = 1e6 * 30; // 30 sec
    settings.rx_timeout = LORA_SYMBOL_TIMEOUT;

    sx1276_configure_lora(ch->_internal.sx1276, &settings);

    /* Setup channel */
    ls_channel_t c = ch->ch;
    sx1276_set_channel(ch->_internal.sx1276, channels_table[c]);
}

static int send_frame(ls_gate_channel_t *ch, ls_addr_t to, ls_type_t type, uint8_t *buf, size_t buflen) {
	assert(ch != NULL);

	ls_gate_t *ls = (ls_gate_t *) ch->_internal.gate;

	prepare_sx1276(ch);

	ls_frame_t frame;
	ls_assemble_frame(to, type, buf, buflen, &frame);

	/* Send frame */
    size_t header_size = sizeof(ls_header_t) + sizeof(ls_payload_len_t);
    size_t payload_size = 0;

    /* The JOIN_ACK frame must be encrypted with the special join key */
    ls_gate_node_t *node;

    uint8_t mic_key[AES_BLOCK_SIZE];
	uint8_t aes_key[AES_BLOCK_SIZE];

	switch (type) {
	case LS_DL_JOIN_ACK:
		ls_encrypt_frame(ls->settings.join_key, ls->settings.join_key, &frame, &payload_size);
		break;

	case LS_DL_ACK:
		node = ls_devlist_get(&ls->devices, to);

		ls_derive_keys(node->last_nonce, node->app_nonce, node->addr, mic_key, NULL);

		ls_encrypt_frame(mic_key, mic_key, &frame, &payload_size);
		break;

	default:
		node = ls_devlist_get(&ls->devices, to);

		ls_derive_keys(node->last_nonce, node->app_nonce, node->addr, mic_key, aes_key);
		ls_encrypt_frame(mic_key, aes_key, &frame, &payload_size);
	}

	/* Anti-collision: sleeping while channel is seems to be occupied */
	while (!sx1276_is_channel_free(ch->_internal.sx1276, channels_table[ch->ch], LS_CHANNEL_FREE_RSSI)) {
		puts("ls-gate: channel is occupied!"); // XXX: debug

		uint16_t millis = random_uint32_range(LS_TX_DELAY_MIN_MS, LS_TX_DELAY_MAX_MS);
		xtimer_usleep(1e3 * millis);
	}

	/* Send frame into LoRa PHY */
	sx1276_send(ch->_internal.sx1276, (uint8_t *) &frame, header_size + payload_size);

	return LS_GATE_OK;
}

static inline void send_join_ack(ls_gate_t *ls, ls_gate_channel_t *ch, uint64_t dev_id, ls_addr_t addr, uint32_t app_nonce) {
	ls_join_ack_t ack = { .addr = addr, .dev_id = dev_id, .app_nonce = app_nonce };

	send_frame(ch, addr, LS_DL_JOIN_ACK, (uint8_t *) &ack, sizeof(ls_join_ack_t));
}

static inline void send_ack(ls_gate_t *ls, ls_gate_channel_t *ch, ls_addr_t addr) {
	send_frame(ch, addr, LS_DL_ACK, NULL, 0);
}

static inline void send_lnkchk_ack(ls_gate_t *ls, ls_gate_channel_t *ch, ls_addr_t addr, bool has_pending) {
	send_frame(ch, addr, (has_pending) ? LS_DL_LNKCHK_P : LS_DL_LNKCHK, NULL, 0);
}

static void device_join_req(ls_gate_t *ls, ls_gate_channel_t *ch, uint64_t dev_id, uint64_t app_id, uint32_t dev_nonce, ls_node_class_t node_class, uint64_t ability) {
	/* Check node acceptance */
	if (ls->accept_node_join_cb != NULL) {
		if (!ls->accept_node_join_cb(dev_id, app_id))
			return;
	}

	/* Check that nonce doesn't repeated */
	ls_gate_devices_t *devlist = &ls->devices;

	if (!ls_devlist_check_nonce(devlist, dev_id, dev_nonce))
		return;

	ls_gate_node_t *node;
	if (!ls_devlist_is_added(devlist, dev_id)) {
		/* Add node to the devices list */
		node = ls_devlist_add(devlist, dev_id, app_id, dev_nonce, ch);
	} else {
		node = add_nonce(devlist, dev_id, dev_nonce);
	}

	/* Set node's class */
	node->node_class = node_class;

	/* Update node's channel */
	node->node_ch = ch;

	/* Update node's last seen time */
	node->last_seen = ls->_internal.ping_count;

	/* Call join handler which returns an app nonce from the application side */
	node->app_nonce = ls->node_joined_cb(node);

	/* Set node's ability bit map */
	node->node_ability = ability;

	/* Send join ACK */
	send_join_ack(ls, ch, dev_id, node->addr, node->app_nonce);
}

static bool app_data_recv(ls_gate_t *ls, ls_gate_channel_t *ch, ls_frame_t *frame) {
	/* Address must be defined */
	if (frame->header.dev_addr == LS_ADDR_UNDEFINED)
		return false;

	ls_gate_node_t *node = ls_devlist_get(&ls->devices, frame->header.dev_addr);
	if (node == NULL)	/* The node must be joined to the network */
		return false;

	/* Derive encryption keys */
    uint8_t mic_key[AES_BLOCK_SIZE];
    uint8_t aes_key[AES_BLOCK_SIZE];
	ls_derive_keys(node->last_nonce, node->app_nonce, node->addr, mic_key, aes_key);

	/* Validate MIC */
	if (!ls_validate_frame_mic(mic_key, frame))
		return false;

	/* Update node's last seen time */
	node->last_seen = ls->_internal.ping_count;

	/* Decrypt frame payload */
	ls_decrypt_frame_payload(aes_key, &frame->payload);

	/* Call handler callback */
	ls->app_data_received_cb(node, ch, frame->payload.data, frame->payload.len);

	return true;
}

static bool frame_recv(ls_gate_t *ls, ls_gate_channel_t *ch, ls_frame_t *frame) {
	switch (frame->header.type) {
	case LS_UL_ACK: {
		/* Address must be defined */
		if (frame->header.dev_addr == LS_ADDR_UNDEFINED)
			return false;

		ls_gate_node_t *node = ls_devlist_get(&ls->devices, frame->header.dev_addr);
		if (node == NULL)	/* The node must be joined to the network */
			return false;

		/* Derive encryption keys */
	    uint8_t mic_key[AES_BLOCK_SIZE];
	    uint8_t aes_key[AES_BLOCK_SIZE];
		ls_derive_keys(node->last_nonce, node->app_nonce, node->addr, mic_key, aes_key);

		/* Validate MIC */
		if (!ls_validate_frame_mic(mic_key, frame))
			return false;

		if (ls->app_data_ack_cb != NULL)
			ls->app_data_ack_cb(node, ch);

		return true;
	}

	case LS_UL_CONF:	/* Uplink data confirmed */
		if (!app_data_recv(ls, ch, frame))
				return false;

		send_ack(ls, ch, frame->header.dev_addr);
		return true;

	case LS_UL_UNC:		/* Uplink data unconfirmed */
		if (!app_data_recv(ls, ch, frame))
			return false;

		return true;

	case LS_UL_LNKCHK:		/* Link check request */
		/* Address must be defined */
		if (frame->header.dev_addr == LS_ADDR_UNDEFINED)
			return false;

		ls_gate_node_t *node = ls_devlist_get(&ls->devices, frame->header.dev_addr);
		if (node == NULL)	/* The node must be joined to the network */
			return false;

		/* Derive encryption keys */
	    uint8_t mic_key[AES_BLOCK_SIZE];
	    uint8_t aes_key[AES_BLOCK_SIZE];
		ls_derive_keys(node->last_nonce, node->app_nonce, node->addr, mic_key, aes_key);

		/* Validate MIC */
		if (!ls_validate_frame_mic(mic_key, frame))
			return false;

		/* Update node's last seen time */
		node->last_seen = ls->_internal.ping_count;

		/* Decrypt frame payload */
		ls_decrypt_frame_payload(aes_key, &frame->payload);

		/* Copy status information into device record */
		memcpy(&node->status, &frame->payload.data, sizeof(ls_device_status_t));

		/* Send acknowledge */
		send_lnkchk_ack(ls, ch, frame->header.dev_addr, node->num_pending > 0);

		/* Decrease pending frames counter */
		if (node->num_pending)
			node->num_pending--;

		/* Notify application about link check to send pending frames */
		if (ls->link_ok_cb != NULL)
			ls->link_ok_cb(node, ch);

		return true;

	case LS_UL_JOIN_REQ:	/* Join request */
		/* Address must be undefined */
		if (frame->header.dev_addr != LS_ADDR_UNDEFINED)
			return false;

		/* Check packet size */
		if (frame->payload.len != sizeof(ls_join_req_t))
			return false;

		if (!ls_validate_frame_mic(ls->settings.join_key, frame))
			return false;

		ls_decrypt_frame_payload(ls->settings.join_key, &frame->payload);

		ls_join_req_t req;
		memcpy(&req, &frame->payload.data, sizeof(ls_join_req_t));

		uint64_t dev_id = req.dev_id;
		uint64_t app_id = req.app_id;
		uint32_t dev_nonce = req.dev_nonce;
		ls_node_class_t node_class = req.node_class;
		uint64_t node_ability = req.node_ability;

		device_join_req(ls, ch, dev_id, app_id, dev_nonce, node_class, node_ability);

		return true;

	default:
	case LS_DL_ACK:			/* Downlink frame acknowledge for confirmed messages */
	case LS_DL:				/* Downlink frame */
	case LS_DL_JOIN_ACK:	/* Downlink join acknowledge */
	case LS_DL_LNKCHK:
		/* Not interested in somehow received downlink frames */
		return false;
	}
}

static void *sx1276_handler(void *arg) {
	assert(arg != NULL);

    puts("ls-gate: sx1276 event handler thread started"); // XXX: debug

    ls_gate_channel_t *ch = (ls_gate_channel_t *) arg;
    ls_gate_t *ls = (ls_gate_t *) ch->_internal.gate;

    msg_init_queue(ch->_internal.sx1276_event_queue, sizeof(ch->_internal.sx1276_event_queue));
    msg_t msg;

    while (1) {
        msg_receive(&msg);

        sx1276_event_t *event = (sx1276_event_t *) msg.content.ptr;
        sx1276_rx_packet_t *packet = (sx1276_rx_packet_t *) event->event_data;

        switch (event->type) {
            case RX_DONE:
            	//printf("RX: %d bytes\n", (unsigned int) packet->size);
            	;

                /* Copy packet's data as a frame to our stack */
                ls_frame_t frame;
                memcpy(&frame, packet->content, packet->size);

                /* It's necessary to free the memory for the content because it was allocated dynamically in the sx1276 library */
                free(packet->content);

                /* Check frame format */
                if (ls_validate_frame((uint8_t *) packet->content, packet->size)) {
                	if (!frame_recv(ls, ch, &frame)) {
                		//puts("ls-gate: well-formed frame discarded");
                	}
                } else {
                	//puts("ls-gate: malformed data discarded");
                }

                break;

            case RX_ERROR_CRC:
                break;

            case TX_DONE:
                //puts("sx1276: transmission done.");
            	prepare_sx1276(ch);
                sx1276_set_rx(ch->_internal.sx1276, ch->_internal.sx1276->settings.lora.rx_timeout);

                break;

            case RX_TIMEOUT:
                break;

            case TX_TIMEOUT:
            	prepare_sx1276(ch);
                sx1276_set_rx(ch->_internal.sx1276, ch->_internal.sx1276->settings.lora.rx_timeout);
                break;

            default:
                printf("sx1276: received event #%d\n", (int) event->type);
                break;
        }
    }

	return NULL;
}

static void *tim_handler(void *arg) {
	assert(arg != NULL);

	ls_gate_t *ls = (ls_gate_t *) arg;
    msg_init_queue(ls->_internal.tim_msg_queue, sizeof(ls->_internal.tim_msg_queue));
    msg_t msg;

    while (1) {
        msg_receive(&msg);

        ls_gate_tim_cmd_t cmd = (ls_gate_tim_cmd_t) msg.content.value;

        switch (cmd) {
        case LS_GATE_PING:
        	ls->_internal.ping_count++;

        	/* Kick all devices with big ping difference */
        	for (int i = 0; i < LS_GATE_MAX_NODES; i++) {
        		if (!ls->devices.nodes_free_list[i]) {
            		ls_gate_node_t *node = &ls->devices.nodes[i];

					int diff = ls->_internal.ping_count - node->last_seen;

					if (diff >= LS_MAX_PING_DIFFERENCE) {
						/* Kick node */
						ls_devlist_remove_device(&ls->devices, i);

						/* Notify application code about kicked node */
						if (ls->node_kicked_cb != NULL)
							ls->node_kicked_cb(node);
					}
        		}
        	}

        	/* Restart timer */
        	xtimer_set_msg(&ls->_internal.ping_timer, LS_PING_TIMEOUT, &msg_ping, ls->_internal.tim_thread_pid);
        	break;
        }
    }

    return NULL;
}

/**
 * @brief Creates timeout timers message handler thread.
 */
static bool create_tim_handler_thread(ls_gate_t *ls) {
    puts("ls_init: creating timeouts handler thread...");

    kernel_pid_t pid_tim = thread_create(ls->_internal.tim_thread_stack, sizeof(ls->_internal.tim_thread_stack), THREAD_PRIORITY_MAIN - 2,
                                        THREAD_CREATE_STACKTEST, tim_handler, ls,
                                        "LS timeouts handler thread");

    if (pid_tim <= KERNEL_PID_UNDEF) {
        puts("ls_init: creation of timer handler thread failed");
        return false;
    }

    ls->_internal.tim_thread_pid = pid_tim;

    return true;
}

static bool open_channel(ls_gate_channel_t *ch) {
	assert(ch != NULL);
	printf("ls_gate_init: opening channel #%d with datarate DR%d\n", (unsigned int) ch->ch, (unsigned int) ch->dr);

	/* Launch channel listener thread */
    kernel_pid_t pid = thread_create(ch->_internal.sx1276_listener_thread_stack,
    		sizeof(ch->_internal.sx1276_listener_thread_stack),
			THREAD_PRIORITY_MAIN - 1, THREAD_CREATE_STACKTEST, sx1276_handler, ch,
                                     "sx1276 channel thread");

    if (pid <= KERNEL_PID_UNDEF) {
    	printf("ls_gate_init: opening channel #%d failed", (unsigned int) ch->ch);
        return false;
    }

    ch->_internal.sx1276->event_handler_thread_pid = pid;

	/* Initialize and configure the transceiver for this channel */
	prepare_sx1276(ch);

    /* Set channel to receive */
    sx1276_set_rx(ch->_internal.sx1276, ch->_internal.sx1276->settings.lora.rx_timeout);

    return true;
}

static bool initialize_channels(ls_gate_t *ls) {
	for (int i = 0; i < ls->num_channels; i++) {
		ls_gate_channel_t* ch = &ls->channels[i];
		assert(ch->_internal.sx1276 != NULL);

		ch->_internal.gate = ls;

		if (!open_channel(ch)) {
			return false;
		}
	}

	return true;
}

/**
 * @brief Initializes the internal gate structures, channels, transceivers, start listening threads.
 */
int ls_gate_init(ls_gate_t *ls) {
	assert(ls != NULL);
	assert(ls->channels != NULL);
	assert(ls->num_channels > 0);

	msg_ping.content.value = LS_GATE_PING;

	if (!create_tim_handler_thread(ls)) {
		return -LS_INIT_E_TIM_THREAD;
	}

	/* Start ping timer */
	xtimer_set_msg(&ls->_internal.ping_timer, LS_PING_TIMEOUT, &msg_ping, ls->_internal.tim_thread_pid);

	ls_devlist_init(&ls->devices);
	initialize_channels(ls);

	return LS_GATE_OK;
}

/**
 * @brief Sends an answer to the node in channel assigned to the node.
 *
 */
int ls_gate_send_to(ls_gate_t *ls, ls_addr_t addr, uint8_t *buf, size_t bufsize) {
	if (!ls_devlist_is_in_network(&ls->devices, addr))
		return -LS_GATE_E_NODEV;

	ls_gate_node_t *node = ls_devlist_get(&ls->devices, addr);
	if (node == NULL)
		return -LS_GATE_E_NODEV;

	printf("Sending %u bytes to 0x%08X\n", (unsigned) bufsize, (unsigned) addr);

	/* Send frame as soon as possible */
	send_frame((ls_gate_channel_t *) node->node_ch, addr, LS_DL, buf, bufsize);

	return LS_GATE_OK;
}

/**
 * @brief Broadcasts a packet to all nodes and channels.
 */
int ls_gate_broadcast(ls_gate_t *ls, uint8_t *buf, size_t bufsize) {
	return LS_GATE_OK;
}

/**
 * @brief Puts gate into sleep mode.
 */
void ls_gate_sleep(ls_gate_t *ls) {
	/* Set all channel transceivers into sleep mode */
	for (int i = 0; i < ls->num_channels; i++) {
		sx1276_t *sx1276 = ls->channels[i]._internal.sx1276;

		sx1276_set_sleep(sx1276);
	}
}

#ifdef __cplusplus
}
#endif
