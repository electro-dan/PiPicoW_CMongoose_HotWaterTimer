# PiPicoW_CMongoose_HotWaterTimer
Hot Water Timer using C and Mongoose Web Server

Set variables to begin:

    export PICO_SDK_PATH=~/GIT/pico-sdk
    export WIFI_SSID=myssid
    export WIFI_PASSWORD=mywifipassword

To build code:

    cd ~/GIT/PiPicoW_CMongoose_HotWaterTimer/build
    cmake ..
    make
    make install

Rebuild:

    cd .. && rm -r build && mkdir build && cd build

To build the file system:

    cd ~/GIT/PiPicoW_CMongoose_HotWaterTimer
    lib/mongoose/test/pack web/* > src/fs.c

Todo list
x Flash read/save
x Relay pin activation with delay
x Boost switch

Bug list
x Web socket not closing when tab losing focus - seems to close on re-focus
x Web socket stops updating (firefox)
