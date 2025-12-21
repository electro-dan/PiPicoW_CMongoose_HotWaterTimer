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
#include "NVSOnboard.h"

#include "mongoose.h"
#include "main.h"

#define HTTP_URL "http://0.0.0.0:80"

struct mg_mgr g_mgr;

uint64_t sntp_refresh_counter = 0;
bool sntp_refresh_required = true;
// Used to determine if data needs to be sent on websocket
// Set to true when:
// Date/Time changes
// Any set API received
// First websocket load
bool state_changed = true; 

// SNTP client connection
static struct mg_connection *s_sntp_conn = NULL;

struct s_status {
	uint8_t current_day = 1; // Day 1-7
	uint16_t current_time = 0; // Time since start of day in minutes
	bool heating_state = false;
	bool is_heating = false;
	uint16_t boost_timer_countdown = 0;
	uint8_t boost_pressed = 0;
	uint16_t timers[6][3] = {{127, 450, 390},{0, 420, 420},{0, 420, 420},{0, 420, 420},{0, 420, 420},{0, 420, 420}};
} g_status;

uint16_t boost_timer = 1800; // timer in seconds (30 minutes)
uint16_t boost_timer_add = 900; // timer increase in seconds (15 minutes)

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
  	(void) arg;
  	cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, !cyw43_arch_gpio_get(CYW43_WL_GPIO_LED_PIN));
}

/***
 * Button polling timer
 * @param arg
 */
static void button_timer(void *arg) {
	// Poll input button
	// See https://www.e-tinkers.com/2021/05/the-simplest-button-debounce-solution/
	static uint16_t state = 0;
	state = (state << 1) | gpio_get(GPIO_BUTTON_PIN) | 0xFE00;
	if (state == 0xff00) {
		// Execute a manual heating 'boost' timer
		if (g_status.boost_timer_countdown == 0) {
			g_status.boost_pressed = 1;
			g_status.boost_timer_countdown = boost_timer;
			g_status.heating_state = true; // Boost will also enable the heating
		} else {
			// If already in boost mode, switch off (does not increase like web front end)
			g_status.boost_pressed = 0;
			g_status.boost_timer_countdown = 0;
		}
		state_changed = true; 
	}
}

/***
 * Relay activation timer
 * @param arg
 */
static void relay_timer(void *arg) {
	// Enable or disable the output
	if (g_status.is_heating) {
		// Run hold first - this will switch the relay into hold state if last run was to activate
		if (gpio_get(GPIO_RELAY_TRIG)) {
			gpio_put(GPIO_RELAY_TRIG, 0);
			gpio_put(GPIO_RELAY_HOLD, 1);
		}
		// This will activate the relay if it is off or not in hold state
        if (!gpio_get(GPIO_RELAY_TRIG) & !gpio_get(GPIO_RELAY_TRIG)) {
			gpio_put(GPIO_RELAY_TRIG, 1);
		}
	} else {
		// Deactivate the relay
		gpio_put(GPIO_RELAY_HOLD, 0);
		gpio_put(GPIO_RELAY_TRIG, 0);
	}
}

/***
 * Relay activation timer
 * @param arg
 */
