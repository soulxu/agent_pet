# agent_pet

把一台 M5Stack 设备变成一只"AI agent 宠物":用**颜色**表达 agent 状态,同时它是一个
**蓝牙键盘**,按一下机身按键就能敲快捷键、批准 agent 的原生 approve。

支持两种设备 (同一份固件, 编译 target 不同):

- **M5StickS3** (有屏, 全功能): 两颗大眼睛 + 颜色表达 agent 状态, 走 WiFi mTLS 拉
  relay 状态; 蓝牙名 **AgentPet**。眼睛状态色:
  - 黄 = 忙碌 (agent 正在干活)
  - 红 = 等待批准 (检测到卡在原生/企业强制 approve) + 自动把 Cursor 切到前台
  - 绿 = 空闲 / 完成
  - 蓝 = WiFi 配网中
- **M5Atom Lite** (单键 + RGB LED, 无屏): **纯蓝牙键盘**, 不联网、不报 agent 状态;
  蓝牙名 **AgentPet-AtomLite**。LED 只做配对指示:**蓝呼吸 = 待配对 / 绿呼吸 = 已连接**。
  > Atom Lite 没有 PSRAM, BLE + WiFi 共存会把内部 RAM 吃光导致 TLS 握手失败, 所以它
  > 只做键盘 (不需要跑 relay / 生成证书, 烧完直接当蓝牙键盘用)。

先支持 **Cursor**,架构可扩展到 Claude Code / Codex(状态上报)。

## 架构 (两条独立通道, 各管各的)

```mermaid
flowchart LR
  subgraph mac [Mac]
    cursor[Cursor] -->|"hooks 上报 (HTTP 回环)"| relay[relay.py]
    relay -->|AX 检测 approve UI| cursor
    relay -->|osascript 切前台| cursor
  end
  relay -->|"HTTPS + mTLS: GET /state"| eyes[StickS3 眼睛]
  kbd[StickS3 蓝牙键盘] -->|"HID: 回车 / Esc"| cursor
```

- **蓝牙**:StickS3 是个**纯 HID 键盘**,macOS 当普通蓝牙键盘连。按键直接吐键,
  零依赖、最稳 —— 避开了 "HID + 自定义 GATT 共存" 在 macOS 上的各种坑。
  - StickS3: BtnA 单击 = **回车**, BtnA 长按1秒 = **Shift+回车**, BtnB 单击 = **Esc**
  - Atom Lite (单键): 单击 = **回车**, 长按1秒 = **Shift+回车**, 双击 = **Esc**
- **WiFi (HTTPS + 双向 TLS)**:agent 状态走 WiFi。StickS3 连上局域网后, 定时向
  Mac 上 relay 的 `GET /state` 拉聚合状态驱动眼睛。这条链路是 **mTLS**:
  - StickS3 烧入客户端私钥+证书, relay 用它的公钥(证书)**强制验证设备身份**(没有
    正确证书的客户端 TLS 握手直接被拒) —— 即"公钥认证"。
  - StickS3 反过来用 relay 的自签证书当 CA 校验 server(本 core 的 `setInsecure()`
    会导致不发客户端证书, 所以必须走 CA 校验模式)。
  - relay 拆两个口:本地 `127.0.0.1:8799` 普通 HTTP 收 Cursor hook 上报(回环不出
    网卡);对外 `0.0.0.0:8443` 才是 HTTPS+mTLS, 只服务 StickS3 的 `/state`。
  - 证书由 `relay/gen_certs.sh` 用 openssl 一键生成 (EC P-256)。**server 证书的
    SAN 会写入运行 `gen_certs.sh` 时探测到的 Mac 局域网 IP**, 设备按 IP 连才能过
    校验 —— 所以 **Mac 的局域网 IP 变了 (换网络/DHCP), 要重跑 `gen_certs.sh` 并
    重烧固件**。

> 实现要点 (踩过的坑):
> - relay 的 TLS 握手放在每连接的工作线程里 (不能包在监听 socket 上, 否则一个慢
>   握手会卡死 accept, 表现为客户端 connection refused)。
> - 设备侧 mbedTLS 握手很吃栈, netTask 给了 48KB (16~20KB 会栈溢出, 握手失败)。
> - 轮询用持久 `WiFiClientSecure` 复用, 避免每次新建解析证书导致的堆泄漏。

### 审批检测 (relay 两层)

企业场景里 Cursor 可能强制人工 approve,hook 返回 `allow` 也绕不过。所以
**hook 一律放行、绝不拦截**,由 relay 判断"是否卡在审批",判到了把状态标红
(眼睛变红)并把 Cursor 切前台,你按 StickS3 的回车键即可批准:

