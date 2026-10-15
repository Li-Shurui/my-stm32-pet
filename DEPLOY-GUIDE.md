# 从零开始：在 ATK-MP157 上跑起桌面宠物

> 适用场景：板子重新刷了系统（或换了一块同型号板子），要把桌面宠物重新部署上去。
> 本文记录完整流程和每一步的原因，照着做即可，全程约 40 分钟。
>
> 测试环境：正点原子 ATK-MP157（STM32MP157C，Cortex-A7 双核）+ 7" RGB 屏
> （1024×600）+ CH340 USB 串口；板子原厂镜像（OpenSTLinux Weston 底包，
> 桌面是 `/usr/local/QDesktop-fb`，开机自启 Qt 桌面）；PC 为 Windows + WSL。

---

## 0. 总览：整体方案

```
┌─────────────┐   串口(base64+MD5校验)   ┌──────────────────────┐
│  Windows PC │ ───────────────────────▶ │  ATK-MP157 (eMMC)    │
│  WSL 交叉编译 │                        │  /usr/local/bin/pet  │
│  pet-armhf  │ ◀─────────────────────  │  systemd 自启动       │
└─────────────┘   串口回显/命令          └──────────────────────┘
```

宠物程序直接驱动 **DRM 叠加平面**（不依赖 Weston/Qt/GPU），以全屏透明叠加层的
形式浮在原有 Qt 桌面上；触摸通过 evdev 直读。板子**不需要**网络、编译器、Qt 开发库。

---

## 1. 硬件连接与前期确认

1. CH340 用 USB 插到 PC，板子上电，确认设备管理器出现 `USB-SERIAL CH340 (COMx)`
   （本文按 **COM3** 描述，你的可能是别的口号，后续命令里替换即可）。
2. 用串口终端（MobaXterm/PuTTY，115200 8N1）连上，应能看到板子启动日志并自动
   登录到 `root@ATK-MP157` 的 shell。
3. 确认板子能正常启动到 Qt 桌面（屏幕上显示原厂桌面）。

> ⚠️ **串口独占**：Windows 上串口同时只允许一个程序打开。用 MobaXterm 占着 COM
> 时，部署脚本无法打开串口——部署期间请先关掉 MobaXterm（或结束 MoTTY 进程）。

---

## 2. 第一步永远是：跑探针，确认环境假设

刷机后板子的内核配置、平面分配、触摸设备名可能变化，**先探针再部署**。
探针程序（`src/probe.c`）会打印：DRM 平面列表及各平面支持的格式、连接器/CRTC/
分辨率、所有输入设备的名字和坐标范围，并实际做一次 5 秒叠加平面测试
（屏幕上会出现一个半透明棋盘格）。

```powershell
# PC 上（WSL 里编译，见第 3 节）得到 probe-armhf 后，按第 4 节传到板子：
#   /tmp/probe
# 然后在串口 shell 里执行：
chmod +x /tmp/probe
/tmp/probe
```

**判断标准（全部满足才能继续）：**

| 探针输出 | 期望值 | 含义 |
|---|---|---|
| `drm master: OK` | OK | 能拿到 DRM 控制权 |
| `plane id=36 crtc_id=0 ... AR24 ...` | 存在 crtc_id=0 的平面且格式含 **AR24**（ARGB8888） | 有空闲叠加平面且支持逐像素透明 |
| `crtc id=35 mode_valid=1 1024x600` | mode_valid=1 | CRTC 已有点亮的模式（Qt 桌面在跑） |
| `/dev/input/event0: "Goodix Capacitive TouchScreen"` | 有触摸设备，ABS 范围接近屏幕分辨率 | 触摸可直接读取 |

如果平面/格式/CRTC 不符合期望，说明底包不同，需要改 `src/pet.c` 顶部的
`PET_SCALE`（宠物尺寸）或找空闲平面的逻辑（见 `drm_setup()`）。

> 探针的 SETPLANE 测试会把棋盘格显示 5 秒。**如果屏幕上能看到**，说明叠加平面
> 架构成立；如果看不到但在 Qt 桌面下面，可能需要调整平面（zpos）或换用
> DRM atomic API。

---

## 3. 第二步：搭建交叉编译环境（PC/WSL）

板子上没有编译器，所有编译在 PC 的 WSL 里完成。静态链接，产物零运行时依赖。

```powershell
# 1) 确认 WSL 有可用的 Linux 发行版（没有的话：wsl --install -d Ubuntu）
wsl -l -v

# 2) 进入 WSL 装交叉工具链（只需一次）
wsl -d Ubuntu-26.04 -u root
apt-get update
apt-get install -y gcc-arm-linux-gnueabihf

# 3) 编译（源码在 D:\code\stm32\pet，WSL 里路径是 /mnt/d/code/stm32/pet）
cd /mnt/d/code/stm32/pet
arm-linux-gnueabihf-gcc -static -Os -o pet-armhf src/pet.c -lm

# 4) 验证产物
file pet-armhf        # 应为: ELF 32-bit LSB executable, ARM, statically linked
```

