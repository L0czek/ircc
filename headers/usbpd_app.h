#ifndef __USBPD_APP_H__
#define __USBPD_APP_H__

/*
 * Bridge between the ST USB-PD stack (USER CODE sections in target/.../USBPD, plain C, sees
 * the ST types) and the controller's USB-C power policy (controller/src/usbpd_app.cc and
 * power_path.cc, C++). Only plain C types cross this boundary: the controller is not built
 * with the USBPD defines, so it must not include the ST headers.
 *
 * Design: docs/usb_pd.md.
 *
 *   usbpd_app_*  : implemented in C++ (controller), called from the ST USER CODE.
 *   usbpd_port_* : implemented in C (USBPD/Target/usbpd_dpm_user.c USER CODE), called from C++.
 *
 * Execution contexts of the usbpd_app_* calls:
 *   CAD thread (osPriorityRealtime), PE thread (osPriorityAboveNormal), UCPD ISR (events).
 * None of them may block or touch the I2C bus: the functions only update shared state
 * (short critical sections) and wake the "usbpd-power" thread, which talks to the BQ25792.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USBPD_APP_ROLE_NONE = 0,
    USBPD_APP_ROLE_SINK = 1,
    USBPD_APP_ROLE_SOURCE = 2,
} usbpd_app_role_t;

/* Partner Rp seen by our sink at attach; values equal ST vRd_Undefined / vRd_USB / vRd_1_5A / vRd_3_0A */
typedef enum {
    USBPD_APP_RP_NONE = 0,
    USBPD_APP_RP_DEFAULT = 1,
    USBPD_APP_RP_1A5 = 2,
    USBPD_APP_RP_3A0 = 3,
} usbpd_app_rp_t;

/* Policy-engine events, translated by the C side from USBPD_NotifyEventValue_TypeDef */
typedef enum {
    USBPD_APP_EV_REQUEST_ACCEPTED = 1,   /* sink: our Request was accepted (transition starts) */
    USBPD_APP_EV_REQUEST_REJECTED = 2,   /* sink: Reject                                        */
    USBPD_APP_EV_REQUEST_WAIT = 3,       /* sink: Wait                                          */
    USBPD_APP_EV_EXPLICIT_CONTRACT = 4,  /* PS_RDY: explicit contract in place (sink or source) */
    USBPD_APP_EV_HARD_RESET = 5,         /* hard reset sent or received                         */
    USBPD_APP_EV_SNK_READY = 6,          /* sink PE reached PE_SNK_Ready                        */
    USBPD_APP_EV_SRC_READY = 7,          /* source PE reached PE_SRC_Ready                      */
    USBPD_APP_EV_PE_DISABLED = 8,        /* partner does not answer PD (Type-C only)            */
} usbpd_app_event_t;

/* Hard reset phases, translated from USBPD_HR_Status_TypeDef */
typedef enum {
    USBPD_APP_HR_START = 1,         /* START_ACK / START_REQ: source turns VBUS off            */
    USBPD_APP_HR_WAIT_VSAFE0V = 2,  /* PE waits for vSafe0V                                     */
    USBPD_APP_HR_WAIT_VSAFE5V = 3,  /* source: turn VBUS back on; sink: waits for vSafe5V       */
    USBPD_APP_HR_COMPLETED = 4,
    USBPD_APP_HR_FAILED = 5,
} usbpd_app_hard_reset_t;

/* Result of usbpd_app_evaluate_request (source role) */
typedef enum {
    USBPD_APP_REQ_ACCEPT = 0,
    USBPD_APP_REQ_REJECT = 1,
    USBPD_APP_REQ_WAIT = 2,
} usbpd_app_request_result_t;

/* ---- VBUS (CAD thread every 2-10 ms, PE thread during hard reset): cached, never blocks ---- */

/* VBUS_USBC in mV (BQ25792 VAC1 ADC + AC1_PRESENT, refreshed by usbpd-power every 20 ms).
   Returns 0 until the first poll. */
uint32_t usbpd_app_vbus_mv(void);

/* False while a sink may not be supplied (source disabled, dock present, battery low/absent).
   The C side then holds the CAD source attach-wait (see docs/usb_pd.md "Source gating"). */
bool usbpd_app_source_allowed(void);

/* ---- Type-C (CAD thread) ---- */

/* role: power role at attach; cc_line: 1 = CC1, 2 = CC2 (UCPD numbering); rp: sink only.
   Returns false to refuse the attach (source no longer allowed): the C side then enters
   error recovery. */
bool usbpd_app_attached(usbpd_app_role_t role, uint8_t cc_line, usbpd_app_rp_t rp);
void usbpd_app_detached(void);

/* ---- Sink (PE thread) ---- */

/* Partner's Source_Capabilities (copied; count <= 7) */
void usbpd_app_source_caps(const uint32_t* pdos, uint32_t count);

/* Request Data Object for the stored Source_Capabilities (policy: docs/usb_pd.md "Sink
   selection"). Never fails: falls back to PDO 1 (vSafe5V). *is_apdo is false (fixed PDO). */
uint32_t usbpd_app_build_request(bool* is_apdo);

/* Our Sink_Capabilities (Get_Sink_Cap answer). Writes up to max PDOs, returns the count. */
uint32_t usbpd_app_sink_pdos(uint32_t* pdos, uint32_t max);

/* ---- Source (PE thread) ---- */

/* Our Source_Capabilities. Writes up to max PDOs, returns the count. */
uint32_t usbpd_app_source_pdos(uint32_t* pdos, uint32_t max);

/* Evaluate the partner's Request (RDO from SetDataInfo RCV_REQ_PDO) */
usbpd_app_request_result_t usbpd_app_evaluate_request(uint32_t rdo);

/* ---- Events (any context, including the UCPD ISR) ---- */

void usbpd_app_event(usbpd_app_event_t event);
void usbpd_app_hard_reset(usbpd_app_role_t role, usbpd_app_hard_reset_t phase);

/* ======================================================================================
 * Implemented by the C side (usbpd_dpm_user.c USER CODE), called from the controller.
 * ====================================================================================== */

typedef struct {
    bool attached;             /* DPM_Params.PE_IsConnected                          */
    uint8_t power_role;        /* usbpd_app_role_t (NONE when detached)               */
    uint8_t data_role;         /* 0 none, 1 UFP, 2 DFP                                 */
    uint8_t cc_line;           /* 0 none, 1 CC1, 2 CC2                                 */
    uint8_t spec_revision;     /* PD major revision: 2 or 3 (0 unknown)               */
    bool explicit_contract;    /* DPM_Params.PE_Power == USBPD_POWER_EXPLICITCONTRACT */
} usbpd_port_info_t;

/* Snapshot of the stack state (any thread) */
void usbpd_port_get_info(usbpd_port_info_t* info);

/* Sink: ask the partner for its Source_Capabilities again (hard_reset = false), which
   re-runs the selection, or send a Hard Reset. Returns 0 ok, 1 not attached as sink,
   2 policy engine busy, 3 stack error. */
int usbpd_port_renegotiate(bool hard_reset);

/* Force Type-C error recovery (both CC open >= 25 ms, then toggling restarts). Used when
   the source role must end (dock arrived, battery low) or after a refused attach. */
void usbpd_port_error_recovery(void);

/* Board hook for main.c USER CODE 2 (before MX_USBPD_Init): DRP toggling, toggle times,
   Rp 3.0 A, role/data swap and VDM policy (overrides the generated DPM_Settings). */
void usbpd_port_early_init(void);

#ifdef __cplusplus
}
#endif

#endif /* __USBPD_APP_H__ */
