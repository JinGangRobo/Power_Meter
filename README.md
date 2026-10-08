# TEST_power_meter_3

基于 STM32C011F6P6 的功率计固件。INA228 采集电压 / 电流 / 功率，SSD1306 OLED 本地显示，
两路串口对外输出（USART1 二进制帧给上位机程序，USART2 文本给人看），
另有一路 50Hz PWM 输出可接电调。

功率直接读取 INA228 内部算好的 POWER 寄存器；固件只做一次零点修正
（减掉 `V × 电流偏移`），不重新做 V×I 乘法。

```
MCU      STM32C011F6P6   Cortex-M0+ / 48MHz HSI / 32KB Flash / 6KB RAM / TSSOP20
工具链   Keil MDK 5 + ARMCLANG V6.23，-Os
代码量   Code 29204 + RO 3280 + RW 36 = 32520 字节（约 31.8KB Flash，余约 248 字节）
         RAM 5320 字节（含 KK_OLED 双缓冲 2KB、2KB 栈、512B 堆），余 824 字节
编译     Program Size 如上，0 Error 0 Warning
显示     KK_OLED（SSD1306 / I2C1 / 128x64 / 1bit 页式 / 阻塞刷新）
界面     KK_UI（首页 4 个图标 + 控制菜单 + 实时数据页 + 电调页 + 关于页），屏幕只有这一条路径
开机图   工程根目录 jgrobo.svg 的标志图形，离线转成 72×54 的 1bit 位图，停留 1.2 秒
编译选项 优化 -Os + 链接时优化(LTO) + MicroLIB —— 这三个开关合计省下约 4.4KB Flash
```

## 目录结构

```
Core/
  Inc/             CubeMX 生成的头文件
  Src/             CubeMX 生成的初始化代码；main.c 只在 USER CODE 区调用 App_Init / App_Task
User/
  Inc/             应用层头文件（字模 kk_pm_font.h、首页图标 kk_pm_icons.h、
                   开机图 kk_pm_splash.h、界面 kk_pm_ui.h）
  Src/             应用层实现（app / ina228 / key / led / esc /
                   字模 kk_pm_font / 开机图 kk_pm_splash / 界面 kk_pm_ui）
  ThirdParty/      第三方库的项目副本：KK_OLED 与 KK_UI，都参与构建
Drivers/           STM32C0xx HAL 驱动 + CMSIS（仅保留编译需要的部分）
MDK-ARM/           Keil 工程，以及现成的 hex
TEST_power_meter_3.ioc   CubeMX 工程配置
```

## 硬件

### 引脚

| 引脚 | 功能 | 说明 |
|---|---|---|
| PA0 / PA1 | USART1 TX / RX | 给上位机传二进制帧，115200 8N1 |
| PA2 / PA3 | USART2 TX / RX | 文本输出，115200 8N1；板上这两根接成了直连，已在 CubeMX 里打开引脚交换修正 |
| PA4 | LED1 | 低电平点亮 |
| PA5 | LED2 | 低电平点亮 |
| PA6 | KEY | 上升沿触发（按下接地），固件补内部上拉 |
| PA7 | KEY2 | 返回键，按下接地；固件配成普通输入 + 内部上拉，不用 EXTI |
| PB6 / PB7 | I2C1 SCL / SDA | INA228 与 OLED 共用这条总线 |
| PA8 | TIM3_CH3 | 电调 PWM 输出 |

### 外设配置

| 外设 | 配置 |
|---|---|
| 系统时钟 | HSI 48MHz，AHB / APB1 均不分频，Flash 等待 1 周期 |
| I2C1 | 快速模式，`Timing = 0x0090194B`，模拟滤波使能（数字滤波系数为 0，未生效） |
| USART1 / USART2 | 115200 8N1，中断使能 |
| TIM3 | 运行期由 `ESC_Init()` 重配为 50Hz（PSC=47，ARR=19999，即 1 格 = 1µs） |
| TIM1 | 运行期由 `INA228_UsNow()` 直配为 1MHz 自由运行计数器，作为 µs 时钟 |
| GPIO | PA4/PA5 推挽输出，PA6 为 `GPIO_MODE_IT_RISING` |
| DMA | DMA1_CH1/CH2 已配置并绑定到 USART1/2 的 TX，但代码使用阻塞发送，实际未使用 |

