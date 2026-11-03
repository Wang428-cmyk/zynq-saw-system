#include <string.h>
#include "lwip/udp.h"
#include "xparameters.h"
#include "xil_printf.h"
#include "xuartps_hw.h"
#include "ad9361/ad9361_api.h"
#include "saw_control.h"
#include "serial_mode.h"

#define UDP_CONN_PORT             4001
#define SAW_UDP_MAX_PACKET        128
#define SAW_UDP_VERSION           0x02

#define SAW_UDP_CMD_CONFIG        0xE0
#define SAW_UDP_CMD_START         0xE1
#define SAW_UDP_CMD_STOP          0xE2
#define SAW_UDP_CMD_STATUS        0xE3
#define SAW_UDP_CMD_RESULTS       0xE4
#define SAW_UDP_CMD_NOTIFY        0xE5
#define SAW_UDP_CMD_FINE_RESULTS  0xE6
#define SAW_UDP_CMD_SUBSCRIBE     0xE7
#define SAW_UDP_CMD_RESULT_DATA   0xE8
#define SAW_UDP_CMD_RESULT_END    0xE9

#define SAW_UDP_CONFIG_WORDS      17
#define SAW_UDP_STATUS_WORDS      37
#define SAW_UDP_RESULT_MAX_POINTS 127
#define SAW_RESULT_MAX_POINTS     127
#define SAW_FINE_MAX_PEAKS        4
#define SAW_FINE_MAX_POINTS       1024
#define SAW_FINE_META_WORDS       11
#define SAW_STREAM_META_WORDS     12
#define SAW_STREAM_END_WORDS      7

typedef char SAW_ConfigWordCountCheck[
    (sizeof(SAW_Config) == (SAW_UDP_CONFIG_WORDS * sizeof(u32))) ? 1 : -1];

#define SAW_SCAN_STAGE_IDLE       0
#define SAW_SCAN_STAGE_COARSE     1
#define SAW_SCAN_STAGE_FINE       2
#define SAW_SCAN_STAGE_READY      3
#define SAW_SCAN_STAGE_ERROR      4

#define SAW_TFT_FRAME_HEADER_0    0xAAU
#define SAW_TFT_FRAME_HEADER_1    0x55U
#define SAW_TFT_WRITE_COMMAND     0xF1U
#define SAW_TFT_HEADER_ADDRESS    0x1000U
#define SAW_TFT_PEAK_ADDRESS      0x1010U
#define SAW_TFT_HEADER_WORDS      8U
#define SAW_TFT_WORDS_PER_PEAK    8U
#define SAW_TFT_PEAK_WORDS        \
    (SAW_FINE_MAX_PEAKS * SAW_TFT_WORDS_PER_PEAK)
#define SAW_TFT_DISPLAY_ADDRESS   0x1000U
#define SAW_TFT_DISPLAY_VALUES    8U
#define SAW_TFT_DISPLAY_WORDS     (SAW_TFT_DISPLAY_VALUES * 2U)
#define SAW_TFT_SG_HALF_WINDOW    5U
#define SAW_TFT_FIT_HALF_WINDOW   7U
#define SAW_TFT_SG_DIVISOR        429.0
#define SAW_TFT_FIT_MIN_R_SQUARED 0.70
#define SAW_TFT_FIT_MAX_OFFSET    3.0
#define SAW_TFT_MIN_PROMINENCE_DB 1.0
#define SAW_TFT_TRACK_ASSOC_HZ     30000.0
#define SAW_TFT_TRACK_JUMP_HZ       5000.0
#define SAW_TFT_TRACK_HISTORY      5U
#define SAW_TFT_TRACK_CONFIRM      3U
#define SAW_TFT_TRACK_ALPHA        0.25

typedef char SAW_TftFloatSizeCheck[(sizeof(float) == 4U) ? 1 : -1];

typedef struct {
    u32 coarse_index;
    u32 target_frequency_hz;
    u32 lo_frequency_hz;
    u32 start_frequency_hz;
    u32 step_hz;
    u32 point_count;
} SAW_FineResultMeta;

typedef struct {
    double frequency_hz;
    double power_dbfs;
} SAW_TftPeakCandidate;

typedef struct {
    u8 active;
    u32 missed_count;
    u32 history_count;
    u32 history_next;
    double frequency_history[SAW_TFT_TRACK_HISTORY];
    double power_history[SAW_TFT_TRACK_HISTORY];
    double output_frequency_hz;
    double output_power_dbfs;
    double pending_frequency_hz;
    u32 pending_count;
} SAW_TftPeakTrack;

extern int EMMC_Write(void);
extern u32 EMMC_Variable[4];
extern struct ad9361_rf_phy *ad9361_phy;

static struct udp_pcb *pcb;
static ip_addr_t saw_notify_addr;
static u16 saw_notify_port;
static u16 saw_notify_sequence;
static u8 saw_notify_valid;
static u8 saw_continuous_enabled;
static u32 saw_stream_packets_sent;
static u32 saw_stream_send_errors;
static u64 saw_result_snapshot[SAW_RESULT_MAX_POINTS];
static u32 saw_result_point_count;
static u8 saw_result_valid;
static u64 saw_fine_result_snapshot[SAW_FINE_MAX_PEAKS][SAW_FINE_MAX_POINTS];
static SAW_FineResultMeta saw_fine_meta[SAW_FINE_MAX_PEAKS];
static u32 saw_fine_peak_count;
static u32 saw_current_fine_peak;
static u32 saw_current_fine_chunk;
static u32 saw_fine_points_written;
static u32 saw_fine_chunk_points;
static u32 saw_scan_stage;
static u32 saw_result_generation;
static u16 saw_job_result;
static u64 saw_published_result_snapshot[SAW_RESULT_MAX_POINTS];
static u32 saw_published_result_point_count;
static u64 saw_published_fine_result_snapshot
    [SAW_FINE_MAX_PEAKS][SAW_FINE_MAX_POINTS];
static SAW_FineResultMeta saw_published_fine_meta[SAW_FINE_MAX_PEAKS];
static u32 saw_published_fine_peak_count;
static SAW_Config saw_published_config;
static u32 saw_published_config_revision;
static u32 saw_published_rx_samples_last;
static u8 saw_published_status[SAW_UDP_STATUS_WORDS * 4];

/*
 * Return log2(value) in signed Q16 format.  The fractional part uses
 * ln(x) = 2 * (z + z^3/3 + z^5/5 + ...), z = (x - 1)/(x + 1).
 * This keeps the UART power report independent of floating-point printf.
 */
static s32 saw_log2_q16(u64 value)
{
    const u64 one_q30 = 1ULL << 30;
    const u64 ln2_q30 = 744261118ULL;
    u64 probe = value;
    u64 mantissa_q30;
    u64 z_q30;
    u64 z2_q30;
    u64 term_q30;
    u64 sum_q30;
    u64 ln_q30;
    u32 integer_part = 0UL;

    if (value == 0ULL) {
        return (s32)0x80000000UL;
    }
    while (probe > 1ULL) {
        probe >>= 1;
        integer_part++;
    }

    if (integer_part >= 30UL) {
        mantissa_q30 = value >> (integer_part - 30UL);
    }
    else {
        mantissa_q30 = value << (30UL - integer_part);
    }

    z_q30 = ((mantissa_q30 - one_q30) << 30) /
            (mantissa_q30 + one_q30);
    z2_q30 = (z_q30 * z_q30) >> 30;
    term_q30 = z_q30;
    sum_q30 = term_q30;
    term_q30 = (term_q30 * z2_q30) >> 30;
    sum_q30 += term_q30 / 3ULL;
    term_q30 = (term_q30 * z2_q30) >> 30;
    sum_q30 += term_q30 / 5ULL;
    term_q30 = (term_q30 * z2_q30) >> 30;
    sum_q30 += term_q30 / 7ULL;
    term_q30 = (term_q30 * z2_q30) >> 30;
    sum_q30 += term_q30 / 9ULL;
    ln_q30 = sum_q30 * 2ULL;

    return (s32)((integer_part << 16) +
                 (u32)((ln_q30 << 16) / ln2_q30));
}

static s32 saw_energy_dbfs_centi(u64 energy)
{
    s64 log2_ratio_q16;
    s64 scaled;

    if ((energy == 0ULL) || (saw_published_rx_samples_last == 0UL) ||
        (saw_published_config.coherent_repetitions == 0UL) ||
        (saw_published_config.noncoherent_segments == 0UL)) {
        return (s32)0x80000000UL;
    }

    /*
     * Match the upper-computer normalization:
     * E / (rx_samples^2 * coherent * noncoherent * 2048^2).
     */
    log2_ratio_q16 =
        (s64)saw_log2_q16(energy) -
        (2LL * (s64)saw_log2_q16(saw_published_rx_samples_last)) -
        (s64)saw_log2_q16(
            saw_published_config.coherent_repetitions) -
        (s64)saw_log2_q16(
            saw_published_config.noncoherent_segments) -
        (22LL << 16);

    /* 10*log10(2) = 3.0102999566 dB per log2 unit. */
    scaled = log2_ratio_q16 * 30103LL;
    if (scaled >= 0LL) {
        scaled += 3276800LL;
    }
    else {
        scaled -= 3276800LL;
    }
    return (s32)(scaled / 6553600LL);
}

#if SAW_SERIAL_MODE == SAW_SERIAL_MODE_DEBUG
static void saw_uart_print_frequency(u32 frequency_hz)
{
    xil_printf("%lu.%06lu MHz",
               (unsigned long)(frequency_hz / 1000000UL),
               (unsigned long)(frequency_hz % 1000000UL));
}

static void saw_uart_print_dbfs(u64 energy)
{
    s32 centi_db = saw_energy_dbfs_centi(energy);
    u32 magnitude;

    if (centi_db == (s32)0x80000000UL) {
        xil_printf("-- dBFS");
        return;
    }
    if (centi_db < 0) {
        magnitude = (u32)(-(s64)centi_db);
        xil_printf("-%lu.%02lu dBFS",
                   (unsigned long)(magnitude / 100UL),
                   (unsigned long)(magnitude % 100UL));
    }
    else {
        magnitude = (u32)centi_db;
        xil_printf("%lu.%02lu dBFS",
                   (unsigned long)(magnitude / 100UL),
                   (unsigned long)(magnitude % 100UL));
    }
}

