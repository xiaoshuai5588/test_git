/* =========================================================================
 * xl9535.c —— XL9535 驱动实现
 *
 * 设计说明：
 *   所有寄存器操作都走 i2c_util.c 的 i2c_read_reg / i2c_write_reg，
 *   它们底层用 I2C_RDWR ioctl，能精确控制重复起始和连续写入。
 *
 * 自动对调机制的正确用法（datasheet 9.6.1，硬件设计）：
 *   XL9535 寄存器对按 [0,1] [2,3] [4,5] [6,7] 配对。
 *   **在同一次 START-STOP 事务内**，写完"奇数寄存器"(后端口)后，
 *   紧接着发的下一个字节会**自动落到"偶数寄存器"(前端口)**。
 *
 *   例：START+addr+W+0x03+D1+D0+STOP
 *        0x03 = Output Port 1  ← 收到 D1
 *        D0   → 自动进入 Output Port 0 (reg 0x02)
 *
 *   一次性写两个同类型寄存器必须**先写奇数再写偶数**；
 *   如果反过来写（先 0x02 再 0x03），第二次 STOP 切断后，
 *   对调机制不生效，两个字节分别落到两个寄存器，虽然也能工作，
 *   但会多一次寄存器地址字节、多一次 STOP/START，效率更低。
 * ========================================================================= */

#include "xl9535.h"

#include <stdio.h>       /* selftest 打印 */
#include <string.h>
#include <errno.h>

/* -------------------------------------------------------------------------
 * 打开 / 关闭
 * ------------------------------------------------------------------------- */

/**
 * @brief 打开 XL9535 设备，自动根据 A2/A1/A0 引脚电平计算 I2C 地址
 *
 * @param[out] dev   输出句柄，成功后填充 fd/addr/path
 * @param[in]  path  适配器路径，如 "/dev/i2c-1"
 * @param[in]  a2    地址引脚 A2 电平（0=GND, 1=VCC）
 * @param[in]  a1    地址引脚 A1 电平
 * @param[in]  a0    地址引脚 A0 电平
 *
 * @return 0  成功
 * @return -1 失败，errno 同 i2c_open（常见：EINVAL / ENOENT / EBUSY）
 */
int xl9535_open(i2c_dev_t *dev, const char *path, int a2, int a1, int a0)
{
    int addr = xl9535_addr(a2, a1, a0);
    return i2c_open(dev, path, addr);
}

/**
 * @brief 关闭 XL9535 句柄。等同 i2c_close，可安全重复调用。
 * @param[in,out] dev  已打开的 XL9535 句柄；允许 NULL
 * @return 无
 */
void xl9535_close(i2c_dev_t *dev)
{
    i2c_close(dev);
}

/* -------------------------------------------------------------------------
 * 方向配置（Config register 6/7）
 *   XL9535 方向约定: bit=1 → 输入(高阻), bit=0 → 输出 —— 别跟其他厂搞反！
 * ------------------------------------------------------------------------- */

/**
 * @brief 配置 Port 0 (P00~P07) 的输入/输出方向
 *
 * mask 每个 bit 对应一根引脚：
 *   1 = 输入（高阻，上电默认）；0 = 输出（可驱动外部电路）
 *   例：mask=0x0F 表示 P00~P03 输出、P04~P07 输入
 *
 * @param[in] dev  已打开的 XL9535 句柄
 * @param[in] mask 方向掩码（8 bit）
 * @return 0 成功 / -1 失败（errno 有效）
 */
int xl9535_set_dir_port0(i2c_dev_t *dev, uint8_t mask)
{
    return i2c_write_reg(dev, XL9535_REG_CONFIG0, &mask, 1);
}

/**
 * @brief 配置 Port 1 (P10~P17) 的输入/输出方向，语义同 xl9535_set_dir_port0
 * @param[in] dev   已打开的 XL9535 句柄
 * @param[in] mask  方向掩码（8 bit）
 * @return 0 成功 / -1 失败（errno 有效）
 */