static void one_second_timer(void *arg) {
	
	// Get the RTC date and time
	datetime_t dt;
	rtc_get_datetime(&dt);

	/*MG_INFO(("RTC: %d-%d-%d %d:%d:%d\n", dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec));*/
	uint16_t new_time = dt.hour * 60 + dt.min;
	// Day will change when time changes, so implied
	if (new_time != g_status.current_time) {
		g_status.current_day = day_of_week(&dt);
		g_status.current_time = new_time;
		state_changed = true;
	}

	// iterate through timers
    g_status.is_heating = false;
    // if heating is enabled
    if (g_status.heating_state) {
		for (char i = 0; i < 6; i++) {
			// if timer is enabled for today (bitwise AND)
			if (g_status.current_day & g_status.timers[i][0]) {
				// if the on and off timer are not the same
				if (g_status.timers[i][1] != g_status.timers[i][2]) {
					if (g_status.timers[i][1] < g_status.timers[i][2]) {
						// if off is after on
						// if the current time is between the on and off times, enable heating
						if (g_status.current_time >= g_status.timers[i][1] && g_status.current_time < g_status.timers[i][2])
							g_status.is_heating = true;
					} else {
						// If off is before on
						// if the current time is outside the on and off times, enable heating
						if (g_status.current_time >= g_status.timers[i][1] || g_status.current_time < g_status.timers[i][2])
							g_status.is_heating = true;
					}
				}
			}
		}
		if (g_status.boost_timer_countdown > 0) {
			g_status.is_heating = true;
            g_status.boost_timer_countdown--; // take off 1 second
			state_changed = true;
		}
	}
	
	// If status changed, send web socket
	if (state_changed) {
		struct mg_mgr *mgr = (struct mg_mgr *) arg;
		struct mg_connection *c;
		for (c = mgr->conns; c != NULL; c = c->next) {
			if (c->data[0] != 'W') continue;
			MG_INFO(("WS Send"));

			mg_ws_printf(c, WEBSOCKET_OP_TEXT, 
				"{%m: %m, %m: %d, %m: %d, %m: %d, %m: %d, %m: %d,%m: [[%d, %d, %d],[%d, %d, %d],[%d, %d, %d],[%d, %d, %d],[%d, %d, %d],[%d, %d, %d]]}\n", 
				MG_ESC("status"), MG_ESC("OK"), MG_ESC("current_day"), g_status.current_day, MG_ESC("current_time"), g_status.current_time, 
				MG_ESC("heating_state"), g_status.heating_state, MG_ESC("is_heating"), g_status.is_heating, 
				MG_ESC("boost_timer_countdown"), g_status.boost_timer_countdown, MG_ESC("timers"), 
				g_status.timers[0][0], g_status.timers[0][1], g_status.timers[0][2],
				g_status.timers[1][0], g_status.timers[1][1], g_status.timers[1][2],
				g_status.timers[2][0], g_status.timers[2][1], g_status.timers[2][2],
				g_status.timers[3][0], g_status.timers[3][1], g_status.timers[3][2],
				g_status.timers[4][0], g_status.timers[4][1], g_status.timers[4][2],
				g_status.timers[5][0], g_status.timers[5][1], g_status.timers[5][2]
			);
		}
		// Sent state, clear status
		state_changed = false;
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
			MG_INFO(("Setting RTC to: %d-%d-%d %d:%d:%d\n", dt.year, dt.month, dt.day, dt.hour, dt.min, dt.sec));
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

/*
	Get data to flash, used to restore timers in case of power loss
*/
static void get_data() {
	NVSOnboard * nvs = NVSOnboard::getInstance();

	bool is_there_data = false;
	char key[]="timer_0_0";
	for (char i = 0; i < 6; i++) {
		for (char j = 0; j < 3; j++) {
			key[6] = i+48;
			key[8] = j+48;
			if (nvs->contains(key)) {
				uint16_t v;
				
				nvs->get_u16(key, &v);
				g_status.timers[i][j] = v;
				
				is_there_data = true;
			}
		}
	}
	if (is_there_data)	{
		if (nvs->contains("is_heating")) {
			nvs->get_bool("is_heating", &g_status.is_heating);
		}
		MG_INFO(("Data read from flash"));
	} else {
		MG_INFO(("No data in flash"));
	}
}

/*
	Save data to flash
*/
static void save_data() {
	NVSOnboard * nvs = NVSOnboard::getInstance();
	
	char key[]="timer_0_0";
	for (char i = 0; i < 6; i++) {
		for (char j = 0; j < 3; j++) {
			key[6] = i+48;
			key[8] = j+48;
			nvs->set_u16(key, g_status.timers[i][j]);
		}
	}
	
	nvs->set_bool("is_heating", g_status.is_heating);

	nvs->commit();

	MG_INFO(("Data saved to flash"));
}

// Activate, increase and deactivate a one-shot boost timer
static void do_boost() {
	// Execute a manual heating 'boost' timer
	if (g_status.boost_timer_countdown == 0) {
		g_status.boost_pressed = 1;
		g_status.boost_timer_countdown = boost_timer;
		g_status.heating_state = true; // Boost will also enable the heating
	} else {
		// Subsequent pushes of the boost button will increase boost timer by 15 minutes until 3 pushes
		if (g_status.boost_pressed < 3) {
			g_status.boost_timer_countdown += boost_timer_add;
			g_status.boost_pressed += 1;
		} else {
			g_status.boost_pressed = 0;
			g_status.boost_timer_countdown = 0;
		}
	}
	state_changed = true; 
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
			state_changed = true; // Ensure current state is sent
		} else if (mg_match(hm->uri, mg_str("/api"), NULL)) {
			char *str_action = mg_json_get_str(hm->body, "$.action");
			
			if (strcmp(str_action, "get_status") == 0) {
				MG_INFO(("Getting status"));
				datetime_t dt;
				rtc_get_datetime(&dt);
				mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
					"{%m: %m, %m: %d, %m: %d, %m: %d, %m: %d, %m: %d,%m: [[%d, %d, %d],[%d, %d, %d],[%d, %d, %d],[%d, %d, %d],[%d, %d, %d],[%d, %d, %d]]}\n", 
					MG_ESC("status"), MG_ESC("OK"), MG_ESC("current_day"), g_status.current_day, MG_ESC("current_time"), g_status.current_time, 
					MG_ESC("heating_state"), g_status.heating_state, MG_ESC("is_heating"), g_status.is_heating, 
					MG_ESC("boost_timer_countdown"), g_status.boost_timer_countdown, MG_ESC("timers"), 
					g_status.timers[0][0], g_status.timers[0][1], g_status.timers[0][2],
					g_status.timers[1][0], g_status.timers[1][1], g_status.timers[1][2],
					g_status.timers[2][0], g_status.timers[2][1], g_status.timers[2][2],
					g_status.timers[3][0], g_status.timers[3][1], g_status.timers[3][2],
					g_status.timers[4][0], g_status.timers[4][1], g_status.timers[4][2],
					g_status.timers[5][0], g_status.timers[5][1], g_status.timers[5][2]
				);
			} else if (strcmp(str_action, "trigger_heating") == 0) {
				MG_INFO(("Trigger heating"));
				// Permanently turn heating off (holiday mode) or on
        		g_status.heating_state = !g_status.heating_state;
				save_data();
				mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{%m: %m, %m: %d}",
					MG_ESC("status"), MG_ESC("OK"), MG_ESC("heating_state"), g_status.heating_state
				);
				state_changed = true; 
			} else if (strcmp(str_action, "boost") == 0) {
				do_boost();
				mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{%m: %m, %m: %d}",
					MG_ESC("status"), MG_ESC("OK"), MG_ESC("boost_timer_countdown"), g_status.boost_timer_countdown
				);
			} else if (strcmp(str_action, "set_timer") == 0) {
				// Change a timer days and on/off time (minutes of day)
				double d_timer_number = 0.0;
				double d_new_days = 0.0;
				double d_new_on_time = 0.0;
				double d_new_off_time = 0.0;
				
				if (!mg_json_get_num(hm->body, "$.timer_number", &d_timer_number)) {
					mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("No timer number"));
					return;
				}
				if (!mg_json_get_num(hm->body, "$.new_days", &d_new_days)) {
					mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("No timer days"));
					return;
				}
				if (!mg_json_get_num(hm->body, "$.new_on_time", &d_new_on_time)) {
					mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("No on time"));
					return;
				}
				if (!mg_json_get_num(hm->body, "$.new_off_time", &d_new_off_time)) {
					mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("No off time"));
					return;
				}
				// Double to uint16_t
				uint16_t timer_number = d_timer_number;
				uint16_t new_days = d_new_days;
				uint16_t new_on_time = d_new_on_time;
				uint16_t new_off_time = d_new_off_time;
				

				// Validate inputs
				if (timer_number < 1 || timer_number > 6) {
					mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("Invalid timer number"));
				} else if (new_days < 0 || new_days > 127) {
					mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("Invalid timer days"));
				} else if (new_on_time < 0 || new_on_time > 1410) {
					mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("Invalid on time"));
				} else if (new_off_time < 0 || new_off_time > 1410) {
					mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("Invalid off time"));
				} else {
					g_status.timers[timer_number - 1][0] = new_days;
					g_status.timers[timer_number - 1][1] = new_on_time;
					g_status.timers[timer_number - 1][2] = new_off_time;
					save_data();
					mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{%m: %m, %m: %d}",
						MG_ESC("status"), MG_ESC("OK"), MG_ESC("timer_number"), timer_number, MG_ESC("new_days"), new_days, MG_ESC("new_on_time"), new_on_time, MG_ESC("new_off_time"), new_off_time
					);
					state_changed = true; 
				}
			} else {
				mg_http_reply(c, 400, "", "{%m: %m, %m: %m}\n", MG_ESC("status"), MG_ESC("ERROR"), MG_ESC("message"), MG_ESC("Unknown Action"));
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
    uint8_t dow = (c / 4 - 2 * c + year + year / 4 + 13 * (month + 1) / 5 + day - 1) % 7;
	// Change Sunday from 0 to 7
	if (dow == 0)
		return 7;
	else
		return dow;
}

