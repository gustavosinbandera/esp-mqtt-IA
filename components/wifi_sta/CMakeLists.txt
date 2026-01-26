idf_component_register(
    SRCS "wifi_sta.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_event esp_netif freertos
)

# Enforce: C++17 + no exceptions + no RTTI (project requirement)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