1. **首选 AX** (`relay/ax_detect.py`):看 Cursor 的 Accessibility 树里有没有
   `Run` 之类的审批**按钮**(精确匹配按钮文案)。最准,需要"辅助功能"权限。
2. **回退启发式**:某命令 `before` 后超过阈值仍没 `after` 且对话静默 → 判等待。

## 目录

```
agent_pet/
  firmware/        固件 (Arduino / M5Unified), 一份代码两个 target
    firmware.ino   主循环: 状态显示 + 按键 + 蓝牙键盘 + WiFi
    eyes.*         眼睛动画引擎 (StickS3, 状态驱动颜色)
    status_led.*   RGB LED 灯 (Atom Lite, 蓝/绿呼吸表示配对/已连接)
    ble_kbd.*      纯 BLE HID 键盘 (回车 / Shift+回车 / Esc)
    net.*          WiFi + SoftAP 配网 + mTLS 轮询 relay /state (独立 task)
    certs.h        (gen_certs.sh 生成) 客户端私钥+证书, 不入库
    app_prefs.*    NVS 配置 (亮度/音效)
    setup.sh       一键装工具链 (arduino-cli + ESP32 core + 依赖库 -> .toolchain/)
    flash.sh       编译 + 烧录 (TARGET=sticks3|atom; 缺工具链时自动调 setup.sh)
  hooks/           Cursor hook (全 allow + 上报活动)
  relay/           Mac 中枢 (HTTP 回环收 hook + HTTPS mTLS 出 /state)
    relay.py  ax_detect.py  gen_certs.sh  requirements.txt
    certs/         (gen_certs.sh 生成) server.* + client.crt, 不入库
  tools/           transcript_probe.py (兜底验证脚本)
  install.sh
```

## 安装与使用

### 1. Mac 侧

```bash
cd agent_pet
./install.sh            # 装 Cursor hooks + 依赖(pyobjc) + 默认配置
./relay/gen_certs.sh    # 生成 mTLS 证书 (relay/certs/ + firmware/certs.h)
python3 relay/relay.py  # HTTP 回环:8799 (hook) + HTTPS mTLS:8443 (StickS3)
```

> **顺序重要**:`gen_certs.sh` 必须在编译固件**之前**跑,因为它会生成
> `firmware/certs.h`(固件要 include)。证书/私钥都在 `.gitignore` 里,不入库;
> 换机器重新生成即可(生成新证书后要重新烧固件 + 重启 relay)。

relay 启动会打印 **Mac 局域网 IP** 和 HTTPS 端口(配网时要填)。推荐给运行 relay 的
终端授"辅助功能"权限(系统设置 → 隐私与安全性 → 辅助功能)以启用 AX 精准检测。

### 2. 编译环境 + 烧固件

固件用 **arduino-cli + ESP32(m5stack) core** 编译。`setup.sh` 会把工具链装成
**自包含**的一套(全在仓库内 `.toolchain/`,不污染系统、不动你的 `~/.arduino15`):

```bash
firmware/setup.sh        # 装 arduino-cli + m5stack:esp32@3.3.7 core + 依赖库
                         # (M5Unified / M5GFX / ArduinoJson). 幂等, 已装会跳过.
```

装好后直接编译烧录(**`flash.sh` 检测到没有工具链会自动调 `setup.sh`**,所以也可
跳过上一步直接烧):

```bash
firmware/flash.sh                 # StickS3: 进入下载模式 (长按机身侧 reset ~2 秒)
TARGET=atom firmware/flash.sh     # Atom Lite: 插上即可; 失败就按住主键再插 USB
firmware/flash.sh build           # 只编译不烧录
```

> - `setup.sh` 会下载 m5stack 工具链(约 300MB,含编译器),首次较慢;装在
>   `.toolchain/`(已 gitignore)。已有 `arduino-cli` 可用 `ARDUINO_CLI=/path/to/arduino-cli
>   firmware/setup.sh` 复用,省去下载 cli。
> - Atom Lite 是纯键盘, **不需要先跑 `gen_certs.sh`**(它不联网, 不 include `certs.h`)。
>   烧完跳到第 4 步连蓝牙即可。StickS3 才需要证书 + 配网。

### 3. StickS3 配网 (WiFi, 仅 StickS3)

没有 WiFi 配置时自动开热点 **AgentPet-XXXX**:
1. 手机/电脑连这个热点
2. 浏览器打开 <http://192.168.4.1>
3. 填:WiFi 名称、WiFi 密码、**Relay 地址**(填 relay 打印的 `Mac IP:8443`,注意是
   HTTPS 端口 **8443**)
4. 保存 → 设备重启并连 WiFi,经 mTLS 轮询 relay,眼睛动起来

