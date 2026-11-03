# Reusable sensor-record validation Skill

The reusable Skill resides in `sensor-session-audit/`, and the validated distributable is `skill.zip`.

* **Scope:** inspect sensor acquisition sessions stored as `session.json` and `summary.csv`, preserving the recorded session states and measurements.
* **Use:** `python skill/sensor-session-audit/scripts/audit_sessions.py --input data/historical/stability --output sim/results/session_audit`.
* **Verification:** Python standard-library unit tests are in `skill/sensor-session-audit/tests/`; the original dataset audit detects 28 sessions and 19,530 rows. Output records are included in `sim/results/session_audit/`.
* **Reuse:** adaptable to other instrument datasets with fields `round`, `track_id` and `valid`; no Vivado-specific dependency.
* **Limits:** does not regenerate signals or determine pressure accuracy; only validates session metadata, record coverage and counts.

See `sensor-session-audit/SKILL.md` for the procedure, input fields and output formats.