static void saw_uart_print_round_result(u16 result)
{
    SAW_FineResultMeta *meta;
    u32 peak_id;
    u32 fine_index;
    u32 fine_peak_index;
    u32 fine_peak_frequency;
    u64 coarse_energy;
    u64 fine_peak_energy;

    xil_printf("[SAW][ROUND %lu] result=%u coarse_points=%lu "
               "fine_peaks=%lu\r\n",
               (unsigned long)saw_result_generation, (unsigned int)result,
               (unsigned long)saw_published_result_point_count,
               (unsigned long)saw_published_fine_peak_count);
    if ((result != SAW_CONTROL_OK) || !saw_result_valid) {
        return;
    }
    if (saw_published_fine_peak_count == 0UL) {
        xil_printf("  no qualified coarse peaks / fine scan disabled\r\n");
        return;
    }

    for (peak_id = 0UL; peak_id < saw_published_fine_peak_count;
         peak_id++) {
        meta = &saw_published_fine_meta[peak_id];
        if ((meta->coarse_index >= saw_published_result_point_count) ||
            (meta->point_count == 0UL) ||
            (meta->point_count > SAW_FINE_MAX_POINTS)) {
            xil_printf("  P%lu invalid result metadata\r\n",
                       (unsigned long)(peak_id + 1UL));
            continue;
        }

        coarse_energy =
            saw_published_result_snapshot[meta->coarse_index];
        fine_peak_index = 0UL;
        fine_peak_energy =
            saw_published_fine_result_snapshot[peak_id][0];
        for (fine_index = 1UL; fine_index < meta->point_count;
             fine_index++) {
            if (saw_published_fine_result_snapshot[peak_id][fine_index] >
                fine_peak_energy) {
                fine_peak_energy =
                    saw_published_fine_result_snapshot[peak_id][fine_index];
                fine_peak_index = fine_index;
            }
        }
        fine_peak_frequency = meta->start_frequency_hz +
                              fine_peak_index * meta->step_hz;

        xil_printf("  P%lu coarse=",
                   (unsigned long)(peak_id + 1UL));
        saw_uart_print_frequency(meta->target_frequency_hz);
        xil_printf(" amp=");
        saw_uart_print_dbfs(coarse_energy);
        xil_printf(" | fine=");
        saw_uart_print_frequency(fine_peak_frequency);
        xil_printf(" amp=");
        saw_uart_print_dbfs(fine_peak_energy);
        xil_printf("\r\n");
    }
}
#else
static u16 saw_tft_crc16(const u8 *data, u32 length)
{
    u16 crc = 0xFFFFU;
    u32 index;
    u32 bit;

    for (index = 0UL; index < length; index++) {
        crc ^= data[index];
        for (bit = 0UL; bit < 8UL; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (u16)((crc >> 1) ^ 0xA001U);
            }
            else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static void saw_tft_send_bytes(const u8 *data, u32 length)
{
    u32 index;

    for (index = 0UL; index < length; index++) {
        XUartPs_SendByte(STDOUT_BASEADDRESS, data[index]);
    }
}

static void saw_tft_write_words(u16 address, const u16 *words,
                                u32 word_count)
{
    u8 frame[9U + (SAW_TFT_PEAK_WORDS * 2U)];
    u32 data_length;
    u32 frame_length;
    u32 index;
    u16 crc;

    if ((words == 0) || (word_count == 0UL) ||
        (word_count > SAW_TFT_PEAK_WORDS)) {
        return;
    }

    /*
     * DCBUS length covers function + address + data + CRC.  The CRC itself
     * covers station through the final data byte, excluding the AA55 header.
     */
    data_length = 5UL + word_count * 2UL;
    frame[0] = SAW_TFT_FRAME_HEADER_0;
    frame[1] = SAW_TFT_FRAME_HEADER_1;
    frame[2] = (u8)SAW_TFT_STATION;
    frame[3] = (u8)data_length;
    frame[4] = SAW_TFT_WRITE_COMMAND;
    frame[5] = (u8)(address >> 8);
    frame[6] = (u8)address;
    for (index = 0UL; index < word_count; index++) {
        frame[7UL + index * 2UL] = (u8)(words[index] >> 8);
        frame[8UL + index * 2UL] = (u8)words[index];
    }

#if SAW_TFT_CRC_ENABLED
    crc = saw_tft_crc16(&frame[2], data_length);
    frame[7UL + word_count * 2UL] = (u8)crc;
    frame[8UL + word_count * 2UL] = (u8)(crc >> 8);
#else
    crc = 0xCCCCU;
    frame[7UL + word_count * 2UL] = (u8)(crc >> 8);
    frame[8UL + word_count * 2UL] = (u8)crc;
#endif
    frame_length = 9UL + word_count * 2UL;
    saw_tft_send_bytes(frame, frame_length);
}

static u16 saw_tft_power_word(s32 centi_dbfs)
{
    if (centi_dbfs < -32767) {
        centi_dbfs = -32767;
    }
    else if (centi_dbfs > 32767) {
        centi_dbfs = 32767;
    }
    return (u16)((s16)centi_dbfs);
}

static void saw_tft_store_float(u16 *words, u32 word_offset, float value)
{
    u32 bits;

    memcpy(&bits, &value, sizeof(bits));
    words[word_offset] = (u16)(bits >> 16);
    words[word_offset + 1UL] = (u16)bits;
}

static double saw_tft_abs_double(double value)
{
    return (value < 0.0) ? -value : value;
}

static u8 saw_tft_energy_dbfs(u64 energy, double *power_dbfs)
{
    s64 log2_ratio_q16;

    if ((power_dbfs == 0) || (energy == 0ULL) ||
        (saw_published_rx_samples_last == 0UL) ||
        (saw_published_config.coherent_repetitions == 0UL) ||
        (saw_published_config.noncoherent_segments == 0UL)) {
        return 0U;
    }

    log2_ratio_q16 =
        (s64)saw_log2_q16(energy) -
        (2LL * (s64)saw_log2_q16(saw_published_rx_samples_last)) -
        (s64)saw_log2_q16(
            saw_published_config.coherent_repetitions) -
        (s64)saw_log2_q16(
            saw_published_config.noncoherent_segments) -
        (22LL << 16);
    *power_dbfs = (double)log2_ratio_q16 *
                  3.010299956639812 / 65536.0;
    return 1U;
}

static u8 saw_tft_solve_three_by_three(double matrix[3][3],
                                       double vector[3],
                                       double solution[3])
{
    double augmented[3][4];
    double temporary;
    double divisor;
    double factor;
    u32 row;
    u32 column;
    u32 pivot;
    u32 best;

    for (row = 0UL; row < 3UL; row++) {
        for (column = 0UL; column < 3UL; column++) {
            augmented[row][column] = matrix[row][column];
        }
        augmented[row][3] = vector[row];
    }

    for (pivot = 0UL; pivot < 3UL; pivot++) {
        best = pivot;
        for (row = pivot + 1UL; row < 3UL; row++) {
            if (saw_tft_abs_double(augmented[row][pivot]) >
                saw_tft_abs_double(augmented[best][pivot])) {
                best = row;
            }
        }
        if (saw_tft_abs_double(augmented[best][pivot]) < 1.0e-12) {
            return 0U;
        }
        if (best != pivot) {
            for (column = 0UL; column < 4UL; column++) {
                temporary = augmented[pivot][column];
                augmented[pivot][column] = augmented[best][column];
                augmented[best][column] = temporary;
            }
        }

        divisor = augmented[pivot][pivot];
        for (column = 0UL; column < 4UL; column++) {
            augmented[pivot][column] /= divisor;
        }
        for (row = 0UL; row < 3UL; row++) {
            if (row == pivot) {
                continue;
            }
            factor = augmented[row][pivot];
            for (column = 0UL; column < 4UL; column++) {
                augmented[row][column] -=
                    factor * augmented[pivot][column];
            }
        }
    }

    for (row = 0UL; row < 3UL; row++) {
        solution[row] = augmented[row][3];
    }
    return 1U;
}

/*
 * Refine one fine-scan peak for the UART stability tracker.  The curve uses
 * the same SG(11) + local quadratic fit as the upper computer, followed by
 * stricter quality gates so a nearly flat/noisy curve does not create a
 * random serial-output frequency.
 */
static u8 saw_tft_refine_fine_peak(
    const SAW_FineResultMeta *meta, const u64 *energies,
    double *frequency_hz, double *power_dbfs)
{
    static const s32 sg_coefficients[11] = {
        -36, 9, 44, 69, 84, 89, 84, 69, 44, 9, -36
    };
    static double raw[SAW_FINE_MAX_POINTS];
    static double smoothed[SAW_FINE_MAX_POINTS];
    static u8 valid[SAW_FINE_MAX_POINTS];
    double matrix[3][3];
    double vector[3];
    double solved[3];
    double sums[5];
    double y0 = 0.0;
    double y1 = 0.0;
    double y2 = 0.0;
    double x;
    double y;
    double prediction;
    double mean_y;
    double total_error = 0.0;
    double residual_error = 0.0;
    double r_squared = 0.0;
    double vertex;
    double fitted_position;
    double fitted_power;
    double left_edge_sum = 0.0;
    double right_edge_sum = 0.0;
    double edge_reference;
    double prominence;
    double weighted_sum;
    u32 valid_count = 0UL;
    u32 smooth_peak = 0UL;
    u32 start;
    u32 stop;
    u32 sample_count = 0UL;
    u32 index;
    u32 window_index;
    u32 source_index;
    u32 power;
    u32 edge_count;
    u32 left_edge_count = 0UL;
    u32 right_edge_count = 0UL;
    u8 window_valid;
    u8 fit_valid = 0U;

    if ((meta == 0) || (energies == 0) || (frequency_hz == 0) ||
        (power_dbfs == 0) || (meta->point_count == 0UL) ||
        (meta->point_count > SAW_FINE_MAX_POINTS)) {
        return 0U;
    }

    for (index = 0UL; index < meta->point_count; index++) {
        raw[index] = 0.0;
        valid[index] = saw_tft_energy_dbfs(energies[index], &raw[index]);
        smoothed[index] = valid[index] ? raw[index] : 0.0;
        if (valid[index]) {
            if ((valid_count == 0UL) ||
                (raw[index] > raw[smooth_peak])) {
                smooth_peak = index;
            }
            valid_count++;
        }
    }
    if (valid_count == 0UL) {
        return 0U;
    }

    if (valid_count >= 7UL) {
        for (index = SAW_TFT_SG_HALF_WINDOW;
             index + SAW_TFT_SG_HALF_WINDOW < meta->point_count;
             index++) {
            weighted_sum = 0.0;
            window_valid = 1U;
            for (window_index = 0UL; window_index < 11UL;
                 window_index++) {
                source_index = index - SAW_TFT_SG_HALF_WINDOW +
                               window_index;
                if (!valid[source_index]) {
                    window_valid = 0U;
                    break;
                }
                weighted_sum += (double)sg_coefficients[window_index] *
                                raw[source_index];
            }
            if (window_valid) {
                smoothed[index] = weighted_sum / SAW_TFT_SG_DIVISOR;
            }
        }

        for (index = 0UL; index < meta->point_count; index++) {
            if (valid[index] && smoothed[index] > smoothed[smooth_peak]) {
                smooth_peak = index;
            }
        }

        start = (smooth_peak > SAW_TFT_FIT_HALF_WINDOW) ?
                smooth_peak - SAW_TFT_FIT_HALF_WINDOW : 0UL;
        stop = smooth_peak + SAW_TFT_FIT_HALF_WINDOW + 1UL;
        if (stop > meta->point_count) {
            stop = meta->point_count;
        }
        for (power = 0UL; power < 5UL; power++) {
            sums[power] = 0.0;
        }
        for (index = start; index < stop; index++) {
            if (!valid[index]) {
                continue;
            }
            x = (double)((s32)index - (s32)smooth_peak);
            y = smoothed[index];
            sums[0] += 1.0;
            sums[1] += x;
            sums[2] += x * x;
            sums[3] += x * x * x;
            sums[4] += x * x * x * x;
            y0 += y;
            y1 += x * y;
            y2 += x * x * y;
            sample_count++;
        }

        matrix[0][0] = sums[4];
        matrix[0][1] = sums[3];
        matrix[0][2] = sums[2];
        matrix[1][0] = sums[3];
        matrix[1][1] = sums[2];
        matrix[1][2] = sums[1];
        matrix[2][0] = sums[2];
        matrix[2][1] = sums[1];
        matrix[2][2] = sums[0];
        vector[0] = y2;
        vector[1] = y1;
        vector[2] = y0;

        if ((sample_count != 0UL) &&
            saw_tft_solve_three_by_three(matrix, vector, solved)) {
            mean_y = y0 / (double)sample_count;
            for (index = start; index < stop; index++) {
                if (!valid[index]) {
                    continue;
                }
                x = (double)((s32)index - (s32)smooth_peak);
                y = smoothed[index];
                prediction = solved[0] * x * x +
                             solved[1] * x + solved[2];
                total_error += (y - mean_y) * (y - mean_y);
                residual_error += (y - prediction) * (y - prediction);
            }
            if (total_error > 1.0e-12) {
                r_squared = 1.0 - residual_error / total_error;
            }
            else {
                r_squared = 1.0;
            }
            if (saw_tft_abs_double(solved[0]) > 1.0e-12) {
                vertex = -solved[1] / (2.0 * solved[0]);
                if ((solved[0] < 0.0) &&
                    (vertex >= -SAW_TFT_FIT_MAX_OFFSET) &&
                    (vertex <= SAW_TFT_FIT_MAX_OFFSET) &&
                    (r_squared >= SAW_TFT_FIT_MIN_R_SQUARED)) {
                    fit_valid = 1U;
                }
            }
        }
    }

    fitted_position = (double)smooth_peak;
    fitted_power = smoothed[smooth_peak];
    if (fit_valid) {
        fitted_position += vertex;
        fitted_power = solved[0] * vertex * vertex +
                       solved[1] * vertex + solved[2];
    }
    if (!fit_valid) {
        return 0U;
    }

    edge_count = (meta->point_count < 6UL) ?
                 (meta->point_count / 2UL) : 3UL;
    for (index = 0UL; index < edge_count; index++) {
        if (valid[index]) {
            left_edge_sum += smoothed[index];
            left_edge_count++;
        }
        source_index = meta->point_count - 1UL - index;
        if (valid[source_index]) {
            right_edge_sum += smoothed[source_index];
            right_edge_count++;
        }
    }
    if ((left_edge_count == 0UL) || (right_edge_count == 0UL)) {
        return 0U;
    }
    left_edge_sum /= (double)left_edge_count;
    right_edge_sum /= (double)right_edge_count;
    edge_reference = (left_edge_sum > right_edge_sum) ?
                     left_edge_sum : right_edge_sum;
    prominence = fitted_power - edge_reference;
    if (prominence < SAW_TFT_MIN_PROMINENCE_DB) {
        return 0U;
    }

    *frequency_hz = (double)meta->start_frequency_hz +
                    fitted_position * (double)meta->step_hz;
    *power_dbfs = fitted_power;
    return 1U;
}

static SAW_TftPeakTrack saw_tft_tracks[SAW_FINE_MAX_PEAKS];
static u32 saw_tft_track_config_revision;
static u8 saw_tft_track_revision_valid;

static void saw_tft_reset_tracks(void)
{
    memset(saw_tft_tracks, 0, sizeof(saw_tft_tracks));
}

static double saw_tft_median(const double *values, u32 count)
{
    double sorted[SAW_TFT_TRACK_HISTORY];
    double value;
    u32 index;
    u32 position;

    if ((values == 0) || (count == 0UL) ||
        (count > SAW_TFT_TRACK_HISTORY)) {
        return 0.0;
    }
    for (index = 0UL; index < count; index++) {
        value = values[index];
        position = index;
        while ((position > 0UL) && (sorted[position - 1UL] > value)) {
            sorted[position] = sorted[position - 1UL];
            position--;
        }
        sorted[position] = value;
    }
    if ((count & 1UL) != 0UL) {
        return sorted[count / 2UL];
    }
    return 0.5 * (sorted[count / 2UL - 1UL] +
                  sorted[count / 2UL]);
}

static void saw_tft_initialize_track(SAW_TftPeakTrack *track,
                                     const SAW_TftPeakCandidate *candidate)
{
    memset(track, 0, sizeof(*track));
    track->active = 1U;
    track->history_count = 1UL;
    track->history_next = 1UL;
    track->frequency_history[0] = candidate->frequency_hz;
    track->power_history[0] = candidate->power_dbfs;
    track->output_frequency_hz = candidate->frequency_hz;
    track->output_power_dbfs = candidate->power_dbfs;
}

static void saw_tft_update_track(SAW_TftPeakTrack *track,
                                 const SAW_TftPeakCandidate *candidate)
{
    double median_frequency;
    double median_power;
    double difference;

    track->frequency_history[track->history_next] =
        candidate->frequency_hz;
    track->power_history[track->history_next] = candidate->power_dbfs;
    track->history_next =
        (track->history_next + 1UL) % SAW_TFT_TRACK_HISTORY;
    if (track->history_count < SAW_TFT_TRACK_HISTORY) {
        track->history_count++;
    }
    track->missed_count = 0UL;

    median_frequency = saw_tft_median(track->frequency_history,
                                      track->history_count);
    median_power = saw_tft_median(track->power_history,
                                  track->history_count);
    difference = saw_tft_abs_double(
        median_frequency - track->output_frequency_hz);

    if (difference <= SAW_TFT_TRACK_JUMP_HZ) {
        track->output_frequency_hz += SAW_TFT_TRACK_ALPHA *
            (median_frequency - track->output_frequency_hz);
        track->output_power_dbfs += SAW_TFT_TRACK_ALPHA *
            (median_power - track->output_power_dbfs);
        track->pending_count = 0UL;
        return;
    }

    if ((track->pending_count != 0UL) &&
        (saw_tft_abs_double(median_frequency -
                            track->pending_frequency_hz) <=
         SAW_TFT_TRACK_JUMP_HZ)) {
        track->pending_count++;
        track->pending_frequency_hz = median_frequency;
    }
    else {
        track->pending_frequency_hz = median_frequency;
        track->pending_count = 1UL;
    }

    if (track->pending_count >= SAW_TFT_TRACK_CONFIRM) {
        track->output_frequency_hz = median_frequency;
        track->output_power_dbfs = median_power;
        track->pending_count = 0UL;
    }
}

static void saw_tft_update_tracks(const SAW_TftPeakCandidate *candidates,
                                  u32 candidate_count)
{
    u8 candidate_used[SAW_FINE_MAX_PEAKS] = {0U, 0U, 0U, 0U};
    u8 track_matched[SAW_FINE_MAX_PEAKS] = {0U, 0U, 0U, 0U};
    u32 matched_candidate[SAW_FINE_MAX_PEAKS];
    u32 track_index;
    u32 candidate_index;
    u32 best_candidate;
    double distance;
    double best_distance;
    u8 near_active_track;

    for (track_index = 0UL; track_index < SAW_FINE_MAX_PEAKS;
         track_index++) {
        matched_candidate[track_index] = SAW_FINE_MAX_PEAKS;
        if (!saw_tft_tracks[track_index].active) {
            continue;
        }
        best_candidate = SAW_FINE_MAX_PEAKS;
        best_distance = SAW_TFT_TRACK_ASSOC_HZ + 1.0;
        for (candidate_index = 0UL; candidate_index < candidate_count;
             candidate_index++) {
            if (candidate_used[candidate_index]) {
                continue;
            }
            distance = saw_tft_abs_double(
                candidates[candidate_index].frequency_hz -
                saw_tft_tracks[track_index].output_frequency_hz);
            if ((distance <= SAW_TFT_TRACK_ASSOC_HZ) &&
                (distance < best_distance)) {
                best_distance = distance;
                best_candidate = candidate_index;
            }
        }
        if (best_candidate < candidate_count) {
            candidate_used[best_candidate] = 1U;
            track_matched[track_index] = 1U;
            matched_candidate[track_index] = best_candidate;
        }
    }

    for (track_index = 0UL; track_index < SAW_FINE_MAX_PEAKS;
         track_index++) {
        if (!saw_tft_tracks[track_index].active) {
            continue;
        }
        if (track_matched[track_index]) {
            saw_tft_update_track(
                &saw_tft_tracks[track_index],
                &candidates[matched_candidate[track_index]]);
        }
        else {
            saw_tft_tracks[track_index].missed_count++;
            if (saw_tft_tracks[track_index].missed_count >=
                SAW_TFT_TRACK_CONFIRM) {
                memset(&saw_tft_tracks[track_index], 0,
                       sizeof(saw_tft_tracks[track_index]));
            }
        }
    }

    for (candidate_index = 0UL; candidate_index < candidate_count;
         candidate_index++) {
        if (candidate_used[candidate_index]) {
            continue;
        }
        near_active_track = 0U;
        for (track_index = 0UL; track_index < SAW_FINE_MAX_PEAKS;
             track_index++) {
            if (saw_tft_tracks[track_index].active &&
                !track_matched[track_index] &&
                (saw_tft_abs_double(
                    candidates[candidate_index].frequency_hz -
                    saw_tft_tracks[track_index].output_frequency_hz) <=
                 2.0 * SAW_TFT_TRACK_ASSOC_HZ)) {
                near_active_track = 1U;
                break;
            }
        }
        if (near_active_track) {
            continue;
        }
        for (track_index = 0UL; track_index < SAW_FINE_MAX_PEAKS;
             track_index++) {
            if (!saw_tft_tracks[track_index].active) {
                saw_tft_initialize_track(
                    &saw_tft_tracks[track_index],
                    &candidates[candidate_index]);
                track_matched[track_index] = 1U;
                break;
            }
        }
    }
}

/*
 * Current display frame, starting at DCBUS address 0x1000:
 *   float frequency_khz[4];
 *   float amplitude_dbfs[4];
 *
 * Each IEEE-754 float is transferred most-significant byte first.  Missing
 * peak slots remain 0.0.  Peak order follows the coarse detector's ascending
 * frequency order.
 */
static void saw_tft_publish_display_round(u16 result)
{
    u16 display_words[SAW_TFT_DISPLAY_WORDS];
    SAW_TftPeakCandidate candidates[SAW_FINE_MAX_PEAKS];
    SAW_FineResultMeta *meta;
    u32 track_order[SAW_FINE_MAX_PEAKS];
    u32 source_peak;
    u32 candidate_count = 0UL;
    u32 output_count = 0UL;
    u32 track_index;
    u32 position;
    u32 selected_track;
    double fitted_frequency_hz;
    double fitted_power_dbfs;
    float frequency_khz;
    float amplitude_dbfs;

    memset(display_words, 0, sizeof(display_words));

    if ((result == SAW_CONTROL_OK) && saw_result_valid) {
        if (!saw_tft_track_revision_valid ||
            (saw_tft_track_config_revision !=
             saw_published_config_revision)) {
            saw_tft_reset_tracks();
            saw_tft_track_config_revision =
                saw_published_config_revision;
            saw_tft_track_revision_valid = 1U;
        }
        for (source_peak = 0UL;
             (source_peak < saw_published_fine_peak_count) &&
             (candidate_count < SAW_FINE_MAX_PEAKS);
             source_peak++) {
            meta = &saw_published_fine_meta[source_peak];
            if ((meta->coarse_index >= saw_published_result_point_count) ||
                (meta->point_count == 0UL) ||
                (meta->point_count > SAW_FINE_MAX_POINTS)) {
                continue;
            }

            if (!saw_tft_refine_fine_peak(
                    meta,
                    saw_published_fine_result_snapshot[source_peak],
                    &fitted_frequency_hz, &fitted_power_dbfs)) {
                continue;
            }

            candidates[candidate_count].frequency_hz =
                fitted_frequency_hz;
            candidates[candidate_count].power_dbfs =
                fitted_power_dbfs;
            candidate_count++;
        }

        saw_tft_update_tracks(candidates, candidate_count);

        for (track_index = 0UL; track_index < SAW_FINE_MAX_PEAKS;
             track_index++) {
            if (!saw_tft_tracks[track_index].active) {
                continue;
            }
            selected_track = track_index;
            position = output_count;
            while ((position > 0UL) &&
                   (saw_tft_tracks[track_order[position - 1UL]]
                        .output_frequency_hz >
                    saw_tft_tracks[selected_track]
                        .output_frequency_hz)) {
                track_order[position] = track_order[position - 1UL];
                position--;
            }
            track_order[position] = selected_track;
            output_count++;
        }

        for (position = 0UL; position < output_count; position++) {
            track_index = track_order[position];
            frequency_khz = (float)(
                saw_tft_tracks[track_index].output_frequency_hz / 1000.0);
            amplitude_dbfs = (float)
                saw_tft_tracks[track_index].output_power_dbfs;

            saw_tft_store_float(display_words,
                                position * 2UL,
                                frequency_khz);
            saw_tft_store_float(display_words,
                                (SAW_FINE_MAX_PEAKS * 2UL) +
                                    position * 2UL,
                                amplitude_dbfs);
        }
    }

    saw_tft_write_words(SAW_TFT_DISPLAY_ADDRESS, display_words,
                        SAW_TFT_DISPLAY_WORDS);
}

/* Original two-frame DCBUS layout retained for optional compatibility. */
static void saw_tft_publish_round(u16 result)
{
    u16 header_words[SAW_TFT_HEADER_WORDS];
    u16 peak_words[SAW_TFT_PEAK_WORDS];
    SAW_FineResultMeta *meta;
    u32 source_peak;
    u32 output_peak = 0UL;
    u32 base;
    u32 fine_index;
    u32 fine_peak_index;
    u32 coarse_frequency_khz;
    u32 fine_frequency_hz;
    u32 fine_frequency_khz;
    u64 coarse_energy;
    u64 fine_peak_energy;
    s32 coarse_power;
    s32 fine_power;

    memset(header_words, 0, sizeof(header_words));
    memset(peak_words, 0, sizeof(peak_words));

    if ((result == SAW_CONTROL_OK) && saw_result_valid) {
        for (source_peak = 0UL;
             (source_peak < saw_published_fine_peak_count) &&
             (output_peak < SAW_FINE_MAX_PEAKS);
             source_peak++) {
            meta = &saw_published_fine_meta[source_peak];
            if ((meta->coarse_index >= saw_published_result_point_count) ||
                (meta->point_count == 0UL) ||
                (meta->point_count > SAW_FINE_MAX_POINTS)) {
                continue;
            }

            coarse_energy =
                saw_published_result_snapshot[meta->coarse_index];
            fine_peak_index = 0UL;
            fine_peak_energy =
                saw_published_fine_result_snapshot[source_peak][0];
            for (fine_index = 1UL; fine_index < meta->point_count;
                 fine_index++) {
                if (saw_published_fine_result_snapshot
                        [source_peak][fine_index] > fine_peak_energy) {
                    fine_peak_energy =
                        saw_published_fine_result_snapshot
                            [source_peak][fine_index];
                    fine_peak_index = fine_index;
                }
            }

            coarse_power = saw_energy_dbfs_centi(coarse_energy);
            fine_power = saw_energy_dbfs_centi(fine_peak_energy);
            if ((coarse_power == (s32)0x80000000UL) ||
                (fine_power == (s32)0x80000000UL)) {
                continue;
            }

            coarse_frequency_khz =
                (meta->target_frequency_hz + 500UL) / 1000UL;
            fine_frequency_hz =
                meta->start_frequency_hz +
                fine_peak_index * meta->step_hz;
            fine_frequency_khz =
                (fine_frequency_hz + 500UL) / 1000UL;

            base = output_peak * SAW_TFT_WORDS_PER_PEAK;
            peak_words[base + 0UL] = 1U;
            peak_words[base + 1UL] =
                (u16)(coarse_frequency_khz >> 16);
            peak_words[base + 2UL] = (u16)coarse_frequency_khz;
            peak_words[base + 3UL] =
                saw_tft_power_word(coarse_power);
            peak_words[base + 4UL] =
                (u16)(fine_frequency_khz >> 16);
            peak_words[base + 5UL] = (u16)fine_frequency_khz;
            peak_words[base + 6UL] =
                saw_tft_power_word(fine_power);
            peak_words[base + 7UL] = 0U;
            output_peak++;
        }
    }

    /*
     * Send all four peak slots first, including zeroed unused slots.  Commit
     * the new round by sending the header block last.
     */
    saw_tft_write_words(SAW_TFT_PEAK_ADDRESS, peak_words,
                        SAW_TFT_PEAK_WORDS);

    header_words[0] = 0x0100U;
    header_words[1] = (u16)(saw_result_generation >> 16);
    header_words[2] = (u16)saw_result_generation;
    header_words[3] = result;
    header_words[4] = (u16)output_peak;
    header_words[5] = 0U; /* Amplitude unit: 0 = dBFS. */
    header_words[6] = (u16)(saw_published_config.fine_enabled ? 1U : 0U);
    header_words[7] = (u16)saw_result_generation;
    saw_tft_write_words(SAW_TFT_HEADER_ADDRESS, header_words,
                        SAW_TFT_HEADER_WORDS);
}
#endif

static u16 read_be16(const u8 *data)
{
    return ((u16)data[0] << 8) | (u16)data[1];
}

static u32 read_be32(const u8 *data)
{
    return ((u32)data[0] << 24) | ((u32)data[1] << 16) |
           ((u32)data[2] << 8) | (u32)data[3];
}

static void write_be16(u8 *data, u16 value)
{
    data[0] = (u8)(value >> 8);
    data[1] = (u8)value;
}

static void write_be32(u8 *data, u32 value)
{
    data[0] = (u8)(value >> 24);
    data[1] = (u8)(value >> 16);
    data[2] = (u8)(value >> 8);
    data[3] = (u8)value;
}

static u32 saw_crc32(const u8 *data, u16 length)
{
    u16 i;
    u8 bit;
    u32 crc = 0xFFFFFFFFUL;

    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc >> 1) ^ ((crc & 1UL) ? 0xEDB88320UL : 0UL);
        }
    }
    return ~crc;
}

