# mqtt_demo_project (ESP-IDF 5.4+)

Proyecto demo **listo para compilar** que incluye:
- `components/wifi_sta/` (tu componente Wi-Fi, copiado tal cual de los archivos compartidos)
- `components/mqtt_client/` (nuevo componente MQTT robusto)

## Build (ESP-IDF)
```bash
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py flash monitor
```

## Flujo en runtime
1) `wifi.begin()`
2) `wifi.wait_has_ip()`
3) `mqtt.init()` + `mqtt.start()`
4) `mqtt.subscribe()` / `mqtt.publish()`

> Nota: Este demo usa credenciales Wi-Fi en `main/app_main.cpp` (estructura `MyCredentials`).
