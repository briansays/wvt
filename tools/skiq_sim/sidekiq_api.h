// The part of Epiq Solutions' sidekiq_api.h that wvt uses, for the simulated
// Sidekiq (skiq_api_sim.cpp). The declarations and values match libsidekiq
// 4.26.0, so src/device/sdr_skiq.cpp builds against either. skiq_param_t has
// only the fields that wvt reads. Build with -DWVT_SKIQ_SIM=ON; for a real
// card, point SIDEKIQ_SDK_DIR at the SDK instead.
#ifndef WVT_SKIQ_API_SIM_H
#define WVT_SKIQ_API_SIM_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
// As in the real header: the SKIQ_LOG_* levels are the syslog priorities.
#include <syslog.h>

#ifdef __cplusplus
#define EPIQ_API extern "C"
#else
#define EPIQ_API extern
#endif

#define SKIQ_MAX_NUM_CARDS (32)
#define SKIQ_SERIAL_NUM_STRLEN (6)
#define SKIQ_RX_HEADER_SIZE_IN_WORDS 6
#define SKIQ_RX_HEADER_SIZE_IN_BYTES (SKIQ_RX_HEADER_SIZE_IN_WORDS * sizeof(uint32_t))

#define RX_TRANSFER_NO_WAIT (0)
#define RX_TRANSFER_WAIT_FOREVER (-1)
#define RX_TRANSFER_WAIT_NOT_SUPPORTED (-2)

#define SKIQ_LOG_DEBUG ((int)LOG_DEBUG)
#define SKIQ_LOG_INFO ((int)LOG_INFO)
#define SKIQ_LOG_WARNING ((int)LOG_WARNING)
#define SKIQ_LOG_ERROR ((int)LOG_ERR)

typedef enum {
    skiq_xport_type_pcie = 0,
    skiq_xport_type_usb,
    skiq_xport_type_custom,
    skiq_xport_type_net,
    skiq_xport_type_max,
    skiq_xport_type_auto,
    skiq_xport_type_unknown,
} skiq_xport_type_t;

typedef enum {
    skiq_xport_init_level_basic = 0,
    skiq_xport_init_level_full,
    skiq_xport_init_level_unknown,
} skiq_xport_init_level_t;

typedef enum { skiq_iq_order_qi = 0, skiq_iq_order_iq } skiq_iq_order_t;

typedef enum {
    skiq_rx_hdl_A1 = 0,
    skiq_rx_hdl_A2 = 1,
    skiq_rx_hdl_B1 = 2,
    skiq_rx_hdl_B2 = 3,
    skiq_rx_hdl_C1 = 4,
    skiq_rx_hdl_D1 = 5,
    skiq_rx_hdl_end = 6
} skiq_rx_hdl_t;

typedef enum { skiq_rx_gain_manual = 0, skiq_rx_gain_auto } skiq_rx_gain_t;

typedef enum {
    skiq_mpcie = 0,
    skiq_m2,
    skiq_x2,
    skiq_z2,
    skiq_x4,
    skiq_m2_2280,
    skiq_z2p,
    skiq_z3u,
    skiq_nv100,
    skiq_x40,
    skiq_nvm2,
    skiq_z4,
    skiq_z4_mp,
    skiq_part_invalid
} skiq_part_t;

typedef enum {
    skiq_ref_clock_internal = 0,
    skiq_ref_clock_external,
    skiq_ref_clock_host,
    skiq_ref_clock_carrier_edge,
    skiq_ref_clock_invalid,
} skiq_ref_clock_select_t;

typedef enum {
    skiq_rx_status_success = 0,
    skiq_rx_status_no_data = -1,
    skiq_rx_status_error_generic = -6,
    skiq_rx_status_error_overrun = -11,
    skiq_rx_status_error_packet_malformed = -12,
    skiq_rx_status_error_card_not_active = -19,
    skiq_rx_status_error_not_streaming = -29,
} skiq_rx_status_t;

typedef enum {
    skiq_rf_port_unknown = -1,
    skiq_rf_port_J1 = 0,
    skiq_rf_port_J2 = 1,
    skiq_rf_port_J3 = 2,
    skiq_rf_port_J4 = 3,
    skiq_rf_port_J5 = 4,
    skiq_rf_port_J6 = 5,
    skiq_rf_port_J7 = 6,
    skiq_rf_port_J300 = 7,
    skiq_rf_port_Jxxx_RX1 = 8,
    skiq_rf_port_Jxxx_TX1RX2 = 9,
    skiq_rf_port_J8 = 10,
    skiq_rf_port_max = 11,
} skiq_rf_port_t;

typedef struct {
    skiq_xport_init_level_t init_level;
    skiq_part_t part_type;
    skiq_xport_type_t xport;
    uint8_t card;
    char serial_string[SKIQ_SERIAL_NUM_STRLEN];
} skiq_card_param_t;

