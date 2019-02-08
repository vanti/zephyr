/*
 * Copyright (c) 2017 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(net_mqtt_publisher_sample, LOG_LEVEL_DBG);

#include <zephyr.h>
#include <gpio.h>
#include <net/socket.h>
#include <net/mqtt.h>

#include <misc/printk.h>
#include <string.h>
#include <errno.h>

#include "config.h"

/* MQTT TOPIC */
#define TOPIC "ew_demo"

/* LED stuff */
#define PORT0  LED0_GPIO_CONTROLLER
#define PORT1  LED1_GPIO_CONTROLLER

#define LED0 LED0_GPIO_PIN
#define LED1 LED1_GPIO_PIN

/*Button stuff */
/* change this to use another GPIO port */
#ifndef SW0_GPIO_CONTROLLER
#ifdef SW0_GPIO_NAME
#define SW0_GPIO_CONTROLLER SW0_GPIO_NAME
#else
#error SW0_GPIO_NAME or SW0_GPIO_CONTROLLER needs to be set in board.h
#endif
#endif
#define PORT    SW0_GPIO_CONTROLLER

/* change this to use another GPIO pin */
#ifdef SW0_GPIO_PIN
#define PIN     SW0_GPIO_PIN
#else
#error SW0_GPIO_PIN needs to be set in board.h
#endif

/* change to use another GPIO pin interrupt config */
#ifdef SW0_GPIO_FLAGS

#define EDGE    (SW0_GPIO_FLAGS | GPIO_INT_EDGE)
#else
/*
 * If SW0_GPIO_FLAGS not defined used default EDGE value.
 * Change this to use a different interrupt trigger
 */
#define EDGE    (GPIO_INT_EDGE | GPIO_INT_ACTIVE_LOW)
#endif

/* change this to enable pull-up/pull-down */
#ifndef SW0_GPIO_FLAGS
#ifdef SW0_GPIO_PIN_PUD
#define SW0_GPIO_FLAGS SW0_GPIO_PIN_PUD
#else
#define SW0_GPIO_FLAGS 0
#endif
#endif

#define PULL_UP SW0_GPIO_FLAGS

/* Buffers for MQTT client. */
static u8_t rx_buffer[APP_MQTT_BUFFER_SIZE];
static u8_t tx_buffer[APP_MQTT_BUFFER_SIZE];

/* The mqtt client struct */
static struct mqtt_client client_ctx;

/* MQTT Broker details. */
static struct sockaddr_storage broker;

static struct pollfd fds[1];
static int nfds;

static bool connected;

/* Maximum size of incoming data */
#define MAX_PAYLOAD 1024
static u8_t payload[MAX_PAYLOAD];

/* prototypes */
static void publisher(void);
static void wait(int timeout);
static char *get_mqtt_topic(void);

#if defined(CONFIG_MQTT_LIB_TLS)

#include "test_certs.h"

#define TLS_SNI_HOSTNAME "test.mosquitto.org" //"192.168.1.102"
#define APP_CA_CERT_TAG 1
#define APP_PSK_TAG 2

static sec_tag_t m_sec_tags[] = {
#if defined(MBEDTLS_X509_CRT_PARSE_C) || defined(CONFIG_NET_SOCKETS_OFFLOAD)
		APP_CA_CERT_TAG,
#endif
#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
		APP_PSK_TAG,
#endif
};

static struct device *gpio0, *gpio1;
int led_cnt = 0;


//K_SEM_DEFINE(sem_publish, 0, 1)
//static struct k_sem sem_publish;
static int buttonPressed = 0;

void button_pressed(struct device *gpiob, struct gpio_callback *cb,
                    u32_t pins)
{
        printk("Button pressed at %d\n", k_cycle_get_32());
	//k_sem_give(&sem_publish);
	buttonPressed = 1;
}

static int app_subscribe(void)
{
        int rc;
        struct mqtt_topic topic;
        struct mqtt_subscription_list sub;

        topic.topic.utf8 = "sensors"; //get_mqtt_topic();
        topic.topic.size = strlen(topic.topic.utf8);
        topic.qos = MQTT_QOS_1_AT_LEAST_ONCE;
        sub.list = &topic;
        sub.list_count = 1;
        sub.message_id = sys_rand32_get();

        rc = mqtt_subscribe(&client_ctx, &sub);
        if (rc != 0) {
                return -1;
        }

        wait(APP_SLEEP_MSECS);
        rc = mqtt_input(&client_ctx);
		if (rc != 0) {
			return -1;
		}

        return 0;
}

