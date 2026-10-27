/* =========================================================================
 * i2c_util.h —— Linux 用户态 I2C 访问工具库（教学示例）
 *
 * 【设计说明】
 *   Linux 用户态访问 I2C 有两条路：
 *     1) /dev/i2c-N 字符设备 + ioctl —— 本例演示的方式，简单直接、无需内核驱动
 *     2) 内核 i2c_driver 驱动 —— 在内核里注册，由驱动暴露 sysfs/字符设备给用户
 *   本例走第 1 条，适合"快速验证传感器 / 写个小工具"，也是理解 I2C 协议的好起点。
 *
 * 【两种访问风格，本例都封装了】
 *   A. SMBus 风格：i2c_smbus_xxx()
 *      - 一次读/写一个"寄存器"，语义固定，最常用（温度传感器、EEPROM 小页）
 *      - 内核替你把"写寄存器地址 + 重复起始 + 读数据"这套时序拼好了
 *      - 局限：只能处理 1/2/4 字节，且 word 按小端解释（有些器件是大端！）
 *   B. 原生 I2C 风格：i2c_read_reg() / i2c_write_reg()
 *      - 用 struct i2c_msg 自己描述"写寄存器地址"和"读 N 字节"两段事务
 *      - 支持任意长度、可精确控制重复起始(Repeated Start)，适合多字节块读
 *      - 本例的 read_reg 就是"写 1 字节寄存器地址 + 重复起始 + 读 len 字节"
 *
 * 【返回约定】
 *   所有函数：成功返回 0（scan 返回发现的设备数），失败返回 -1 并设置 errno。
 *   调用方用 strerror(errno) 打印原因。常见 errno 含义：
 *     ENXIO   —— 该地址上没有设备应答（没接、地址错、上拉缺失）
 *     EBUSY   —— 该地址已被内核驱动占用（先用 i2cdetect -l 或卸载驱动）
 *     EREMOTEIO —— 适配器报告 NACK（从机拒绝本次传输）
 *     EOPNOTSUPP —— 适配器不支持该操作（如纯 SMBus 适配器不支持 I2C_RDWR）
 * ========================================================================= */

#ifndef I2C_UTIL_H
#define I2C_UTIL_H

#include <stdint.h>
#include <stddef.h>

/* 一次传输的最大字节数。i2c_msg.len 是 16 位，但一般适配器单次上限
 * 也就几十字节（受 FIFO / DMA 限制），这里取 64 足够教学使用。 */
#define I2C_MAX_XFER 64

/* I2C 设备句柄：把 fd 和从机地址绑在一起，避免调用方到处传 addr。
 *
 * 为什么需要保存 addr？
 *   I2C_SLAVE ioctl 设置地址是"fd 级别"的状态，而 I2C_RDWR 又要求在
 *   每条 i2c_msg 里重新填 addr。句柄把两者绑在一起，调用更不易错。
 *
 * 注意：一个句柄不要多线程并发使用（fd 的 ioctl 状态是共享的）。
 */
typedef struct {
    int  fd;                 /* /dev/i2c-N 打开的 fd，-1 表示未打开 */
    int  addr;               /* 7 位从机地址，如 0x68 */
    char path[64];           /* 设备节点路径，出错打印用 */
} i2c_dev_t;

/**
 * @brief 打开 I2C 适配器并绑定从机地址
 *
 * 打开 /dev/i2c-N 字符设备，通过 I2C_SLAVE ioctl 设置从机地址，
 * 把 fd/addr/path 填到 i2c_dev_t 句柄里。不验证芯片应答。
 *
 * @param[out] dev  待初始化的设备句柄
 * @param[in]  path 设备节点路径，如 "/dev/i2c-1"
 * @param[in]  addr 7 位从机地址（0x08 ~ 0x77 有效）
 *
 * @return 0 成功，-1 失败（errno 有效，常见见文件头）
 */
int i2c_open(i2c_dev_t *dev, const char *path, int addr);

/**
 * @brief 关闭 I2C 设备句柄
 *
 * 关闭 fd 并清零句柄内容。对未打开的句柄可安全重复调用。
 *
 * @param[in,out] dev 设备句柄
 *
 * @return 无
 */
void i2c_close(i2c_dev_t *dev);

/**
 * @brief 扫描 I2C 总线上有应答的从设备
 *
 * 对 0x08~0x77 每个地址发起 SMBus QUICK 写（只有起始+地址，无数据），
 * 有 ACK 就说明该地址上有设备。扫描结果打印到 stdout，
 * 同时在返回值里给出设备数量。
 *
 * 注意：个别设备不支持 QUICK 写会被漏掉，改用 `i2cdetect -y -r <bus>` 更可靠。
 *
 * @param[in] path I2C 设备节点路径，如 "/dev/i2c-1"
 *
 * @return >=0 发现的设备数量；<0 打开总线失败（errno 有效）
 */
int i2c_scan(const char *path);

/**
 * @brief 读寄存器（原生 I2C 组合事务，推荐用于多字节块读）
 *
 * 时序：START | addr+W | reg | 重复START | addr+R | data... | STOP。
 * 正是大多数器件手册里"读寄存器"的标准时序。
 * 利用 I2C_RDWR ioctl，支持任意长度和精确的重复起始控制。
 *
 * @param[in,out] dev  已打开的设备句柄
 * @param[in]     reg  起始寄存器地址
 * @param[out]    buf  读回数据缓冲区（长度 >= len）
 * @param[in]     len  读取字节数（最大 I2C_MAX_XFER）
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int i2c_read_reg(i2c_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len);

/**
 * @brief 写寄存器（原生 I2C）
 *
 * 时序：START | addr+W | reg | data... | STOP。
 * 内核把 reg + buf 拼成一次连续写事务。
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[in]     reg 目标寄存器地址
 * @param[in]     buf 待写入数据缓冲区（可传 NULL 当 len=0）
 * @param[in]     len 待写入字节数（最大 I2C_MAX_XFER - 1，因为还要发 reg 字节）
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int i2c_write_reg(i2c_dev_t *dev, uint8_t reg, const uint8_t *buf, size_t len);

/* ---- SMBus 便捷接口（简单场景用，内核帮你拼时序） ---- */

/**
 * @brief SMBus 风格读 1 字节寄存器
 *
 * 内核自动完成"写寄存器地址 + 重复起始 + 读 1 字节"的时序。
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[in]     reg 寄存器地址
 *
 * @return 0~255 读回值；-1 失败（errno 有效）
 */
int i2c_smbus_read_byte_data(i2c_dev_t *dev, uint8_t reg);

/**
 * @brief SMBus 风格写 1 字节寄存器
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[in]     reg 寄存器地址
 * @param[in]     val 待写入值（0~255）
 *
 * @return 0 成功，-1 失败（errno 有效）
 */
int i2c_smbus_write_byte_data(i2c_dev_t *dev, uint8_t reg, uint8_t val);

/**
 * @brief SMBus 风格读 2 字节（Word Data）
 *
 * 按 SMBus 规范为小端（低字节先）。若器件手册是"高字节先"（如 LM75、AD7998），
 * 不要用本函数，改用 i2c_read_reg 自己控制时序。
 *
 * @param[in,out] dev 已打开的设备句柄
 * @param[in]     reg 寄存器地址
 *
 * @return 0~0xFFFF 读回值；-1 失败（errno 有效）
 */
int i2c_smbus_read_word_data(i2c_dev_t *dev, uint8_t reg);

#endif /* I2C_UTIL_H */