### 从机地址

| 器件 | 7 位地址 | 说明 |
|---|---|---|
| INA228 | 0x40 | A0 接 GND 为 0x40，接 VS 为 0x41 |
| SSD1306 | 0x3C | SA0 接地；HAL 使用左移一位后的 0x78 |

## 编译与下载

1. 需要 Keil MDK 5（含 ARMCLANG V6.23）以及器件包 **Keil.STM32C0xx_DFP 2.3.0**；
2. 用 Keil 打开 `MDK-ARM/TEST_power_meter_3.uvprojx`，直接编译。工程已配置 `-Os`；
3. 生成的 hex 在 `MDK-ARM/TEST_power_meter_3/TEST_power_meter_3.hex`，也可以直接用仓库里这一份；
4. 下载算法 `STM32C0x_32.FLM` 的配置保存在 **`.uvoptx`** 里，不要删除该文件，否则需要手动添加；
5. 命令行批量编译：
   ```
   UV4.exe -b TEST_power_meter_3.uvprojx -j0 -o build.log
   ```
   退出码 0=成功，1=有警告，2=有错误。Keil GUI 开着时命令行编译可能失败，先确认没有 `UV4.exe` 进程。

注意事项：
- 修改外设配置请在 CubeMX 里改完重新生成，直接手改 `Core/Src` 下的文件会在下次生成时被覆盖（例如 `usart.c` 的波特率）。
- CubeMX 重新生成后需要复查：`usart.c` 的波特率、`uvprojx` 里 User 组的文件清单与 `-Os`、下载算法。
- 从别处拷来的 `.c/.h` 可能是 GBK 编码，需要转成 UTF-8（本工程源文件为 UTF-8 with BOM）。

## 运行流程

```
startup → main() → HAL_Init() → SystemClock_Config() → MX_xxx_Init()
        → App_Init()            仅执行一次
        → while(1) { App_Task(); }
```

`App_Init()`：初始化 LED / 按键 / INA228（写 SHUNT_CAL + 读 DEVICE_ID 自检）/ OLED / 电调 PWM，
然后打印开机信息，并登记"开机 1 秒后做一次零点校准"。

`App_Task()` 每轮依次调用：

| 顺序 | 调用 | 作用 |
|---|---|---|
| 1 | `INA228_Task()` | 读芯片（中断配合），解码 + 滤波 |
| 2 | `OLED_Task()` | 空函数（当前直接写屏，无需周期任务） |
| 3 | `Key_Task()` | 按键状态机 |
| 4 | `LED_Task()` | 两个指示灯 |
| 5 | `ESC_Task()` | 电调 PWM 爬升 / 保持 / 故障回退 |
| 6 | 串口命令解析 | S / X / C，USART1 与 USART2 都收 |
| 7 | 开机零点校准 | 仅开机 1 秒后执行一次 |
| 8 | 按键事件处理 | 短按 / 长按 |
| 9 | 校准完成标志 | 取走并清零 |
| 10 | `App_Periodic()` | 峰值与保护灯 100ms、二进制帧 500Hz、文本 10Hz、刷屏 200ms |

程序分为两个执行上下文：主循环负责浮点运算、字符串拼接、刷屏等慢操作；
中断只负责搬字节和置标志。两者通过 `volatile` 变量通信。

## I2C 总线仲裁

INA228 与 OLED 挂在同一条 I2C 上，同一时刻只能有一个设备通信。`app.c` 中实现了一套抢占机制：

- `i2c_busy` + `i2c_owner`（NONE / INA228 / OLED）记录当前占用者；
- `I2C_TryAcquire()` 抢不到立即返回 0，不等待；
- INA228 使用 `_IT` 收发，中断回调按 owner 分发；OLED 为阻塞写，通过 `OLED_BusLock()` 有界等待 50ms 插空；
- 初始化阶段的 `I2C_WaitDone()` 带超时，保证总线上没有设备时也不会卡死开机。

## 对外接口

### USART1 —— 二进制帧（给上位机程序）

115200 8N1，常开，每 2ms 一帧（500Hz），每帧固定 12 字节，全部小端：

