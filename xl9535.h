/* =========================================================================
 * xl9535.h —— XL9535 / XL9555 16-bit I2C I/O 扩展器 驱动接口
 *
 * 芯片概述：
 *   XL9535/XL9555 是芯鲁达(Xinluda)的 I2C GPIO 扩展器，功能与 NXP
 *   PCA9535/PCA9555 完全一致。通过 I2C 总线给 MCU 额外提供 16 个 GPIO，
 *   分为 Port 0 (P00~P07) 和 Port 1 (P10~P17)。
 *
 * 电气参数（datasheet 关键值）：
 *   - 供电：2.3 V ~ 5.5 V（5V I/O 容忍）
 *   - I2C 速度：标准模式 100 kHz / 快速模式 400 kHz
 *   - 输出驱动：IOL 25 mA @ VOL=0.4 V（低电平强驱，拉电流 10 mA）
 *   - 中断：INT 引脚，开漏，输入变化时拉低
 *   - 上电默认：所有引脚为输入（高阻），Output 默认 0xFF
 *
 * 设备地址（7 位）：
 *     bit 7..4 = 0b0100（固定）
 *     bit 3..1 = A2 A1 A0（硬件引脚接 VCC 或 GND）
 *     bit 0    = R/W（由 I2C 控制器自动填入）
 *   全部接地 → 0x20；全部接 VCC → 0x27；总线上最多 8 颗。
 *
 * 寄存器映射（datasheet Table 6 —— Command Byte）：
 *     地址  名称                     读/写   上电默认
 *     0x00  Input Port 0             只读    XX（随引脚）
 *     0x01  Input Port 1             只读    XX
 *     0x02  Output Port 0            R/W     0xFF
 *     0x03  Output Port 1            R/W     0xFF
 *     0x04  Polarity Inversion Port 0 R/W    0x00
 *     0x05  Polarity Inversion Port 1 R/W    0x00
 *     0x06  Configuration Port 0      R/W     0xFF（1=输入）
 *     0x07  Configuration Port 1      R/W     0xFF（1=输入）
 *
 * 方向配置约定：
 *     Config bit = 1 → 输入（高阻，上电默认）
 *     Config bit = 0 → 输出
 *     跟 PCA95xx 系列一致，但**注意别跟其他厂商搞反**！
 *
 * 【危险！】自动对调机制（datasheet 9.6.1）：
 *   XL9535 有一个"寄存器对自动对调"的怪行为：
 *   如果连续写同一类寄存器的两个端口，第二个字节会被送到"对侧"寄存器。
 *   例：先写 Output Port 1 (reg 0x03)，再写 1 字节 → 该字节被送到
 *       Output Port 0 (reg 0x02)，**不是**预期的 Port 1！
 *   这是芯片硬件设计，不是 bug。
 *   为规避此坑，本驱动里：
 *     - 单独写某一个寄存器 → safe（每次 i2c_write_reg 只发 1 数据 + STOP）
 *     - 一次性写两个同类型寄存器 → 用专门的 16-bit API，内部已经处理了
 *       对调顺序（先发 Port 1 再发 Port 0 的数据，但寄存器指针反过来）
 *
 * 设计原则：
 *   - 上层用 i2c_dev_t + i2c_util.h 已有的基础 I2C 访问
 *   - 本头文件只封装 XL9535 特有的寄存器读写语义
 *   - 返回值统一：0 成功，-1 失败（设置 errno）
 * ========================================================================= */

#ifndef XL9535_H
#define XL9535_H

#include <stdint.h>
#include "i2c_util.h"

/* ---- 设备地址 ---- */

/* 基础地址（A2=A1=A0=GND）。其他组合 = XL9535_BASE + (A2<<2 | A1<<1 | A0) */
#define XL9535_BASE_ADDR   0x20

/**
 * @brief 根据 A2/A1/A0 地址引脚电平计算 7-bit I2C 地址
 *
 * XL9535 地址 = 0x20 + (A2<<2 | A1<<1 | A0)，结果范围 0x20~0x27。
 *
 * @param[in] a2 地址引脚 A2 电平（0=GND, 1=VCC）
 * @param[in] a1 地址引脚 A1 电平
 * @param[in] a0 地址引脚 A0 电平
 *
 * @return 计算得到的 7-bit 地址
 */
static inline int xl9535_addr(int a2, int a1, int a0)
{
    return XL9535_BASE_ADDR + (((a2 & 1) << 2) | ((a1 & 1) << 1) | (a0 & 1));
}