int xl9535_set_dir_port1(i2c_dev_t *dev, uint8_t mask)
{
    return i2c_write_reg(dev, XL9535_REG_CONFIG1, &mask, 1);
}

/**
 * @brief 一次性配置两个端口的方向 —— 利用芯片自动对调机制
 *
 * 底层时序（高效！1 次 START+STOP 搞定两个寄存器）：
 *   START+addr+W+0x07+port1_mask+port0_mask+STOP
 *                ↑Config1         ↑自动进 Config0(0x06)
 *
 * 陷阱：若反过来写（先 Config0 再 Config1），对调机制不工作
 *       （因为 Config0 是偶数寄存器，写完后下一字节不会自动跳到 Config1），
 *       虽也能工作但多 1 次寄存器地址字节 + 多 1 次 STOP/START。
 *
 * @param[in] dev         已打开的 XL9535 句柄
 * @param[in] port0_mask  Port 0 方向（8 bit，1=输入 0=输出）
 * @param[in] port1_mask  Port 1 方向
 * @return 0 成功 / -1 失败（errno 有效）
 */
int xl9535_set_dir_both(i2c_dev_t *dev, uint8_t port0_mask, uint8_t port1_mask)
{
    uint8_t buf[2] = { port1_mask, port0_mask };
    return i2c_write_reg(dev, XL9535_REG_CONFIG1, buf, 2);
}

/* -------------------------------------------------------------------------
 * 输出（Output register 2/3）
 * ------------------------------------------------------------------------- */

/**
 * @brief 设置 Port 0 (P00~P07) 的输出锁存值
 *
 * 注意：只有对应引脚被配置为输出时，这个值才会真正驱动引脚；
 * 若引脚是输入方向，写 Output 寄存器值会被锁存但不会出现在引脚上。
 *
 * @param[in] dev  已打开的 XL9535 句柄
 * @param[in] val  8 bit 输出值，bit=1 输出高电平，bit=0 低电平
 * @return 0 成功 / -1 失败（errno 有效）
 */
int xl9535_write_port0(i2c_dev_t *dev, uint8_t val)
{
    return i2c_write_reg(dev, XL9535_REG_OUTPUT0, &val, 1);
}

/**
 * @brief 设置 Port 1 (P10~P17) 的输出锁存值，语义同 xl9535_write_port0
 * @param[in] dev  已打开的 XL9535 句柄
 * @param[in] val  8 bit 输出值
 * @return 0 成功 / -1 失败（errno 有效）
 */
int xl9535_write_port1(i2c_dev_t *dev, uint8_t val)
{
    return i2c_write_reg(dev, XL9535_REG_OUTPUT1, &val, 1);
}

/**
 * @brief 一次性设置两个端口的输出值 —— 利用自动对调（跟 xl9535_set_dir_both 同理）
 *
 * @param[in] dev  已打开的 XL9535 句柄
 * @param[in] out0 Port 0 输出值（低位 8 bit）
 * @param[in] out1 Port 1 输出值（高位 8 bit）
 * @return 0 成功 / -1 失败（errno 有效）
 */
int xl9535_write_outputs16(i2c_dev_t *dev, uint8_t out0, uint8_t out1)
{
    uint8_t buf[2] = { out1, out0 };   /* 必须 out1 在前（奇数寄存器 Output1=0x03） */
    return i2c_write_reg(dev, XL9535_REG_OUTPUT1, buf, 2);
}

/* -------------------------------------------------------------------------
 * 输入（Input register 0/1）—— 只读
 *
 * Input register 没有自动对调的便捷方式 —— 读操作必须先设好寄存器指针
 * （write-restart-read），所以两个端口分两次 i2c_read_reg。
 * ------------------------------------------------------------------------- */

/**
 * @brief 读 Port 0 (P00~P07) 的**实际引脚电平**（8 bit，bit=1=高电平）
 *
 * 注意：Input register 反映外部引脚的**真实电平**，
 * 不管该引脚被配置为输入还是输出。这意味着：
 *   - 配置为输出时，读 Input 可以验证输出锁存是否真的生效（回读测试）
 *   - 配置为输入时，读 Input 反映外部信号源的电平
 *
 * @param[in] dev 已打开的 XL9535 句柄
 * @return 0~255 引脚电平值；-1 失败（errno 有效）
 */
