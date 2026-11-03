#ifndef SAW_CONTROL_H
#define SAW_CONTROL_H

#include "xil_types.h"

#define SAW_CONTROL_OK                    0
#define SAW_CONTROL_ERR_ARGUMENT          1
#define SAW_CONTROL_ERR_RANGE             2
#define SAW_CONTROL_ERR_SAMPLE_RATE       3
#define SAW_CONTROL_ERR_AD9361            4
#define SAW_CONTROL_ERR_STOP_TIMEOUT      5
#define SAW_CONTROL_ERR_BUSY              6
#define SAW_CONTROL_ERR_STORAGE           7

typedef struct {
    u32 start_freq_hz;
    u32 bandwidth_hz;
    u32 step_hz;
    u32 tx_time_us;
    u32 guard1_time_us;
    u32 rx_time_us;
    u32 guard2_time_us;
    u32 coherent_repetitions;
    u32 noncoherent_segments;
    u32 tx_sampling_freq_hz;
    u32 rx_sampling_freq_hz;
    u32 fine_enabled;
    u32 fine_half_bandwidth_hz;
    u32 fine_step_hz;
    u32 fine_lo_offset_hz;
    u32 coarse_lo_blind_hz;
    u32 coarse_lo_guard_hz;
} SAW_Config;

typedef struct {
    u32 pl_status;
    u32 pl_debug;
    u32 current_freq_index;
    u32 config_revision;
    u32 point_count;
    u32 rx_samples_last;
    u32 rx_capture_depth;
} SAW_RuntimeStatus;

extern SAW_Config g_saw_config;

int SAW_ValidateConfig(const SAW_Config *config);
int SAW_ApplyConfig(const SAW_Config *config);
int SAW_StartScan(void);
int SAW_StopScan(void);
void SAW_GetRuntimeStatus(SAW_RuntimeStatus *status);
u32 SAW_GetPointCount(const SAW_Config *config);
int SAW_ReadPointEnergy(u32 point_index, u64 *energy);
int SAW_PrepareFineSweep(u32 target_freq_hz, u32 start_freq_hz,
                         u32 point_count, u32 *lo_freq_hz);
int SAW_RestoreCoarseSweep(void);
int SAW_ReadSweepPointEnergy(u32 point_index, u32 point_count, u64 *energy);
int SAW_ConfigStore_Init(void);
int SAW_ConfigStore_Load(SAW_Config *config, u32 *receiver_ipv4,
                         u16 *receiver_port);
int SAW_ConfigStore_Save(const SAW_Config *config, u32 receiver_ipv4,
                         u16 receiver_port);
void SAW_ConfigStore_GetStatus(u32 *mounted, u32 *valid,
                               u32 *generation, u32 *last_error);
void SAW_ConfigStore_GetReceiver(u32 *receiver_ipv4, u16 *receiver_port);
void SAW_AutonomousStart(void);
void FPGA_Init(void);

#endif
