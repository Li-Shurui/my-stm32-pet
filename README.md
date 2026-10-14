# ATK-MP157 桌面宠物（Desktop Pet）

在正点原子 ATK-MP（Linux 5.4 + OpenSTLinux Weston）的 7" RGB 屏
（1024×600）上运行的程序化绘制桌面宠物。**不需要 Qt、不需要 GPU、不需要在板子上装
编译器**——单个静态 ARM 二进制，直接驱动 DRM 叠加平面，浮在现有 Qt 桌面上。

## 交互方式

启动后屏幕右下角有一个**猫脸 App 图标**（呼吸脉冲动画），此时触摸屏完全归 Qt 桌面，
不影响正常使用。

| 操作 | 效果 |
|---|---|
| 点猫脸图标 | 宠物从图标里跳出来（开始接管触摸屏） |
| 点宠物 | 跳起来冒爱心 |
| 快速连点宠物 3 次 | 休眠（冒 Z，触摸屏还给桌面） |
| 拖拽宠物 | 跟着手指走 |
| 点桌面空白处 | 掉落猫粮，宠物走过去吃 |
| 蝴蝶飞过 | 宠物可能追过去扑抓 |
| 长按宠物 1.2 秒 | 退出宠物，收回图标 |
| 点睡着的宠物 | 醒来并重新接管触摸屏 |

## 技术架构

```
┌──────────────────────────────────────────────┐
│  LTDC 硬件合成（每像素 alpha）                 │
│  ┌────────────────────────────────────────┐  │
│  │ layer2 (plane-36) ← 宠物的全屏 ARGB8888 │  │  ← 只画宠物/蝴蝶/猫粮/图标
│  │  叠加平面，本程序独占                    │  │
│  ├────────────────────────────────────────┤  │
│  │ layer1 (plane-0)  ← fbcon + QDesktop-fb │  │  ← 原有 Qt 桌面，不受影响
│  └────────────────────────────────────────┘  │
└──────────────────────────────────────────────┘
```

- **渲染**：`/dev/dri/card0` + `DRM_IOCTL_MODE_SETPLANE`（经典 API，兼容 5.4 内核），
  1024×600 ARGB8888 dumb buffer 双缓冲，程序化绘制（椭圆/三角/线段混合），12fps
- **输入**：直接读 `/dev/input/event0`（Goodix GT9147）；宠物醒着时
  `EVIOCGRAB` 独占触摸屏，避免误触后面的 Qt UI；休眠/退出时释放
- **无资源文件**：宠物造型、蝴蝶、猫粮、图标全部代码绘制，二进制仅 ~485KB（静态）

## 文件位置

**板子端（eMMC，重启后保留）：**
- `/usr/local/bin/pet` — 宠物程序
- `/usr/local/bin/rx.py` — 串口文件接收器（部署工具，可留可删）
- `/etc/systemd/system/atk-pet.service` — 开机自启动服务

**PC 端（`D:\code\stm32\pet\`）：**
- `src/pet.c` — 宠物源码（v4，含完整注释）
- `src/probe.c` — DRM/evdev 侦查程序（架构验证用）
- `pet-armhf` / `probe-armhf` — 交叉编译产物
- `deploy/send-file.ps1` — 串口部署脚本（base64 + 逐行 MD5 校验 + 自动重传）
- `deploy/rx.py` — 板端接收器源码

## 服务管理

```sh
systemctl status atk-pet      # 查看状态
systemctl stop atk-pet        # 停止（图标消失）
systemctl start atk-pet       # 启动
systemctl disable atk-pet     # 取消开机自启
journalctl -u atk-pet -f      # 看日志（状态机会打印：sleep/walk/eat/caught...）
```

## 重新编译（PC 端 WSL Ubuntu）

```sh
wsl -d Ubuntu-26.04 -u root
apt-get install -y gcc-arm-linux-gnueabihf
cd /mnt/d/code/stm32/pet
arm-linux-gnueabihf-gcc -static -Os -o pet-armhf src/pet.c -lm
```

## 部署到板子（CH340 串口 COM3 @115200）

```powershell
cd D:\code\stm32\pet
powershell -ExecutionPolicy Bypass -NoProfile -File deploy\send-file.ps1 `
  -File pet-armhf -DestPath /usr/local/bin/pet
```

> 部署原理：板子无网络无编译器，文件走串口控制台。`rx.py` 协议为每行 base64 附加
> 8 位 MD5，丢字符的行自动重传（实测串口输入会周期性丢字符，约每 7 行一个，
> 与 Qt 桌面占用 CPU 导致 tty 读取延迟有关），最终整文件 MD5 校验。
> 注意：板子 `/tmp` 会被 systemd-tmpfiles 定期清理，接收器必须放持久化目录。

## 已知限制

- 宠物醒着时独占触摸屏，Qt 桌面暂时无法触摸（长按宠物退出/三连点休眠可归还）
- 睡着的宠物被点醒时，那一下触摸也会透传给 Qt（单次事件，无累积影响）
- 12fps 软渲染，CPU 占用约 3-5%（双核 A7 之一）