static err_t send_udp_bytes(struct udp_pcb *upcb, const ip_addr_t *addr,
                            u16 port, const u8 *data, u16 length)
{
    struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, length, PBUF_RAM);
    err_t result = ERR_MEM;

    if (reply == 0) {
        return ERR_MEM;
    }
    if (pbuf_take(reply, data, length) == ERR_OK) {
        result = udp_sendto(upcb, reply, addr, port);
    }
    pbuf_free(reply);
    return result;
}

static void fill_status_payload(u8 *payload)
{
    SAW_RuntimeStatus runtime;
    u32 values[SAW_UDP_STATUS_WORDS];
    const u32 *config_values = (const u32 *)&g_saw_config;
    u32 status_index;
    u32 i;

    SAW_GetRuntimeStatus(&runtime);
    values[0] = runtime.pl_status;
    values[1] = runtime.pl_debug;
    values[2] = runtime.current_freq_index;
    values[3] = runtime.config_revision;
    values[4] = runtime.point_count;
    for (i = 0UL; i < SAW_UDP_CONFIG_WORDS; i++) {
        values[5UL + i] = config_values[i];
    }
    status_index = 5UL + SAW_UDP_CONFIG_WORDS;
    values[status_index++] = runtime.rx_samples_last;
    values[status_index++] = runtime.rx_capture_depth;
    values[status_index++] = saw_scan_stage;
    values[status_index++] = saw_published_fine_peak_count;
    values[status_index++] = saw_current_fine_peak;
    values[status_index++] = saw_current_fine_chunk;
    values[status_index++] = saw_result_generation;
    values[status_index++] = saw_continuous_enabled;
    SAW_ConfigStore_GetStatus(&values[status_index],
                              &values[status_index + 1UL],
                              &values[status_index + 2UL],
                              &values[status_index + 6UL]);
    status_index += 3UL;
    values[status_index++] = saw_notify_valid;
    values[status_index++] = saw_stream_packets_sent;
    values[status_index++] = saw_stream_send_errors;

    for (i = 0; i < SAW_UDP_STATUS_WORDS; i++) {
        write_be32(payload + (i * 4UL), values[i]);
    }
}

