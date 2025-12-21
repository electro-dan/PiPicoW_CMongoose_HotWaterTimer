include(files.cmake)
add_executable(${NAME}
        main.cpp
        ${SRC_FILES}
)

# Pull in our pico_stdlib which pulls in commonly used features
target_link_libraries(${NAME} 
    pico_stdlib 
    pico_cyw43_arch_none
    hardware_rtc
    hardware_spi 
    pico_rand 
    pico_cyw43_driver  
    hardware_flash 
    pico_bootrom
    mongoose
)

# create map/bin/hex file etc.
pico_add_extra_outputs(${NAME})

# enable usb output, disable uart output
pico_enable_stdio_usb(${NAME} 1)
pico_enable_stdio_uart(${NAME} 0)

target_compile_definitions(${NAME} PRIVATE
    WIFI_SSID=\"$ENV{WIFI_SSID}\"
    WIFI_PASS=\"$ENV{WIFI_PASSWORD}\"
    CMAKE_EXPORT_COMPILE_COMMANDS=1
)
