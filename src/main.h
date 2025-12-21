#define HTTP_URL "http://0.0.0.0:80"

#define GPIO_BUTTON_PIN 18
#define GPIO_RELAY_TRIG 28
#define GPIO_RELAY_HOLD 27

uint64_t sntp_refresh_counter = 0;
bool sntp_refresh_required = true;

uint16_t boost_timer = 1800; // timer in seconds (30 minutes)
uint16_t boost_timer_add = 900; // timer increase in seconds (15 minutes)

static void blink_timer(void *arg);
static void button_timer(void *arg);
static void relay_timer(void *arg);
static void one_second_timer(void *arg);
static void net_check_timer(void *arg);
static void sntp_timer(void *arg);

static void get_data();
static void save_data();
static void do_boost();
uint8_t day_of_week(datetime_t *dt);

static void sfn(struct mg_connection *c, int ev, void *ev_data);
static void http_ev_handler(struct mg_connection *c, int ev, void *ev_data);
