/*
 * main.cpp
 *
 * A hot water timer with web control using Mongoose Application to pass back static web pages and handle rest requests and websocket
 * Using Mongoose TCPIP stack and Drivers
 * Listen on port 80
 *  Created on: November 2025
 *      Author: electro-dan
 */

/* TODO

Add time request
Add flash save / load

*/

#include <cstdio>
#include <pico/stdlib.h>
#include "pico/util/datetime.h"
#include <time.h>
#include "hardware/rtc.h"

#include "mongoose.h"
#include "main.h"

#define HTTP_URL "http://0.0.0.0:80"

struct mg_mgr g_mgr;

uint64_t sntp_refresh_counter = 0;
bool sntp_refresh_required = true;

// SNTP client connection
static struct mg_connection *s_sntp_conn = NULL;

/***
 * Setup credentials for WiFi
 * @param data
 */
void main_setconfig(void *data) {
	struct mg_tcpip_driver_pico_w_data *d = (struct mg_tcpip_driver_pico_w_data *)data;
	d->ssid = (char *)WIFI_SSID;
	d->pass = (char *)WIFI_PASS;
}

/***
 * Blink Status LED as call back from Mongoose
 * @param arg
 */
static void blink_timer(void *arg) {
  	//(void) arg;
  	cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, !cyw43_arch_gpio_get(CYW43_WL_GPIO_LED_PIN));
}

static void ws_timer(void *arg) {
	// Get the RTC date and time
	datetime_t dt;
	rtc_get_datetime(&dt);
	/*MG_INFO(("RTC: %d-%d-%d %d:%d:%d\n",
			dt.year,
			dt.month,
			dt.day,
			dt.hour,
			dt.min,
			dt.sec));*/

	struct mg_mgr *mgr = (struct mg_mgr *) arg;
	struct mg_connection *c;
	for (c = mgr->conns; c != NULL; c = c->next) {
		if (c->data[0] != 'W') continue;
		mg_ws_printf(c, WEBSOCKET_OP_TEXT, "{\"status\": \"OK\","
            		"\"current_day\": %d,"
            		"\"current_time\": %d,"
            		"\"heating_state\": 1,"
            		"\"is_heating\": 1,"
            		"\"boost_timer_countdown\": 0,"
            		"\"timers\": [[127, 450, 390],[0, 420, 420],[0, 420, 420],[0, 420, 420],[0, 420, 420],[0, 420, 420]]"
					"}\n", day_of_week(&dt), dt.hour * 60 + dt.min);
	}
}

// SNTP client callback
static void sfn(struct mg_connection *c, int ev, void *ev_data) {
	if (ev == MG_EV_SNTP_TIME) {
		// Time received, the internal protocol handler updates what mg_now() returns
		uint64_t curtime = mg_now();
		MG_INFO(("SNTP-updated current time is: %llu ms from epoch", curtime));
		// otherwise, you can process the server returned data yourself
		{
			uint64_t t = *(uint64_t *) ev_data;
			datetime_t dt;
			time_to_datetime(t / 1000, &dt);
			MG_INFO(("Setting RTC to: %d-%d-%d %d:%d:%d\n",
				dt.year,
				dt.month,
				dt.day,
				dt.hour,
				dt.min,
				dt.sec));
			rtc_set_datetime(&dt);
			// Reset counter and refresh required flag
			sntp_refresh_counter = 0;
			sntp_refresh_required = false;
		}
	} else if (ev == MG_EV_CLOSE) {
		s_sntp_conn = NULL;
	}
	(void) c;
}

// Called every 60 seconds - resets network state if stuck in DHCP REQUESTING or DOWN
static void net_check_timer(void *arg) {
	/* check state */
	//MG_INFO(("State: %d", g_mgr.ifp->state));
	if (g_mgr.ifp->state == MG_TCPIP_STATE_DOWN) {
		// If interface is down, change it to MG_TCPIP_STATE_REQ, next run of this timer will bring it down and request connection again
		g_mgr.ifp->state = MG_TCPIP_STATE_REQ;
		MG_INFO(("State was MG_TCPIP_STATE_DOWN, reset state to MG_TCPIP_STATE_REQ"));
	} else if (g_mgr.ifp->state == MG_TCPIP_STATE_REQ) {
		// Reset interface status to down, mg_tcpip_poll will cause a reconnect
		g_mgr.ifp->state = MG_TCPIP_STATE_DOWN;
		MG_INFO(("State was MG_TCPIP_STATE_REQ, reset state to MG_TCPIP_STATE_DOWN"));
	}
}

// Called every 5 seconds. Increase that for production case.
static void sntp_timer(void *arg) {
	// Check if refresh was at least 24h ago
	if (sntp_refresh_counter > 8640)
		sntp_refresh_required = true;
	if (sntp_refresh_required) {
		struct mg_mgr *mgr = (struct mg_mgr *) arg;
		if (s_sntp_conn == NULL) { // connection issues a request
			s_sntp_conn = mg_sntp_connect(mgr, NULL, sfn, NULL);
		} else {
			mg_sntp_request(s_sntp_conn);
		}
	}
	sntp_refresh_counter++;
}

