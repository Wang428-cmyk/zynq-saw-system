#include <string.h>
#include "xil_printf.h"
#include "ff.h"
#include "saw_control.h"

#define SAW_CONFIG_PATH_A        "0:/SAWCFGA.BIN"
#define SAW_CONFIG_PATH_B        "0:/SAWCFGB.BIN"
#define SAW_LEGACY_PATH          "0:/Variable.txt"
#define SAW_CONFIG_MAGIC         0x53415743UL
#define SAW_CONFIG_VERSION       2U
#define SAW_CONFIG_VERSION_V1    1U
#define SAW_MIGRATED_LO_GUARD_HZ 300000UL

typedef struct __attribute__((packed)) {
    u32 center_freq_hz;
    u32 half_bandwidth_hz;
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
} SAW_ConfigV1;

typedef struct __attribute__((packed)) {
    SAW_ConfigV1 config;
    u32 receiver_ipv4;
    u32 receiver_port;
} SAW_ConfigPayloadV1;

typedef struct __attribute__((packed)) {
    u32 magic;
    u16 version;
    u16 header_size;
    u32 payload_size;
    u32 generation;
    u32 payload_crc32;
    SAW_ConfigPayloadV1 payload;
} SAW_ConfigRecordV1;

typedef struct __attribute__((packed)) {
    SAW_Config config;
    u32 receiver_ipv4;
    u32 receiver_port;
} SAW_ConfigPayload;

typedef struct __attribute__((packed)) {
    u32 magic;
    u16 version;
    u16 header_size;
    u32 payload_size;
    u32 generation;
    u32 payload_crc32;
    SAW_ConfigPayload payload;
} SAW_ConfigRecord;

static FATFS saw_fatfs;
static u32 saw_store_mounted;
static u32 saw_store_valid;
static u32 saw_store_generation;
static u32 saw_store_last_error;
static u32 saw_store_receiver_ipv4;
static u16 saw_store_receiver_port;

extern u32 EMMC_Variable[4];

static u32 saw_store_crc32(const u8 *data, u32 length)
{
    u32 index;
    u8 bit;
    u32 crc = 0xFFFFFFFFUL;

    for (index = 0UL; index < length; index++) {
        crc ^= data[index];
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^
                  ((crc & 1UL) ? 0xEDB88320UL : 0UL);
        }
    }
    return ~crc;
}

