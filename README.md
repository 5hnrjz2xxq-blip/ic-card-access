# IC卡门禁系统设计

数字信号处理课程 · 电子系统设计实训

## 项目简介

基于单片机的 IC 卡门禁系统课设：读卡 → 鉴权 → 控门 → 提示/记录。

## 技术选型（待定稿）

| 模块 | 方案 |
|------|------|
| 主控 | STM32（具体型号待确认） |
| 读卡 | MFRC522 + Mifare S50 |
| 显示 | OLED I2C |
| 执行 | 继电器模拟门锁 |
| 开发环境 | VS Code + PlatformIO |
| 代码托管 | GitHub Private |

## 开发环境（Windows / Mac）

本仓库在 **Windows 与 Mac** 之间切换开发，工具链统一为 VS Code + PlatformIO。

### 通用步骤

1. 安装 [VS Code](https://code.visualstudio.com/) 与 PlatformIO 插件
2. 安装并登录 GitHub CLI：`gh auth login`（协议选 HTTPS）
3. 同步代码：

```bash
git clone <本仓库地址>
cd <仓库目录>
```

4. 编译：

```bash
pio run
```

5. 烧录 / 监视（端口因机器而异，勿写死进仓库）：

```bash
pio run -t upload
pio device monitor
```

### 协作约定

- 开工先 `git pull`
- 改完 `commit` 后 `push`
- `upload_port` / `monitor_port` 各机器本地处理，不提交
- 仓库默认 **Private**

### 端口差异

| 系统 | 串口示例 |
|------|----------|
| Windows | `COM3` |
| macOS | `/dev/cu.usbmodem*` / `/dev/cu.usbserial*` |

## 目录结构

```text
├── README.md
├── .gitignore
├── .gitattributes
├── src/          # 固件源码（PlatformIO）
├── include/      # 头文件
├── lib/          # 项目私有库
├── docs/         # 计划、接线、答辩材料
└── hardware/     # 原理图 / BOM（可选）
```

## 开发进度

| 阶段 | 内容 | 状态 |
|------|------|------|
| 0 | 环境与仓库 | 进行中 |
| 1 | 需求与方案 | 未开始 |
| 2 | 读卡模块联调 | 未开始 |
| 3 | 白名单鉴权与控门 | 未开始 |
| 4 | 管理与记录 | 未开始 |
| 5 | 文档与答辩 | 未开始 |

## 组员

（待补充）