/***
 * Main
 * @return
 */
int main(){
	stdio_init_all();
	sleep_ms(3000);

	printf("Go\n");

	get_data();

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

	// Boost button setup
    gpio_init(GPIO_BUTTON_PIN); // Initialise the GPIO pin
    gpio_set_dir(GPIO_BUTTON_PIN, GPIO_IN); // Set it as an input
    gpio_pull_up(GPIO_BUTTON_PIN); // Enable internal pull-up resistor

	// Relay pins
	gpio_init(GPIO_RELAY_TRIG); // Initialise the GPIO pin
    gpio_set_dir(GPIO_RELAY_TRIG, GPIO_OUT); // Set it as an output
	gpio_init(GPIO_RELAY_HOLD); // Initialise the GPIO pin
    gpio_set_dir(GPIO_RELAY_HOLD, GPIO_OUT); // Set it as an output
    

	// do not access the CYW43 LED before Mongoose initializes !
	MG_INFO(("Hardware initialised, starting firmware..."));
	
	// This blocks forever. Call it at the end of main()
	mg_mgr_init(&g_mgr);      // Initialise event manager
	mg_log_set(MG_LL_DEBUG);  // Set log level to debug
	MG_INFO(("Starting HTTP listener"));
	mg_http_listen(&g_mgr, HTTP_URL, http_ev_handler, NULL);

	// This timer checks the time every 1s and enables/disables the relay
	mg_timer_add(&g_mgr, 1000, MG_TIMER_REPEAT, blink_timer, NULL);
	mg_timer_add(&g_mgr, 1, MG_TIMER_REPEAT, button_timer, NULL);
	mg_timer_add(&g_mgr, 300, MG_TIMER_REPEAT, relay_timer, NULL);
	// This timer activates any timers and sends status to open web sockets
	mg_timer_add(&g_mgr, 1000, MG_TIMER_REPEAT, one_second_timer, &g_mgr);
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
