/* =========================================================================
 * ad7998.c —— AD7998 (12-bit) / AD7997 (10-bit) 8-channel I2C ADC 驱动
 *
 * 本文件实现 datasheet Mode 2 (COMMAND MODE)：
 *   I2C 写操作本身就触发转换，芯片自动唤醒 → 采样 → 转换（≈3μs），
 *   紧接着读 Conversion Result Register 就拿到结果。
 *
 * 用户态最方便的模式：
 *   write(fd, 1字节) 触发  →  i2c_read_reg 读 2 字节结果
 *   转换时间 3μs 远小于两次 I2C 事务间的自然间隔（≥几十 μs），
 *   不需要手动 sleep。
 *
 * 外部参考：4.096V（接 REFIN 引脚），LSB = 4.096/4096 = 1mV
 * 输入电压范围：0V ~ REFIN
 *
 * 依赖：i2c_util.h / i2c_util.c（i2c_open / i2c_read_reg / i2c_write_reg）
 *       + 直接使用 <linux/i2c-dev.h> 的 struct i2c_msg / I2C_RDWR
 * ========================================================================= */

#include "ad7998.h"

#include <stdio.h>       /* selftest 打印 */
#include <string.h>
#include <errno.h>
#include <unistd.h>      /* write(fd, ...) */
#include <sys/ioctl.h>
#include <fcntl.h>

/* 注意顺序：先 <linux/i2c.h> 再 <linux/i2c-dev.h>
 *
 * 原因见 i2c_util.c 头部说明 —— 工具链自带的 glibc 版 i2c-dev.h
 * 不自动 include i2c.h，而 struct i2c_msg 在 i2c.h 里定义。
 */
#include <linux/i2c.h>
#include <linux/i2c-dev.h>

/* -------------------------------------------------------------------------
 * 打开 / 关闭
 * ------------------------------------------------------------------------- */

/**
 * @brief 打开 AD7998/AD7997 设备，绑定指定 I2C 地址
 *
 * AD7998 地址由**单一 AS 引脚**电平 + 芯片版本决定（datasheet Table 6）：
 *   AD7998-0 + AS Float → 0x20 （默认，最常见）
 *   AD7998-0 + AS AGND  → 0x21
 *   AD7998-0 + AS VDD   → 0x22
 *   AD7998-1 + AS AGND  → 0x23
 *   AD7998-1 + AS VDD   → 0x24
 *
 * 7-bit 格式：bit7..4 = 0100（固定），bit3..1 = AS 决定，bit0 = R/W 自动填。
 * 同一条总线最多 5 颗。
 *
 * @param[out] dev   输出句柄，成功后填充 fd/addr/path
 * @param[in]  path  适配器路径，如 "/dev/i2c-1"
 * @param[in]  addr  7 bit I2C 地址（用 AD7998_ADDR_xxx 常量或手动写 0x20~0x24）
 *
 * @return 0  成功
 * @return -1 失败，errno 同 i2c_open（常见：EINVAL / ENOENT / EBUSY）
 */
int ad7998_open(i2c_dev_t *dev, const char *path, int addr)
{
    return i2c_open(dev, path, addr);
}

/**
 * @brief 关闭 AD7998 句柄。等同 i2c_close，可安全重复调用。
 * @param[in,out] dev  已打开的 AD7998 句柄；允许 NULL
 * @return 无
 */
void ad7998_close(i2c_dev_t *dev)
{
    i2c_close(dev);
}

/* -------------------------------------------------------------------------
 * 读写 Configuration Register（16-bit）
 * ------------------------------------------------------------------------- */

/**
 * @brief 读 Configuration Register（16 bit，MSB first）
 *
 * 寄存器位布局（datasheet Table 9）：
 *   D15..D12: DONTC
 *   D11..D4 : CH8..CH1 通道选择位（1=选，D11=CH8, D4=CH1）
 *   D3      : FLTR  1=开启 SDA/SCL 滤波（上电默认 1）
 *   D2      : ALERT_EN 1=开启 ALERT 硬件功能
 *   D1      : BUSY/ALERT（配合 D2 决定 Pin17 功能）
 *   D0      : POLARITY 0=ALERT/BUSY 低有效，1=高有效
 *   上电默认：0x0008（只有 FLTR=1）
 *
 * 底层时序：START+addr+W+[0x02]+重复START+addr+R+[byte1][byte2]+STOP
 *   → byte1 = D15..D8 (MSB), byte2 = D7..D0 (LSB)
 *
 * @param[in] dev  已打开的 AD7998 句柄
 * @return 0~0xFFFF  16 bit 配置寄存器值；-1 失败（errno 有效）
 */