**要点：**
- `-static` 是关键：产物不依赖板子上的任何 .so，也绕开了 PC 工具链 glibc 版本
  与板子 glibc（2.31）不一致的问题。
- 颜文字位图头文件 `src/kaomoji_data.h` 由 `tools/gen_kaomoji.py` 在 PC 上生成
  （需要 Pillow），已随仓库提交，**一般不需要重新生成**；只有要增删颜文字时才跑：
  `python tools/gen_kaomoji.py`

---

## 4. 第三步：建立串口传输通道（核心基建）

板子无网络、无 SD 卡，文件只能走串口。而**串口输入会周期性丢字符**
（实测约每 7 行丢一个，与 Qt 桌面占 CPU 导致 tty 读取延迟有关），裸传必坏，
所以用下面的**逐行校验 + 自动重传**协议。

### 4.1 板子端：接收器 rx.py（首次引导）

`deploy/rx.py` 已随仓库在 PC 上。**第一次**把它传到板子用「heredoc + base64」
的简单方式（此时还没有 rx.py 可用，所以不能用协议传；文件很小，多重试几次即可）：

```powershell
# PC 上执行：把 rx.py 内容 base64 后逐行通过 heredoc 送到板子，md5 校验
$rx = 'D:\code\stm32\pet\deploy\rx.py'
$b64 = [Convert]::ToBase64String([IO.File]::ReadAllBytes($rx))
$md5 = (Get-FileHash $rx -Algorithm MD5).Hash.ToLower()
$port = New-Object System.IO.Ports.SerialPort 'COM3',115200,'None',8,'One'
$port.DtrEnable=$true; $port.RtsEnable=$true
$port.Open()
$port.Write("base64 -d > /usr/local/bin/rx.py <<'PYEOF'`r"); Start-Sleep 1
for ($i=0; $i -lt $b64.Length; $i+=1000) {
  $port.Write($b64.Substring($i,[Math]::Min(1000,$b64.Length-$i))+"`n")
  Start-Sleep -Milliseconds 150
}
$port.Write("PYEOF`n"); Start-Sleep 2
$port.Write("md5sum /usr/local/bin/rx.py`r"); Start-Sleep 2
$port.ReadExisting(); $port.Close()
# 输出的 md5 应与 $md5 一致；不一致就重跑上面几行
```

之后所有文件（包括以后更新的 rx.py 自身）都走 send-file.ps1 协议传输。

rx.py 的协议（每行一条，从 stdin 读）：

```
"<行号> <md5前8位> <base64数据>"   # 数据行
"__END__"                          # 数据段结束，报告坏行
"<行号> <md5前8位> <base64数据>"   # 修复段：只重发坏行
"__REPORT__"                       # 再报告一次（可多轮）
"__DONE__"                         # 拼装、写文件、报告整文件 MD5
```

接收器打印 `RESULT lines=N badlist=[..] missing=[..] md5=...`，PC 端据此重发坏行，
直到 `badlist=[] missing=[]` 且 MD5 与本地一致。

### 4.2 PC 端：部署脚本 send-file.ps1

```powershell
cd D:\code\stm32\pet
powershell -ExecutionPolicy Bypass -NoProfile -File deploy\send-file.ps1 `
  -File pet-armhf -DestPath /usr/local/bin/pet
```

脚本自动完成：静音内核日志（`dmesg -n 1`）→ 关回显（`stty -echo`，否则 ~500KB
回显会堵住输出）→ 启动 rx.py → 流式发送 → 读报告 → 重发坏行 → 校验 → 恢复回显。

> ⚠️ **rx.py 必须放持久化目录**（`/usr/local/bin/rx.py`）。板子的 `/tmp` 会被
> systemd-tmpfiles 定期清空，放 /tmp 过一会儿就没了（我们踩过这个坑）。

---

## 5. 第四步：安装宠物程序（持久化 + 开机自启）

```sh
# 板子串口 shell 里执行：
mkdir -p /usr/local/bin          # 原厂镜像可能没有这个目录

# ---- 装主程序（用第 4 节的 send-file.ps1 传到 /usr/local/bin/pet）----
chmod +x /usr/local/bin/pet

# ---- 装控制命令 petctl ----
cat > /usr/local/bin/petctl <<'PETCTLEOF'
<把 deploy/petctl 的内容原样粘进来>
PETCTLEOF
chmod +x /usr/local/bin/petctl

# ---- 建 systemd 服务 ----
cat > /etc/systemd/system/atk-pet.service <<'SVCEOF'
[Unit]
Description=Desktop Pet (ATK-MP157)
After=atk-qtapp-start.service

[Service]
Type=simple
ExecStartPre=/bin/sleep 3
ExecStart=/usr/local/bin/pet
Restart=on-failure
RestartSec=2

[Install]
WantedBy=multi-user.target
SVCEOF

systemctl daemon-reload
systemctl enable atk-pet        # 开机自启
systemctl start atk-pet         # 立即启动
systemctl status atk-pet        # 应为 active (running)
journalctl -u atk-pet -n 10     # 看启动日志
```