/* ---- 寄存器地址（datasheet Table 6） ---- */

#define XL9535_REG_INPUT0     0x00
#define XL9535_REG_INPUT1     0x01
#define XL9535_REG_OUTPUT0    0x02
#define XL9535_REG_OUTPUT1    0x03
#define XL9535_REG_POL0       0x04
#define XL9535_REG_POL1       0x05
#define XL9535_REG_CONFIG0    0x06
#define XL9535_REG_CONFIG1    0x07

/* ---- 方向配置约定 ---- */
#define XL9535_DIR_INPUT      1    /* 对应 config bit = 1 */
#define XL9535_DIR_OUTPUT     0    /* 对应 config bit = 0 */

/* ---- 初始化 / 反初始化 ---- */

/**
 * @brief 打开 XL9535 设备
 *
 * 内部调用 i2c_open，把 a2/a1/a0 拼合成地址。不验证芯片存在——
 * 第一次寄存器读写返回 ENXIO 才说明地址无应答。
 *
 * @param[out] dev  待初始化的设备句柄
 * @param[in]  path I2C 设备节点路径，如 "/dev/i2c-1"
 * @param[in]  a2   地址引脚 A2 电平（0=GND, 1=VCC）
 * @param[in]  a1   地址引脚 A1 电平
 * @param[in]  a0   地址引脚 A0 电平
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_open(i2c_dev_t *dev, const char *path, int a2, int a1, int a0);

/**
 * @brief 关闭 XL9535 设备句柄
 *
 * 等同于 i2c_close，可安全重复调用。
 *
 * @param[in,out] dev 设备句柄
 *
 * @return 无
 */
void xl9535_close(i2c_dev_t *dev);

/* ---- 方向配置 ---- */

/**
 * @brief 设置 Port 0 的方向掩码
 *
 * mask 每个 bit 对应一根引脚；1=输入(高阻，安全)，0=输出。
 * 例：0x0F 表示 P00~P03 输出，P04~P07 输入。
 *
 * @param[in,out] dev  已打开的设备句柄
 * @param[in]     mask 方向掩码
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_set_dir_port0(i2c_dev_t *dev, uint8_t mask);

/**
 * @brief 设置 Port 1 的方向掩码
 *
 * 语义同 xl9535_set_dir_port0，操作 XL9535_REG_CONFIG1 (0x07)。
 *
 * @param[in,out] dev  已打开的设备句柄
 * @param[in]     mask 方向掩码
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_set_dir_port1(i2c_dev_t *dev, uint8_t mask);

/**
 * @brief 同时设置两个端口的方向（一次事务完成）
 *
 * 内部利用芯片"自动对调"机制：先发 Config1 (0x07) + port1_mask，
 * 紧接着发 port0_mask → 芯片自动写入 Config0 (0x06)。
 * 比单独调用两次 set_dir_xxx 少一次 START/STOP。
 *
 * @param[in,out] dev        已打开的设备句柄
 * @param[in]     port0_mask Port 0 方向掩码（1=输入, 0=输出）
 * @param[in]     port1_mask Port 1 方向掩码
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_set_dir_both(i2c_dev_t *dev, uint8_t port0_mask, uint8_t port1_mask);

/* ---- 输出 ---- */

/**
 * @brief 设置 Port 0 的输出值
 *
 * 只对已配置为输出的引脚生效；输入引脚的 bit 在 Output 寄存器里写了也没用。
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[in]     val 输出值（0~255）
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_write_port0(i2c_dev_t *dev, uint8_t val);

/**
 * @brief 设置 Port 1 的输出值
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[in]     val 输出值（0~255）
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_write_port1(i2c_dev_t *dev, uint8_t val);

/**
 * @brief 同时设置两个端口的输出值（一次事务完成）
 *
 * 内部利用芯片自动对调：先发 Output Port 1 (0x03) + out1，
 * 紧接着发 out0 → 芯片自动写入 Output Port 0 (0x02)。
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[in]     out0 Port 0 (P00~P07) 的输出值
 * @param[in]     out1 Port 1 (P10~P17) 的输出值
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_write_outputs16(i2c_dev_t *dev, uint8_t out0, uint8_t out1);

/* ---- 输入 ---- */

