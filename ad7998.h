/* =========================================================================
 * ad7998.h —— Analog Devices AD7997 (10-bit) / AD7998 (12-bit)
 *            8-channel I2C ADC 驱动接口
 *
 * 芯片概述（datasheet Rev. 0）：
 *   8 通道逐次逼近型 ADC，I2C 接口，12-bit（AD7998）/10-bit（AD7997），
 *   转换时间 2μs，自动关断（不转时仅 1~2μA）。
 *
 *   AD7998 datasheet 明确写着："The AD7997/AD7998 require an EXTERNAL
 *   reference that should be applied to the REFIN pin." ——
 *   芯片没有内部参考，用户必须接外部参考电压到 REFIN 引脚，
 *   范围 1.2V ~ VDD。本 demo 用 **4.096V 外部参考**。
 *
 * 设备地址（datasheet Table 6）：
 *   7-bit 格式: 010 A2 A1 A0 （MSB 3 bit 固定 010）
 *   版本 + AS 引脚电平（AS=AGND / VDD / Float）决定低 3 bit：
 *     AD7998-x + AS Float → 010 0000 = 0x20  ← 默认，最常见
 *     AD7998-0 + AS AGND   → 010 0001 = 0x21
 *     AD7998-0 + AS VDD    → 010 0010 = 0x22
 *     AD7998-1 + AS AGND   → 010 0011 = 0x23
 *     AD7998-1 + AS VDD    → 010 0100 = 0x24
 *   同一条 I2C 总线最多 5 颗（两个版本 × AS 三态）。
 *
 *   ⚠️ AD7998 只有一根 AS 引脚，不是 A0/A1/A2 三根！地址可组合的选项
 *   没有 XL9535 多。上总线最多 5 颗（两个版本各三档 AS）。
 *
 * 寄存器映射（datasheet Table 8 —— Address Pointer 的 P3..P0 选寄存器）：
 *     P3 P2 P1 P0  寄存器                   读/写  大小
 *     0000         Conversion Result         只读   16bit
 *     0001         Alert Status              R/W    8bit
 *     0010         Configuration             R/W   16bit
 *     0011         Cycle Timer               R/W    8bit
 *     0100~0110    DATALOW/DATAHIGH/Hysteresis for CH1~CH4
 *     1000~1110    同上，CH2~CH4
 *
 * 地址指针字节（每次 I2C 写事务的第 1 字节，datasheet Table 7）：
 *     C4 C3 C2 C1  P3 P2 P1 P0
 *     ─── Mode 2 命令位 ───  ── 寄存器选择 ──
 *
 *     C4..C1 = 0000 → 不触发转换，纯寄存器访问
 *     C4..C1 = 1xxx → 触发 VINx 转换（见 Table 26）
 *     C4..C1 = 0111 → 序列模式，按 Configuration Register 里的 D11..D4
 *                      通道位顺序扫描多个通道
 *
 * 工作模式（datasheet Page 28~30）：
 *   Mode 1: CONVST 引脚 —— 用户态难用，需要额外 GPIO 控制
 *   Mode 2: COMMAND MODE —— I2C 写操作本身就触发转换！
 *     1 次 START+addr+W+[C4..C1 + P3..P0]+STOP 就完成：
 *       → 芯片唤醒 → 开始 C4..C1 指定的通道转换（约 3μs）
 *       → 接着再读 2 字节就拿到结果
 *   Mode 3: Automatic Cycle —— Cycle Timer 自动触发转换
 *
 *   本驱动只实现 Mode 2（最适合用户态命令行 demo）。
 *
 * Conversion Result Register 16-bit（datasheet Table 13 + 14）：
 *   Byte 1 (First Read / MSB):
 *     D15    D14 D13 D12   D11  D10 D9 D8
 *     Alert  CH_ID2 CH_ID1 CH_ID0  MSB  B10  B9  B8
 *   Byte 2 (Second Read / LSB):
 *     D7..D0 = B7 B6 B5 B4 B3 B2 B1 B0
 *
 *   12-bit ADC 值 = (MSB << 11) | (B10..B0) = ((byte1 & 0x0F) << 8) | byte2
 *   CH_ID = (byte1 >> 4) & 0x7  → 000=CH1, ..., 111=CH8
 *   Alert_Flag = (byte1 >> 7) & 1
 *
 * 电压换算（外部参考 4.096V）：
 *   LSB = VREF / 4096 = 4.096 / 4096 = 0.001 V = 1 mV
 *   VIN = raw * VREF_V / 4096.0
 *
 * Configuration Register 16-bit（datasheet Table 9）：
 *   D15..D12: DONTC
 *   D11..D4 : CH8..CH1 通道选择位（1=选，D11=CH8, D4=CH1）
 *   D3      : FLTR       1=开启 SDA/SCL 滤波（上电默认 1）
 *   D2      : ALERT_EN   1=开启 ALERT 硬件功能
 *   D1      : BUSY/ALERT 配合 D2 决定 Pin 17 功能
 *   D0      : POLARITY   0=低有效 1=高有效
 *   上电默认 0x0008（只有 FLTR=1）
 *
 * 依赖：i2c_util.h / i2c_util.c —— 复用 i2c_open/read_reg/write_reg
 * ========================================================================= */

