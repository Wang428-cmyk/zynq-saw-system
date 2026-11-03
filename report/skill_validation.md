# 采集会话审计技能验证记录

`sensor-session-audit` 使用 Python 标准库汇总传感器采集会话，以 `session.json` 和 `summary.csv` 为输入，按轮次、通道及有效标志整理观测记录，输出 `sessions.csv` 与 `audit_summary.json`。

归档验证覆盖 28 组历史会话、19,530 条观测记录。脚本保留各会话的原始状态与配置，汇总记录行数、轮次数、通道数和有效观测数。3 项自动化测试通过，结果保存在 `sim/results/`。

```text
python skill/sensor-session-audit/scripts/audit_sessions.py --input data/historical/stability --output sim/results/session_audit
python -m unittest discover -s skill/sensor-session-audit/tests -v
```

该技能提供统一的采集记录索引，可通过字段映射适配其他具有轮次、通道和有效标志的传感器日志。技能说明及数据字段见 `skill/sensor-session-audit/SKILL.md` 和 `references/file_formats.md`。
