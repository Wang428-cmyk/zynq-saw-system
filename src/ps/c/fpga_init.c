#include "xbram_hw.h"
#include "ad9361/ad9361_api.h"
#include "ad9361/ad9361.h"
#include "sleep.h"
#include "xil_printf.h"
#include "saw_control.h"

extern int AD9361_Init(void);
extern void PL_Reset(void);
extern struct ad9361_rf_phy *ad9361_phy;

#define SAW_STATE_CLK_HZ         122880000ULL
#define SAW_DDS_SAMPLE_FREQ_HZ   61440000LL
#define SAW_MAX_POINT_COUNT      127UL
#define SAW_FINE_MAX_POINT_COUNT 1024UL
#define SAW_RX_CAPTURE_DEPTH     65536UL
#define SAW_1R1T_FRAME_CYCLES    2UL
#define SAW_MIN_RF_FREQ_HZ       70000000UL
#define SAW_MAX_RF_FREQ_HZ       4000000000UL
#define SAW_FINE_RF_GUARD_HZ     200000UL
#define SAW_COARSE_RF_GUARD_HZ   200000UL

#define SAW_REG_CONTROL          0x0004
#define SAW_REG_STATUS           0x0008
#define SAW_REG_TX_SAMPLES       0x000C
#define SAW_REG_GUARD1_SAMPLES   0x0010
#define SAW_REG_RX_SAMPLES       0x0014
#define SAW_REG_POINT_COUNT      0x001C
#define SAW_REG_START_PHASE      0x0020
#define SAW_REG_STEP_PHASE       0x0024
#define SAW_REG_GUARD2_SAMPLES   0x0028
#define SAW_REG_RX_CAPTURED      0x002C
#define SAW_REG_N_COH            0x0030
#define SAW_REG_M_NC             0x0034
#define SAW_REG_DEBUG            0x0038
#define SAW_REG_CURRENT          0x003C
#define SAW_REG_DATA_BASE        0x0400
#define SAW_RESULT_STRIDE        0x0020
#define SAW_RESULT_ENERGY_LOW    0x0004
#define SAW_RESULT_ENERGY_HIGH   0x0008

#define SAW_CMD_START            0xAA550201
#define SAW_CMD_STOP             0xAA550203

SAW_Config g_saw_config = {
    912000000UL,
    6000000UL,
    60000UL,
    10UL,
    25UL,
    20UL,
    5UL,
    64UL,
    16UL,
    61440000UL,
    61440000UL,
    0UL,
    100000UL,
    1000UL,
    1000000UL,
    180000UL,
    300000UL
};

uint32_t TX_ATTEN          = 0e3;
uint32_t TX_LO_FREQ        = 911700000UL;
uint32_t RX_LO_FREQ        = 911700000UL;
uint32_t TX_RF_BANDWIDTH   = 13000000UL;
uint32_t RX_RF_BANDWIDTH   = 13000000UL;
uint32_t TX_SAMPLING_FREQ  = SAW_DDS_SAMPLE_FREQ_HZ;
uint32_t RX_SAMPLING_FREQ  = SAW_DDS_SAMPLE_FREQ_HZ;
extern int32_t RX_MANUAL_GAIN_DB;

u32 EMMC_Variable[4];
u32 BRAM_CMD[4] = {0xAA5501BD, 0xAA5501BC, 0xAA5501BB, 0xAA5501AA};

static u32 saw_config_revision;

static int SAW_ProgramRf(u32 center_freq_hz, u32 bandwidth_hz)
{
    int ret;

    ret = ad9361_set_tx_lo_freq(ad9361_phy, center_freq_hz);
    if (ret < 0) {
        xil_printf("SAW AD9361 TX LO failed: %d\r\n", ret);
        return ret;
    }
    ret = ad9361_set_rx_lo_freq(ad9361_phy, center_freq_hz);
    if (ret < 0) {
        xil_printf("SAW AD9361 RX LO failed: %d\r\n", ret);
        return ret;
    }
    ret = ad9361_set_tx_rf_bandwidth(ad9361_phy, bandwidth_hz);
    if (ret < 0) {
        xil_printf("SAW AD9361 TX bandwidth failed: %d\r\n", ret);
        return ret;
    }
    ret = ad9361_set_rx_rf_bandwidth(ad9361_phy, bandwidth_hz);
    if (ret < 0) {
        xil_printf("SAW AD9361 RX bandwidth failed: %d\r\n", ret);
        return ret;
    }
    return 0;
}