#ifndef AD7998_H
#define AD7998_H

#include <stdint.h>
#include <stdbool.h>
#include "i2c_util.h"

/* ---- 版本 ---- */
#define AD7998_VERSION      12      /* 12-bit ADC */
#define AD7998_NUM_CHANNELS 8

/* ---- 用户指定的外部参考电压 ----
 * 接 4.096V 高精度参考源到 REFIN 引脚，芯片只有外部参考（无内部）。
 * 改成别的值（如 2.5V / 3.3V）直接改这个宏即可。 */
#define AD7998_VREF_V       4.096

/* ---- 地址 ---- */

/* 地址固定高 3 bit + AS 引脚配置。常用值：
 *   AS Float (默认) → 0x20
 *   AD7998-0 + AS AGND → 0x21
 *   AD7998-0 + AS VDD  → 0x22
 *   AD7998-1 + AS AGND → 0x23
 *   AD7998-1 + AS VDD  → 0x24 */
#define AD7998_ADDR_DEFAULT       0x20
#define AD7998_ADDR_AS_AGND       0x21    /* AD7998-0 */
#define AD7998_ADDR_AS_VDD        0x22    /* AD7998-0 */
#define AD7998_ADDR1_AS_AGND      0x23    /* AD7998-1 */
#define AD7998_ADDR1_AS_VDD       0x24    /* AD7998-1 */

/* ---- 寄存器地址（Address Pointer 的 P3..P0）---- */

#define AD7998_REG_RESULT          0x00    /* 只读 16-bit */
#define AD7998_REG_ALERT_STATUS    0x01    /* R/W 8-bit   */
#define AD7998_REG_CONFIG          0x02    /* R/W 16-bit  */
#define AD7998_REG_CYCLE_TIMER     0x03    /* R/W 8-bit   */

/* ---- Address Pointer 的 C4..C1 命令位（Mode 2，datasheet Table 26）---- */

/**
 * @brief 构造 AD7998 Address Pointer 字节
 *
 * datasheet Table 7：低 4 bit = P3..P0（寄存器选择），高 4 bit = C4..C1（命令位）。
 * 供 Mode 2 单次转换 / 序列模式 / 寄存器访问等场景复用。
 *
 * @param[in] cmd_bits Mode 2 命令位（C4..C1，0~15）
 * @param[in] reg_sel  寄存器选择位（P3..P0，0~15）
 *
 * @return 组合后的 Address Pointer 字节
 */
static inline uint8_t ad7998_make_ptr(uint8_t cmd_bits, uint8_t reg_sel)
{
    return (uint8_t)(((cmd_bits & 0x0F) << 4) | (reg_sel & 0x0F));
}

/* 单次通道转换命令（channel 1~8，cmd = 1000 + channel-1 的 3-bit） */
#define AD7998_CMD_VIN(n)    (uint8_t)(0x08 | ((n - 1) & 0x07))   /* VIN1=0x08 .. VIN8=0x0F */

/* 序列模式命令：让 ADC 扫描 Config Register D11..D4 里选的所有通道 */
#define AD7998_CMD_SEQUENCE  0x07

/* ---- Configuration Register 位 ---- */

/* CH1~CH8 通道位（D4~D11） */
#define AD7998_CH1  (1u << 4)
#define AD7998_CH2  (1u << 5)
#define AD7998_CH3  (1u << 6)
#define AD7998_CH4  (1u << 7)
#define AD7998_CH5  (1u << 8)
#define AD7998_CH6  (1u << 9)
#define AD7998_CH7  (1u << 10)
#define AD7998_CH8  (1u << 11)

/* 便捷宏：用 0-bit 通道号（0..7）得到对应位 */
#define AD7998_CH_BIT(ch)  ((ch >= 0 && ch < 8) ? (1u << (4 + ch)) : 0u)

/* FLTR: 1=开启 SDA/SCL 滤波（上电默认 1） */
#define AD7998_CFG_FLTR      (1u << 3)

/* ALERT_EN / BUSY/ALERT / POLARITY：默认都关（全部 0） */

/* 上电默认 Config 值：0x0008（只有 FLTR 置 1） */
#define AD7998_CFG_POWERUP_DEFAULT  0x0008

/* ---- Conversion Result 解包 ---- */