int ad7998_read_config(i2c_dev_t *dev)
{
    uint8_t buf[2];
    if (i2c_read_reg(dev, AD7998_REG_CONFIG, buf, 2) < 0)
        return -1;
    /* AD7998 datasheet 明确：MSB first */
    return (int)((buf[0] << 8) | buf[1]);
}

/**
 * @brief 写 Configuration Register（16 bit，MSB first）
 *
 * 底层时序：START+addr+W+[0x02]+[cfg_hi]+[cfg_lo]+STOP
 *   （这正好是 i2c_write_reg(dev, 0x02, [cfg_hi, cfg_lo], 2) 的行为）
 *
 * @param[in] dev  已打开的 AD7998 句柄
 * @param[in] cfg  16 bit 新值，低 12 bit 有效；D15..D12 忽略
 * @return 0 成功 / -1 失败（errno 有效）
 */
int ad7998_write_config(i2c_dev_t *dev, uint16_t cfg)
{
    uint8_t buf[2] = { (uint8_t)(cfg >> 8), (uint8_t)(cfg & 0xFF) };
    return i2c_write_reg(dev, AD7998_REG_CONFIG, buf, 2);
}

/* -------------------------------------------------------------------------
 * 内部：解包 2 字节 Conversion Result → raw + ch_num + alert
 *
 * datasheet Table 13 + 14:
 *   byte1 (First Read, MSB):
 *     D7    D6  D5  D4  D3  D2  D1  D0
 *     Alert CH_ID2 CH_ID1 CH_ID0  MSB B10 B9 B8
 *   byte2 (Second Read, LSB):
 *     D7..D0 = B7 B6 B5 B4 B3 B2 B1 B0
 *
 *   raw    = ((byte1 & 0x0F) << 8) | byte2    (12 bit straight binary)
 *   ch_id  = (byte1 >> 4) & 0x7               → 000=CH1, ..., 111=CH8
 *   alert  = (byte1 >> 7) & 1
 *
 * 【注意】AD7997 (10 bit) 只用 D11..D2 10 bit，B1/B0 = 0；
 *          AD7998 (12 bit) 用 D11..D0 全部 12 bit。
 *          本函数不区分型号 —— 统一按 12 bit 解析，对 AD7997 自动高位对齐。
 * ------------------------------------------------------------------------- */
static inline void ad7998_unpack(const uint8_t buf[2],
                                 int *raw, int *ch_num, int *alert)
{
    if (alert)   *alert   = (buf[0] >> 7) & 0x1;
    if (ch_num)  *ch_num  = ad7998_ch_id_to_chnum((buf[0] >> 4) & 0x7);
    if (raw)     *raw     = ((int)(buf[0] & 0x0F) << 8) | (int)buf[1];
}

/* -------------------------------------------------------------------------
 * Mode 2 单次通道转换（最常用）
 * ------------------------------------------------------------------------- */

/**
 * @brief Mode 2 —— 对单个通道 VINn 发起一次转换并读回结果
 *
 * 底层流程（datasheet Page 29 Figure 33）：
 *   1) write(fd, 1 byte):  START+addr+W+[C4..C1 + P3..P0]+STOP
 *      C4..C1 = 1000 + (channel-1) 的低 3 bit，P3..P0 = 0000
 *      芯片收到 STOP → 立刻唤醒 → 采样 → 转换（≈3μs）
 *
 *   2) i2c_read_reg(dev, 0x00, buf, 2):
 *      START+addr+W+[0x00]+重复START+addr+R+[byte1][byte2]+STOP
 *      读回 Conversion Result Register（就是刚才那次转换的结果）
 *
 * 【为什么不用手动 sleep 3μs？】
 *   write() 返回时芯片已经开始转换；
 *   紧接着的 i2c_read_reg 两次 START+addr+ACK 加起来的时间
 *   （400kHz 下约 60μs，100kHz 下约 250μs）远大于 3μs，
 *   所以芯片在主控真正读之前早就转完了。
 *
 * 【内部 write() vs i2c_write_reg 的差异】
 *   这里我选 write(fd, &trigger, 1) 而不是 i2c_write_reg(dev, 0x00, &trigger, 1)，
 *   是因为 write() 正好发 "START+addr+W+byte+STOP"，而 i2c_write_reg 会拼
 *   "reg + data" —— 对单次 1 字节没有寄存器概念。用 write() 更直接，
 *   也不会出错（芯片只关心收到这 1 字节 Command/Address Pointer 就够了）。
 *
 * @param[in]  dev      已打开的 AD7998 句柄
 * @param[in]  channel  目标通道号 1..8（VIN1 ~ VIN8）
 * @param[out] out_raw   [out] 12 bit 原始码值（0~4095），可传 NULL
 * @param[out] out_ch    [out] 芯片回传的通道号（应等于 channel），可传 NULL
 * @param[out] out_alert [out] Alert_Flag 位（0=正常, 1=某通道超阈值），可传 NULL
 * @param[out] out_volts [out] 换算后的电压 = raw * VREF / 4096，可传 NULL
 *
 * @return 0  成功
 * @return -1 失败，errno 有效：
 *             EINVAL    dev 空 或 channel 超出 1..8
 *             ENXIO     该地址无应答（接线/参考/AS 引脚问题）
 *             EOPNOTSUPP 适配器不支持 I2C_RDWR
 */
