# Historical measurement records

`stability/` contains the 28 raw acquisition session directories from the source upper-computer archive. Recorded session states are 15 completed, 11 stopped and 2 without complete session-status metadata; all session directories are preserved without filling missing values. The records contain 19,530 observation rows across `summary.csv` files. The session audit is provided at `sim/results/session_audit/` and can be reproduced with the bundled Skill.

`sweep/` contains the original timestamped sweep CSV files, including empty files as received. `ila/` contains two original FPGA baseband ILA CSV exports. Filenames are normalized for packaging; source file hashes and original names are recorded in `source_provenance.csv`.

Frequency ranges and acquisition conditions vary between historical sessions. The `summary.csv` frequency dispersion and valid-rate results describe signal-readout stability, not a pressure-sensitivity or absolute-pressure-accuracy calibration.
