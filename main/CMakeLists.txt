idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES wifi_sta mqtt_client nvs_flash
)

# Enforce: C++17 + no exceptions + no RTTI
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-exceptions -fno-rtti)
