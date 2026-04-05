cmake_minimum_required(VERSION 3.16)

# IDF_TARGET must be set by the build command or VS Code extension setting,
# not hardcoded here.  See sdkconfig.defaults.esp32 / sdkconfig.defaults.esp32s3
# and the build instructions in CLAUDE.md.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(BleUartBridge)