int ad7998_read_channel(i2c_dev_t *dev, int channel,
                        int *out_raw, int *out_ch, int *out_alert,
                        double *out_volts)
{
    if (!dev || dev->fd < 0) { errno = EINVAL; return -1; }
    if (channel < 1 || channel > AD7998_NUM_CHANNELS) {
        errno = EINVAL; return -1;
    }

    /* Step 1: 发 Mode 2 触发命令 —— 1 字节 write
     *   C4..C1 = 1000 + (ch-1) 的低 3 bit
     *   P3..P0 = 0000（下次读指向 Conversion Result Register）
     *   例: ch=1 → 0x80, ch=2 → 0x90, ..., ch=8 → 0xF0 */
    uint8_t trigger = ad7998_make_ptr(AD7998_CMD_VIN(channel), 0x00);

    ssize_t n = write(dev->fd, &trigger, 1);
    if (n != 1) {
        /* errno 典型值：
         *   EPIPE   - 总线意外断开
         *   EIO     - NACK（多半是地址错了）
         *   ENXIO   - 地址无应答 */
        return -1;
    }

    /* Step 2: 读 Conversion Result Register —— 2 字节，MSB first */
    uint8_t buf[2];
    if (i2c_read_reg(dev, AD7998_REG_RESULT, buf, 2) < 0)
        return -1;

    /* Step 3: 解包 + 填充输出 */
    int raw, ch_num, alert;
    ad7998_unpack(buf, &raw, &ch_num, &alert);

    if (out_ch)     *out_ch     = ch_num;
    if (out_raw)    *out_raw    = raw;
    if (out_alert)  *out_alert  = alert;
    if (out_volts)  *out_volts  = (double)raw * (double)AD7998_VREF_V / 4096.0;

    /* 注意：这里**不硬校验** ch_num == channel
     *   因为序列模式时会触发流水线下一个通道；
     *   单独调用时 ch_num 应该等于 channel，用户可通过 out_ch 自己验证。 */
    return 0;
}

/* -------------------------------------------------------------------------
 * Mode 2 序列模式 —— 一次命令 + 连续读 2*N 字节 = 流水线采样 N 通道
 *
 * datasheet Page 29 Figure 34 的核心 insight：
 *   write(0x70) → 芯片按 Config Register 里的 D11..D4 通道位启动扫描
 *   读第 1 个 2 字节 → 拿到 CH1 结果，**同时**芯片正在转 CH2（流水线！）
 *   读第 2 个 2 字节 → 拿到 CH2 结果，**同时**芯片正在转 CH3
 *   ...依次类推
 *
 * 流水线自动推进，**读操作本身就跟转换同步** —— 不需要任何手动延时！
 * ------------------------------------------------------------------------- */

/**
 * @brief 辅助：popcount 16 bit 掩码里的 1 个数
 *
 * @param[in] v 16 bit 位掩码
 * @return 1 的个数（0~16）
 */
static int popcount_16(uint16_t v)
{
    int c = 0;
    while (v) { c += v & 1; v >>= 1; }
    return c;
}