static int SAW_ProgramLo(u32 center_freq_hz)
{
    int ret;

    ret = ad9361_set_tx_lo_freq(ad9361_phy, center_freq_hz);
    if (ret < 0) {
        xil_printf("SAW AD9361 TX LO failed: %d\r\n", ret);
        return ret;
    }
    ret = ad9361_set_rx_lo_freq(ad9361_phy, center_freq_hz);
    if (ret < 0) {
        xil_printf("SAW AD9361 RX LO failed: %d\r\n", ret);
        return ret;
    }
    return 0;
}

static u32 SAW_TimeUsToSamples(u32 time_us)
{
    u32 cycles = (u32)((((u64)time_us * SAW_STATE_CLK_HZ) + 999999ULL) /
                       1000000ULL);

    return (cycles + SAW_1R1T_FRAME_CYCLES - 1UL) &
           ~(SAW_1R1T_FRAME_CYCLES - 1UL);
}

static u32 SAW_GetRxSampleCount(const SAW_Config *config)
{
    return SAW_TimeUsToSamples(config->rx_time_us) /
           SAW_1R1T_FRAME_CYCLES;
}

static u32 SAW_FreqHzToPhaseWord(s32 freq_hz)
{
    s64 word = ((s64)freq_hz * (1LL << 32)) / SAW_DDS_SAMPLE_FREQ_HZ;
    return (u32)((s32)word);
}

u32 SAW_GetPointCount(const SAW_Config *config)
{
    if ((config == 0) || (config->step_hz == 0UL)) {
        return 0UL;
    }

    return (config->bandwidth_hz / config->step_hz) + 1UL;
}

static u32 SAW_GetFinePointCount(const SAW_Config *config)
{
    if ((config == 0) || (config->fine_step_hz == 0UL)) {
        return 0UL;
    }
    return ((config->fine_half_bandwidth_hz * 2UL) /
            config->fine_step_hz) + 1UL;
}

static u32 SAW_GetRequiredRfBandwidth(const SAW_Config *config)
{
    u64 coarse_bandwidth =
        ((u64)config->coarse_lo_guard_hz +
         (u64)config->bandwidth_hz + SAW_COARSE_RF_GUARD_HZ) * 2ULL;
    u64 fine_bandwidth = 0ULL;

    if (config->fine_enabled) {
        fine_bandwidth =
            ((u64)config->fine_lo_offset_hz +
             (u64)config->fine_half_bandwidth_hz +
             SAW_FINE_RF_GUARD_HZ) * 2ULL;
    }
    return (u32)((coarse_bandwidth > fine_bandwidth) ?
                 coarse_bandwidth : fine_bandwidth);
}

