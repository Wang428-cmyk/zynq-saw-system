# PC / Zynq PS communication protocol

## Network architecture

The Python HTTP server serves the Web UI on `127.0.0.1:8080`, and relays control commands over UDP to the original device endpoint `192.168.2.10:4001`. Result streaming can be subscribed on UDP port `4002` by default. The IP and ports are configurable in `server.py` command-line flags.

## UDP v2 packet format

Requests start with `AA 55 02`; the wire format is a command byte, big-endian 16-bit sequence, big-endian 16-bit payload length, payload bytes, and a big-endian CRC32. Commands observed in the original PS source are:

| Hex | Command |
| --- | --- |
| E0 | CONFIG (17 scan configuration words) |
| E1 | START |
| E2 | STOP |
| E3 | STATUS |
| E4 | RESULTS |
| E5 | NOTIFY |
| E6 | FINE_RESULTS |
| E7 | SUBSCRIBE |
| E8 | RESULT_DATA |
| E9 | RESULT_END |

The device serializes the 17 configuration words as big-endian `uint32`. The software supports segmented result packets, a sweep with at most 127 coarse points per PL chunk, up to four selected fine peaks, and assembly of multi-chunk fine curves. The browser graphs scan intensity and extracted resonance-frequency estimates.

For field-level details, consult `src/ps/c/udp_perf_server.c`, `src/ps/c/saw_control.h` and `src/upper_computer/server.py`. The source files define the protocol behavior.
