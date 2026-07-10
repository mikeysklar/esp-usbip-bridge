cmake_minimum_required(VERSION 3.16)

# ---- Auto-generated firmware version (regenerated every build) ----
# Generate version_autogen.h into the build directory so that
# harness_scpi and http_server can include it.
set(VERSION_OUTPUT_DIR "${CMAKE_BINARY_DIR}/include")
file(MAKE_DIRECTORY "${VERSION_OUTPUT_DIR}")

# Custom target that runs on every build, regenerating the version header
# with the latest git describe and build timestamp.
add_custom_target(generate_version ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUTPUT_DIR="${VERSION_OUTPUT_DIR}"
        -P "${CMAKE_CURRENT_LIST_DIR}/scripts/generate_version.cmake"
    BYPRODUCTS "${VERSION_OUTPUT_DIR}/version_autogen.h"
    COMMENT "Generating firmware version header"
)

# Make esp-harness components available to the IDF build.  Whether they're
# compiled is gated by main's REQUIRES list, which checks
# CONFIG_USBIP_VIRTUAL_HARNESS.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/esp-harness/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Force Full-Speed only mode on the DWC OTG USB controller.
# Split transactions (TT) required for FS devices behind HS hubs are not
# supported in the DWC OTG's Scatter/Gather DMA (DDMA) mode. By forcing
# FS-only, hubs negotiate at FS and all downstream FS devices work natively.
idf_build_set_property(COMPILE_DEFINITIONS "USB_DWC_FSLS_ONLY" APPEND)

project(esp32p4_usbip_bridge)

# Make the auto-generated version header visible to components that need it.
# The harness_scpi component defines HARNESS_FW_VERSION in its public header;
# the main component's http_server displays it on the status page.
# Also ensure the version header is regenerated before these targets build.
target_include_directories(__idf_harness_scpi PRIVATE "${VERSION_OUTPUT_DIR}")
target_include_directories(__idf_main PRIVATE "${VERSION_OUTPUT_DIR}")
add_dependencies(__idf_harness_scpi generate_version)
add_dependencies(__idf_main generate_version)