以后想改配置:长按 BtnB(1 秒)重进配网,或联网后浏览器直接打开屏幕右下角那个 IP。
想**彻底重置 WiFi**(忘记已存的网络/Relay):**BtnB 一直按住到 3 秒**,或在配网页点
**"重置 WiFi 配置"** 按钮 —— 都会清空 NVS 里的配置并重启,回到空白配网页重新填。

### 4. 连蓝牙键盘

系统设置 → 蓝牙,把设备当键盘连接(StickS3 = **AgentPet**,Atom Lite =
**AgentPet-AtomLite**)。若弹出"键盘设置助理"要求按键识别,用你的**内置键盘**按它
要求的键完成(或关掉它,按默认 ANSI)。Atom Lite 连上后 LED 由蓝呼吸转为绿呼吸。

> **重新配对(换 Mac / 配对错乱 / "点连接就消失")**:StickS3 上**同时按住
> BtnA + BtnB 约 1.5 秒**,设备会断开当前连接、清掉本机侧旧配对密钥并重新开始广播
> (屏幕右上角提示"重新配对")。再去 macOS 蓝牙设置里把旧的 **AgentPet** "移除此设备",
> 然后重新连接即可干净重配。(Atom Lite 无屏单键,清配对走开机 `AGENTPET_CLEAR_BONDS`
> 专用固件,见 `firmware.ino`。)

### 5. 在 Cursor 里对齐快捷键

确保 Cursor 审批的"运行/接受"快捷键是**回车**(默认通常就是),取消是 **Esc**。
这样 StickS3 的 BtnA/BtnB 直接对应。

## 按键

**StickS3** (双键):

| 按键 | 功能 |
|------|------|
| BtnA 单击 | 敲**回车** → 批准 |
| BtnA 长按1秒 | 敲 **Shift+回车** → 批准 (场景二) |
| BtnB 单击 | 敲 **Esc** → 取消/拒绝 |
| BtnB 长按 1 秒 | 进入 WiFi 配网 (SoftAP) |
| **BtnB 继续按住到 3 秒** | **重置 WiFi**(清空已存的 WiFi/Relay 配置并重启 → 空白配网) |
| **BtnA + BtnB 短按一下** | **切换蓝牙主机**(BT1 ↔ BT2 循环,连到另一台 Mac) |
| **BtnA + BtnB 同按 1.5 秒** | **重新蓝牙配对**(断开 + 清本机配对记录 + 重新广播) |

顶栏三个小点:HID(绿=蓝牙键盘已连) / WiFi(蓝=已联网) / relay(白=拿到状态)。
小点右边 `BT1`/`BT2` = 当前连的是哪台主机(绿=该台已连,灰=未连)。

#### 复杂手势详解 (StickS3 维护操作)

下面三个是**按住时长 / 组合键**触发的维护手势,屏幕都会有提示,按时长升级:

| 手势 | 屏幕提示 | 做了什么 | 什么时候用 | 之后还要做 |
|------|----------|----------|------------|------------|
| **BtnB 长按 1 秒** | `配网中/续按重置` | 开 SoftAP 热点 `AgentPet-XXXX`,进入配网页(表单**带回原值**) | 想改 WiFi 名/密码 或 Relay 地址 | 手机/电脑连热点 → 浏览器开 `http://192.168.4.1` 改完保存 |
| **BtnB 一直按住到 3 秒** | 先 `配网中/续按重置`,到 3 秒变 `WiFi 重置` | 清空 NVS 里的 WiFi/Relay 配置并**重启**,开机回到**空白**配网页 | 换网络、配错了想从头来、忘记旧网络 | 重启后照"配网"步骤重新填一遍 |
| **BtnA + BtnB 短按一下** | `BT1` / `BT2` | 断开当前 Mac → 切到另一个**主机槽**(独立蓝牙身份)→ 重新广播,那台 Mac 自动重连 | 在两台 Mac 间来回切 | 第一次用某槽要先在那台 Mac 上配一次对(见下) |
| **BtnA + BtnB 同按 1.5 秒** | `重新配对` | 断开当前蓝牙连接 + 清掉**全部主机槽**的配对密钥 + 重新开始广播 | 配对错乱、"在系统蓝牙里点连接就消失" | 到 macOS 蓝牙设置把旧的 **AgentPet** "移除此设备",再重新连接 |

> 说明:
> - **BtnB 是分级长按**——松手早(1~3 秒间)只进配网、不清配置;按住够久(≥3 秒)才清。
>   所以"只想改配置"别按过 3 秒。
> - **A+B 区分长短**——快按快松(切主机)/ 按住 1.5 秒(重新配对)。短按要两键基本同时按下。
> - **重新蓝牙配对**会清掉**所有主机槽**的配对,且只能清设备这一侧,macOS 那侧的旧记录还得
>   手动"移除此设备"才算干净重配(蓝牙双向绑定的固有限制)。
> - 以上动作都**不影响** WiFi/relay 那条通道(反之 WiFi 操作也不动蓝牙)。