int xl9535_read_port0(i2c_dev_t *dev)
{
    uint8_t val;
    if (i2c_read_reg(dev, XL9535_REG_INPUT0, &val, 1) < 0)
        return -1;
    return (int)val;
}

/**
 * @brief 读 Port 1 (P10~P17) 的实际引脚电平，语义同 xl9535_read_port0
 * @param[in] dev 已打开的 XL9535 句柄
 * @return 0~255 引脚电平值；-1 失败
 */
int xl9535_read_port1(i2c_dev_t *dev)
{
    uint8_t val;
    if (i2c_read_reg(dev, XL9535_REG_INPUT1, &val, 1) < 0)
        return -1;
    return (int)val;
}

/**
 * @brief 同时读两个端口的引脚电平（两次独立 i2c_read_reg）
 *
 * @param[in]  dev  已打开的 XL9535 句柄
 * @param[out] in0  [out] Port 0 电平，可传 NULL 不关心
 * @param[out] in1  [out] Port 1 电平，可传 NULL 不关心
 * @return 0 成功 / -1 失败（只要其中一个端口读失败就算失败）
 */
int xl9535_read_inputs16(i2c_dev_t *dev, uint8_t *in0, uint8_t *in1)
{
    int ret0 = xl9535_read_port0(dev);
    int ret1 = xl9535_read_port1(dev);
    if (ret0 < 0 || ret1 < 0) return -1;
    if (in0) *in0 = (uint8_t)ret0;
    if (in1) *in1 = (uint8_t)ret1;
    return 0;
}

/* -------------------------------------------------------------------------
 * 极性反转（Polarity Inversion register 4/5）
 *   bit=1 → Input Port 对应位被取反后再读
 *   典型用途：接低电平有效按键，配置 bit=1 后按下时读 1（自然语义）
 * ------------------------------------------------------------------------- */

/**
 * @brief 设置 Port 0 的极性反转掩码
 * @param[in] dev   已打开的 XL9535 句柄
 * @param[in] mask  8 bit 反转掩码，1=反转对应引脚极性
 * @return 0 成功 / -1 失败
 */
int xl9535_set_pol_port0(i2c_dev_t *dev, uint8_t mask)
{
    return i2c_write_reg(dev, XL9535_REG_POL0, &mask, 1);
}

/**
 * @brief 设置 Port 1 的极性反转掩码，语义同 xl9535_set_pol_port0
 * @param[in] dev   已打开的 XL9535 句柄
 * @param[in] mask  8 bit 反转掩码
 * @return 0 成功 / -1 失败
 */
int xl9535_set_pol_port1(i2c_dev_t *dev, uint8_t mask)
{
    return i2c_write_reg(dev, XL9535_REG_POL1, &mask, 1);
}

/* -------------------------------------------------------------------------
 * 便捷 API：16-bit 整体操作
 * ------------------------------------------------------------------------- */

/**
 * @brief 一次性配置全部 16 个引脚的方向
 *
 * dir16 位布局：低 8 bit = Port0(P00..P07)，高 8 bit = Port1(P10..P17)
 * 每个 bit = 1 → 输入（高阻），0 → 输出
 *
 * 例：dir16 = 0xFFFF → 全部输入（上电默认）
 *     dir16 = 0x0000 → 全部输出
 *     dir16 = 0xFF00 → Port0 输出，Port1 输入
 *
 * @param[in] dev    已打开的 XL9535 句柄
 * @param[in] dir16  16 bit 方向掩码
 * @return 0 成功 / -1 失败
 */
int xl9535_configure_all(i2c_dev_t *dev, uint16_t dir16)
{
    uint8_t port0 = (uint8_t)(dir16 & 0xFF);
    uint8_t port1 = (uint8_t)((dir16 >> 8) & 0xFF);
    return xl9535_set_dir_both(dev, port0, port1);
}

