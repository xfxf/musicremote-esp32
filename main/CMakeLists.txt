set(local_env "${CMAKE_CURRENT_LIST_DIR}/../.env")
if(NOT EXISTS "${local_env}")
    message(FATAL_ERROR "Missing .env. Copy .env.example and fill in the local settings.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${local_env}")

file(STRINGS "${local_env}" env_lines)
foreach(line IN LISTS env_lines)
    if(line MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
        set(name "${CMAKE_MATCH_1}")
        set(value "${CMAKE_MATCH_2}")
        string(STRIP "${value}" value)
        if(value MATCHES "^\"(.*)\"$")
            set(value "${CMAKE_MATCH_1}")
        elseif(value MATCHES "^'(.*)'$")
            set(value "${CMAKE_MATCH_1}")
        endif()
        set("LOCAL_${name}" "${value}")
    endif()
endforeach()

foreach(name WIFI_SSID WIFI_PASSWORD AIMP_HOST AIMP_PORT AIMP_PASSWORD)
    if(NOT DEFINED LOCAL_${name})
        message(FATAL_ERROR "${name} is missing from .env")
    endif()
endforeach()

if(NOT DEFINED LOCAL_PLAYLIST_STORAGE)
    set(LOCAL_PLAYLIST_STORAGE "auto")
endif()
if(DEFINED MUSICREMOTE_PLAYLIST_STORAGE_OVERRIDE)
    set(LOCAL_PLAYLIST_STORAGE "${MUSICREMOTE_PLAYLIST_STORAGE_OVERRIDE}")
endif()
if(NOT LOCAL_PLAYLIST_STORAGE MATCHES "^(auto|sd|flash)$")
    message(FATAL_ERROR "PLAYLIST_STORAGE must be auto, sd, or flash")
endif()

if(NOT LOCAL_AIMP_PORT MATCHES "^[0-9]+$" OR LOCAL_AIMP_PORT LESS 1 OR LOCAL_AIMP_PORT GREATER 65535)
    message(FATAL_ERROR "AIMP_PORT must be an integer from 1 through 65535")
endif()

string(LENGTH "${LOCAL_WIFI_SSID}" wifi_ssid_length)
string(LENGTH "${LOCAL_WIFI_PASSWORD}" wifi_password_length)
string(LENGTH "${LOCAL_AIMP_HOST}" aimp_host_length)
string(LENGTH "${LOCAL_AIMP_PASSWORD}" aimp_password_length)
if(wifi_ssid_length LESS 1 OR wifi_ssid_length GREATER 32)
    message(FATAL_ERROR "WIFI_SSID must contain 1 through 32 bytes")
endif()
if(wifi_password_length GREATER 0 AND wifi_password_length LESS 8)
    message(FATAL_ERROR "WIFI_PASSWORD must be empty or contain 8 through 63 printable ASCII bytes, or 64 hexadecimal digits")
endif()
if(wifi_password_length GREATER 64)
    message(FATAL_ERROR "WIFI_PASSWORD must be empty or contain 8 through 63 printable ASCII bytes, or 64 hexadecimal digits")
endif()
if(wifi_password_length GREATER 0 AND wifi_password_length LESS 64 AND NOT LOCAL_WIFI_PASSWORD MATCHES "^[ -~]+$")
    message(FATAL_ERROR "A Wi-Fi passphrase must contain only printable ASCII bytes")
endif()
if(wifi_password_length EQUAL 64 AND NOT LOCAL_WIFI_PASSWORD MATCHES "^[0-9A-Fa-f]+$")
    message(FATAL_ERROR "A 64-byte WIFI_PASSWORD must contain only hexadecimal digits")
endif()
if(aimp_host_length LESS 1 OR aimp_host_length GREATER 127)
    message(FATAL_ERROR "AIMP_HOST must contain 1 through 127 bytes")
endif()
if(aimp_password_length GREATER 127)
    message(FATAL_ERROR "AIMP_PASSWORD must contain at most 127 bytes")
endif()

function(escape_c_string input output)
    set(value "${input}")
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    set("${output}" "${value}" PARENT_SCOPE)
endfunction()

escape_c_string("${LOCAL_WIFI_SSID}" CONFIG_WIFI_SSID)
escape_c_string("${LOCAL_WIFI_PASSWORD}" CONFIG_WIFI_PASSWORD)
escape_c_string("${LOCAL_AIMP_HOST}" CONFIG_AIMP_HOST)
escape_c_string("${LOCAL_AIMP_PASSWORD}" CONFIG_AIMP_PASSWORD)
set(CONFIG_AIMP_PORT "${LOCAL_AIMP_PORT}")
set(CONFIG_PLAYLIST_STORAGE "${LOCAL_PLAYLIST_STORAGE}")
set(generated_include_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated_include_dir}")
configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/musicremote_config.hpp.in"
    "${generated_include_dir}/musicremote_config.hpp"
    @ONLY
)

idf_component_register(
    SRCS
        "app_main.cpp"
        "artwork_decoder.cpp"
        "device_settings.cpp"
        "monkeymote_client.cpp"
        "playlist_index.cpp"
        "music_ui.cpp"
    INCLUDE_DIRS "." "${generated_include_dir}"
    REQUIRES
        esp_event
        esp_netif
        esp_wifi
        esp_http_server
        esp_rom
        fatfs
        spiffs
        nvs_flash
        mdns
        mbedtls
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