static void send_saw_reply(struct udp_pcb *upcb, const ip_addr_t *addr,
                           u16 port, u8 command, u16 sequence, u16 result)
{
    u8 reply[10 + (SAW_UDP_STATUS_WORDS * 4) + 4];
    const u16 payload_length = SAW_UDP_STATUS_WORDS * 4;
    const u16 crc_offset = 10 + payload_length;

    reply[0] = 0x7F;
    reply[1] = 0x4E;
    reply[2] = SAW_UDP_VERSION;
    reply[3] = command;
    write_be16(reply + 4, sequence);
    write_be16(reply + 6, result);
    write_be16(reply + 8, payload_length);
    fill_status_payload(reply + 10);
    write_be32(reply + crc_offset, saw_crc32(reply, crc_offset));
    send_udp_bytes(upcb, addr, port, reply, crc_offset + 4);
}

static void send_saw_results_reply(struct udp_pcb *upcb,
                                   const ip_addr_t *addr, u16 port,
                                   u16 sequence, u32 start_index,
                                   u32 requested_points)
{
    u8 reply[10 + 12 + (SAW_UDP_RESULT_MAX_POINTS * 8) + 4];
    u16 result = SAW_CONTROL_OK;
    u16 payload_length;
    u16 crc_offset;
    u32 returned_points = 0;
    u32 available_points;
    u32 i;
    if (!saw_result_valid) {
        result = SAW_CONTROL_ERR_BUSY;
    }
    else if ((requested_points == 0UL) ||
        (requested_points > SAW_UDP_RESULT_MAX_POINTS) ||
        (start_index >= saw_published_result_point_count)) {
        result = SAW_CONTROL_ERR_ARGUMENT;
    }

    if (result == SAW_CONTROL_OK) {
        available_points = saw_published_result_point_count - start_index;
        returned_points = (requested_points < available_points) ?
                          requested_points : available_points;
    }

    payload_length = (u16)(12UL + (returned_points * 8UL));
    crc_offset = (u16)(10U + payload_length);
    reply[0] = 0x7F;
    reply[1] = 0x4E;
    reply[2] = SAW_UDP_VERSION;
    reply[3] = SAW_UDP_CMD_RESULTS;
    write_be16(reply + 4, sequence);
    write_be16(reply + 6, result);
    write_be16(reply + 8, payload_length);
    write_be32(reply + 10, saw_published_result_point_count);
    write_be32(reply + 14, start_index);
    write_be32(reply + 18, returned_points);

    for (i = 0; i < returned_points; i++) {
        write_be32(reply + 22 + (i * 8UL),
                   (u32)(saw_published_result_snapshot[start_index + i]
                         >> 32));
        write_be32(reply + 26 + (i * 8UL),
                   (u32)saw_published_result_snapshot[start_index + i]);
    }

    write_be32(reply + crc_offset, saw_crc32(reply, crc_offset));
    send_udp_bytes(upcb, addr, port, reply, crc_offset + 4);
}