| 偏移 | 内容 | 类型 | 单位 |
|---|---|---|---|
| 0-1 | `0xAA 0x55` | 帧头 | |
| 2 | `0x46`（'F'） | 帧类型，全量帧 | |
| 3-4 | 电压 | uint16 | mV |
| 5-6 | 电流 | int16，有符号 | mA |
| 7-8 | 功率 | uint16 | 0.01W |
| 9-10 | 峰值功率 | uint16 | 0.01W |
| 11 | 校验 | 前 11 字节求和的低 8 位 | |

速率 12 × 500 = 6000 字节/秒，占 115200 带宽约 52%。

接收端处理：在字节流中查找 `AA 55`，收满 12 字节后用 SUM8 校验；校验失败丢弃该帧并重新找帧头。
负载中偶尔出现 `AA 55` 会被误判为帧头，由校验过滤，会自动恢复，无需转义。

字段量程上限（超出后在帧内饱和）：

| 字段 | 上限 |
|---|---|
| 电压 | 65.535V |
| 电流 | −32.768A ~ +32.767A |
| 功率 / 峰值功率 | 655.35W |

### USART2 —— 文本

115200 8N1，10Hz，每行以 `\r\n` 结束：

```
V=12.345V I=1.234A P=12.345W PK=23.456W
```

### 下行命令

USART1 与 USART2 均可接收，单字符命令：

| 命令 | 作用 |
|---|---|
| `S` | 启动电调 PWM |
| `X` | 停止电调 PWM |
| `C` | 重新做零点校准 |

接收方式为主循环每轮轮询 1 字节（超时 0），适合单字符命令，连续快速发送会丢字节。

## 人机交互

### 按键（PA6 + PA7）

| 按键 | 操作 | 界面里的作用 |
|---|---|---|
| KEY1（PA6） | 短按（<1 秒松手） | 下一个（菜单/首页下移一项） |
| KEY1（PA6） | 长按（满 1 秒） | 确认 / 进入 |
| KEY2（PA7） | 短按 | 返回上一页 |

原来的"短按清峰值 / 长按启停电调"没有丢：两项都进了控制菜单，
峰值清零是 `Peak clear`，电调启停是 `ESC output`。

### LED

LED1（PA4）：

| 现象 | 含义 |
|---|---|
| 1Hz 慢闪 | 正常，正在上传数据 |
| 单闪 200ms | 峰值清零 / PWM 启停的应答 |
| 常亮 | 保留状态，当前固件不产生 |

LED2（PA5），按优先级从高到低，只亮一个：

| 现象 | 含义 |
|---|---|
| 4Hz 快闪 | INA228 通信失败 |
| 常亮 | 电流超过 25A（正负都算） |
| 1Hz 慢闪 | 电压超过 40V |
| 熄灭 | 正常 |

### 屏幕

屏幕只有 KK_UI 这一条路径（旧的自写 4 行显示已经删除，见下面的「KK_UI 界面」）。
上电先显示 1.2 秒开机图片（`jgrobo.svg` 的标志图形，72×54 居中），随后进入首页图标选择器。

实时页显示：

```
V=12.345 M=40.00     第 1 行：当前电压 / 锁存的最大电压
I= 1.234 M=25.00     第 2 行：当前电流 / 锁存的最大电流（取绝对值，反灌也算）
████ P=12.345W ████  第 3 行：当前功率，反相圆角块突出显示
PK=23.456W           第 4 行：锁存的最大功率
```

第 3 行是整屏的视觉重点：功率是本机的核心读数，用实心圆角块反白显示。
第 4 行会被临时提示占用（`CAL OK` / `ESC ON` / `ESC OFF` / `PEAK CLEARED` 等），
2 秒后自动换回 PK。

电流和功率都做了零点修正：`I` 减掉校准时记下的电流偏移，`P` 再减掉 `V × 偏移` 这一份，
所以空载时两者都读 0。

## 电调 PWM

标准 50Hz 舵机 / 电调帧，脉宽即油门指令。启动后从 1000µs（零速点）线性递增到 1300µs，
每 10ms 递增 5µs（500µs/s），全程 0.6 秒，到顶后保持。

保护联动：电流超过 25A 或 INA228 通信失败时，按 2 倍步长回退到 1000µs 后彻底关闭输出。

## 参数