typedef struct {
    skiq_rx_hdl_t handle;
    uint8_t gain_index_max;
    uint8_t gain_index_min;
    uint8_t iq_resolution;
    uint64_t lo_freq_max;
    uint64_t lo_freq_min;
    uint32_t sample_rate_max;
    uint32_t sample_rate_min;
    uint8_t num_fixed_rf_ports;
    skiq_rf_port_t fixed_rf_ports[skiq_rf_port_max];
    uint8_t num_trx_rf_ports;
    skiq_rf_port_t trx_rf_ports[skiq_rf_port_max];
} skiq_rx_param_t;

typedef struct {
    skiq_card_param_t card_param;
    skiq_rx_param_t rx_param[skiq_rx_hdl_end];
} skiq_param_t;

typedef struct __attribute__((__packed__, aligned(8))) {
    volatile uint64_t rf_timestamp;
    volatile uint64_t sys_timestamp;
    union {
        volatile uint64_t raw_meta;  // __raw_meta in the SDK
        struct {
            volatile uint64_t hdl : 6;
            volatile uint64_t overload : 1;
            volatile uint64_t rfic_control : 8;
            volatile uint64_t id : 8;
            volatile uint64_t system_meta : 6;
            volatile uint64_t version : 3;
            volatile uint64_t user_meta : 32;
        };
    };
    volatile int16_t data[];
} skiq_rx_block_t;

EPIQ_API void skiq_register_logging(void (*log_msg)(int32_t priority, const char *message));
EPIQ_API int32_t skiq_read_libsidekiq_version(uint8_t *p_major, uint8_t *p_minor, uint8_t *p_patch,
                                              const char **p_label);
EPIQ_API int32_t skiq_get_cards(skiq_xport_type_t xport_type, uint8_t *p_num_cards, uint8_t *p_cards);
EPIQ_API int32_t skiq_get_card_from_serial_string(char *p_serial_num, uint8_t *p_card);
EPIQ_API int32_t skiq_is_card_avail(uint8_t card, pid_t *p_card_owner);
EPIQ_API int32_t skiq_init(skiq_xport_type_t type, skiq_xport_init_level_t level, uint8_t *p_card_nums,
                           uint8_t num_cards);
EPIQ_API int32_t skiq_exit(void);
EPIQ_API int32_t skiq_read_parameters(uint8_t card, skiq_param_t *p_param);
EPIQ_API const char *skiq_part_string(skiq_part_t part);
EPIQ_API const char *skiq_rf_port_string(skiq_rf_port_t rf_port);
EPIQ_API int32_t skiq_write_ref_clock_select(uint8_t card, skiq_ref_clock_select_t ref_clock_source);
EPIQ_API int32_t skiq_write_iq_order_mode(uint8_t card, skiq_iq_order_t mode);
EPIQ_API int32_t skiq_write_rx_rf_port_for_hdl(uint8_t card, skiq_rx_hdl_t hdl, skiq_rf_port_t rf_port);
EPIQ_API int32_t skiq_read_rx_rf_port_for_hdl(uint8_t card, skiq_rx_hdl_t hdl, skiq_rf_port_t *p_rf_port);
EPIQ_API int32_t skiq_write_rx_sample_rate_and_bandwidth(uint8_t card, skiq_rx_hdl_t hdl, uint32_t rate,
                                                         uint32_t bandwidth);
EPIQ_API int32_t skiq_read_rx_sample_rate_and_bandwidth(uint8_t card, skiq_rx_hdl_t hdl, uint32_t *p_rate,
                                                        double *p_actual_rate, uint32_t *p_bandwidth,
                                                        uint32_t *p_actual_bandwidth);
EPIQ_API int32_t skiq_write_rx_LO_freq(uint8_t card, skiq_rx_hdl_t hdl, uint64_t freq);
EPIQ_API int32_t skiq_read_rx_LO_freq(uint8_t card, skiq_rx_hdl_t hdl, uint64_t *p_freq, double *p_actual_freq);
EPIQ_API int32_t skiq_write_rx_gain_mode(uint8_t card, skiq_rx_hdl_t hdl, skiq_rx_gain_t gain_mode);
EPIQ_API int32_t skiq_read_rx_gain_index_range(uint8_t card, skiq_rx_hdl_t hdl, uint8_t *p_gain_index_min,
                                               uint8_t *p_gain_index_max);
EPIQ_API int32_t skiq_write_rx_gain(uint8_t card, skiq_rx_hdl_t hdl, uint8_t gain_index);
EPIQ_API int32_t skiq_read_rx_overload_state(uint8_t card, skiq_rx_hdl_t hdl, uint8_t *p_overload);
EPIQ_API int32_t skiq_set_rx_transfer_timeout(const uint8_t card, const int32_t timeout_us);
EPIQ_API int32_t skiq_get_rx_transfer_timeout(const uint8_t card, int32_t *p_timeout_us);
EPIQ_API int32_t skiq_start_rx_streaming(uint8_t card, skiq_rx_hdl_t hdl);
EPIQ_API int32_t skiq_stop_rx_streaming(uint8_t card, skiq_rx_hdl_t hdl);
EPIQ_API skiq_rx_status_t skiq_receive(uint8_t card, skiq_rx_hdl_t *p_hdl, skiq_rx_block_t **pp_block,
                                       uint32_t *p_data_len);

#endif
