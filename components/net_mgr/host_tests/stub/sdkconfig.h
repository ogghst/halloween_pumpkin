/* Minimal sdkconfig.h stub for host builds of the net_mgr pure core:
 * net_backoff.c reads the Kconfig cap; on target it comes from the real
 * generated sdkconfig.h, here it is pinned to the Kconfig default. */
#ifndef STUB_SDKCONFIG_H
#define STUB_SDKCONFIG_H

#define CONFIG_PUMPKIN_NET_BACKOFF_MAX_MS 60000

#endif /* STUB_SDKCONFIG_H */