/**
 * @brief 读 Port 0 的实际引脚电平
 *
 * 读 Input Port 0 (0x00)。无论是输入还是输出引脚，都能通过 Input Port 读到
 * 其实际电平（Output 引脚的电平和 Output Register 应该一致）。
 *
 * @param[in,out] dev 已打开的设备句柄
 *
 * @return 0~255 引脚电平；-1 失败（errno 有效）
 */
int xl9535_read_port0(i2c_dev_t *dev);

/**
 * @brief 读 Port 1 的实际引脚电平
 *
 * @param[in,out] dev 已打开的设备句柄
 *
 * @return 0~255 引脚电平；-1 失败（errno 有效）
 */
int xl9535_read_port1(i2c_dev_t *dev);

/**
 * @brief 同时读两个端口的引脚电平
 *
 * 由于芯片在读方向没有自动对调的便捷路径，需要两次独立 I2C 事务
 * （先设 reg 0x00 读 Port 0，再设 reg 0x01 读 Port 1）。
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[out]    in0 Port 0 读回值（可传 NULL）
 * @param[out]    in1 Port 1 读回值（可传 NULL）
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_read_inputs16(i2c_dev_t *dev, uint8_t *in0, uint8_t *in1);

/* ---- 极性反转 ---- */

/**
 * @brief 设置 Port 0 的极性反转掩码
 *
 * polarity bit = 1 时，输入读值会被取反（高电平读 0，低电平读 1）。
 * 默认全 0 = 正常极性。适合低电平有效的按键输入。
 *
 * @param[in,out] dev  已打开的设备句柄
 * @param[in]     mask 极性反转掩码（每 bit 对应一根引脚）
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_set_pol_port0(i2c_dev_t *dev, uint8_t mask);

/**
 * @brief 设置 Port 1 的极性反转掩码
 *
 * @param[in,out] dev  已打开的设备句柄
 * @param[in]     mask 极性反转掩码
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_set_pol_port1(i2c_dev_t *dev, uint8_t mask);

/* ---- 便捷：一次性初始化 16 个引脚的方向 ---- */

/**
 * @brief 一次性配置全部 16 个引脚的方向
 *
 * 底层调用 xl9535_set_dir_both，利用自动对调 一次事务搞定两个端口。
 *
 * @param[in,out] dev   已打开的设备句柄
 * @param[in]     dir16 16-bit 方向掩码：bit 0..7 = Port 0, bit 8..15 = Port 1。
 *                      1 = INPUT（高阻，安全默认），0 = OUTPUT。
 *
 * @return 0 成功，-1 失败（errno 有效）
 *
 * 示例：
 *   xl9535_configure_all(dev, 0x0000) → 全部输出
 *   xl9535_configure_all(dev, 0xFFFF) → 全部输入（上电默认）
 *   xl9535_configure_all(dev, 0xFF00) → Port0 输出，Port1 输入
 */
int xl9535_configure_all(i2c_dev_t *dev, uint16_t dir16);

/**
 * @brief 读全部 16 个输入引脚的实际电平
 *
 * 底层调用 xl9535_read_inputs16。
 *
 * @param[in,out] dev 已打开的设备句柄
 *
 * @return >=0 16-bit 电平值：(Port1 << 8) | Port0（Port 0 低字节, Port 1 高字节）；
 *         -1 失败（errno 有效）
 */
int xl9535_read_all_inputs(i2c_dev_t *dev);

/**
 * @brief 写全部 16 个输出引脚的目标值
 *
 * 底层调用 xl9535_write_outputs16，一次事务完成。
 *
 * @param[in,out] dev   已打开的设备句柄
 * @param[in]     val16 16-bit 输出值：低字节 = Port 0, 高字节 = Port 1
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_write_all_outputs(i2c_dev_t *dev, uint16_t val16);

/* ---- 自检工具 ---- */

/**
 * @brief XL9535 硬件自检：读回配置寄存器验证芯片应答 + 可选安全态恢复
 *
 * 流程：
 *   1) 读 Configuration 寄存器确认芯片有应答；
 *   2) safe_inputs=1：把所有引脚设为输入（高阻，上电默认，安全）；
 *      safe_inputs=0：把所有引脚设为输出 + 全低（适合 LED 测试，点亮低有效 LED）。
 *
 * 主要用于 demo 和 bring-up 阶段确认接线正确。
 *
 * @param[in,out] dev         已打开的设备句柄
 * @param[in]     safe_inputs 1=恢复安全态（全部 INPUT）；0=全部 OUTPUT+全低
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int xl9535_selftest(i2c_dev_t *dev, int safe_inputs);

#endif /* XL9535_H */