static void send_saw_fine_results_reply(struct udp_pcb *upcb,
                                        const ip_addr_t *addr, u16 port,
                                        u16 sequence, u32 peak_id,
                                        u32 start_index,
                                        u32 requested_points)
{
    u8 reply[10 + (SAW_FINE_META_WORDS * 4) +
             (SAW_UDP_RESULT_MAX_POINTS * 8) + 4];
    u16 result = SAW_CONTROL_OK;
    u16 payload_length;
    u16 crc_offset;
    u32 returned_points = 0;
    u32 available_points = 0;
    u32 i;
    SAW_FineResultMeta *meta = 0;

    if (!saw_result_valid) {
        result = SAW_CONTROL_ERR_BUSY;
    }
    else if ((peak_id >= saw_published_fine_peak_count) ||
             (requested_points == 0UL) ||
             (requested_points > SAW_UDP_RESULT_MAX_POINTS)) {
        result = SAW_CONTROL_ERR_ARGUMENT;
    }
    else {
        meta = &saw_published_fine_meta[peak_id];
        if (start_index >= meta->point_count) {
            result = SAW_CONTROL_ERR_ARGUMENT;
        }
        else {
            available_points = meta->point_count - start_index;
            returned_points = (requested_points < available_points) ?
                              requested_points : available_points;
        }
    }

    payload_length = (u16)((SAW_FINE_META_WORDS * 4UL) +
                           (returned_points * 8UL));
    crc_offset = (u16)(10U + payload_length);
    reply[0] = 0x7F;
    reply[1] = 0x4E;
    reply[2] = SAW_UDP_VERSION;
    reply[3] = SAW_UDP_CMD_FINE_RESULTS;
    write_be16(reply + 4, sequence);
    write_be16(reply + 6, result);
    write_be16(reply + 8, payload_length);
    write_be32(reply + 10, saw_result_generation);
    write_be32(reply + 14, saw_published_fine_peak_count);
    write_be32(reply + 18, peak_id);
    write_be32(reply + 22, (meta != 0) ? meta->coarse_index : 0UL);
    write_be32(reply + 26,
               (meta != 0) ? meta->target_frequency_hz : 0UL);
    write_be32(reply + 30,
               (meta != 0) ? meta->lo_frequency_hz : 0UL);
    write_be32(reply + 34,
               (meta != 0) ? meta->start_frequency_hz : 0UL);
    write_be32(reply + 38, (meta != 0) ? meta->step_hz : 0UL);
    write_be32(reply + 42, (meta != 0) ? meta->point_count : 0UL);
    write_be32(reply + 46, start_index);
    write_be32(reply + 50, returned_points);

    for (i = 0; i < returned_points; i++) {
        write_be32(reply + 54 + (i * 8UL),
                   (u32)(saw_published_fine_result_snapshot
                         [peak_id][start_index + i] >> 32));
        write_be32(reply + 58 + (i * 8UL),
                   (u32)saw_published_fine_result_snapshot
                         [peak_id][start_index + i]);
    }

    write_be32(reply + crc_offset, saw_crc32(reply, crc_offset));
    send_udp_bytes(upcb, addr, port, reply, crc_offset + 4);
}

static void saw_stream_account(err_t result)
{
    if (result == ERR_OK) {
        saw_stream_packets_sent++;
    }
    else {
        saw_stream_send_errors++;
    }
}

static void saw_send_stream_data(u32 scan_type, u32 peak_id,
                                 const SAW_FineResultMeta *meta,
                                 const u64 *energies, u32 total_points,
                                 u32 start_index, u32 returned_points)
{
    u8 packet[10 + (SAW_STREAM_META_WORDS * 4) +
              (SAW_UDP_RESULT_MAX_POINTS * 8) + 4];
    u16 payload_length =
        (u16)((SAW_STREAM_META_WORDS * 4UL) + returned_points * 8UL);
    u16 crc_offset = (u16)(10U + payload_length);
    u32 index;
    u32 coarse_index = 0xFFFFFFFFUL;
    u32 target_frequency = 0UL;
    u32 lo_frequency = saw_published_config.start_freq_hz -
                       saw_published_config.coarse_lo_guard_hz;
    u32 start_frequency = saw_published_config.start_freq_hz;
    u32 step_hz = saw_published_config.step_hz;

    if (meta != 0) {
        coarse_index = meta->coarse_index;
        target_frequency = meta->target_frequency_hz;
        lo_frequency = meta->lo_frequency_hz;
        start_frequency = meta->start_frequency_hz;
        step_hz = meta->step_hz;
    }

    packet[0] = 0x7F;
    packet[1] = 0x4E;
    packet[2] = SAW_UDP_VERSION;
    packet[3] = SAW_UDP_CMD_RESULT_DATA;
    write_be16(packet + 4, ++saw_notify_sequence);
    write_be16(packet + 6, SAW_CONTROL_OK);
    write_be16(packet + 8, payload_length);
    write_be32(packet + 10, saw_result_generation);
    write_be32(packet + 14, saw_published_config_revision);
    write_be32(packet + 18, scan_type);
    write_be32(packet + 22, peak_id);
    write_be32(packet + 26, coarse_index);
    write_be32(packet + 30, target_frequency);
    write_be32(packet + 34, lo_frequency);
    write_be32(packet + 38, start_frequency);
    write_be32(packet + 42, step_hz);
    write_be32(packet + 46, total_points);
    write_be32(packet + 50, start_index);
    write_be32(packet + 54, returned_points);
    for (index = 0UL; index < returned_points; index++) {
        write_be32(packet + 58 + index * 8UL,
                   (u32)(energies[start_index + index] >> 32));
        write_be32(packet + 62 + index * 8UL,
                   (u32)energies[start_index + index]);
    }
    write_be32(packet + crc_offset, saw_crc32(packet, crc_offset));
    saw_stream_account(send_udp_bytes(pcb, &saw_notify_addr,
                                      saw_notify_port, packet,
                                      (u16)(crc_offset + 4U)));
}

