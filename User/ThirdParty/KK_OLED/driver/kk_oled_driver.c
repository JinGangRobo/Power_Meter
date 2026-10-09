/* kk_oled_driver.c —— KK_OLED 在本工程的硬件适配层
 *
 * 本文件是 KK_OLED 的固定硬件适配边界，本工程与显示相关的硬件事实集中在这里：
 *
 *   MCU / SDK   STM32C011F6P6 + STM32C0xx HAL
 *   总线        I2C1（hi2c1）
 *   互斥        I2C1 与 INA228 共用，靠 app.c 的仲裁接口
 *               I2C_TryAcquire() / I2C_Release() 保证同一时刻只有一个设备收发
 *   控制器      SSD1306，128x64，1 bit 页式显存，可见列偏移 0
 *   从机地址    7 位 0x3C（public_config.h 的 OLED_I2C_ADDR），
 *               HAL 使用左移一位后的 0x78
 *   刷新方式    仅阻塞；IT / DMA 返回 OLED_UNSUPPORTED
 *
 * 事实来源：
 *   - 从机地址、控制字节、初始化序列、页寻址命令格式来自本工程原来那份自写的
 *     SSD1306 驱动（原 User/legacy_oled/，已在本板实机验证过，改版时已删除）；
 *   - 物理分辨率 128x64、8 页沿用那份驱动的 OLED_WIDTH / OLED_HEIGHT / OLED_PAGES。
 *
 * 未实现 IT / DMA 的原因：
 *   I2C1 的中断状态机被 INA228 的两步式（先发寄存器地址、再收数据）读写占用，
 *   回调在 app.c 中按 i2c_owner 分发；DMA 通道在 CubeMX 中只配了 USART1/2 的 TX，
 *   没有 I2C1 的 DMA 请求。按 KK_OLED driver 契约，这里保留阻塞实现，
 *   让异步入口返回 OLED_UNSUPPORTED，而不是用阻塞调用冒充异步。
 */

#include "kk_oled_driver.h"

#include "app.h"            /* I2C 总线仲裁：I2C_TryAcquire / I2C_Release */
#include "i2c.h"            /* hi2c1 */
#include "kk_oled_internal.h"
#include "public_config.h"  /* OLED_I2C_ADDR */

/** HAL 使用左移一位后的 8 位形式设备地址（0x3C -> 0x78）。 */
#define OLED_I2C_ADDRESS ((uint16_t)(OLED_I2C_ADDR << 1U))
/** I2C Memory Address 字节：后续内容为 SSD1306 命令。 */
#define OLED_CONTROL_COMMAND 0x00U
/** I2C Memory Address 字节：后续内容为显示数据。 */
#define OLED_CONTROL_DATA 0x40U
/** 本模组可见第 0 列相对 SSD1306 内部显存的列偏移，本板为 0。 */
#define OLED_COLUMN_OFFSET 0U
/** 所有阻塞 I2C 调用的最长等待时间。 */
#define OLED_BLOCKING_TIMEOUT_MS 100U
/** 争用共用 I2C 总线时的最长等待时间，和原 OLED_BusLock() 保持一致。 */
#define OLED_BUS_LOCK_TIMEOUT_MS 50U

static volatile bool oled_driver_busy; /**< 阻塞传输期间为 true。 */
static uint8_t oled_driver_command[3]; /**< 页号、列低位、列高位命令。 */

/** 将 STM32 HAL 状态转换为库的统一状态。 */
static OLED_Status oled_hal_status(HAL_StatusTypeDef status)
{
    if (status == HAL_OK) {
        return OLED_OK;
    }
    if (status == HAL_BUSY) {
        return OLED_BUSY;
    }
    return OLED_ERROR;
}

