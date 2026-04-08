idf_component_register(SRCS "main.cpp" "web_server.cpp" "dali_sniffer.cpp" "dali_protocol.cpp" "mqtt_bridge.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_driver_gpio esp_driver_gptimer esp_event esp_http_server esp_netif esp_wifi json mqtt nvs_flash
                       EMBED_TXTFILES "web_dist/index.html" "web_dist/assets/app.css" "web_dist/assets/app.js")