static u32 saw_stream_packet_count(void)
{
    u32 count;
    u32 peak_id;

    count = (saw_published_result_point_count +
             SAW_UDP_RESULT_MAX_POINTS - 1UL) /
            SAW_UDP_RESULT_MAX_POINTS;
    for (peak_id = 0UL; peak_id < saw_published_fine_peak_count;
         peak_id++) {
        count += (saw_published_fine_meta[peak_id].point_count +
                  SAW_UDP_RESULT_MAX_POINTS - 1UL) /
                 SAW_UDP_RESULT_MAX_POINTS;
    }
    return count;
}

static void saw_send_stream_end(u16 result, u32 packet_count)
{
    u8 packet[10 + (SAW_STREAM_END_WORDS * 4) +
              (SAW_UDP_STATUS_WORDS * 4) + 4];
    u16 payload_length =
        (u16)((SAW_STREAM_END_WORDS + SAW_UDP_STATUS_WORDS) * 4UL);
    u16 crc_offset = (u16)(10U + payload_length);

    packet[0] = 0x7F;
    packet[1] = 0x4E;
    packet[2] = SAW_UDP_VERSION;
    packet[3] = SAW_UDP_CMD_RESULT_END;
    write_be16(packet + 4, ++saw_notify_sequence);
    write_be16(packet + 6, result);
    write_be16(packet + 8, payload_length);
    write_be32(packet + 10, saw_result_generation);
    write_be32(packet + 14, result);
    write_be32(packet + 18, packet_count);
    write_be32(packet + 22, (result == SAW_CONTROL_OK) ?
               saw_published_result_point_count : 0UL);
    write_be32(packet + 26, (result == SAW_CONTROL_OK) ?
               saw_published_fine_peak_count : 0UL);
    write_be32(packet + 30, 0UL);
    write_be32(packet + 34, SAW_UDP_STATUS_WORDS);
    memcpy(packet + 38, saw_published_status,
           SAW_UDP_STATUS_WORDS * 4UL);
    write_be32(packet + crc_offset, saw_crc32(packet, crc_offset));
    saw_stream_account(send_udp_bytes(pcb, &saw_notify_addr,
                                      saw_notify_port, packet,
                                      (u16)(crc_offset + 4U)));
}

static void saw_send_published_result(u16 result)
{
    u32 packet_count = 0UL;
    u32 peak_id;
    u32 start_index;
    u32 returned_points;
    SAW_FineResultMeta *meta;

    if ((pcb == 0) || !saw_notify_valid) {
        return;
    }
    if ((result == SAW_CONTROL_OK) && saw_result_valid) {
        packet_count = saw_stream_packet_count();
        for (start_index = 0UL;
             start_index < saw_published_result_point_count;
             start_index += SAW_UDP_RESULT_MAX_POINTS) {
            returned_points =
                saw_published_result_point_count - start_index;
            if (returned_points > SAW_UDP_RESULT_MAX_POINTS) {
                returned_points = SAW_UDP_RESULT_MAX_POINTS;
            }
            saw_send_stream_data(
                0UL, 0xFFFFFFFFUL, 0,
                saw_published_result_snapshot,
                saw_published_result_point_count, start_index,
                returned_points);
        }
        for (peak_id = 0UL;
             peak_id < saw_published_fine_peak_count; peak_id++) {
            meta = &saw_published_fine_meta[peak_id];
            for (start_index = 0UL; start_index < meta->point_count;
                 start_index += SAW_UDP_RESULT_MAX_POINTS) {
                returned_points = meta->point_count - start_index;
                if (returned_points > SAW_UDP_RESULT_MAX_POINTS) {
                    returned_points = SAW_UDP_RESULT_MAX_POINTS;
                }
                saw_send_stream_data(
                    1UL, peak_id, meta,
                    saw_published_fine_result_snapshot[peak_id],
                    meta->point_count, start_index, returned_points);
            }
        }
    }
    saw_send_stream_end(result, packet_count);
}

static u32 saw_abs_diff_u32(u32 left, u32 right)
{
    return (left >= right) ? (left - right) : (right - left);
}

static u8 saw_coarse_point_excluded(u32 index)
{
    u32 frequency = g_saw_config.start_freq_hz +
                    index * g_saw_config.step_hz;
    u32 lo_frequency = g_saw_config.start_freq_hz -
                       g_saw_config.coarse_lo_guard_hz;

    return saw_abs_diff_u32(frequency, lo_frequency) <=
           g_saw_config.coarse_lo_blind_hz;
}

static u64 saw_median3_u64(u64 left, u64 center, u64 right)
{
    if (left > center) {
        u64 temporary = left;
        left = center;
        center = temporary;
    }
    if (center > right) {
        u64 temporary = center;
        center = right;
        right = temporary;
    }
    return (left > center) ? left : center;
}

static u64 saw_saturating_add_u64(u64 left, u64 right)
{
    return ((~0ULL - left) < right) ? ~0ULL : left + right;
}

static void saw_sort_u64(u64 *values, u32 count)
{
    u32 i;
    u32 position;
    u64 value;

    for (i = 1UL; i < count; i++) {
        value = values[i];
        position = i;
        while ((position > 0UL) && (values[position - 1UL] > value)) {
            values[position] = values[position - 1UL];
            position--;
        }
        values[position] = value;
    }
}

static u32 saw_detect_coarse_peaks(void)
{
    u64 detection_values[SAW_RESULT_MAX_POINTS];
    u64 quiet_values[SAW_RESULT_MAX_POINTS];
    u32 candidate_indices[SAW_RESULT_MAX_POINTS];
    u64 candidate_scores[SAW_RESULT_MAX_POINTS];
    u8 candidate_valid[SAW_RESULT_MAX_POINTS];
    u32 selected_indices[SAW_FINE_MAX_PEAKS];
    u32 valid_count = 0UL;
    u32 quiet_count;
    u32 candidate_count = 0UL;
    u32 selected_count = 0UL;
    u32 radius;
    u32 separation;
    u32 index;
    u32 distance;
    u32 i;
    u32 selected;
    u32 position;
    u32 best_candidate;
    u64 noise_floor;
    u64 energy;
    u64 left_min;
    u64 right_min;
    u64 valley;
    u64 raw_energy;
    u64 raw_left_min;
    u64 raw_right_min;
    u64 raw_valley;
    u64 best_energy;
    u64 best_score;
    u64 score;
    u64 excess;
    u8 have_left;
    u8 have_right;

    for (index = 0UL; index < saw_result_point_count; index++) {
        detection_values[index] = saw_result_snapshot[index];
        if ((index > 0UL) && (index + 1UL < saw_result_point_count) &&
            !saw_coarse_point_excluded(index - 1UL) &&
            !saw_coarse_point_excluded(index) &&
            !saw_coarse_point_excluded(index + 1UL)) {
            detection_values[index] = saw_median3_u64(
                saw_result_snapshot[index - 1UL],
                saw_result_snapshot[index],
                saw_result_snapshot[index + 1UL]);
        }
        if (!saw_coarse_point_excluded(index)) {
            quiet_values[valid_count++] = detection_values[index];
        }
    }
    if (valid_count < 3UL) {
        return 0UL;
    }

    saw_sort_u64(quiet_values, valid_count);
    quiet_count = (valid_count * 3UL + 3UL) / 4UL;
    if (quiet_count == 0UL) {
        return 0UL;
    }
    noise_floor = quiet_values[quiet_count / 2UL];
    radius = (120000UL + g_saw_config.step_hz - 1UL) /
             g_saw_config.step_hz;
    if (radius < 4UL) {
        radius = 4UL;
    }
    if (radius > 24UL) {
        radius = 24UL;
    }
    separation = radius;

    for (index = 1UL; index + 1UL < saw_result_point_count; index++) {
        if (saw_coarse_point_excluded(index) ||
            saw_coarse_point_excluded(index - 1UL) ||
            saw_coarse_point_excluded(index + 1UL)) {
            continue;
        }
        raw_energy = saw_result_snapshot[index];
        energy = detection_values[index];
        if ((raw_energy == 0ULL) ||
            (raw_energy < saw_result_snapshot[index - 1UL]) ||
            (raw_energy <= saw_result_snapshot[index + 1UL])) {
            continue;
        }

        left_min = energy;
        right_min = energy;
        raw_left_min = raw_energy;
        raw_right_min = raw_energy;
        have_left = 0U;
        have_right = 0U;
        for (distance = 1UL; distance <= radius; distance++) {
            if ((index >= distance) &&
                !saw_coarse_point_excluded(index - distance)) {
                if (detection_values[index - distance] < left_min) {
                    left_min = detection_values[index - distance];
                }
                if (saw_result_snapshot[index - distance] < raw_left_min) {
                    raw_left_min = saw_result_snapshot[index - distance];
                }
                have_left = 1U;
            }
            if ((index + distance < saw_result_point_count) &&
                !saw_coarse_point_excluded(index + distance)) {
                if (detection_values[index + distance] < right_min) {
                    right_min = detection_values[index + distance];
                }
                if (saw_result_snapshot[index + distance] < raw_right_min) {
                    raw_right_min = saw_result_snapshot[index + distance];
                }
                have_right = 1U;
            }
        }
        if (!have_left || !have_right) {
            continue;
        }
        valley = (left_min > right_min) ? left_min : right_min;
        raw_valley = (raw_left_min > raw_right_min) ?
                     raw_left_min : raw_right_min;
        /* A supported SAW peak passes the median-filtered path. A genuine
           response narrower than the coarse grid may occupy only one point;
           retain it only with substantially stricter raw SNR/prominence so
           ordinary one-point glitches remain rejected. */
        if ((((energy / 6ULL) < noise_floor) ||
             ((energy / 4ULL) < valley)) &&
            (((raw_energy / 16ULL) < noise_floor) ||
             ((raw_energy / 10ULL) < raw_valley))) {
            continue;
        }
        score = 0ULL;
        for (distance = 0UL; distance <= 2UL; distance++) {
            if ((index >= distance) &&
                !saw_coarse_point_excluded(index - distance)) {
                excess = (saw_result_snapshot[index - distance] > noise_floor) ?
                         saw_result_snapshot[index - distance] - noise_floor :
                         0ULL;
                score = saw_saturating_add_u64(score, excess);
            }
            if ((distance > 0UL) &&
                (index + distance < saw_result_point_count) &&
                !saw_coarse_point_excluded(index + distance)) {
                excess = (saw_result_snapshot[index + distance] > noise_floor) ?
                         saw_result_snapshot[index + distance] - noise_floor :
                         0ULL;
                score = saw_saturating_add_u64(score, excess);
            }
        }
        candidate_indices[candidate_count] = index;
        candidate_scores[candidate_count] = score;
        candidate_valid[candidate_count] = 1U;
        candidate_count++;
    }

    while ((selected_count < SAW_FINE_MAX_PEAKS) &&
           (selected_count < candidate_count)) {
        best_candidate = candidate_count;
        best_energy = 0ULL;
        best_score = 0ULL;
        for (i = 0UL; i < candidate_count; i++) {
            if (candidate_valid[i] &&
                ((best_candidate == candidate_count) ||
                 (candidate_scores[i] > best_score) ||
                 ((candidate_scores[i] == best_score) &&
                  (saw_result_snapshot[candidate_indices[i]] > best_energy)))) {
                best_candidate = i;
                best_score = candidate_scores[i];
                best_energy = saw_result_snapshot[candidate_indices[i]];
            }
        }
        if (best_candidate == candidate_count) {
            break;
        }
        selected_indices[selected_count++] =
            candidate_indices[best_candidate];
        for (i = 0UL; i < candidate_count; i++) {
            if (saw_abs_diff_u32(candidate_indices[i],
                                 candidate_indices[best_candidate]) <
                separation) {
                candidate_valid[i] = 0U;
            }
        }
    }

    for (i = 1UL; i < selected_count; i++) {
        selected = selected_indices[i];
        position = i;
        while ((position > 0UL) &&
               (selected_indices[position - 1UL] > selected)) {
            selected_indices[position] = selected_indices[position - 1UL];
            position--;
        }
        selected_indices[position] = selected;
    }

    for (i = 0UL; i < selected_count; i++) {
        saw_fine_meta[i].coarse_index = selected_indices[i];
        saw_fine_meta[i].target_frequency_hz =
            g_saw_config.start_freq_hz +
            selected_indices[i] * g_saw_config.step_hz;
        saw_fine_meta[i].lo_frequency_hz = 0UL;
        saw_fine_meta[i].start_frequency_hz =
            saw_fine_meta[i].target_frequency_hz -
            g_saw_config.fine_half_bandwidth_hz;
        saw_fine_meta[i].step_hz = g_saw_config.fine_step_hz;
        saw_fine_meta[i].point_count =
            ((g_saw_config.fine_half_bandwidth_hz * 2UL) /
             g_saw_config.fine_step_hz) + 1UL;
    }
    return selected_count;
}