/** 取共用 I2C 总线：INA228 正在采样时最多等 OLED_BUS_LOCK_TIMEOUT_MS。 */
static bool oled_bus_acquire(void)
{
    uint32_t start = HAL_GetTick();

    while (I2C_TryAcquire(I2C_OWNER_OLED) == 0u) {
        if ((int32_t)(HAL_GetTick() - start) >= (int32_t)OLED_BUS_LOCK_TIMEOUT_MS) {
            return false;
        }
    }
    return true;
}

/** 通过控制字节 0x00 阻塞发送一组 SSD1306 命令。 */
static HAL_StatusTypeDef oled_send_command_blocking(const uint8_t *command,
                                                    uint16_t length)
{
    HAL_StatusTypeDef status;

    if (!oled_bus_acquire()) {
        return HAL_BUSY;
    }
    status = HAL_I2C_Mem_Write(&hi2c1, OLED_I2C_ADDRESS, OLED_CONTROL_COMMAND,
                               I2C_MEMADD_SIZE_8BIT, (uint8_t *)command, length,
                               OLED_BLOCKING_TIMEOUT_MS);
    I2C_Release();
    return status;
}

/** 通过控制字节 0x40 阻塞发送连续显存数据。 */
static HAL_StatusTypeDef oled_send_data_blocking(const uint8_t *data,
                                                 uint16_t length)
{
    HAL_StatusTypeDef status;

    if (!oled_bus_acquire()) {
        return HAL_BUSY;
    }
    status = HAL_I2C_Mem_Write(&hi2c1, OLED_I2C_ADDRESS, OLED_CONTROL_DATA,
                               I2C_MEMADD_SIZE_8BIT, (uint8_t *)data, length,
                               OLED_BLOCKING_TIMEOUT_MS);
    I2C_Release();
    return status;
}

/** 根据当前页的首个差异列生成三字节页寻址命令。 */
static void oled_prepare_page_command(uint8_t page, uint8_t min_x)
{
    uint8_t visible_column = (uint8_t)(min_x + OLED_COLUMN_OFFSET); /* 加上列偏移后的控制器列号。 */

    /* SSD1306 页寻址模式分别设置页号、列地址低四位和高四位。 */
    oled_driver_command[0] = (uint8_t)(0xB0U | page);
    oled_driver_command[1] = (uint8_t)(visible_column & 0x0FU);
    oled_driver_command[2] = (uint8_t)(0x10U | (visible_column >> 4U));
}

/**
 * 等待屏幕上电稳定，发送初始化序列，逐页清零后再点亮。
 * 初始化阶段使用阻塞调用，保证返回时屏幕处于确定状态。
 */
OLED_Status OLED_DriverInit(void)
{
    /* 本板实机验证过的 SSD1306 初始化序列：首字节 AE 保持显示关闭，
     * 最后一步在清屏成功之后才单独发送 AF。 */
    static const uint8_t init_commands[] = {
        0xAEU,        /* 关显示 */
        0xD5U, 0x80U, /* 时钟分频 */
        0xA8U, 0x3FU, /* 多路复用比 64 */
        0xD3U, 0x00U, /* 显示偏移 0 */
        0x40U,        /* 显示起始行 0 */
        0xA1U,        /* 段重映射 */
        0xC8U,        /* COM 扫描方向 */
        0xDAU, 0x12U, /* COM 引脚配置 */
        0x81U, 0xCFU, /* 对比度 */
        0xD9U, 0xF1U, /* 预充电周期 */
        0xDBU, 0x30U, /* VCOMH 电压 */
        0xA4U,        /* 按 RAM 内容显示 */
        0xA6U,        /* 正常显示，不反白 */
        0x8DU, 0x14U  /* 开电荷泵 */
    };
    static const uint8_t zeros[OLED_PHYSICAL_WIDTH] = {0}; /**< 初始化清屏数据。 */
    uint8_t page;                                          /**< 当前清零页。 */

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    oled_driver_busy = true;
    HAL_Delay(OLED_POWERON_DELAY_MS);

    if (oled_send_command_blocking(init_commands, sizeof(init_commands)) != HAL_OK) {
        oled_driver_busy = false;
        return OLED_ERROR;
    }
    /* SSD1306 采用页寻址，必须逐页设置地址并写入 128 个零字节。 */
    for (page = 0U; page < OLED_PHYSICAL_PAGES; ++page) {
        uint8_t command[3];

        oled_prepare_page_command(page, 0U);
        command[0] = oled_driver_command[0];
        command[1] = oled_driver_command[1];
        command[2] = oled_driver_command[2];
        if (oled_send_command_blocking(command, sizeof(command)) != HAL_OK ||
            oled_send_data_blocking(zeros, sizeof(zeros)) != HAL_OK) {
            oled_driver_busy = false;
            return OLED_ERROR;
        }
    }
    {
        /* 所有页清零成功后才发送 AF，避免上电随机画面。 */
        const uint8_t display_on = 0xAFU;
        if (oled_send_command_blocking(&display_on, 1U) != HAL_OK) {
            oled_driver_busy = false;
            return OLED_ERROR;
        }
    }
    oled_driver_busy = false;
    return OLED_OK;
}

