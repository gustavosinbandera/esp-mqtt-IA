# mqtt_client (ESP-IDF 5.4+, C++17)

Componente MQTT robusto (esp-mqtt) con:
- Reconexión con **backoff exponencial + jitter** (one-shot timer)
- Handler liviano (solo estado + encola)
- RX callback ejecutado en tarea interna (xTaskCreateStatic)
- publish/subscribe thread-safe
- Outbox fijo opcional (sin heap)

Integración típica:
- `wifi.begin()`
- `wifi.wait_has_ip()`
- `mqtt.init()` / `mqtt.start()`