static int saw_capture_coarse_results(void)
{
    SAW_RuntimeStatus runtime;
    u32 point_index;

    SAW_GetRuntimeStatus(&runtime);
    if ((runtime.point_count == 0UL) ||
        (runtime.point_count > SAW_RESULT_MAX_POINTS)) {
        return SAW_CONTROL_ERR_RANGE;
    }
    for (point_index = 0UL; point_index < runtime.point_count;
         point_index++) {
        if (SAW_ReadPointEnergy(point_index,
                                &saw_result_snapshot[point_index]) !=
            SAW_CONTROL_OK) {
            return SAW_CONTROL_ERR_ARGUMENT;
        }
    }
    saw_result_point_count = runtime.point_count;
    return SAW_CONTROL_OK;
}

static int saw_start_fine_chunk(void)
{
    SAW_FineResultMeta *meta;
    u32 remaining;
    u32 chunk_start_frequency;
    int result;

    if (saw_current_fine_peak >= saw_fine_peak_count) {
        return SAW_CONTROL_ERR_ARGUMENT;
    }
    meta = &saw_fine_meta[saw_current_fine_peak];
    remaining = meta->point_count - saw_fine_points_written;
    saw_fine_chunk_points = (remaining > SAW_RESULT_MAX_POINTS) ?
                            SAW_RESULT_MAX_POINTS : remaining;
    chunk_start_frequency = meta->start_frequency_hz +
                            saw_fine_points_written * meta->step_hz;
    saw_current_fine_chunk =
        saw_fine_points_written / SAW_RESULT_MAX_POINTS;
    result = SAW_PrepareFineSweep(
        meta->target_frequency_hz, chunk_start_frequency,
        saw_fine_chunk_points, &meta->lo_frequency_hz);
    if (result != SAW_CONTROL_OK) {
        return result;
    }
    saw_scan_stage = SAW_SCAN_STAGE_FINE;
    return SAW_StartScan();
}

static int saw_capture_fine_chunk(void)
{
    u32 index;

    if ((saw_current_fine_peak >= saw_fine_peak_count) ||
        ((saw_fine_points_written + saw_fine_chunk_points) >
         SAW_FINE_MAX_POINTS)) {
        return SAW_CONTROL_ERR_RANGE;
    }
    for (index = 0UL; index < saw_fine_chunk_points; index++) {
        if (SAW_ReadSweepPointEnergy(
                index, saw_fine_chunk_points,
                &saw_fine_result_snapshot[saw_current_fine_peak]
                                         [saw_fine_points_written + index])
            != SAW_CONTROL_OK) {
            return SAW_CONTROL_ERR_ARGUMENT;
        }
    }
    saw_fine_points_written += saw_fine_chunk_points;
    return SAW_CONTROL_OK;
}

static int saw_start_job(void);

static void saw_finish_job(u16 result)
{
    SAW_RuntimeStatus runtime;
    int restore_result = SAW_RestoreCoarseSweep();

    if ((result == SAW_CONTROL_OK) &&
        (restore_result != SAW_CONTROL_OK)) {
        result = (u16)restore_result;
    }
    saw_job_result = result;
    saw_scan_stage = (result == SAW_CONTROL_OK) ?
                     SAW_SCAN_STAGE_READY : SAW_SCAN_STAGE_ERROR;
    saw_result_generation++;
    saw_current_fine_peak = 0xFFFFFFFFUL;
    saw_current_fine_chunk = 0UL;

    if (result == SAW_CONTROL_OK) {
        saw_published_result_point_count = saw_result_point_count;
        memcpy(saw_published_result_snapshot, saw_result_snapshot,
               saw_result_point_count * sizeof(u64));
        saw_published_fine_peak_count = saw_fine_peak_count;
        memcpy(saw_published_fine_meta, saw_fine_meta,
               saw_fine_peak_count * sizeof(SAW_FineResultMeta));
        memcpy(saw_published_fine_result_snapshot,
               saw_fine_result_snapshot,
               sizeof(saw_published_fine_result_snapshot));
        saw_published_config = g_saw_config;
        SAW_GetRuntimeStatus(&runtime);
        saw_published_config_revision = runtime.config_revision;
        saw_published_rx_samples_last = runtime.rx_samples_last;
        saw_result_valid = 1U;
    }
    else {
        saw_continuous_enabled = 0U;
    }

    fill_status_payload(saw_published_status);
    saw_send_published_result(result);

    if (saw_continuous_enabled) {
        u16 start_result = (u16)saw_start_job();
        if (start_result != SAW_CONTROL_OK) {
            saw_scan_stage = SAW_SCAN_STAGE_ERROR;
            saw_continuous_enabled = 0U;
            xil_printf("[SAW] next round start failed: %u\r\n",
                       (unsigned int)start_result);
        }
    }
#if SAW_SERIAL_MODE == SAW_SERIAL_MODE_DEBUG
    saw_uart_print_round_result(result);
#else
    if (SAW_TFT_LEGACY_FRAME_ENABLED != 0) {
        saw_tft_publish_round(result);
    }
    saw_tft_publish_display_round(result);
#endif
}

static int saw_start_job(void)
{
    int result;

    saw_result_point_count = 0UL;
    saw_fine_peak_count = 0UL;
    saw_current_fine_peak = 0xFFFFFFFFUL;
    saw_current_fine_chunk = 0UL;
    saw_fine_points_written = 0UL;
    saw_fine_chunk_points = 0UL;
    saw_job_result = SAW_CONTROL_OK;
    memset(saw_fine_meta, 0, sizeof(saw_fine_meta));

    result = SAW_RestoreCoarseSweep();
    if (result != SAW_CONTROL_OK) {
        saw_scan_stage = SAW_SCAN_STAGE_ERROR;
        return result;
    }
    saw_scan_stage = SAW_SCAN_STAGE_COARSE;
    result = SAW_StartScan();
    if (result != SAW_CONTROL_OK) {
        saw_scan_stage = SAW_SCAN_STAGE_ERROR;
    }
    return result;
}

static int decode_saw_config(const u8 *payload, u16 length,
                             SAW_Config *config)
{
    u32 *fields = (u32 *)config;
    u32 i;

    if (length != (SAW_UDP_CONFIG_WORDS * 4)) {
        return SAW_CONTROL_ERR_ARGUMENT;
    }
    for (i = 0; i < SAW_UDP_CONFIG_WORDS; i++) {
        fields[i] = read_be32(payload + (i * 4UL));
    }
    return SAW_CONTROL_OK;
}