改参数只需要动 `User/Inc/public_config.h`：

| 宏 | 当前值 | 含义 |
|---|---|---|
| `INA228_SHUNT_R_OHM` | 0.001 | 采样电阻（欧姆） |
| `INA228_CURRENT_LSB_A` | 0.001 | 电流分辨率，1mA |
| `INA228_SHUNT_CAL` | 自动计算 | 由 LSB 与采样电阻推出，不需要手改 |
| `INA228_VBUS_LSB_UV` | 195.3125 | VBUS 寄存器 LSB |
| `INA228_SET_PERIOD_US` | 600 | 两轮连读之间的间隔；实际数据率还要加上一轮 3 次 I2C 传输的耗时 |
| `INA228_BURST_TIMEOUT_US` | 10000 | 连读单步超时；一次读只要几百 µs，超时说明这一笔没回来，作废本轮重新开始 |
| `INA228_FILTER_SAMPLES` | 8 | 滑动平均深度，必须是 2 的幂 |
| `OVERCURRENT_THRESHOLD_A` | 25.0 | 过流阈值（取绝对值） |
| `OVERVOLTAGE_THRESHOLD_V` | 40.0 | 过压阈值 |
| `KEY_DEBOUNCE_MS` | 20 | 消抖时间 |
| `KEY_LONG_PRESS_MS` | 1000 | 长按判定时间 |
| `LED_ACTIVE_HIGH` | 0 | 0 = 低电平点亮 |
| `ESC_PULSE_START_US` | 1000 | 电调起始脉宽 |
| `ESC_PULSE_MAX_US` | 1300 | 电调上限脉宽 |
| `ESC_RAMP_STEP_US` | 5 | 每 `ESC_TASK_PERIOD_MS` 的步进 |
| `UPLOAD_FULL_PERIOD_US` | 2000 | 二进制帧周期，500Hz |
| `UPLOAD_TEXT_PERIOD_MS` | 100 | 文本周期，10Hz |
| `DISPLAY_UPDATE_PERIOD_MS` | 200 | 刷屏周期 |
| `OLED_I2C_ADDR` | 0x3C | 屏地址 |
| `INA228_I2C_ADDR` | 0x40 | 芯片地址 |

## 模块说明

| 文件 | 职责 |
|---|---|
| `User/Src/app.c` | I2C 总线仲裁、HAL 回调分发、串口发送与二进制帧、显示渲染、周期调度、应用入口 |
| `User/Src/ina228.c` | INA228 驱动：中断收发状态机、寄存器解码、滑动平均滤波、零点校准、连读超时保护、µs 时钟 |
| `User/ThirdParty/KK_OLED/` | KK_OLED 图形库：图元、文字、位图、旋转、双缓冲差异提交 |
| `User/ThirdParty/KK_OLED/driver/kk_oled_driver.c` | 本工程的硬件适配层：SSD1306 + I2C1 + 总线仲裁 + 仅阻塞刷新 |
| `User/Src/kk_pm_font.c` | LEDFont API 生成的 14 号字模（界面实际用到的 60 字形 / 784 字节） |
| `User/Src/kk_pm_ui.c` | KK_UI 界面：首页 / 控制菜单 / 实时数据页 / 关于页、双按键输入归一化 |
| `User/Inc/kk_pm_icons.h` | 首页四个 32×32 XBM 图标（波形 / 齿轮 / 信息 / 方波），512 字节 |
| `User/Src/kk_pm_splash.c` | 开机图片：`jgrobo.svg` 的标志图形转成 72×54 的 1bit 位图，486 字节 |
| `User/Src/key.c` | 两个按键的状态机：消抖、短按 / 长按判定、事件队列、KEY1 的 EXTI 回调 |
| `User/Src/led.c` | 两个 LED 的状态合成与输出 |
| `User/Src/esc.c` | 电调 PWM：运行期配置 50Hz、线性爬升、保持、故障回退 |
| `Core/Src/stm32c0xx_it.c` | 中断入口，转交 HAL 处理函数，未作改动 |
| `Core/Src/i2c.c` 等 | CubeMX 生成的外设初始化 |

## 显示子系统（KK_OLED）

显示已经从自写驱动切到 **KK_OLED**（`User/ThirdParty/KK_OLED/`）：

