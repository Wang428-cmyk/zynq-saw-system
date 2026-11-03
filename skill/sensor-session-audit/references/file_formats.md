# Supported session formats

* `session.json`: `state` may be `completed`, `stopped` or another application-specific state. `target_rounds` and `completed_rounds` are recorded as supplied, not inferred. `tracks[].track_id` and `tracks[].metrics.valid_count/total_count` may provide application metrics for comparison.
* `summary.csv`: each row describes one `(round, track_id)` observation. `valid` accepts `0/1`, `false/true`, `yes/no`. Repeated `(round, track_id)` rows are reported, not silently removed.
* `config.json` and `report.csv`: retained raw source files but not modified by this count checker.

Results distinguish **status**, **completion counts**, **valid record counts**, and **consistency checks**. They do not estimate calibration, force or pressure precision.