int SAW_ValidateConfig(const SAW_Config *config)
{
    u32 points;
    u32 fine_points;
    u32 required_bandwidth;

    if (config == 0) {
        return SAW_CONTROL_ERR_ARGUMENT;
    }
    if ((config->start_freq_hz < SAW_MIN_RF_FREQ_HZ) ||
        (config->start_freq_hz > SAW_MAX_RF_FREQ_HZ) ||
        (config->bandwidth_hz < 200000UL) ||
        ((u64)config->start_freq_hz + config->bandwidth_hz >
         SAW_MAX_RF_FREQ_HZ) ||
        (config->step_hz == 0UL) ||
        ((config->bandwidth_hz % config->step_hz) != 0UL) ||
        (config->coarse_lo_guard_hz <= config->coarse_lo_blind_hz) ||
        (config->start_freq_hz <
         (SAW_MIN_RF_FREQ_HZ + config->coarse_lo_guard_hz)) ||
        ((u64)config->coarse_lo_guard_hz + config->bandwidth_hz >=
         (SAW_DDS_SAMPLE_FREQ_HZ / 2LL)) ||
        (config->tx_time_us == 0UL) ||
        (config->rx_time_us == 0UL) ||
        (config->coherent_repetitions == 0UL) ||
        (config->coherent_repetitions > 65535UL) ||
        (config->noncoherent_segments == 0UL) ||
        (config->noncoherent_segments > 65535UL)) {
        return SAW_CONTROL_ERR_RANGE;
    }

    if (SAW_GetRxSampleCount(config) > SAW_RX_CAPTURE_DEPTH) {
        return SAW_CONTROL_ERR_RANGE;
    }

    points = SAW_GetPointCount(config);
    if ((points == 0UL) || (points > SAW_MAX_POINT_COUNT)) {
        return SAW_CONTROL_ERR_RANGE;
    }

    if (config->fine_enabled) {
        fine_points = SAW_GetFinePointCount(config);
        if ((config->fine_enabled > 1UL) ||
            (config->fine_half_bandwidth_hz == 0UL) ||
            (config->fine_step_hz == 0UL) ||
            (config->fine_lo_offset_hz == 0UL) ||
            ((u64)config->fine_half_bandwidth_hz * 2ULL <
             config->step_hz) ||
            ((u64)config->fine_half_bandwidth_hz +
             (u64)config->coarse_lo_blind_hz >=
             (u64)config->fine_lo_offset_hz) ||
            ((u64)config->fine_lo_offset_hz +
             (u64)config->fine_half_bandwidth_hz >=
             (SAW_DDS_SAMPLE_FREQ_HZ / 2LL)) ||
            ((u64)config->start_freq_hz <
             ((u64)SAW_MIN_RF_FREQ_HZ +
              config->fine_half_bandwidth_hz)) ||
            ((u64)config->start_freq_hz +
             config->bandwidth_hz +
             config->fine_half_bandwidth_hz >
             SAW_MAX_RF_FREQ_HZ) ||
            (fine_points == 0UL) ||
            (fine_points > SAW_FINE_MAX_POINT_COUNT)) {
            return SAW_CONTROL_ERR_RANGE;
        }
    }

    required_bandwidth = SAW_GetRequiredRfBandwidth(config);
    if ((required_bandwidth < 200000UL) ||
        (required_bandwidth > 56000000UL)) {
        return SAW_CONTROL_ERR_RANGE;
    }

    /* The present PL clock wizard is designed for 61.44 MSPS only. */
    if ((config->tx_sampling_freq_hz != 61440000UL) ||
        (config->rx_sampling_freq_hz != 61440000UL)) {
        return SAW_CONTROL_ERR_SAMPLE_RATE;
    }

    return SAW_CONTROL_OK;
}

static void SAW_WritePlSweep(s32 start_freq_offset_hz, u32 step_hz,
                             u32 point_count)
{
    u32 base = XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR;

    XBram_WriteReg(base, SAW_REG_POINT_COUNT, point_count);
    XBram_WriteReg(base, SAW_REG_START_PHASE,
                   SAW_FreqHzToPhaseWord(start_freq_offset_hz));
    XBram_WriteReg(base, SAW_REG_STEP_PHASE,
                   SAW_FreqHzToPhaseWord((s32)step_hz));
}

static void SAW_WritePlConfig(const SAW_Config *config)
{
    u32 base = XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR;

    XBram_WriteReg(base, SAW_REG_TX_SAMPLES,
                   SAW_TimeUsToSamples(config->tx_time_us));
    XBram_WriteReg(base, SAW_REG_GUARD1_SAMPLES,
                   SAW_TimeUsToSamples(config->guard1_time_us));
    XBram_WriteReg(base, SAW_REG_RX_SAMPLES,
                   SAW_TimeUsToSamples(config->rx_time_us));
    XBram_WriteReg(base, SAW_REG_GUARD2_SAMPLES,
                   SAW_TimeUsToSamples(config->guard2_time_us));
    SAW_WritePlSweep((s32)config->coarse_lo_guard_hz,
                     config->step_hz, SAW_GetPointCount(config));
    XBram_WriteReg(base, SAW_REG_N_COH, config->coherent_repetitions);
    XBram_WriteReg(base, SAW_REG_M_NC, config->noncoherent_segments);
}

