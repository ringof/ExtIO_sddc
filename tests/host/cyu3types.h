/*
 * Host-test stub for the FX3 SDK's <cyu3types.h>.  Provides only the base
 * types that SDDC_FX3/health.h and health.c need to compile on a
 * workstation.  This is NOT the real Cypress header — it exists so the
 * health-module host unit test can build with no FX3 SDK installed.
 * Picked up only when tests/host is on the include path (host build);
 * the firmware build uses the real SDK header.
 */
#ifndef HOSTTEST_CYU3TYPES_H
#define HOSTTEST_CYU3TYPES_H

#include <stdint.h>

typedef int CyBool_t;
#define CyTrue  1
#define CyFalse 0

typedef int CyU3PReturnStatus_t;
#define CY_U3P_SUCCESS 0

#endif /* HOSTTEST_CYU3TYPES_H */
