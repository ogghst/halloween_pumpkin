/* Internal constants of the net_mgr reconnect backoff. The public
 * declaration of net_backoff_compute lives in net_mgr.h; nothing here is
 * part of the component API. */
#ifndef NET_BACKOFF_H
#define NET_BACKOFF_H

/* Delay for attempt 0, doubled at every subsequent attempt. */
#define NET_BACKOFF_BASE_MS 1000u

/* Jitter half-span: rand_pct 0..99 maps deterministically to
 * [-15%, +15%] of the doubled base (0 -> -15%, 99 -> +15%). */
#define NET_BACKOFF_JITTER_PCT 15u

#endif /* NET_BACKOFF_H */