/**
 * @brief Mode 2 序列模式：一次触发多个通道的流水线转换
 *
 * 【内部 3 步】
 *   1) **读-改-写** Configuration Register：
 *      先读当前值 → 清 D11..D4 旧通道位 → OR 新 config_mask → 写回
 *      这能保留 FLTR / ALERT_EN 等其他上电默认位不被覆盖。
 *   2) 发 Sequence 触发命令 (C4..C1=0111, P3..P0=0000)
 *      —— write(fd, [0x70], 1)
 *   3) **一次性** ioctl I2C_RDWR 读 2×N 字节（流水线自动推进）
 *      —— 不能用 i2c_read_reg（会每两个字节重新 START+写寄存器指针）
 *
 * @param[in]  dev           已打开的 AD7998 句柄
 * @param[in]  config_mask   通道位掩码，如 AD7998_CH1 | AD7998_CH3 | AD7998_CH5
 *                           只有 bit4..bit11 有效（对应 CH1..CH8）；其他位自动忽略
 * @param[out] out_raw_buf   输出数组，按"通道号从小到大"存放 12 bit 原始值；至少 popcount(config_mask) 个元素
 * @param[out] out_ch_buf    输出数组，对应每个通道号 1..8；可传 NULL
 * @param[out] count         [out] 实际采样的通道数；可传 NULL
 *
 * @return 0  成功
 * @return -1 失败，errno 有效：
 *             EINVAL   dev 空 / config_mask==0 / 没选任何通道
 *             ENXIO    地址无应答
 *             EOPNOTSUPP 适配器不支持 I2C_RDWR（两次用到：写 Config + 流水线读）
 */
int ad7998_read_sequence(i2c_dev_t *dev, uint16_t config_mask,
                         int *out_raw_buf, int *out_ch_buf, int *count)
{
    if (!dev || dev->fd < 0) { errno = EINVAL; return -1; }
    if (config_mask == 0)    { errno = EINVAL; return -1; }

    /* 只保留 bit4..bit11（CH1..CH8），其他位屏蔽 */
    uint16_t clean_mask = config_mask & 0x0FF0;
    if (clean_mask == 0) { errno = EINVAL; return -1; }

    int nch = popcount_16(clean_mask);
    if (count) *count = nch;

    /* === Step 1: 写 Configuration Register ===
     * 读当前 Config → 清旧通道位 → OR 新 mask → 写回
     * 这样 FLTR / ALERT_EN 等其他位保留不变。 */
    int cur_cfg = ad7998_read_config(dev);
    if (cur_cfg < 0) return -1;

    uint16_t new_cfg = (uint16_t)cur_cfg;
    new_cfg &= (uint16_t)~0x0FF0;    /* 清旧 D11..D4 */
    new_cfg |= clean_mask;            /* 设新通道位 */

    if (ad7998_write_config(dev, new_cfg) < 0) return -1;

    /* === Step 2: 发 Sequence 触发命令 —— 1 字节 write ===
     * C4..C1 = 0111 (Sequence), P3..P0 = 0000 (下次读 Result Register) */
    uint8_t trigger = ad7998_make_ptr(AD7998_CMD_SEQUENCE, 0x00);
    if (write(dev->fd, &trigger, 1) != 1) return -1;

    /* === Step 3: 流水线读 2*nch 字节 ===
     * 这里**不能**用 i2c_read_reg，因为 Sequence 模式下的读事务
     * 不需要（也不应该）再写寄存器指针 —— 直接 START+addr+R 就可以。
     *
     * 所以手动组 struct i2c_msg[] + ioctl(I2C_RDWR)。
     * 一次 I2C_RDWR 调用对应总线上：
     *   START+addr+R+[2*nch byte 连续读]+STOP
     * 芯片流水线自动把 N 个通道的结果依次吐出来。 */
    size_t read_len = (size_t)nch * 2;
    uint8_t *buf = (uint8_t *)malloc(read_len);
    if (!buf) { errno = ENOMEM; return -1; }

    struct i2c_msg msgs[1];
    struct i2c_rdwr_ioctl_data xfer;

    msgs[0].addr  = (unsigned short)dev->addr;
    msgs[0].flags = I2C_M_RD;
    msgs[0].len   = (unsigned short)read_len;
    msgs[0].buf   = buf;

    xfer.msgs  = msgs;
    xfer.nmsgs = 1;

    if (ioctl(dev->fd, I2C_RDWR, &xfer) < 0) {
        int saved = errno;
        free(buf);
        errno = saved;
        return -1;
    }

    /* Step 4: 解包 N 个通道结果
     * 芯片按"通道号从小到大"顺序输出（datasheet Page 29：
     * "sequence starting with lowest channel"）。 */
    for (int i = 0; i < nch; i++) {
        uint8_t *entry = buf + i * 2;
        int raw, ch_num;
        ad7998_unpack(entry, &raw, &ch_num, NULL);
        if (out_raw_buf) out_raw_buf[i] = raw;
        if (out_ch_buf)  out_ch_buf[i]  = ch_num;
    }

    free(buf);
    return 0;
}
