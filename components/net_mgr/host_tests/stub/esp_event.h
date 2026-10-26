/* Minimal esp_event.h stub for host builds of the net_mgr pure core:
 * net_mgr.h only declares the NET_EVENT base; the pure core never posts
 * events itself (the IDF layer does). */
#ifndef STUB_ESP_EVENT_H
#define STUB_ESP_EVENT_H

typedef const char *esp_event_base_t;

#define ESP_EVENT_DECLARE_BASE(base) extern const esp_event_base_t base

#endif /* STUB_ESP_EVENT_H */