/** 按页阻塞发送核心生成的连续差异区间。 */
OLED_Status OLED_DriverWriteBlocking(void)
{
    const uint8_t *buffer = OLED_InternalGetTransferBuffer(); /**< 冻结的传输帧。 */
    uint8_t page;                                             /**< 当前检查或发送的物理页。 */

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    oled_driver_busy = true;
    for (page = 0U; page < OLED_PHYSICAL_PAGES; ++page) {
        uint8_t min_x = OLED_InternalGetTransferMinX(page); /**< 本页首个差异列。 */
        uint8_t max_x = OLED_InternalGetTransferMaxX(page); /**< 本页最后差异列。 */
        uint16_t length;                                    /**< 本页需要连续发送的字节数。 */

        if (min_x >= OLED_PHYSICAL_WIDTH) {
            continue;
        }
        /* 每个脏页先重新定位，再从 min_x 连续写到 max_x。 */
        oled_prepare_page_command(page, min_x);
        length = (uint16_t)max_x - (uint16_t)min_x + 1U;
        if (oled_send_command_blocking(oled_driver_command,
                                       sizeof(oled_driver_command)) != HAL_OK ||
            oled_send_data_blocking(buffer + (uint16_t)page * OLED_PHYSICAL_WIDTH + min_x,
                                    length) != HAL_OK) {
            oled_driver_busy = false;
            return OLED_ERROR;
        }
    }
    oled_driver_busy = false;
    return OLED_OK;
}

/* 本工程 I2C1 的中断状态机由 INA228 占用，未实现异步刷新。
 * 按 driver 契约返回 OLED_UNSUPPORTED，不用阻塞调用冒充异步。 */

OLED_Status OLED_DriverWriteIT(void)
{
    return OLED_UNSUPPORTED;
}

OLED_Status OLED_DriverWriteDMA(void)
{
    return OLED_UNSUPPORTED;
}

bool OLED_DriverIsBusy(void)
{
    return oled_driver_busy;
}

OLED_Status OLED_DriverSetContrast(uint8_t value)
{
    const uint8_t command[2] = {0x81U, value}; /* 对比度命令及参数。 */

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    return oled_hal_status(oled_send_command_blocking(command, sizeof(command)));
}

OLED_Status OLED_DriverSetPowerSave(bool enable)
{
    uint8_t command = enable ? 0xAEU : 0xAFU; /* AE 关屏，AF 开屏。 */

    if (oled_driver_busy) {
        return OLED_BUSY;
    }
    return oled_hal_status(oled_send_command_blocking(&command, 1U));
}

void OLED_DriverHandleMemTxComplete(void)
{
    /* 本工程不使用 KK_OLED 的异步刷新，没有需要推进的状态机。 */
}

void OLED_DriverHandleError(void)
{
    /* 本工程不使用 KK_OLED 的异步刷新，阻塞传输的错误已由返回值交回核心。 */
}
