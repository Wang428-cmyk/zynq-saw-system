#ifndef SAW_SERIAL_MODE_H
#define SAW_SERIAL_MODE_H

/*
 * UART1 operating modes.
 *
 * The production default is TFT mode: UART1 carries DCBUS binary frames only
 * and all xil_printf/printf output is discarded by the application outbyte().
 *
 * To restore the original text console, add the compiler definition
 *     SAW_SERIAL_MODE=SAW_SERIAL_MODE_DEBUG
 * or change the default below.
 */
#define SAW_SERIAL_MODE_DEBUG 0
#define SAW_SERIAL_MODE_TFT   1

#ifndef SAW_SERIAL_MODE
#define SAW_SERIAL_MODE SAW_SERIAL_MODE_TFT
#endif

#if ((SAW_SERIAL_MODE != SAW_SERIAL_MODE_DEBUG) && \
     (SAW_SERIAL_MODE != SAW_SERIAL_MODE_TFT))
#error "SAW_SERIAL_MODE must be SAW_SERIAL_MODE_DEBUG or SAW_SERIAL_MODE_TFT"
#endif

#ifndef SAW_TFT_STATION
#define SAW_TFT_STATION 0U
#endif

#ifndef SAW_TFT_CRC_ENABLED
#define SAW_TFT_CRC_ENABLED 1
#endif

/*
 * Retain the original status + peak DCBUS framing code for compatibility,
 * but do not transmit those two legacy frames by default.  Set this macro
 * to 1 only when an older receiver needs the original address map.
 */
#ifndef SAW_TFT_LEGACY_FRAME_ENABLED
#define SAW_TFT_LEGACY_FRAME_ENABLED 0
#endif

#if ((SAW_TFT_LEGACY_FRAME_ENABLED != 0) && \
     (SAW_TFT_LEGACY_FRAME_ENABLED != 1))
#error "SAW_TFT_LEGACY_FRAME_ENABLED must be 0 or 1"
#endif

#endif
