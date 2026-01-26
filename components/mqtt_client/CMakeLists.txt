idf_component_register(
    SRCS
        "mqtt_client.cpp"
    INCLUDE_DIRS
        "include"
    REQUIRES
        esp_event
        mqtt
)

# Force C++17 in THIS component (do not assume project-wide flags)
set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17 -fno-exceptions -fno-rtti)
