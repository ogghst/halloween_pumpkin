/* Stub esp_log.h per gli host test: i log del layer IDF sono scartati
 * (nessun rumore su stdout, dove Unity stampa i risultati). L'header
 * reale arriva da ESP-IDF nei build su target. */
#ifndef STUB_ESP_LOG_H
#define STUB_ESP_LOG_H

#define ESP_LOGI(tag, format, ...) \
    do { (void)(tag); } while (0)
#define ESP_LOGW(tag, format, ...) \
    do { (void)(tag); } while (0)
#define ESP_LOGE(tag, format, ...) \
    do { (void)(tag); } while (0)

#endif /* STUB_ESP_LOG_H */