static void handle_saw_packet(struct udp_pcb *upcb, const ip_addr_t *addr,
                              u16 port, const u8 *packet, u16 length)
{
    u8 command = packet[3];
    u16 sequence = read_be16(packet + 4);
    u16 payload_length = read_be16(packet + 6);
    u16 result = SAW_CONTROL_ERR_ARGUMENT;
    SAW_Config config;
    SAW_Config previous_config;
    u32 received_crc;
    u32 calculated_crc;
    u32 notify_port;
    u32 persist_receiver;
    u8 resend_published = 0U;
    u8 previous_continuous;

    if ((payload_length > (SAW_UDP_MAX_PACKET - 12)) ||
        (length != (u16)(8 + payload_length + 4))) {
        send_saw_reply(upcb, addr, port, command, sequence, result);
        return;
    }

    received_crc = read_be32(packet + 8 + payload_length);
    calculated_crc = saw_crc32(packet, 8 + payload_length);
    if (received_crc != calculated_crc) {
        send_saw_reply(upcb, addr, port, command, sequence, result);
        return;
    }

    switch (command) {
    case SAW_UDP_CMD_CONFIG:
        previous_continuous = saw_continuous_enabled;
        result = decode_saw_config(packet + 8, payload_length, &config);
        if (result == SAW_CONTROL_OK) {
            result = SAW_ValidateConfig(&config);
            if (result == SAW_CONTROL_OK) {
                previous_config = g_saw_config;
                result = SAW_ApplyConfig(&config);
                if (result == SAW_CONTROL_OK) {
                    result = SAW_ConfigStore_Save(
                        &config,
                        saw_notify_valid ? saw_notify_addr.addr : 0UL,
                        saw_notify_valid ? saw_notify_port : 0U);
                }
                if (result != SAW_CONTROL_OK) {
                    (void)SAW_ApplyConfig(&previous_config);
                    if (previous_continuous) {
                        saw_continuous_enabled = 1U;
                        (void)saw_start_job();
                    }
                }
            }
        }
        if (result == SAW_CONTROL_OK) {
            saw_continuous_enabled = 1U;
            result = (u16)saw_start_job();
        }
        break;
    case SAW_UDP_CMD_START:
        if (payload_length == 4) {
            notify_port = read_be32(packet + 8);
            if ((notify_port == 0UL) || (notify_port > 65535UL)) {
                result = SAW_CONTROL_ERR_ARGUMENT;
                break;
            }
            saw_notify_addr = *addr;
            saw_notify_port = (u16)notify_port;
            saw_notify_valid = 1;
        }
        else if (payload_length != 0) {
            result = SAW_CONTROL_ERR_ARGUMENT;
            break;
        }
        saw_continuous_enabled = 1U;
        if ((saw_scan_stage == SAW_SCAN_STAGE_COARSE) ||
            (saw_scan_stage == SAW_SCAN_STAGE_FINE)) {
            result = SAW_CONTROL_OK;
        }
        else {
            result = (u16)saw_start_job();
        }
        break;
    case SAW_UDP_CMD_STOP:
        if (payload_length != 0U) {
            result = SAW_CONTROL_ERR_ARGUMENT;
            break;
        }
        saw_continuous_enabled = 0U;
        result = SAW_RestoreCoarseSweep();
        saw_scan_stage = (result == SAW_CONTROL_OK) ?
                         SAW_SCAN_STAGE_IDLE : SAW_SCAN_STAGE_ERROR;
        break;
    case SAW_UDP_CMD_STATUS:
        result = (payload_length == 0) ? SAW_CONTROL_OK :
                 SAW_CONTROL_ERR_ARGUMENT;
        break;
    case SAW_UDP_CMD_RESULTS:
        if (payload_length == 8) {
            send_saw_results_reply(upcb, addr, port, sequence,
                                   read_be32(packet + 8),
                                   read_be32(packet + 12));
        }
        else {
            send_saw_results_reply(upcb, addr, port, sequence, 0UL, 0UL);
        }
        return;
    case SAW_UDP_CMD_FINE_RESULTS:
        if (payload_length == 12) {
            send_saw_fine_results_reply(
                upcb, addr, port, sequence,
                read_be32(packet + 8),
                read_be32(packet + 12),
                read_be32(packet + 16));
        }
        else {
            send_saw_fine_results_reply(
                upcb, addr, port, sequence, 0UL, 0UL, 0UL);
        }
        return;
    case SAW_UDP_CMD_SUBSCRIBE:
        if ((payload_length != 4U) && (payload_length != 8U)) {
            result = SAW_CONTROL_ERR_ARGUMENT;
            break;
        }
        notify_port = read_be32(packet + 8);
        persist_receiver = (payload_length == 8U) ?
                           read_be32(packet + 12) : 0UL;
        if ((notify_port == 0UL) || (notify_port > 65535UL) ||
            (persist_receiver > 1UL)) {
            result = SAW_CONTROL_ERR_ARGUMENT;
            break;
        }
        saw_notify_addr = *addr;
        saw_notify_port = (u16)notify_port;
        saw_notify_valid = 1U;
        result = SAW_CONTROL_OK;
        if (persist_receiver) {
            result = SAW_ConfigStore_Save(
                &g_saw_config, saw_notify_addr.addr, saw_notify_port);
        }
        resend_published = (result == SAW_CONTROL_OK) &&
                           saw_result_valid;
        break;
    default:
        result = SAW_CONTROL_ERR_ARGUMENT;
        break;
    }

    send_saw_reply(upcb, addr, port, command, sequence, result);
    if (resend_published) {
        saw_send_published_result(SAW_CONTROL_OK);
    }
}

static void handle_legacy_packet(struct udp_pcb *upcb, const ip_addr_t *addr,
                                 u16 port, const u8 *packet, u16 length)
{
    u8 reply[3] = {0x7F, 0x4E, packet[3]};
    u32 data;
    u32 i;
    u32 *rx_buffer_state = (u32 *)XPAR_BRAM_0_BASEADDR;

    if (length < 8) {
        return;
    }

    if (packet[3] == 0xBA) {
        EMMC_Variable[3] = read_be32(packet + 4) * 100000UL;
        ad9361_set_tx_lo_freq(ad9361_phy, EMMC_Variable[3]);
        EMMC_Write();
    }
    else {
        for (i = 0; (i + 3) < length; i += 4) {
            data = read_be32(packet + i);
            *rx_buffer_state = data;
        }
        if (packet[3] == 0xBB) {
            EMMC_Variable[2] = read_be32(packet + 4);
        }
        else if (packet[3] == 0xBC) {
            EMMC_Variable[1] = read_be32(packet + 4);
        }
        else if (packet[3] == 0xBD) {
            EMMC_Variable[0] = read_be32(packet + 4);
        }
        EMMC_Write();
        *rx_buffer_state = 0xAA5501AA;
    }

    send_udp_bytes(upcb, addr, port, reply, sizeof(reply));
}

static void udp_recv_callback(void *arg, struct udp_pcb *upcb,
                              struct pbuf *p, const ip_addr_t *addr, u16 port)
{
    u8 packet[SAW_UDP_MAX_PACKET];
    u16 length;

    (void)arg;
    if (p == 0) {
        return;
    }

    length = (p->tot_len > SAW_UDP_MAX_PACKET) ?
             SAW_UDP_MAX_PACKET : (u16)p->tot_len;
    pbuf_copy_partial(p, packet, length, 0);

    if ((length >= 4) && (packet[0] == 0xAA) &&
        (packet[1] == 0x55) && (packet[2] == SAW_UDP_VERSION)) {
        if (packet[3] >= SAW_UDP_CMD_CONFIG) {
            if (length >= 12) {
                handle_saw_packet(upcb, addr, port, packet, length);
            }
        }
        else {
            handle_legacy_packet(upcb, addr, port, packet, length);
        }
    }

    pbuf_free(p);
}

void start_application(void)
{
    err_t err;

    saw_scan_stage = SAW_SCAN_STAGE_IDLE;
    saw_current_fine_peak = 0xFFFFFFFFUL;
    saw_job_result = SAW_CONTROL_OK;
    pcb = udp_new();
    if (pcb == 0) {
        xil_printf("UDP server: PCB allocation failed\r\n");
        return;
    }

    err = udp_bind(pcb, IP_ADDR_ANY, UDP_CONN_PORT);
    if (err != ERR_OK) {
        xil_printf("UDP server: bind port %d failed, err=%d\r\n",
                   UDP_CONN_PORT, err);
        udp_remove(pcb);
        pcb = 0;
        return;
    }

    udp_recv(pcb, udp_recv_callback, 0);
    xil_printf("SAW UDP control listening on port %d\r\n", UDP_CONN_PORT);
}

void SAW_AutonomousStart(void)
{
    u32 receiver_ipv4 = 0UL;
    u16 receiver_port = 0U;
    int result;

    SAW_ConfigStore_GetReceiver(&receiver_ipv4, &receiver_port);
    if ((receiver_ipv4 != 0UL) && (receiver_port != 0U)) {
        saw_notify_addr.addr = receiver_ipv4;
        saw_notify_port = receiver_port;
        saw_notify_valid = 1U;
    }
    saw_continuous_enabled = 1U;
    result = saw_start_job();
    if (result != SAW_CONTROL_OK) {
        saw_continuous_enabled = 0U;
        saw_scan_stage = SAW_SCAN_STAGE_ERROR;
        xil_printf("SAW autonomous start failed: %d\r\n", result);
    }
    else {
        xil_printf("SAW autonomous continuous scanning started\r\n");
    }
}

void SAW_NotificationService(void)
{
    SAW_RuntimeStatus runtime;
    int result;

    if (((saw_scan_stage != SAW_SCAN_STAGE_COARSE) &&
         (saw_scan_stage != SAW_SCAN_STAGE_FINE))) {
        return;
    }

    SAW_GetRuntimeStatus(&runtime);
    if (((runtime.pl_status & 0x4UL) == 0UL) ||
        ((runtime.pl_status & 0x2UL) != 0UL)) {
        return;
    }

    if (saw_scan_stage == SAW_SCAN_STAGE_COARSE) {
        result = saw_capture_coarse_results();
        if (result != SAW_CONTROL_OK) {
            saw_finish_job((u16)result);
            return;
        }
        if (!g_saw_config.fine_enabled) {
            saw_finish_job(SAW_CONTROL_OK);
            return;
        }

        saw_fine_peak_count = saw_detect_coarse_peaks();
        if (saw_fine_peak_count == 0UL) {
            saw_finish_job(SAW_CONTROL_OK);
            return;
        }
        saw_current_fine_peak = 0UL;
        saw_current_fine_chunk = 0UL;
        saw_fine_points_written = 0UL;
        result = saw_start_fine_chunk();
        if (result != SAW_CONTROL_OK) {
            saw_finish_job((u16)result);
        }
        return;
    }

    result = saw_capture_fine_chunk();
    if (result != SAW_CONTROL_OK) {
        saw_finish_job((u16)result);
        return;
    }
    if (saw_fine_points_written <
        saw_fine_meta[saw_current_fine_peak].point_count) {
        result = saw_start_fine_chunk();
        if (result != SAW_CONTROL_OK) {
            saw_finish_job((u16)result);
        }
        return;
    }

    saw_current_fine_peak++;
    if (saw_current_fine_peak < saw_fine_peak_count) {
        saw_current_fine_chunk = 0UL;
        saw_fine_points_written = 0UL;
        result = saw_start_fine_chunk();
        if (result != SAW_CONTROL_OK) {
            saw_finish_job((u16)result);
        }
        return;
    }

    saw_finish_job(SAW_CONTROL_OK);
}
