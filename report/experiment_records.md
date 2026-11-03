# 原始实验材料索引

1. `data/historical/stability/`为原始上位机保存的28组会话目录，每组可能包含`config.json`、`session.json`、`summary.csv`、`report.csv`及精扫曲线。
2. `data/historical/sweep/`为原始扫频CSV，包含不同测量日期与频率范围的曲线。
3. `data/historical/ila/`为原始FPGA基带采集CSV。
4. `sim/results/upper_computer_unittest.log`记录11项Python上位机单元测试的执行结果。
5. `sim/results/session_audit/audit_summary.json`与`sessions.csv`为由原始数据生成的可复核会话索引。
6. `build/reports/`保留原始Vivado 2021.2综合与实现报告，`board/firmware/`保留PS源目录对应固件。

原始历史记录没有被模拟数据替换。频率稳定度是对谐振峰跟踪的统计，独立压力标定关系需依据相应传感器实验数据建立。