static int saw_store_read_record(const char *path, SAW_ConfigRecord *record)
{
    FIL file;
    UINT bytes_read = 0U;
    FRESULT status;
    SAW_ConfigRecordV1 legacy;
    SAW_Config validation_config;
    u32 migrated_bandwidth;
    u32 migrated_guard;

    memset(record, 0, sizeof(*record));
    status = f_open(&file, path, FA_READ);
    if (status != FR_OK) {
        return (int)status;
    }
    status = f_read(&file, record, sizeof(*record), &bytes_read);
    (void)f_close(&file);
    if (status != FR_OK) {
        return (status == FR_OK) ? -1 : (int)status;
    }
    if ((bytes_read == sizeof(legacy)) &&
        (record->version == SAW_CONFIG_VERSION_V1)) {
        memcpy(&legacy, record, sizeof(legacy));
        if ((legacy.magic != SAW_CONFIG_MAGIC) ||
            (legacy.header_size !=
             (u16)(sizeof(legacy) - sizeof(legacy.payload))) ||
            (legacy.payload_size != sizeof(legacy.payload)) ||
            (legacy.payload.receiver_port > 65535UL) ||
            (legacy.payload_crc32 !=
             saw_store_crc32((const u8 *)&legacy.payload,
                             sizeof(legacy.payload))) ||
            (legacy.payload.config.step_hz == 0UL) ||
            (legacy.payload.config.center_freq_hz <
             legacy.payload.config.half_bandwidth_hz)) {
            return -1;
        }
        migrated_bandwidth =
            ((legacy.payload.config.half_bandwidth_hz * 2UL) /
             legacy.payload.config.step_hz) *
            legacy.payload.config.step_hz;
        migrated_guard = legacy.payload.config.coarse_lo_blind_hz +
                         legacy.payload.config.step_hz;
        if (migrated_guard < SAW_MIGRATED_LO_GUARD_HZ) {
            migrated_guard = SAW_MIGRATED_LO_GUARD_HZ;
        }
        memset(record, 0, sizeof(*record));
        record->magic = SAW_CONFIG_MAGIC;
        record->version = SAW_CONFIG_VERSION;
        record->header_size =
            (u16)(sizeof(*record) - sizeof(record->payload));
        record->payload_size = sizeof(record->payload);
        record->generation = legacy.generation;
        record->payload.config.start_freq_hz =
            legacy.payload.config.center_freq_hz -
            legacy.payload.config.half_bandwidth_hz;
        record->payload.config.bandwidth_hz = migrated_bandwidth;
        record->payload.config.step_hz = legacy.payload.config.step_hz;
        record->payload.config.tx_time_us = legacy.payload.config.tx_time_us;
        record->payload.config.guard1_time_us =
            legacy.payload.config.guard1_time_us;
        record->payload.config.rx_time_us = legacy.payload.config.rx_time_us;
        record->payload.config.guard2_time_us =
            legacy.payload.config.guard2_time_us;
        record->payload.config.coherent_repetitions =
            legacy.payload.config.coherent_repetitions;
        record->payload.config.noncoherent_segments =
            legacy.payload.config.noncoherent_segments;
        record->payload.config.tx_sampling_freq_hz =
            legacy.payload.config.tx_sampling_freq_hz;
        record->payload.config.rx_sampling_freq_hz =
            legacy.payload.config.rx_sampling_freq_hz;
        record->payload.config.fine_enabled =
            legacy.payload.config.fine_enabled;
        record->payload.config.fine_half_bandwidth_hz =
            legacy.payload.config.fine_half_bandwidth_hz;
        record->payload.config.fine_step_hz =
            legacy.payload.config.fine_step_hz;
        record->payload.config.fine_lo_offset_hz =
            legacy.payload.config.fine_lo_offset_hz;
        record->payload.config.coarse_lo_blind_hz =
            legacy.payload.config.coarse_lo_blind_hz;
        record->payload.config.coarse_lo_guard_hz = migrated_guard;
        record->payload.receiver_ipv4 = legacy.payload.receiver_ipv4;
        record->payload.receiver_port = legacy.payload.receiver_port;
        record->payload_crc32 = saw_store_crc32(
            (const u8 *)&record->payload, sizeof(record->payload));
    }
    else if (bytes_read != sizeof(*record)) {
        return -1;
    }
    memcpy(&validation_config, &record->payload.config,
           sizeof(validation_config));
    if ((record->magic != SAW_CONFIG_MAGIC) ||
        (record->version != SAW_CONFIG_VERSION) ||
        (record->header_size !=
         (u16)(sizeof(*record) - sizeof(record->payload))) ||
        (record->payload_size != sizeof(record->payload)) ||
        (record->payload.receiver_port > 65535UL) ||
        (record->payload_crc32 !=
         saw_store_crc32((const u8 *)&record->payload,
                         sizeof(record->payload))) ||
        (SAW_ValidateConfig(&validation_config) != SAW_CONTROL_OK)) {
        return -1;
    }
    return 0;
}

static int saw_store_write_record(const char *path,
                                  const SAW_ConfigRecord *record)
{
    FIL file;
    UINT bytes_written = 0U;
    FRESULT status;

    status = f_open(&file, path, FA_CREATE_ALWAYS | FA_WRITE);
    if (status != FR_OK) {
        return (int)status;
    }
    status = f_write(&file, record, sizeof(*record), &bytes_written);
    if ((status == FR_OK) && (bytes_written == sizeof(*record))) {
        status = f_sync(&file);
    }
    (void)f_close(&file);
    if ((status != FR_OK) || (bytes_written != sizeof(*record))) {
        return (status == FR_OK) ? -1 : (int)status;
    }
    return 0;
}

