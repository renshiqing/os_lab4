# Lab4：页映射与内核线程

教师 lab4.zip 为基准，LAB4 标记署名 2412396，保留教师测试与既有断言。默认 PMM 管理器接口名为 default_pmm_manager，内部适配本组已验证的 Best-Fit。

```bash
source ~/os-course/env.sh
make -j4
make qemu
make grade
make -j4
python3 tools/run_validation.py --out /tmp/lab4-extra
python3 tools/debug_threads.py --out /tmp/lab4-gdb
```

官方满分 30；默认线程打印后到原 do_exit 占位 panic，再经原 kmonitor 的 SBI 调用关机。未实现完整退出/回收。多架构 GDB 在无 ELF 的教师批处理连接中需要先设 RV64；tools/grade_gdb.sh 仅添加架构初始化，评分脚本和测试未改。

补充测试在临时树注入测例与故障；正式构建不含测试钩子。GDB 脚本对真实内核设置硬件断点并执行单步，输出完整命令、QEMU 记录和摘要。

参见 [报告](../report/report.md)、[测例与边界](../report/test-plan.md)、[演示及手动调试](../report/debug-plan.md)。