#### 多主机蓝牙 (Easy-Switch,2 台)

StickS3 像 Logitech/Keychron 那样支持**在 2 台 Mac 间一键切换**(`A+B` 短按),顶栏 `BT1`/`BT2`
显示当前是哪台:

- 原理:每个主机槽用一个**独立的蓝牙身份**(不同的静态随机地址),所以**每台 Mac 都要各自
  配一次对**——在 Mac A 上对着 `BT1` 配,切到 `BT2` 再到 Mac B 上配。配过之后,切到哪个槽,
  对应那台 Mac 就会自动重连(切换 + 重连约 1~3 秒,正常蓝牙速度)。
- 上限 **2 台**(受预编译蓝牙库 `CONFIG_BT_NIMBLE_MAX_BONDS=3` 约束)。当前槽记在 NVS 里,
  重启后保持上次那台。
- 仅 **StickS3**(NimBLE)支持;Atom Lite(Bluedroid 单键)是单主机,不参与。
- 升级到本版本后,因为蓝牙地址方案变了,**原来配过的需要重新配一次对**(一次性)。

**Atom Lite** (单键, 纯键盘):

| 操作 | 功能 |
|------|------|
| 单击 | 敲**回车** → 批准 |
| 长按1秒 | 敲 **Shift+回车** → 批准 (场景二) |
| 双击 | 敲 **Esc** → 取消/拒绝 |

LED:**蓝色呼吸 = 待配对 / 绿色呼吸 = 已连接**。(Atom Lite 不联网, 不显示 agent 状态。)

## 配置

`~/.config/agent_pet/config.json` (relay + hook 共用):

```json
{
  "url": "http://127.0.0.1:8799",
  "activity": true,
  "app_name": "Cursor",
  "detect_mode": "auto",
  "wait_threshold_ms": 2500,
  "activate_on_wait": true
}
```

- `url`: hook 上报地址 (回环, 固定)
- `detect_mode`: `auto`(AX 优先,不可用回退启发式) / `ax` / `heuristic` / `off`
- `app_name`: 要检测/切前台的应用名 (默认 Cursor)
- `ax_keywords`: 额外的审批按钮文案 (精确匹配, 追加到内置的 `run` 等)

> 按键敲的是固件里写死的回车/Esc, 不在这里配。WiFi 配置存在 StickS3 的 NVS 里
> (配网页填), 不在这个文件。

## 验证脚本

```bash
# AX 能否看到 Cursor 的审批按钮 (审批弹出时跑)
python3 relay/ax_detect.py buttons          # 列出当前所有按钮
python3 relay/ax_detect.py watch            # 实时看 approve_ui 是否命中
python3 relay/ax_detect.py dump --grep Run

# 兜底: transcript JSONL 能否区分悬空 tool 调用
python3 tools/transcript_probe.py --auto

# mTLS 自测 (relay 跑着时): 带证书应返回 JSON, 不带证书应被拒
curl -sk --cert relay/certs/client.crt --key relay/certs/client.key \
  https://127.0.0.1:8443/state
curl -sk https://127.0.0.1:8443/state   # 应失败 (exit 35)
```

## 排错

- 眼睛不动:顶栏 WiFi 点是不是蓝的?relay 点是不是白的?
  - WiFi 蓝但 relay 不白:
    - relay 地址/端口不对(要 `IP:8443`,HTTPS 端口);
    - **换了网络 / Mac 局域网 IP 变了** —— server 证书 SAN 绑了旧 IP, 重跑
      `gen_certs.sh` + 重烧固件 + 重启 relay;
    - 证书不匹配(重跑过 `gen_certs.sh` 但没重烧固件): 两边证书要同一套;
    - 想看具体 TLS 失败原因: `AGENT_PET_DEBUG=1 python3 relay/relay.py` 看 relay 端,
      或串口看设备端 `[net] GET 失败 ...`。
  - 都不亮:WiFi 没连上,长按 BtnB 重新配网。
- 按键没反应:顶栏 HID 点是不是绿的?系统设置里 AgentPet 是不是当键盘连上了?
  "键盘设置助理"完成了吗?Cursor 的审批快捷键是不是回车?
- 一直不变红:`detect_mode=auto` 下没授辅助功能权限会回退启发式;用
  `ax_detect.py watch` 确认 AX 能否命中,不行调 `wait_threshold_ms`。

## 卸载

```bash
./install.sh --uninstall
```