int SAW_ConfigStore_Init(void)
{
    FRESULT status;

    status = f_mount(&saw_fatfs, "0:/", 1);
    saw_store_last_error = (u32)status;
    saw_store_mounted = (status == FR_OK) ? 1UL : 0UL;
    if (!saw_store_mounted) {
        xil_printf("SAW eMMC mount failed: %d; filesystem preserved\r\n",
                   (int)status);
        return SAW_CONTROL_ERR_STORAGE;
    }
    return SAW_CONTROL_OK;
}

int SAW_ConfigStore_Load(SAW_Config *config, u32 *receiver_ipv4,
                         u16 *receiver_port)
{
    SAW_ConfigRecord record_a;
    SAW_ConfigRecord record_b;
    const SAW_ConfigRecord *selected = 0;
    int valid_a;
    int valid_b;

    if ((config == 0) || (receiver_ipv4 == 0) || (receiver_port == 0)) {
        return SAW_CONTROL_ERR_ARGUMENT;
    }
    if (!saw_store_mounted) {
        return SAW_CONTROL_ERR_STORAGE;
    }

    valid_a = (saw_store_read_record(SAW_CONFIG_PATH_A, &record_a) == 0);
    valid_b = (saw_store_read_record(SAW_CONFIG_PATH_B, &record_b) == 0);
    if (valid_a && valid_b) {
        selected = (record_b.generation > record_a.generation) ?
                   &record_b : &record_a;
    }
    else if (valid_a) {
        selected = &record_a;
    }
    else if (valid_b) {
        selected = &record_b;
    }
    if (selected == 0) {
        saw_store_valid = 0UL;
        saw_store_last_error = (u32)FR_NO_FILE;
        return SAW_CONTROL_ERR_STORAGE;
    }

    *config = selected->payload.config;
    *receiver_ipv4 = selected->payload.receiver_ipv4;
    *receiver_port = (u16)selected->payload.receiver_port;
    saw_store_receiver_ipv4 = *receiver_ipv4;
    saw_store_receiver_port = *receiver_port;
    saw_store_generation = selected->generation;
    saw_store_valid = 1UL;
    saw_store_last_error = 0UL;
    return SAW_CONTROL_OK;
}