static struct gpio_callback gpio_cb;

static int tls_init(void)
{
	int err = -EINVAL;

#if defined(MBEDTLS_X509_CRT_PARSE_C) || defined(CONFIG_NET_SOCKETS_OFFLOAD)
	err = tls_credential_add(APP_CA_CERT_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
				 ca_certificate, sizeof(ca_certificate));
	if (err < 0) {
		LOG_ERR("Failed to register public certificate: %d", err);
		return err;
	}
#endif

#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
	err = tls_credential_add(APP_PSK_TAG, TLS_CREDENTIAL_PSK,
				 client_psk, sizeof(client_psk));
	if (err < 0) {
		LOG_ERR("Failed to register PSK: %d", err);
		return err;
	}

	err = tls_credential_add(APP_PSK_TAG, TLS_CREDENTIAL_PSK_ID,
				 client_psk_id, sizeof(client_psk_id) - 1);
	if (err < 0) {
		LOG_ERR("Failed to register PSK ID: %d", err);
	}
#endif

	return err;
}

#endif /* CONFIG_MQTT_LIB_TLS */

static void prepare_fds(struct mqtt_client *client)
{
	if (client->transport.type == MQTT_TRANSPORT_NON_SECURE) {
		fds[0].fd = client->transport.tcp.sock;
	}
#if defined(CONFIG_MQTT_LIB_TLS)
	else if (client->transport.type == MQTT_TRANSPORT_SECURE) {
		fds[0].fd = client->transport.tls.sock;
	}
#endif

	fds[0].events = ZSOCK_POLLIN;
	nfds = 1;
}

static void clear_fds(void)
{
	nfds = 0;
}

static void wait(int timeout)
{
	if (nfds > 0) {
		if (poll(fds, nfds, timeout) < 0) {
			printk("poll error: %d\n", errno);
		}
	}
}


void publish_handler(struct mqtt_client *const client,
		    const struct mqtt_evt *evt)
{
	int rc;
	u8_t buf[16];
	u32_t read = 0U;

	printk("message received. Size=%d\n",
		evt->param.publish.message.payload.len);

	if (evt->result != 0) {
		printk("MQTT PUBLISH error: %d\n", evt->result);
		goto error;
	}

	while (read != evt->param.publish.message.payload.len) {
		wait(APP_SLEEP_MSECS);
		rc = mqtt_read_publish_payload(client, buf, sizeof(buf));
		if (rc <= 0 && rc != -EAGAIN) {
			printk("Failed to receive payload, err: %d\n", -rc);
			goto error;
		}

		if (MAX_PAYLOAD < read + rc) {
			printk("Too much data received!\n");
			goto error;
		}
		
		read += rc;
	}

	gpio_pin_write(gpio1, LED1, (++led_cnt) % 2);

error:
	return;
}

void mqtt_evt_handler(struct mqtt_client *const client,
		      const struct mqtt_evt *evt)
{
	int err;

	switch (evt->type) {
	case MQTT_EVT_CONNACK:
		if (evt->result != 0) {
			printk("MQTT connect failed %d\n", evt->result);
			break;
		}

		connected = true;
		printk("[%s:%d] MQTT client connected!\n", __func__, __LINE__);

		break;

	case MQTT_EVT_DISCONNECT:
		printk("[%s:%d] MQTT client disconnected %d\n", __func__,
		       __LINE__, evt->result);

		connected = false;
		clear_fds();

		break;

	case MQTT_EVT_PUBLISH:
		publish_handler(client, evt);

		break;

	case MQTT_EVT_PUBACK:
		if (evt->result != 0) {
			printk("MQTT PUBACK error %d\n", evt->result);
			break;
		}

		printk("[%s:%d] PUBACK packet id: %u\n", __func__, __LINE__,
				evt->param.puback.message_id);

		break;

	case MQTT_EVT_PUBREC:
		if (evt->result != 0) {
			printk("MQTT PUBREC error %d\n", evt->result);
			break;
		}

		printk("[%s:%d] PUBREC packet id: %u\n", __func__, __LINE__,
		       evt->param.pubrec.message_id);

		const struct mqtt_pubrel_param rel_param = {
			.message_id = evt->param.pubrec.message_id
		};

		err = mqtt_publish_qos2_release(client, &rel_param);
		if (err != 0) {
			printk("Failed to send MQTT PUBREL: %d\n", err);
		}

		break;

	case MQTT_EVT_PUBCOMP:
		if (evt->result != 0) {
			printk("MQTT PUBCOMP error %d\n", evt->result);
			break;
		}

		printk("[%s:%d] PUBCOMP packet id: %u\n", __func__, __LINE__,
		       evt->param.pubcomp.message_id);

		break;

	case MQTT_EVT_SUBACK:
		if (evt->result != 0) {
			printk("MQTT SUBACK error %d\n", evt->result);
			break;
		}


		printk("[%s:%d] items: %d packet id: %u\n", __func__,
			 __LINE__, evt->param.suback.return_codes.len,
			 evt->param.suback.message_id);


		break;

	case MQTT_EVT_UNSUBACK:
		if (evt->result != 0) {
			printk("MQTT UNSUBACK error %d\n", evt->result);
			break;
		}

		printk("[%s:%d] packet id: %u\n", __func__, __LINE__,
			 evt->param.unsuback.message_id);

		break;

	default:
		printk("[%s:%d] Invalid MQTT packet\n", __func__, __LINE__);
		break;
	}
}