- **驱动适配**：`driver/kk_oled_driver.c` 是本工程唯一的硬件适配边界，集中了 SSD1306 的
  7 位地址 0x3C、初始化序列、页寻址命令格式和列偏移（本板为 0）。这些事实来自原
  `User/Src/OLED.c`，该驱动已在本板实机验证过；
- **总线互斥**：每次阻塞传输前用 `I2C_TryAcquire(I2C_OWNER_OLED)` 抢总线（最多等 50ms），
  发完 `I2C_Release()`，与 INA228 的 `_IT` 采样共用 I2C1 而不打架；
- **刷新方式**：只有阻塞刷新。I2C1 的中断状态机被 INA228 的两步式读写（先发寄存器地址、
  再收数据）占用，CubeMX 也没有为 I2C1 配置 DMA，因此 `OLED_UpdateIT()` /
  `OLED_UpdateDMA()` 按 driver 契约返回 `OLED_UNSUPPORTED`，不用阻塞调用冒充异步；
- **差异提交**：KK_OLED 内部维护双缓冲，只把变化的页按列区间发给屏幕。原来 `app.c` 里
  手写的"逐字符比较、只重画变化字符"逻辑已经删除，应用层只负责描述这一帧画什么；
- **字模**：`User/Src/kk_pm_font.c` 由 LEDFont Agent API 按需生成，只含界面实际用到的
  可打印 ASCII（95 字形 / 1192 字节），编译期不依赖在线服务。许可说明见
  `User/Inc/kk_pm_font.h`。

换控制器、换总线或换分辨率时只需要改 `kk_oled_driver.c`，图形核心与应用层不动。

## KK_UI 界面

界面用 KK_UI 重做，**屏幕只有这一条路径**（旧的自写 4 行显示与 `User/legacy_oled/`
已经删除）。产品功能没有删减——两路串口、电调 PWM、峰值记录、零点校准全部保留，
只是把入口从"按键 + 串口命令"搬进了菜单：

```
User/ThirdParty/KK_UI/   KK_UI 运行时（可编辑项目副本）
User/Inc/kk_pm_ui.h      本工程的界面：首页 + 控制菜单 + 实时数据页 + 关于页
User/Src/kk_pm_ui.c      页面描述、事件处理、双按键输入归一化
User/Inc/kk_pm_icons.h   首页三个 32x32 XBM 图标（波形 / 齿轮 / 信息）
```

| 首页 / 菜单项 | 类型 | 对应原有功能 |
|---|---|---|
| Monitor（波形图标） | 自定义页 | 原来的 4 行实时显示：V / Vmax / I / Imax / P / PK |
| Control（齿轮图标） | 菜单 | 下面三项操作的入口 |
| About（信息图标） | 信息页 | 设备与驱动状态，含 INA228 通信状态 |
| PWM（方波图标） | 自定义页 | **PWM 输出直通入口**：大字显示通断，确定键直接切换 |
| Peak clear | 动作 | 原来**按键短按**的峰值清零 |
| Zero cal | 动作 | 原来串口 `C` 的零点校准 |
| PWM: ON / OFF | 动作 | 原来**按键长按** / 串口 `S`、`X` 的 PWM 输出启停，**标签跟着实际状态变** |

上电停在首页图标选择器。菜单还会自动追加一个返回项。按键：
KEY1（PA6）**短按 = 下一个、长按 = 确认进入**；KEY2（PA7）**短按 = 返回**。

三个动作都有串口回执（USART2，115200）：`PEAK CLEARED`、`CAL OK offset=..`、
`ESC ON (ramp 1000->1300us)` / `ESC OFF`，界面操作和串口命令的输出一致。

### 32KB Flash 下怎么装下的

全功能 KK_UI 直接编进来会溢出约 9KB。按本工程实际用到的模块裁剪、并打开两个编译器选项后
才装下：

| 措施 | 效果 |
|---|---:|
| 按实际用到的模块裁剪 KK_UI（关掉编辑器 / 确认框 / 消息框 / Toast） | 省约 8.8KB |
| 打开**链接时优化 `-flto`**（原为关闭） | 与下面合计省约 4.4KB |
| 打开 **MicroLIB**（原为关闭，工程不用 printf/malloc，安全） | 同上 |
| 字模从 95 字形（1192B）收缩为界面实际用到的 60 字形（784B） | 省 408B |
| 删掉界面层里重复的一份浮点格式化（改用 app.c 的实现） | 省 308B |
| 删掉旧的自写 4 行显示（连同 8×16 字库） | 约 400B |
| 移除**已配置但从未使用**的 DMA 通道 | 约 880B |

