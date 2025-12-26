cmake_minimum_required(VERSION 3.12)

# Change your executable name to something creative!
set(NAME HotWaterTimer) # <-- Name your project/executable here!
set(PICO_BOARD pico_w)
set(PICO_PLATFORM rp2040)

include("$ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake")


project(${NAME} C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 26)

# Initialize the SDK
pico_sdk_init()

SET(MONGOOSE_PATH "${CMAKE_CURRENT_LIST_DIR}/lib/mongoose" CACHE STRING "Common Lib")
include(mongoose.cmake)

set(NVS_PATH "${CMAKE_CURRENT_LIST_DIR}/lib/RPIPicoOnboardNVS/")
    message("Using local NVS_PATH '${NVS_PATH}')")
include(picoOnboardNVS.cmake)

add_subdirectory(src)

#Set up files for the release packages
install(CODE "execute_process(COMMAND picotool load ${CMAKE_CURRENT_BINARY_DIR}/src/${NAME}.elf -f -x)")

# Set up files for the release packages
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/src/${NAME}.uf2
    DESTINATION ${CMAKE_CURRENT_BINARY_DIR}
)

set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_GENERATOR "ZIP" "TGZ")
include(CPack)
