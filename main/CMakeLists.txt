set(local_env "${CMAKE_CURRENT_LIST_DIR}/../.env")
if(NOT EXISTS "${local_env}")
    message(FATAL_ERROR "Missing .env. Copy .env.example and fill in the local settings.")
endif()

file(STRINGS "${local_env}" env_lines)
foreach(line IN LISTS env_lines)
    if(line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
        set("LOCAL_${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}")
    endif()
endforeach()

foreach(name WIFI_SSID WIFI_PASSWORD AIMP_HOST AIMP_PORT AIMP_PASSWORD)
    if(NOT DEFINED LOCAL_${name})
        message(FATAL_ERROR "${name} is missing from .env")
    endif()
endforeach()

idf_component_register(
    SRCS
        "app_main.cpp"
        "monkeymote_client.cpp"
        "music_ui.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        esp_event
        esp_netif
        esp_wifi
        esp_http_server
        nvs_flash
        mdns
        mbedtls
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MUSICREMOTE_WIFI_SSID="${LOCAL_WIFI_SSID}"
    MUSICREMOTE_WIFI_PASSWORD="${LOCAL_WIFI_PASSWORD}"
    MUSICREMOTE_AIMP_HOST="${LOCAL_AIMP_HOST}"
    MUSICREMOTE_AIMP_PORT=${LOCAL_AIMP_PORT}
    MUSICREMOTE_AIMP_PASSWORD="${LOCAL_AIMP_PASSWORD}"
)
