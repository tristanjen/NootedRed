# CLAUDE.md

本仓库是 NootedRed 的 fork（https://github.com/tristanjen/NootedRed），当前分支 `vcn1-upstream`：
为 Ryzen 5 3500U（Picasso，iGPU `1002:15D8`，VCN 1.0）在 macOS 15.8.1 (24H32) 上补硬件视频解码。

**先读 `docs/vcn1-handoff.md`**（进度、当前阻塞、下一步），再读 `docs/vcn1-analysis.md`（§1–§37 逆向全过程）。

## 工作约定

- 每次改完文件立即 commit + push：

  ```bash
  git -c http.version=HTTP/1.1 push "https://x-access-token:$(/usr/local/bin/gh auth token)@github.com/tristanjen/NootedRed.git" vcn1-upstream
  ```

- 构建：`nohup xcodebuild -project NootedRed.xcodeproj -scheme NootedRed -configuration Release -derivedDataPath build/ddr build > build/vcn/buildNN.log 2>&1 &`，再轮询日志（单条命令别超过 ~120s）。
- 实机验证：装 `build/ddr/Build/Products/Release/NootedRed.kext` → 重启 → Safari/QuickTime 播 1080p → `ioreg -l | grep -o '"NRed[A-Za-z0-9]*" = [0-9a-fA-Fx]*'`。
- 反汇编要 `PYTHONPATH=/Users/tristanjen/Desktop/NootedRed/build/vcn/pylib`（capstone 5.0.9）；切片在 `/Users/tristanjen/Desktop/NootedRed/build/vcn/`。
- 完整历史对话（grep 用）：`build/vcn/vcn1-transcript.md`。
- 不要提交：`build/`、`build/vcn/pylib/`、切片 `*.kc.bin`、`session.jsonl`。
