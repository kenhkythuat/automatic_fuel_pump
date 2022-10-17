cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Update the below line to match the path to the ttn-esp32 library,
# e.g. list(APPEND EXTRA_COMPONENT_DIRS "/Users/me/Documents/ttn-esp32")
list(APPEND EXTRA_COMPONENT_DIRS "../..")
add_definitions(
    #-DLORA_COMM_ENABLE 
     -DBLE_COMM_ENABLE 
     -DWIFI_COMM_ENABLE
   # -DPROVISIONING_ENABLE 
    -DFIRMWARE_OTA_ENABLE
    -DMEDKLINN_INT_TEST
    -DMODBUS_COMM_ENABLE 
    -DMODBUS_SIMULATION_ENABLE 
   # -DHELTEC_WIRELESS_STICK_LITE
    -DMODBUS_INPUT_REGISTER
    -DNODE_VERSION=\"v1.00.17092021\"
) #adding definitions here 
#add_definitions(-DWIFI_COMM_ENABLE -DBLE_COMM_ENABLE -DPROVISIONING_ENABLE -DMODBUS_COMM_ENABLE -DMODBUS_SIMULATION_ENABLE ) #adding definitions here 
#list(APPEND EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)
#list(APPEND EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/qrcode)

# (Not part of the boilerplate)
# This example uses an extra component for common functions such as Wi-Fi and Ethernet connection.
#set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common $ENV{IDF_PATH}/examples/common_components/qrcode)
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/qrcode $ENV{IDF_PATH}/components/wifi_provisioning)
project(atc_wifi_fw)
set(CMAKE_CONFIGURATION_TYPES LORA_ENABLE)
target_add_binary_data(${CMAKE_PROJECT_NAME}.elf "main/ca_s3_cert.pem" TEXT)