static char *get_mqtt_payload(enum mqtt_qos qos)
{
#if APP_BLUEMIX_TOPIC
	static char payload[30];

	snprintk(payload, sizeof(payload), "{d:{temperature:%d}}",
		 (u8_t)sys_rand32_get());
#else
	static char payload[] = "DOORS:OPEN_QoSx";

	payload[strlen(payload) - 1] = '0' + qos;
#endif

	return payload;
}

static char *get_mqtt_topic(void)
{
#if APP_BLUEMIX_TOPIC
	return "iot-2/type/"BLUEMIX_DEVTYPE"/id/"BLUEMIX_DEVID
	       "/evt/"BLUEMIX_EVENT"/fmt/"BLUEMIX_FORMAT;
#else
	return TOPIC;
#endif
}

static int publish(struct mqtt_client *client, enum mqtt_qos qos)
{
	struct mqtt_publish_param param;

	param.message.topic.qos = qos;
	param.message.topic.topic.utf8 = (u8_t *)get_mqtt_topic();
	param.message.topic.topic.size =
			strlen(param.message.topic.topic.utf8);
	param.message.payload.data = get_mqtt_payload(qos);
	param.message.payload.len =
			strlen(param.message.payload.data);
	param.message_id = sys_rand32_get();
	param.dup_flag = 0;
	param.retain_flag = 0;

	return mqtt_publish(client, &param);
}

#define RC_STR(rc) ((rc) == 0 ? "OK" : "ERROR")

#define PRINT_RESULT(func, rc) \
	printk("[%s:%d] %s: %d <%s>\n", __func__, __LINE__, \
	       (func), rc, RC_STR(rc))

static void broker_init(void)
{
#if defined(CONFIG_NET_IPV6)
	struct sockaddr_in6 *broker6 = (struct sockaddr_in6 *)&broker;

	broker6->sin6_family = AF_INET6;
	broker6->sin6_port = htons(SERVER_PORT);
	inet_pton(AF_INET6, SERVER_ADDR, &broker6->sin6_addr);
#else
	struct sockaddr_in *broker4 = (struct sockaddr_in *)&broker;

	broker4->sin_family = AF_INET;
	broker4->sin_port = htons(SERVER_PORT);
	inet_pton(AF_INET, SERVER_ADDR, &broker4->sin_addr);
#endif
}

static void client_init(struct mqtt_client *client)
{
	mqtt_client_init(client);

	broker_init();

	/* MQTT client configuration */
	client->broker = &broker;
	client->evt_cb = mqtt_evt_handler;
	client->client_id.utf8 = (u8_t *)MQTT_CLIENTID;
	client->client_id.size = strlen(MQTT_CLIENTID);
	client->password = NULL;
	client->user_name = NULL;
	client->protocol_version = MQTT_VERSION_3_1_1;

	/* MQTT buffers configuration */
	client->rx_buf = rx_buffer;
	client->rx_buf_size = sizeof(rx_buffer);
	client->tx_buf = tx_buffer;
	client->tx_buf_size = sizeof(tx_buffer);

	/* MQTT transport configuration */
#if defined(CONFIG_MQTT_LIB_TLS)
	client->transport.type = MQTT_TRANSPORT_SECURE;

	struct mqtt_sec_config *tls_config = &client->transport.tls.config;

	tls_config->peer_verify = 2;
	tls_config->cipher_list = NULL;
	tls_config->sec_tag_list = m_sec_tags;
	tls_config->sec_tag_count = ARRAY_SIZE(m_sec_tags);
#if defined(MBEDTLS_X509_CRT_PARSE_C) || defined(CONFIG_NET_SOCKETS_OFFLOAD)
	tls_config->hostname = TLS_SNI_HOSTNAME;
#else
	tls_config->hostname = NULL;
#endif

#else
	client->transport.type = MQTT_TRANSPORT_NON_SECURE;
#endif
}