int SAW_StopScan(void)
{
    u32 timeout;
    u32 base = XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR;

    XBram_WriteReg(base, SAW_REG_CONTROL, SAW_CMD_STOP);
    usleep(100);
    for (timeout = 0; timeout < 1000UL; timeout++) {
        if ((XBram_ReadReg(base, SAW_REG_STATUS) & 0x2UL) == 0UL) {
            return SAW_CONTROL_OK;
        }
        usleep(10);
    }

    return SAW_CONTROL_ERR_STOP_TIMEOUT;
}

int SAW_StartScan(void)
{
    XBram_WriteReg(XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR,
                   SAW_REG_CONTROL, SAW_CMD_START);
    usleep(100);
    return SAW_CONTROL_OK;
}

int SAW_ApplyConfig(const SAW_Config *config)
{
    int ret;
    u32 bandwidth;
    u32 previous_bandwidth;
    u32 coarse_lo;
    u32 previous_coarse_lo;

    ret = SAW_ValidateConfig(config);
    if (ret != SAW_CONTROL_OK) {
        return ret;
    }

    ret = SAW_StopScan();
    if (ret != SAW_CONTROL_OK) {
        return ret;
    }

    /* In 1R1T mode logical RX1 maps to the selected physical RX2. Apply
       gain first so a gain error cannot leave LO and PL settings mismatched. */
    ret = ad9361_set_rx_gain_control_mode(ad9361_phy, RX1, RF_GAIN_MGC);
    if (ret < 0) {
        xil_printf("SAW AD9361 MGC mode failed: %d\r\n", ret);
        return SAW_CONTROL_ERR_AD9361;
    }
    ret = ad9361_set_rx_rf_gain(ad9361_phy, RX1, RX_MANUAL_GAIN_DB);
    if (ret < 0) {
        xil_printf("SAW AD9361 RX gain failed: %d\r\n", ret);
        return SAW_CONTROL_ERR_AD9361;
    }

    bandwidth = SAW_GetRequiredRfBandwidth(config);
    previous_bandwidth = SAW_GetRequiredRfBandwidth(&g_saw_config);
    coarse_lo = config->start_freq_hz - config->coarse_lo_guard_hz;
    previous_coarse_lo = g_saw_config.start_freq_hz -
                         g_saw_config.coarse_lo_guard_hz;
    ret = SAW_ProgramRf(coarse_lo, bandwidth);
    if (ret < 0) {
        /* Best-effort rollback keeps the RF LO plan aligned with the PL
           configuration when any later AD9361 operation fails. */
        (void)SAW_ProgramRf(previous_coarse_lo, previous_bandwidth);
        return SAW_CONTROL_ERR_AD9361;
    }

    g_saw_config = *config;
    TX_LO_FREQ = coarse_lo;
    RX_LO_FREQ = coarse_lo;
    TX_RF_BANDWIDTH = bandwidth;
    RX_RF_BANDWIDTH = bandwidth;
    SAW_WritePlConfig(&g_saw_config);
    saw_config_revision++;

    xil_printf("SAW configuration updated; scan remains stopped\r\n");
    return SAW_CONTROL_OK;
}

void SAW_GetRuntimeStatus(SAW_RuntimeStatus *status)
{
    u32 base = XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR;

    if (status == 0) {
        return;
    }
    status->pl_status = XBram_ReadReg(base, SAW_REG_STATUS);
    status->pl_debug = XBram_ReadReg(base, SAW_REG_DEBUG);
    status->current_freq_index = XBram_ReadReg(base, SAW_REG_CURRENT);
    status->config_revision = saw_config_revision;
    status->point_count = SAW_GetPointCount(&g_saw_config);
    status->rx_samples_last = XBram_ReadReg(base, SAW_REG_RX_CAPTURED);
    status->rx_capture_depth = SAW_RX_CAPTURE_DEPTH;
}

