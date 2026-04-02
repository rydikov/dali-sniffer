idf_component_register(SRCS "blink_example_main.c" "web_server.c" "wifi_state.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_http_server esp_netif esp_wifi nvs_flash
                       EMBED_TXTFILES "web_dist/index.html" "web_dist/assets/app.css" "web_dist/assets/app.js")