/***
 * Main event callback handler for Mongoose
 * @param c
 * @param ev
 * @param ev_data
 */
static void http_ev_handler(struct mg_connection *c, int ev, void *ev_data) {
	if (ev == MG_EV_HTTP_MSG){
		struct mg_http_message *hm = (struct mg_http_message *) ev_data;  // Parsed HTTP request
		if (mg_match(hm->uri, mg_str("/websocket"), NULL)) {
			// Upgrade to websocket. From now on, a connection is a full-duplex
			// Websocket connection, which will receive MG_EV_WS_MSG events.
			mg_ws_upgrade(c, hm, NULL);
			// Set some unique mark on the connection
			c->data[0] = 'W';
		} else if (mg_match(hm->uri, mg_str("/api"), NULL)) {
			char *str_action = mg_json_get_str(hm->body, "$.action");
			
			if (strcmp(str_action, "get_status") == 0) {
				MG_INFO(("Getting status"));
				datetime_t dt;
				rtc_get_datetime(&dt);
				mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
					"{\"status\": \"OK\","
            		"\"current_day\": %d,"
            		"\"current_time\": %d,"
            		"\"heating_state\": 1,"
            		"\"is_heating\": 1,"
            		"\"boost_timer_countdown\": 0,"
            		"\"timers\": [[127, 450, 390],[0, 420, 420],[0, 420, 420],[0, 420, 420],[0, 420, 420],[0, 420, 420]]"
					"}\n", 
					day_of_week(&dt), dt.hour * 60 + dt.min);
			} else {
				mg_http_reply(c, 400, "", "{\"status\": \"ERROR\", \"message\": \"Unkown action\"}\n", 123);
				MG_INFO(("Unknown action"));
			}
			
			mg_free(str_action);
		} else {
			MG_INFO(("Got: %s", hm->uri));
			struct mg_http_serve_opts opts = {
				.root_dir = "/web",
				.fs = &mg_fs_packed
			 };
			mg_http_serve_dir(c, hm, &opts);
		}
	/*} else if (ev == MG_EV_WS_MSG) {
		// Got websocket frame. Received data is wm->data. Echo it back!
		struct mg_ws_message *wm = (struct mg_ws_message *) ev_data;
		mg_ws_send(c, wm->data.buf, wm->data.len, WEBSOCKET_OP_TEXT);*/
	}
}

// https://forums.raspberrypi.com/viewtopic.php?t=312419
uint8_t day_of_week(datetime_t *dt) {
    uint8_t day = dt->day; 
    uint8_t month = dt->month; 
    uint16_t year = dt->year;

    // adjust month year
    if (month < 3) {
        month += 12;
        year -= 1;
    }

    // split year
    uint32_t c = year / 100;
    year = year % 100;

    // Zeller's congruence
    return (c / 4 - 2 * c + year + year / 4 + 13 * (month + 1) / 5 + day - 1) % 7;
}

/***
 * Main
 * @return
 */
int main(){
	stdio_init_all();
	sleep_ms(3000);

	printf("Go\n");

	// RTC init with default date and time
	datetime_t dt;
    dt.year = 2000;
    dt.month = 1;
    dt.day = 1;
    dt.hour = 0;
    dt.min = 0;
    dt.sec = 0;
	rtc_init();
	rtc_set_datetime(&dt);

	// do not access the CYW43 LED before Mongoose initializes !
	MG_INFO(("Hardware initialised, starting firmware..."));
	
	// This blocks forever. Call it at the end of main()
	mg_mgr_init(&g_mgr);      // Initialise event manager
	mg_log_set(MG_LL_DEBUG);  // Set log level to debug
	MG_INFO(("Starting HTTP listener"));
	mg_http_listen(&g_mgr, HTTP_URL, http_ev_handler, NULL);

	// This timer checks the time every 1s and enables/disables the relay
	mg_timer_add(&g_mgr, 1000, MG_TIMER_REPEAT, blink_timer, NULL);
	// This timer sends status to open web sockets
	mg_timer_add(&g_mgr, 1000, MG_TIMER_REPEAT, ws_timer, &g_mgr);
	// This timer does an SNTP refresh. Refresh happens once a day, but timer checks if the time needs setting every 10s
	mg_timer_add(&g_mgr, 10000, MG_TIMER_REPEAT | MG_TIMER_RUN_NOW, sntp_timer, &g_mgr);
	// This timer is a network reset check
	mg_timer_add(&g_mgr, 60000, MG_TIMER_REPEAT | MG_TIMER_RUN_NOW, net_check_timer, &g_mgr);
	for (;;) {
		mg_mgr_poll(&g_mgr, 10);
	}
	mg_mgr_free(&g_mgr); // Free manager resources

	return 0;
}