int SAW_ConfigStore_Save(const SAW_Config *config, u32 receiver_ipv4,
                         u16 receiver_port)
{
    SAW_ConfigRecord record_a;
    SAW_ConfigRecord record_b;
    SAW_ConfigRecord verify;
    SAW_ConfigRecord next;
    const char *target_path;
    int valid_a;
    int valid_b;
    int result;
    u32 generation = 1UL;

    if ((config == 0) ||
        (SAW_ValidateConfig(config) != SAW_CONTROL_OK)) {
        return SAW_CONTROL_ERR_ARGUMENT;
    }
    if (!saw_store_mounted) {
        return SAW_CONTROL_ERR_STORAGE;
    }

    valid_a = (saw_store_read_record(SAW_CONFIG_PATH_A, &record_a) == 0);
    valid_b = (saw_store_read_record(SAW_CONFIG_PATH_B, &record_b) == 0);
    if (valid_a && valid_b) {
        generation = ((record_a.generation > record_b.generation) ?
                      record_a.generation : record_b.generation) + 1UL;
        target_path = (record_a.generation <= record_b.generation) ?
                      SAW_CONFIG_PATH_A : SAW_CONFIG_PATH_B;
    }
    else if (valid_a) {
        generation = record_a.generation + 1UL;
        target_path = SAW_CONFIG_PATH_B;
    }
    else if (valid_b) {
        generation = record_b.generation + 1UL;
        target_path = SAW_CONFIG_PATH_A;
    }
    else {
        target_path = SAW_CONFIG_PATH_A;
    }

    memset(&next, 0, sizeof(next));
    next.magic = SAW_CONFIG_MAGIC;
    next.version = SAW_CONFIG_VERSION;
    next.header_size = (u16)(sizeof(next) - sizeof(next.payload));
    next.payload_size = sizeof(next.payload);
    next.generation = generation;
    next.payload.config = *config;
    next.payload.receiver_ipv4 = receiver_ipv4;
    next.payload.receiver_port = receiver_port;
    next.payload_crc32 =
        saw_store_crc32((const u8 *)&next.payload, sizeof(next.payload));

    result = saw_store_write_record(target_path, &next);
    if (result == 0) {
        result = saw_store_read_record(target_path, &verify);
    }
    if ((result != 0) ||
        (verify.generation != next.generation) ||
        (memcmp(&verify.payload, &next.payload,
                sizeof(next.payload)) != 0)) {
        saw_store_last_error = (result <= 0) ?
                               0xFFFFFFFFUL : (u32)result;
        xil_printf("SAW eMMC config save failed: path=%s error=%lu\r\n",
                   target_path, (unsigned long)saw_store_last_error);
        return SAW_CONTROL_ERR_STORAGE;
    }

    saw_store_receiver_ipv4 = receiver_ipv4;
    saw_store_receiver_port = receiver_port;
    saw_store_generation = generation;
    saw_store_valid = 1UL;
    saw_store_last_error = 0UL;
    return SAW_CONTROL_OK;
}

void SAW_ConfigStore_GetStatus(u32 *mounted, u32 *valid,
                               u32 *generation, u32 *last_error)
{
    if (mounted != 0) {
        *mounted = saw_store_mounted;
    }
    if (valid != 0) {
        *valid = saw_store_valid;
    }
    if (generation != 0) {
        *generation = saw_store_generation;
    }
    if (last_error != 0) {
        *last_error = saw_store_last_error;
    }
}

void SAW_ConfigStore_GetReceiver(u32 *receiver_ipv4, u16 *receiver_port)
{
    if (receiver_ipv4 != 0) {
        *receiver_ipv4 = saw_store_receiver_ipv4;
    }
    if (receiver_port != 0) {
        *receiver_port = saw_store_receiver_port;
    }
}

/* Legacy entry points are retained for the old four-word command protocol. */
int EMMC_Init(void)
{
    return (SAW_ConfigStore_Init() == SAW_CONTROL_OK) ? 0 : 1;
}

int EMMC_Read(void)
{
    FIL file;
    UINT bytes_read = 0U;
    FRESULT status;

    if (!saw_store_mounted) {
        return 1;
    }
    status = f_open(&file, SAW_LEGACY_PATH, FA_READ);
    if (status != FR_OK) {
        return (int)status;
    }
    status = f_read(&file, EMMC_Variable, sizeof(u32) * 4U, &bytes_read);
    (void)f_close(&file);
    return ((status == FR_OK) &&
            (bytes_read == (sizeof(u32) * 4U))) ? 0 : 1;
}

int EMMC_Write(void)
{
    FIL file;
    UINT bytes_written = 0U;
    FRESULT status;

    if (!saw_store_mounted) {
        return 1;
    }
    status = f_open(&file, SAW_LEGACY_PATH,
                    FA_CREATE_ALWAYS | FA_WRITE);
    if (status != FR_OK) {
        return (int)status;
    }
    status = f_write(&file, EMMC_Variable, sizeof(u32) * 4U,
                     &bytes_written);
    if ((status == FR_OK) &&
        (bytes_written == (sizeof(u32) * 4U))) {
        status = f_sync(&file);
    }
    (void)f_close(&file);
    return ((status == FR_OK) &&
            (bytes_written == (sizeof(u32) * 4U))) ? 0 : 1;
}
