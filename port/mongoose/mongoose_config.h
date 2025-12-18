#define MG_ENABLE_LWIP 0
#define MG_ENABLE_PACKED_FS 1
#define MG_ARCH MG_ARCH_PICOSDK
//#define MG_OTA MG_OTA_PICOSDK

#define MG_ENABLE_TCPIP 1
#define MG_ENABLE_DRIVER_PICO_W 1
#define MG_ENABLE_DRIVER_W 1
#define MG_IO_SIZE 2048


// Using a custom function:
extern void main_setconfig(void *data);
#define MG_SET_WIFI_CONFIG(data) main_setconfig(data)