**几个关键设计的原因：**
- `After=atk-qtapp-start.service` + `ExecStartPre=/bin/sleep 3`：宠物需要 CRTC
  已经点亮（Qt 桌面先启动并设置显示模式），否则 `drm_setup()` 找不到 active CRTC
  会退出；`Restart=on-failure` 是兜底。
- 服务以 root 运行，无需特殊环境变量。
- **更新程序前必须 `systemctl stop atk-pet`**，否则覆盖正在运行的二进制会报
  `Text file busy`。

---

## 6. 第五步：验证与日常使用

```sh
# 屏幕上应出现右下角猫脸图标（启动器模式），此时触摸屏归 Qt 桌面
petctl status                  # mode=launcher ...
petctl summon                  # 召唤宠物（出场动画+冲击波）
petctl list                    # 列出全部颜文字
petctl say happy               # 说颜文字（shell 立即回显）
petctl dance                   # 跳舞
petctl food 500 300            # 掉猫粮
petctl butterfly               # 放蝴蝶
petctl sleep / wake            # 休眠/唤醒
petctl exit                    # 收回图标
journalctl -u atk-pet -f       # 跟踪宠物日志
```

触摸交互：点宠物=跳爱心，**长按 1.5 秒=休眠**（还触摸屏给桌面），
**快速连点 3 次=退出**，拖动=跟随，点桌面空白=掉猫粮。

---

## 7. 常见问题（都实际踩过）

| 现象 | 原因 | 解决 |
|---|---|---|
| `git push` 连不上 github.com | HTTPS 被干扰，且 FlClash 代理端口没监听 | 用 SSH over 443：`git remote set-url origin ssh://git@ssh.github.com:443/<user>/<repo>.git` |
| 部署脚本打不开 COM3 | MobaXterm/PuTTY 占着串口 | 关掉终端软件再部署 |
| `Text file busy` | 服务在跑，二进制被占用 | 先 `systemctl stop atk-pet` |
| rx.py 报告大量坏行 | 串口丢字符（周期性） | 正常现象，协议自动重传；若首轮坏行 >20% 检查是否有内核日志刷屏（`dmesg -n 1`） |
| 宠物重启后消失 | 装在了 /tmp（被 tmpfiles 清理） | 一律装 `/usr/local/bin`，用 systemd 自启 |
| `petctl` 无输出/无反应 | FIFO 不存在（服务没起） | `systemctl start atk-pet`；`petctl` 只在宠物运行时有效 |
| 串口输出颜文字显示 `?` | 读取端把串口当 ASCII 解码 | 终端设 UTF-8（MobaXterm 默认即可）；PowerShell 读串口要 `$port.Encoding = [Text.Encoding]::UTF8` |
| 宠物看不见/在桌面下面 | 叠加平面被其他程序占用或 zpos 不对 | 先跑探针确认有空闲平面；`systemctl stop atk-pet` 后再试 |
| 编译报 `screen_w undeclared` 之类 | 源码版本不一致 | 用仓库里的 `src/pet.c` 原版，别手工拼接 |

---

## 8. 附录 A：一键重装流程（刷机后）

```powershell
# ===== PC 侧 =====
# 1. 编译工具链就绪后：
wsl -d Ubuntu-26.04 -u root -e bash -lc "cd /mnt/d/code/stm32/pet && arm-linux-gnueabihf-gcc -static -Os -o pet-armhf src/pet.c -lm"

# 2. 关掉 MobaXterm，先传探针，串口里跑 /tmp/probe 确认环境（见第 2 节）
# 3. 装 rx.py 到板子（deploy/send-file.ps1 首次可用 heredoc 方式，见 4.1）
# 4. 传主程序：
powershell -ExecutionPolicy Bypass -NoProfile -File deploy\send-file.ps1 -File pet-armhf -DestPath /usr/local/bin/pet

# ===== 板子串口 shell =====
mkdir -p /usr/local/bin
chmod +x /usr/local/bin/pet
# 装 petctl（第 5 节 heredoc）
# 建 atk-pet.service（第 5 节 heredoc）
systemctl daemon-reload
systemctl enable --now atk-pet
petctl summon        # 完事
```

## 9. 附录 B：文件清单与职责

| 文件 | 位置 | 职责 |
|---|---|---|
| `src/pet.c` | PC | 宠物源码（v6，含全部特效与命令处理） |
| `src/probe.c` | PC | 环境探针（刷机后第一个跑） |
| `src/kaomoji_data.h` | PC | 颜文字 alpha 位图（生成物） |
| `deploy/send-file.ps1` | PC | 串口部署脚本（校验+重传） |
| `deploy/rx.py` | PC→板子 | 板端接收器 |
| `deploy/petctl` | PC→板子 | 控制命令 |
| `tools/gen_kaomoji.py` | PC | 颜文字位图生成器（可选） |
| `/usr/local/bin/pet` | 板子 | 宠物程序 |
| `/usr/local/bin/petctl` | 板子 | 控制命令 |
| `/etc/systemd/system/atk-pet.service` | 板子 | 开机自启 |

---

*本文档与源码同步维护于 git 仓库；重刷系统后按此操作即可完整复原。*