/**
 * @brief 一次性设置全部 16 个输出引脚的锁存值
 *
 * val16 位布局：低 8 bit = Port0，高 8 bit = Port1
 * 同样利用自动对调机制（2 字节在同一次 START-STOP 内完成）。
 *
 * @param[in] dev    已打开的 XL9535 句柄
 * @param[in] val16  16 bit 输出值
 * @return 0 成功 / -1 失败
 */
int xl9535_write_all_outputs(i2c_dev_t *dev, uint16_t val16)
{
    uint8_t out0 = (uint8_t)(val16 & 0xFF);
    uint8_t out1 = (uint8_t)((val16 >> 8) & 0xFF);
    return xl9535_write_outputs16(dev, out0, out1);
}

/**
 * @brief 读全部 16 个输入引脚的电平，打包成一个 16 bit 值
 *
 * 返回值位布局：低 8 bit = Port0，高 8 bit = Port1
 *
 * @param[in] dev 已打开的 XL9535 句柄
 * @return 0~0xFFFF 16 bit 电平值；-1 失败
 */
int xl9535_read_all_inputs(i2c_dev_t *dev)
{
    uint8_t in0, in1;
    if (xl9535_read_inputs16(dev, &in0, &in1) < 0)
        return -1;
    return ((int)in1 << 8) | (int)in0;
}

/* -------------------------------------------------------------------------
 * 自检 / Bring-up
 * ------------------------------------------------------------------------- */

/**
 * @brief 硬件自检 —— 验证芯片 I2C 应答正常，可选恢复安全默认态
 *
 * 检查逻辑：
 *   1) 连续读 Input Port 0 和 Input Port 1
 *      —— 只要都能成功读回（不管值是什么），说明 SDA/SCL/供电/地址 全正确
 *   2) 打印读回的电平（取决于外部引脚接线）
 *   3) 可选 safe_inputs=1：把所有引脚重新配置为输入高阻
 *      （上电默认就是输入，但显式配置一次保证无意外输出）
 *
 * @param[in] dev          已打开的 XL9535 句柄
 * @param[in] safe_inputs  0=读完就退出；1=读完后把所有引脚设为 INPUT（安全默认）
 *
 * @return 0  自检通过
 * @return -1 自检失败（其中一次 read 返回 errno）
 */
int xl9535_selftest(i2c_dev_t *dev, int safe_inputs)
{
    /* 读 Input Port 0 —— 只要能成功读回任意值，就说明芯片 I2C 应答正常。
     * Input Port 读值取决于外部引脚电平（上电默认所有引脚为输入高阻），
     * 所以不检查具体值，只检查"能读通"。 */
    int input0 = xl9535_read_port0(dev);
    if (input0 < 0) {
        fprintf(stderr, "xl9535_selftest: 读 Input Port 0 失败 errno=%d (%s)\n",
                errno, strerror(errno));
        return -1;
    }

    int input1 = xl9535_read_port1(dev);
    if (input1 < 0) {
        fprintf(stderr, "xl9535_selftest: 读 Input Port 1 失败 errno=%d (%s)\n",
                errno, strerror(errno));
        return -1;
    }

    printf("xl9535[%s @ 0x%02x] 自检通过:\n", dev->path, dev->addr);
    printf("  Input Port 0 = 0x%02X  (取决于外部引脚电平)\n", input0);
    printf("  Input Port 1 = 0x%02X\n", input1);
    printf("  I2C 应答正常，硬件接线 OK\n");

    if (safe_inputs) {
        /* 安全默认：所有引脚设为输入（高阻），防止输出状态驱动外部电路 */
        if (xl9535_configure_all(dev, 0xFFFF) < 0) {
            fprintf(stderr, "xl9535_selftest: set all inputs failed\n");
            return -1;
        }
        printf("  → 所有 16 引脚已配置为 INPUT（安全态）\n");
    }
    return 0;
}
