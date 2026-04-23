#ifndef OCSVM_PLATFORM_H
#define OCSVM_PLATFORM_H

#include <stdint.h>

/*
 * Optional auto-discovery of generated SoC map headers.
 * If soc_defs.h is available on the include path, monitor base and tile count
 * are derived automatically.
 */
#if defined(__has_include)
#if __has_include("soc_defs.h")
#include "soc_defs.h"
#endif
#endif

/*
 * Default addresses keep current behavior. Override from build flags or
 * generated headers when available.
 */
#ifndef ESP_MON_BASE_ADDR
#ifdef MONITOR_BASE_ADDR
#define ESP_MON_BASE_ADDR ((uint32_t)(MONITOR_BASE_ADDR))
#else
#define ESP_MON_BASE_ADDR 0x60090000UL
#endif
#endif

#ifndef N_TILES
#if defined(SOC_ROWS) && defined(SOC_COLS)
#define N_TILES ((SOC_ROWS) * (SOC_COLS))
#else
#define N_TILES 9
#endif
#endif

#ifndef UART_BASE
#define UART_BASE 0x40000000UL
#endif

#ifndef SOCURITY_ALERT_BASE
#define SOCURITY_ALERT_BASE 0x20070000UL
#endif

#ifndef SOCURITY_ALERT_TRIGGER
#define SOCURITY_ALERT_TRIGGER 0x00UL
#endif

#ifndef SOCURITY_ALERT_ITERATION
#define SOCURITY_ALERT_ITERATION 0x04UL
#endif

#ifndef SOCURITY_ALERT_CLEAR
#define SOCURITY_ALERT_CLEAR 0x08UL
#endif

#ifndef SOCURITY_ALERT_STATUS
#define SOCURITY_ALERT_STATUS 0x0CUL
#endif

#ifndef SOCURITY_ENABLE_IRQ_SELFTEST
#define SOCURITY_ENABLE_IRQ_SELFTEST 1
#endif

#ifndef SOCURITY_ENABLE_ALERT_TRIGGER
#define SOCURITY_ENABLE_ALERT_TRIGGER 1
#endif

#ifndef SOCURITY_SHARED_FLAG_ADDR
#define SOCURITY_SHARED_FLAG_ADDR 0x80000000UL
#endif

#endif /* OCSVM_PLATFORM_H */