idf_component_register(SRCS "main.cpp" "web_server.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_http_server esp_netif esp_wifi json nvs_flash
                       EMBED_TXTFILES "web_dist/index.html" "web_dist/assets/app.css" "web_dist/assets/app.js")