`KK_UI_ENABLE_*` 开关集中在 Keil 的 C/C++ → Define 里。**Flash 只剩约 220 字节**，
再加功能或打开更多模块前必须先评估。

### 移除 DMA 的注意事项

CubeMX 原本为 USART1_TX / USART2_TX 配了 DMA1 通道，但代码一直用阻塞发送，
通道从未使用（本文档早先也这么记着）。为了给界面和开机图腾空间，本轮把它移除了：

- `main.c` 去掉 `MX_DMA_Init()` 调用；
- `usart.c` 去掉 `hdma_usart1_tx` / `hdma_usart2_tx` 的 `HAL_DMA_Init`、`__HAL_LINKDMA`
  与 `HAL_DMA_DeInit`；
- `stm32c0xx_it.c` 去掉两个 DMA 中断入口；
- `usart.h` 去掉对应的 `extern`。

`Core/Src/dma.c` 与 HAL 的 DMA 源文件仍留在工程里，但已无人引用，会被链接器剔除。
**如果以后用 CubeMX 重新生成代码，`.ioc` 里还留着 DMA 配置，这些改动会被恢复**，
需要按上面的清单再删一次；或者直接在 CubeMX 里把两个 DMA 请求删掉再生成。

### 给 KK_UI 核心加的最小接口

KK_UI 原本没有"返回"输入，返回只能靠在菜单里选中自动追加的返回项。为了让 KEY2 直接
返回，在项目副本里加了一个公开函数（`kk_ui.h` / `kk_ui.c`）：

```c
KK_UI_Status KK_UI_NavigateBackRequest(void);
```

它只是把内部的 `KK_UI_NavigateBack()` 包一层，用最近一次 `KK_UI_Update()` 的时间基准，
没有改动任何页面布局或状态机。这是本工程对 KK_UI 核心唯一的改动。

## 已知限制

1. 开机约 1 秒后自动做一次零点校准，把当时的电流作为零点，因此开机时应空载。
   串口发 `C` 可随时重新校准。
2. 上传速率受串口带宽限制：115200 8N1 每秒最多 11520 字节，当前占用约 52%。
   如需继续提速，必须先提高波特率或缩短帧，否则发送会长时间占住主循环，
   采样和按键响应都会被拖慢。
3. I2C 为共用总线，INA228 优先。OLED 为阻塞写，字符变化较多时会推迟采样，
   表现为瞬时速率略降，不会丢数据。
4. INA228 连读带超时保护：某一笔读数超过 `INA228_BURST_TIMEOUT_US` 没回来
   （I2C 出错，或一直抢不到总线），就作废本轮并在下一轮重新开始，不会停死。
   出错瞬间 `comm_ok` 清零，LED2 会 4Hz 快闪提示，恢复后自动转正常。
   注意这只是软件超时，如果总线被从机拉死（SDA/SCL 一直为低），
   硬件层面还需要断电重启才能恢复。
5. INA228 的通信状态按"地址是否有应答"判定，不做 ID 匹配，`DEVICE_ID` 仅打印供参考。
6. 外设中断优先级均为 0，SysTick 为 `TICK_INT_PRIORITY 3`（最低），未做进一步分级；
   程序中没有看门狗。
7. DMA 通道已配置但未使用，发送走阻塞方式。若要提高上传速率，改用 DMA 发送是第一步。
8. USART2 的引脚交换在 CubeMX 中打开，用于修正板上 TX / RX 直连的问题，换板时需确认实际接法。

## 未使用的代码

以下函数与宏当前没有被调用或使用，保留备用：

```
LED1_SetReportActive()    INA228_IsDataReady()      INA228_GetCurrentRaw_A()
```

另外，`User/ThirdParty/KK_UI/` 与 `User/Src/kk_pm_ui.c` 目前只作为项目源码副本保留，
没有参与构建，原因和启用步骤见上面的「KK_UI 的现状」。