int SAW_ReadPointEnergy(u32 point_index, u64 *energy)
{
    return SAW_ReadSweepPointEnergy(
        point_index, SAW_GetPointCount(&g_saw_config), energy);
}

int SAW_ReadSweepPointEnergy(u32 point_index, u32 point_count, u64 *energy)
{
    u32 base = XPAR_AXI_BRAM_CTRL_0_S_AXI_BASEADDR;
    u32 result_offset;
    u32 low;
    u32 high;

    if ((energy == 0) || (point_count == 0UL) ||
        (point_count > SAW_MAX_POINT_COUNT) ||
        (point_index >= point_count)) {
        return SAW_CONTROL_ERR_ARGUMENT;
    }

    result_offset = SAW_REG_DATA_BASE + (point_index * SAW_RESULT_STRIDE);
    /* The capture RAM adds one registered read stage beyond the AXI BRAM
       controller's configured latency. Prime each address once so the second
       transaction returns the requested word instead of the preceding field. */
    (void)XBram_ReadReg(base, result_offset + SAW_RESULT_ENERGY_LOW);
    low = XBram_ReadReg(base, result_offset + SAW_RESULT_ENERGY_LOW);
    (void)XBram_ReadReg(base, result_offset + SAW_RESULT_ENERGY_HIGH);
    high = XBram_ReadReg(base, result_offset + SAW_RESULT_ENERGY_HIGH);
    *energy = ((u64)high << 32) | (u64)low;
    return SAW_CONTROL_OK;
}

int SAW_PrepareFineSweep(u32 target_freq_hz, u32 start_freq_hz,
                         u32 point_count, u32 *lo_freq_hz)
{
    u32 lo;
    s64 nco_start_hz;
    u64 stop_freq_hz;
    int ret;

    if ((lo_freq_hz == 0) || (point_count == 0UL) ||
        (point_count > SAW_MAX_POINT_COUNT)) {
        return SAW_CONTROL_ERR_ARGUMENT;
    }
    stop_freq_hz = (u64)start_freq_hz +
                   ((u64)point_count - 1ULL) *
                   g_saw_config.fine_step_hz;
    if ((start_freq_hz < SAW_MIN_RF_FREQ_HZ) ||
        (stop_freq_hz > SAW_MAX_RF_FREQ_HZ)) {
        return SAW_CONTROL_ERR_RANGE;
    }

    if (((u64)target_freq_hz + g_saw_config.fine_lo_offset_hz) <=
        SAW_MAX_RF_FREQ_HZ) {
        lo = target_freq_hz + g_saw_config.fine_lo_offset_hz;
    }
    else if (target_freq_hz >
             (SAW_MIN_RF_FREQ_HZ + g_saw_config.fine_lo_offset_hz)) {
        lo = target_freq_hz - g_saw_config.fine_lo_offset_hz;
    }
    else {
        return SAW_CONTROL_ERR_RANGE;
    }

    nco_start_hz = (s64)start_freq_hz - (s64)lo;
    if ((nco_start_hz <= -(SAW_DDS_SAMPLE_FREQ_HZ / 2LL)) ||
        (nco_start_hz >= (SAW_DDS_SAMPLE_FREQ_HZ / 2LL))) {
        return SAW_CONTROL_ERR_RANGE;
    }

    ret = SAW_StopScan();
    if (ret != SAW_CONTROL_OK) {
        return ret;
    }
    if (TX_LO_FREQ != lo) {
        ret = SAW_ProgramLo(lo);
        if (ret < 0) {
            return SAW_CONTROL_ERR_AD9361;
        }
        TX_LO_FREQ = lo;
        RX_LO_FREQ = lo;
        usleep(100);
    }
    SAW_WritePlSweep((s32)nco_start_hz, g_saw_config.fine_step_hz,
                     point_count);
    *lo_freq_hz = lo;
    return SAW_CONTROL_OK;
}