/**
 * @brief 把 Conversion Result 里的 CH ID 字段（3-bit）转换成通道号（1~8）
 *
 * datasheet Table 13：CH ID = 000 → CH1, ..., 111 → CH8。
 * 即通道号 = CH ID + 1。
 *
 * @param[in] ch_id_bits Conversion Result byte1 的 bit[11:9]（取值 0~7）
 *
 * @return 通道号（1~8）
 */
static inline int ad7998_ch_id_to_chnum(int ch_id_bits)
{
    return ch_id_bits + 1;     /* 000=CH1, ..., 111=CH8 */
}

/* ---- 对外 API ---- */

/**
 * @brief 打开 AD7998 设备
 *
 * 内部调用 i2c_open，不验证芯片应答（第一次寄存器读写返回 ENXIO 才说明
 * 地址无应答）。addr 用本头文件里定义的 AD7998_ADDR_xxx 宏。
 *
 * @param[out] dev  待初始化的设备句柄
 * @param[in]  path I2C 设备节点路径，如 "/dev/i2c-1"
 * @param[in]  addr AD7998 7-bit I2C 地址（AS 引脚 + 版本决定，见文件头）
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int ad7998_open(i2c_dev_t *dev, const char *path, int addr);

/**
 * @brief 关闭 AD7998 设备句柄
 *
 * 等同于 i2c_close，可安全重复调用。
 *
 * @param[in,out] dev 设备句柄
 *
 * @return 无
 */
void ad7998_close(i2c_dev_t *dev);

/**
 * @brief 读取 AD7998 Configuration Register（16-bit）
 *
 * 按 datasheet Table 9 字段定义解析后原样返回，上层自行解码 FLTR、
 * ALERT_EN、CH8..CH1 位等。
 *
 * @param[in,out] dev 已打开的设备句柄
 *
 * @return 0~0xFFFF 寄存器值；-1 失败（errno 有效）
 */
int ad7998_read_config(i2c_dev_t *dev);

/**
 * @brief 写入 AD7998 Configuration Register（16-bit）
 *
 * cfg 低 16 位有效，D15..D12（datasheet 写 DONTC）忽略。
 * 典型用法：cfg = AD7998_CH1 | AD7998_CH3 | AD7998_CFG_FLTR。
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[in]     cfg 待写入的配置值
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int ad7998_write_config(i2c_dev_t *dev, uint16_t cfg);

/**
 * @brief Mode 2：触发单次通道转换并读回结果
 *
 * 内部时序（datasheet Page 29 Mode 2）：
 *   1) 写 1 字节 Address Pointer：[C4..C1=1000+(ch-1)][P3..P0=0000]
 *      → 芯片唤醒 + 采样 + 转换（约 3μs）
 *   2) 紧接着 i2c_read_reg(dev, 0x00, 2字节) 拿到 Conversion Result
 *
 * 所有 out_xxx 输出参数均可传 NULL（调用方只关心部分字段）。
 *
 * @param[in,out] dev      已打开的设备句柄
 * @param[in]     channel  通道号（1~8，对应 VIN1~VIN8）
 * @param[out]    out_raw  原始 12-bit ADC 值（0~4095），可传 NULL
 * @param[out]    out_ch   读到的 CH ID（应该 = channel），可传 NULL
 * @param[out]    out_alert Alert_Flag 位（0/1），可传 NULL
 * @param[out]    out_volts 换算后的电压（V），公式 = raw * VREF_V / 4096，可传 NULL
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int ad7998_read_channel(i2c_dev_t *dev, int channel,
                        int *out_raw, int *out_ch, int *out_alert,
                        double *out_volts);

/**
 * @brief Mode 2 序列模式：一次读取多个通道的转换结果
 *
 * 内部流程：
 *   1) write_config(cfg_mask | AD7998_CFG_FLTR) 设置通道序列（不影响 FLTR）
 *   2) 发 Sequence 命令（C4..C1=0111, P3..P0=0000）→ 触发全部通道转换
 *   3) N 次连续读 Conversion Result Register，每次拿到 1 通道结果
 *      （序列模式下读操作与芯片内部流水线同步，不需要逐通道再触发）
 *
 * @param[in,out] dev         已打开的设备句柄
 * @param[in]     config_mask 要转换的通道位掩码（如 AD7998_CH1 | AD7998_CH3）
 * @param[out]    out_raw_buf 原始 12-bit 值输出数组（长度 >= popcount(config_mask)）
 * @param[out]    out_ch_buf  对应通道号输出数组（元素值 1~8），可传 NULL
 * @param[out]    count       实际读回的通道数，可传 NULL
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int ad7998_read_sequence(i2c_dev_t *dev, uint16_t config_mask,
                         int *out_raw_buf, int *out_ch_buf, int *count);

#endif /* AD7998_H */
