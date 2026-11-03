SAW PS firmware
===============

Boot sequence
-------------

1. Mount the eMMC FAT filesystem. Mount failure never triggers formatting.
2. Read `SAWCFGA.BIN` and `SAWCFGB.BIN`, validate version, length, CRC and
   every SAW configuration field, and select the newest valid generation.
3. Fall back to the compiled `g_saw_config` defaults when no valid record is
   available.
4. Configure AD9361 and PL, then start the autonomous continuous scan state
   machine.

Configuration is stored as alternating A/B records so an interrupted write
does not destroy the previous valid configuration. The persisted payload
contains all 17 `SAW_Config` fields plus the upper-computer result receiver
IPv4 address and UDP port.

Runtime
-------

UDP control listens on port 4001. CONFIG validates, applies and persists the
new values, then starts continuous scanning. START is idempotent and enables
continuous scanning. STOP stops PL and disables continuous scanning while
retaining the last published result snapshot.

After each complete coarse/fine job, PS copies the work buffers to published
buffers, sends E8 data chunks and an E9 end packet to the subscribed receiver,
and immediately starts the next job. It does not wait for an upper-computer
acknowledgement or another START command.

The coarse scan is single-sided. AD9361 LO is programmed to
`start_freq_hz - coarse_lo_guard_hz`; PL scans positive NCO offsets from the
guard through guard plus bandwidth. `coarse_lo_blind_hz` remains persisted and
is used only by coarse peak detection. PS does not modify uploaded energy
samples. Configuration record version 2 loads version-1 symmetric records by
migrating their actual sampled start/stop range and receiver endpoint in
memory; the next successful CONFIG save writes the version-2 single-sided
record.

Coarse peak positions always use the original local-maximum index. Median-of-3
samples are used only to prove support for ordinary peaks. A one-bin response
can bypass that support test only when its raw SNR is at least about 12 dB and
its raw prominence is at least about 10 dB; this retains narrow SAW responses
without re-admitting small one-point glitches.