int SAW_RestoreCoarseSweep(void)
{
    int ret;
    u32 coarse_lo = g_saw_config.start_freq_hz -
                    g_saw_config.coarse_lo_guard_hz;

    ret = SAW_StopScan();
    if (ret != SAW_CONTROL_OK) {
        return ret;
    }
    if (TX_LO_FREQ != coarse_lo) {
        ret = SAW_ProgramLo(coarse_lo);
        if (ret < 0) {
            return SAW_CONTROL_ERR_AD9361;
        }
        TX_LO_FREQ = coarse_lo;
        RX_LO_FREQ = coarse_lo;
        usleep(100);
    }
    SAW_WritePlConfig(&g_saw_config);
    return SAW_CONTROL_OK;
}

static void SAW_PrintConfig(void)
{
    u32 points = SAW_GetPointCount(&g_saw_config);
    u32 start_khz = g_saw_config.start_freq_hz / 1000UL;
    u32 stop_khz = (g_saw_config.start_freq_hz +
                    g_saw_config.bandwidth_hz) / 1000UL;
    u32 lo_khz = (g_saw_config.start_freq_hz -
                  g_saw_config.coarse_lo_guard_hz) / 1000UL;

    xil_printf("SAW scan: LO=%lu kHz range=%lu-%lu kHz step=%lu kHz points=%lu\r\n",
               (unsigned long)lo_khz,
               (unsigned long)start_khz, (unsigned long)stop_khz,
               (unsigned long)(g_saw_config.step_hz / 1000UL),
               (unsigned long)points);
    xil_printf("SAW frame: tx=%lu us g1=%lu us rx=%lu us g2=%lu us samples=%lu coh=%lu nc=%lu\r\n",
               (unsigned long)g_saw_config.tx_time_us,
               (unsigned long)g_saw_config.guard1_time_us,
               (unsigned long)g_saw_config.rx_time_us,
               (unsigned long)g_saw_config.guard2_time_us,
               (unsigned long)SAW_GetRxSampleCount(&g_saw_config),
               (unsigned long)g_saw_config.coherent_repetitions,
               (unsigned long)g_saw_config.noncoherent_segments);
}

void FPGA_Init(void)
{
    SAW_Config stored_config;
    u32 receiver_ipv4 = 0UL;
    u16 receiver_port = 0U;

    if (SAW_ConfigStore_Init() == SAW_CONTROL_OK) {
        if (SAW_ConfigStore_Load(&stored_config, &receiver_ipv4,
                                 &receiver_port) == SAW_CONTROL_OK) {
            g_saw_config = stored_config;
            xil_printf("SAW configuration loaded from eMMC\r\n");
        }
        else {
            xil_printf("SAW eMMC has no valid saved configuration; "
                       "using compiled defaults\r\n");
        }
    }

    TX_LO_FREQ = g_saw_config.start_freq_hz -
                 g_saw_config.coarse_lo_guard_hz;
    RX_LO_FREQ = TX_LO_FREQ;
    TX_RF_BANDWIDTH = SAW_GetRequiredRfBandwidth(&g_saw_config);
    RX_RF_BANDWIDTH = TX_RF_BANDWIDTH;
    TX_SAMPLING_FREQ = g_saw_config.tx_sampling_freq_hz;
    RX_SAMPLING_FREQ = g_saw_config.rx_sampling_freq_hz;

    if (AD9361_Init() < 0) {
        xil_printf("AD9361 initialization failed\r\n");
        return;
    }

    PL_Reset();

    /* �������BIST���� */
//    if (ad9361_bist_tone(ad9361_phy,
//                         BIST_INJ_RX,
//                       //  3840000,
//						 1920000,
//                         6,
//                         0) < 0) {
//        xil_printf("AD9361 RX BIST tone enable failed\r\n");
//        return;
//    }

    SAW_WritePlConfig(&g_saw_config);
    saw_config_revision = 1UL;
    SAW_PrintConfig();
}