/* In this routine we block until the connected variable is 1 */
static int try_to_connect(struct mqtt_client *client)
{
	int rc, i = 0;

	while (i++ < APP_CONNECT_TRIES && !connected) {

		client_init(client);

		rc = mqtt_connect(client);
		if (rc != 0) {
			PRINT_RESULT("mqtt_connect", rc);
			k_sleep(APP_SLEEP_MSECS);
			continue;
		}

		prepare_fds(client);

		wait(APP_SLEEP_MSECS);
		mqtt_input(client);

		if (!connected) {
			mqtt_abort(client);
		}
	}

	if (connected) {
		return 0;
	}

	return -EINVAL;
}

static int process_mqtt_and_sleep(struct mqtt_client *client, int timeout)
{
	s64_t remaining = timeout;
	s64_t start_time = k_uptime_get();
	int rc;

	while (remaining > 0 && connected) {
		wait(remaining);

		rc = mqtt_live(client);
		if (rc != 0) {
			PRINT_RESULT("mqtt_live", rc);
			return rc;
		}

		rc = mqtt_input(client);
		if (rc != 0) {
			PRINT_RESULT("mqtt_input", rc);
			return rc;
		}

		remaining = timeout + start_time - k_uptime_get();
	}

	return 0;
}

#define SUCCESS_OR_EXIT(rc) { if (rc != 0) { return; } }
#define SUCCESS_OR_BREAK(rc) { if (rc != 0) { break; } }

static void publisher(void)
{
	int i, rc;

	/* TODO: fix this crappy code */
	while(1) {
		rc = mqtt_ping(&client_ctx);
		PRINT_RESULT("mqtt_ping", rc);
		SUCCESS_OR_BREAK(rc);

		rc = process_mqtt_and_sleep(&client_ctx, APP_SLEEP_MSECS);
		SUCCESS_OR_BREAK(rc);

		if (buttonPressed)
		{
			rc = publish(&client_ctx, MQTT_QOS_0_AT_MOST_ONCE);
			PRINT_RESULT("mqtt_publish", rc);
			buttonPressed = 0;
			SUCCESS_OR_BREAK(rc);
		}

		rc = process_mqtt_and_sleep(&client_ctx, APP_SLEEP_MSECS);
		SUCCESS_OR_BREAK(rc);

	}

}

void main(void)
{
        struct device *gpiob;

	/* Init LEDs */
        gpio0 = device_get_binding(PORT0);
        gpio1 = device_get_binding(PORT1);

        gpio_pin_configure(gpio0, LED0, GPIO_DIR_OUT);
        gpio_pin_configure(gpio1, LED1, GPIO_DIR_OUT);

        gpio_pin_write(gpio0, LED0, 0);
	//gpio_pin_write(gpio1, LED1, (++led_cnt) % 2);

        printk("Press the button on the board to light up the other's LED\n");
        gpiob = device_get_binding(PORT);
        if (!gpiob) {
                printk("error\n");
                return;
        }

        gpio_pin_configure(gpiob, PIN,
                           GPIO_DIR_IN | GPIO_INT |  PULL_UP | EDGE);

        gpio_init_callback(&gpio_cb, button_pressed, BIT(PIN));

        gpio_add_callback(gpiob, &gpio_cb);
        gpio_pin_enable_callback(gpiob, PIN);


#if defined(CONFIG_MQTT_LIB_TLS)
	int rc;

	rc = tls_init();
	PRINT_RESULT("tls_init", rc);
#endif

	printk("attempting to connect: ");
	rc = try_to_connect(&client_ctx);
	PRINT_RESULT("try_to_connect", rc);
	SUCCESS_OR_EXIT(rc);

	rc = app_subscribe();
	PRINT_RESULT("app_subscribe", rc);
	SUCCESS_OR_EXIT(rc);

	while (1) {
		publisher();
		k_sleep(5000);
	}
}
