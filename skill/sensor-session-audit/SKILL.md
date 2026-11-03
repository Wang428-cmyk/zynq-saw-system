---
name: sensor-session-audit
description: Audit time-series sensor measurement sessions saved as session.json and summary.csv. Use when validating a sensor acquisition experiment, distinguishing completed and stopped sessions, checking per-track valid counts and producing a reproducible CSV/JSON summary without changing original measurements.
---

# Sensor session audit

1. Locate a measurement root containing subfolders with `summary.csv` and `session.json`.
2. Run `python scripts/audit_sessions.py --input <root> --output <destination>` with Python 3.10+.
3. Review `audit_summary.json` for session states, round totals, track counts and any consistency mismatches.
4. Review `sessions.csv` to compare completed and stopped sessions; never merge stopped runs into completed-only performance statistics without labeling them.
5. Confirm raw CSV/JSON files remain unchanged; this skill is read-only on the input tree.
6. When documenting measurements, distinguish sampling completion, valid peak identification and calibrated physical measurement accuracy. Do not infer pressure accuracy from frequency stability alone.

## Input/Output

Input is a directory of session subfolders, or a single session directory; each session may include `summary.csv`, `session.json`, `config.json` and `report.csv`. The required summary columns are `round`, `track_id`, `valid`. Missing fields are reported without fabricating values.

Output is `sessions.csv` and `audit_summary.json`, written to the selected output folder. No network access or non-standard Python packages are required.

## Reuse and limits

Reusable for other instruments that record iteration number, channel ID and validity as CSV. For different field names, adapt the field mapping in `scripts/audit_sessions.py`. Sensor units and peak-fitting algorithms are not reconstructed by this utility. The original measurement software remains authoritative for statistics beyond these count checks.

Read `references/file_formats.md` for semantics. Run `python -m unittest discover -s tests -v` to verify parsing on synthetic sample sessions.
