/* =========================================================================
 * i2c_util.c —— i2c_util.h 的实现
 *
 * 依赖：Linux 内核头文件 <linux/i2c-dev.h>（其中已包含 <linux/i2c.h>）。
 *   这些头文件由内核/发行版提供，不需要额外安装库。
 *   注意不要同时 include <i2c/smbus.h>，否则 union i2c_smbus_data 等
 *   符号会与 <linux/i2c-dev.h> 冲突（这是新手常见编译错误）。
 * ========================================================================= */

#include "i2c_util.h"

#include <stdio.h>        /* snprintf */
#include <string.h>       /* memcpy */
#include <errno.h>        /* errno */
#include <fcntl.h>        /* open/O_RDWR/O_CLOEXEC */
#include <unistd.h>       /* close */
#include <sys/ioctl.h>    /* ioctl */

/* 注意顺序：先 <linux/i2c.h> 再 <linux/i2c-dev.h>
 *
 * 原因：Arm GNU Toolchain 自带的 <linux/i2c-dev.h> 是 glibc 旧版用户态头，
 * 它只定义 ioctl 命令（I2C_SLAVE / I2C_RDWR / I2C_SMBUS），
 * 但**不会**自动 include <linux/i2c.h>，
 * 因此缺失 I2C_SMBUS_WRITE / I2C_SMBUS_QUICK / I2C_SMBUS_READ /
 * struct i2c_msg / union i2c_smbus_data 等全部 SMBus 相关符号。
 *
 * 而工具链自带的 <linux/i2c.h> 是完整内核版，
 * 上面这些符号全部在里面。先 include 它，就能让所有符号就位。
 *
 * （在板子原生编译时，发行版提供的 i2c-dev.h 是内核版，自己会 include i2c.h，
 *  所以顺序不影响。这里写显式的 include 只是为了让交叉编译也能用。）
 */
#include <linux/i2c.h>     /* struct i2c_msg / union i2c_smbus_data / I2C_SMBUS_* 常量 */
#include <linux/i2c-dev.h> /* I2C_SLAVE / I2C_RDWR / I2C_SMBUS / struct i2c_smbus_ioctl_data */

/* -------------------------------------------------------------------------
 * 打开适配器 + 绑定从机地址
 * ------------------------------------------------------------------------- */
/**
 * @brief 打开 I2C 适配器字符设备并绑定目标从机地址
 *
 * 本函数完成两个系统调用：
 *   1) open(path, O_RDWR | O_CLOEXEC) —— 打开 /dev/i2c-N 字符设备
 *   2) ioctl(fd, I2C_SLAVE, addr)    —— 告诉内核"这个 fd 默认访问 addr"
 *
 * 注意：I2C_SLAVE 只是设置 fd 的**默认目标**，并不发起任何总线传输，
 * 所以此时不会发现"没接设备"——真正的检测要等第一次 read/write 才暴露
 * （返回 ENXIO 表示该地址无应答）。
 *
 * 不要用 I2C_SLAVE_FORCE 强抢已被内核驱动绑定的地址（EBUSY），
 * 会与驱动打架。正确做法是先卸载驱动或换地址引脚。
 *
 * @param[out] dev   输出句柄，函数成功后 dev->fd / dev->addr / dev->path 全部填充
 * @param[in]  path  适配器设备节点，如 "/dev/i2c-1"
 * @param[in]  addr  目标从机 7 位地址，范围 0x08~0x77（0x00~0x07 和 0x78~0x7F 保留）
 *
 * @return 0  成功
 * @return -1 失败，errno 标识具体原因：
 *             EINVAL   dev/path 为空 或 addr 超范围
 *             ENOENT   path 不存在（i2c-dev 模块未加载 /dev/i2c-N）
 *             EACCES   权限不足
 *             EBUSY    该地址已被内核驱动占用
 *             ENXIO    真正的第一次读写时才会出现，这里不会
 */
int i2c_open(i2c_dev_t *dev, const char *path, int addr)
{
    if (!dev || !path) {
        errno = EINVAL;
        return -1;
    }
    /* I2C 7 位地址范围 0x08~0x77；0x00~0x07 与 0x78~0x7F 是保留地址：
     * 0x00 是广播地址，0x01~0x03 用于特殊用途，0x78~0x7F 是 10 位地址前缀。 */
    if (addr < 0x08 || addr > 0x77) {
        errno = EINVAL;
        return -1;
    }

    /* O_CLOEXEC：fork+exec 时自动关闭，避免 fd 泄漏到子进程。 */
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;                    /* errno: ENOENT/EPERM/EACCES 等 */

    /* I2C_SLAVE 绑定地址 */
    if (ioctl(fd, I2C_SLAVE, addr) < 0) {
        int saved = errno;
        close(fd);                    /* 失败也要关 fd，防止泄漏 */
        errno = saved;                /* close 可能覆盖 errno，恢复原错误 */
        return -1;
    }

    dev->fd   = fd;
    dev->addr = addr;
    snprintf(dev->path, sizeof(dev->path), "%s", path);
    return 0;
}

/**
 * @brief 关闭 I2C 句柄，释放内核资源
 *
 * 关闭 fd 后内核自动释放该 fd 上 I2C_SLAVE 绑定的从机句柄，
 * 允许同地址被其他进程打开。dev->fd 被重置为 -1，
 * 可安全重复调用（重复调用不报错）。
 *
 * @param[in,out] dev  由 i2c_open 得到的句柄；允许 NULL（直接 return）
 *
 * @return 无
 */
void i2c_close(i2c_dev_t *dev)
{
    if (!dev)
        return;
    if (dev->fd >= 0) {
        close(dev->fd);               /* 关闭后内核自动释放该从机句柄 */
        dev->fd = -1;
    }
}

/* -------------------------------------------------------------------------
 * 总线扫描：对 0x08~0x77 每个地址做一次 QUICK 写探测
 * ------------------------------------------------------------------------- */
/**
 * @brief 扫描指定 I2C 适配器上有应答的从机设备
 *
 * 对地址范围 0x08~0x77 逐一：
 *   1) ioctl I2C_SLAVE 临时把 fd 绑到该地址
 *   2) ioctl I2C_SMBUS QUICK 写 —— 只发 START + 地址 + W + STOP，
 *      不要求器件有寄存器概念，是"最少打扰"的探测方式
 *   3) 成功（ioctl 返回 >=0）= 器件 ACK 了地址，说明在线
 *
 * 结果打印到 stdout，格式参考 i2cdetect -y <bus>。
 *
 * 【局限】
 *   - 有些器件不支持/不应答 QUICK，会被漏掉；
 *   - 有些适配器对 NACK 返回 EREMOTEIO，老适配器可能返回其他错误；
 *   - 扫描本身有副作用风险（个别器件把任意写当命令），生产环境慎用。
 *
 * @param[in] path  适配器设备节点，如 "/dev/i2c-1"
 *
 * @return >=0  成功，返回扫到的在线设备数量
 * @return -1   打开适配器失败（errno 有效）
 */
int i2c_scan(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return -1;

    int found = 0;
    printf("Scanning %s ...\n     ", path);

    for (int addr = 0x08; addr <= 0x77; addr++) {
        struct i2c_smbus_ioctl_data args;

        /* 先设默认地址；EBUSY 表示被内核驱动占用，也跳过 */
        if (ioctl(fd, I2C_SLAVE, addr) < 0)
            continue;

        args.read_write = I2C_SMBUS_WRITE;   /* QUICK 写 */
        args.command    = 0;                 /* QUICK 协议不使用 command */
        args.size       = I2C_SMBUS_QUICK;   /* 协议类型：无数据 */
        args.data       = NULL;

        /* 成功（>=0）= 器件 ACK 了地址，说明在线 */
        if (ioctl(fd, I2C_SMBUS, &args) >= 0) {
            printf("0x%02x ", addr);
            found++;
        }
    }
    printf("\nFound %d device(s).\n", found);

    close(fd);
    return found;
}

/* -------------------------------------------------------------------------
 * 原生 I2C：读寄存器（写地址 + 重复起始 + 读数据）
 * ------------------------------------------------------------------------- */
/**
 * @brief 从从机寄存器读取 len 字节（最常用的读寄存器方式）
 *
 * 底层时序（对应绝大多数器件手册的"读寄存器"时序图）：
 *   START | addr+W | reg | 重复START(无STOP) | addr+R | buf[0..len-1] | STOP
 *
 * 两段 i2c_msg 之间**必须是重复起始**（不是 STOP+START），
 * 因为多数器件的"寄存器指针"在 STOP 后可能复位或被其他主机事务打断。
 *
 * 内核 I2C_RDWR ioctl 会把相邻 msg 自动拼成重复起始，无需用户手动控制。
 *
 * @param[in,out] dev  已打开的从机句柄
 * @param[in]     reg  目标寄存器地址（8 bit）
 * @param[out]    buf  读回数据存放缓冲区，至少 len 字节
 * @param[in]     len  要读的字节数，范围 1 ~ I2C_MAX_XFER (64)
 *
 * @return 0  成功
 * @return -1 失败，errno 有效：
 *             EINVAL    参数错误（dev/buf 空、len=0 或超范围）
 *             ENXIO     从机在该地址无应答
 *             EREMOTEIO 从机发了 NACK
 *             EOPNOTSUPP 适配器不支持 I2C_RDWR（纯 SMBus-only 老适配器）
 */
int i2c_read_reg(i2c_dev_t *dev, uint8_t reg, uint8_t *buf, size_t len)
{
    if (!dev || dev->fd < 0 || !buf || len == 0 || len > I2C_MAX_XFER) {
        errno = EINVAL;
        return -1;
    }

    struct i2c_msg msgs[2];
    struct i2c_rdwr_ioctl_data xfer;

    /* 第一段：写寄存器地址（flags=0 表示写） */
    msgs[0].addr  = (unsigned short)dev->addr;
    msgs[0].flags = 0;
    msgs[0].len   = 1;
    msgs[0].buf   = &reg;

    /* 第二段：读 len 字节（I2C_M_RD 表示读） */
    msgs[1].addr  = (unsigned short)dev->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len   = (unsigned short)len;
    msgs[1].buf   = buf;

    xfer.msgs  = msgs;
    xfer.nmsgs = 2;

    /* I2C_RDWR 是"原子"的：内核保证两段之间是重复起始，不会被别的
     * 主机事务插进来。失败时 errno 通常为 ENXIO/EREMOTEIO/EOPNOTSUPP。 */
    if (ioctl(dev->fd, I2C_RDWR, &xfer) < 0)
        return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * 原生 I2C：写寄存器（地址 + 连续数据，一次传输）
 *   START, addr+W, reg, data[0..len-1], STOP
 * ------------------------------------------------------------------------- */
/**
 * @brief 向从机寄存器写入 len 字节数据
 *
 * 把"寄存器地址 + 数据"拼成一段连续缓冲，通过单次 i2c_msg 完成。
 * 时序：START | addr+W | reg | data[0..len-1] | STOP
 *
 * @param[in] dev  已打开的从机句柄
 * @param[in] reg  目标寄存器地址（8 bit）
 * @param[in] buf  待写数据，至少 len 字节
 * @param[in] len  要写的字节数，len+1 不得超过 I2C_MAX_XFER (64)
 *
 * @return 0  成功
 * @return -1 失败，errno 同 i2c_read_reg
 */
int i2c_write_reg(i2c_dev_t *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
    if (!dev || dev->fd < 0 || !buf || len == 0 ||
        len + 1 > I2C_MAX_XFER) {
        errno = EINVAL;
        return -1;
    }

    /* 把"寄存器地址 + 数据"拼成一段连续缓冲：I2C 写时序就是连续发字节 */
    uint8_t tmp[I2C_MAX_XFER];
    tmp[0] = reg;
    memcpy(tmp + 1, buf, len);

    struct i2c_msg msg;
    struct i2c_rdwr_ioctl_data xfer;

    msg.addr  = (unsigned short)dev->addr;
    msg.flags = 0;
    msg.len   = (unsigned short)(len + 1);
    msg.buf   = tmp;

    xfer.msgs  = &msg;
    xfer.nmsgs = 1;

    if (ioctl(dev->fd, I2C_RDWR, &xfer) < 0)
        return -1;
    return 0;
}

/* -------------------------------------------------------------------------
 * SMBus 封装
 *
 * I2C_SMBUS ioctl 的参数是 struct i2c_smbus_ioctl_data：
 *   read_write : I2C_SMBUS_READ / I2C_SMBUS_WRITE
 *   command    : 器件寄存器地址（SMBus 称之为 command）
 *   size       : 协议类型（QUICK/BYTE/BYTE_DATA/WORD_DATA/BLOCK_DATA...）
 *   data       : 指向 union i2c_smbus_data，承载读回/待写的数据
 * ------------------------------------------------------------------------- */

/**
 * @brief SMBus 底层通用传输（被三个公开 API 复用）
 *
 * @param[in,out] dev        已打开的从机句柄
 * @param[in]     read_write I2C_SMBUS_READ 或 I2C_SMBUS_WRITE
 * @param[in]     reg        SMBus command（对多数器件就是寄存器地址）
 * @param[in]     size       SMBus 协议类型：I2C_SMBUS_QUICK / BYTE / BYTE_DATA / WORD_DATA / ...
 * @param[in,out] data       union i2c_smbus_data 指针，承载输出或返回值
 *
 * @return >=0 ioctl 成功（具体值 SMBus 规范未定义，>=0 视为成功）
 * @return -1  失败，errno 有效
 */
static int smbus_xfer(i2c_dev_t *dev, char read_write, uint8_t reg,
                      uint8_t size, union i2c_smbus_data *data)
{
    if (!dev || dev->fd < 0) {
        errno = EINVAL;
        return -1;
    }

    struct i2c_smbus_ioctl_data args;
    args.read_write = read_write;
    args.command    = reg;
    args.size       = size;
    args.data       = data;

    return ioctl(dev->fd, I2C_SMBUS, &args);
}

/**
 * @brief SMBus Read Byte Data —— 从指定寄存器读 1 字节
 *
 * 适合只有简单"读寄存器"语义的器件（如 EEPROM 的小页读）。
 *
 * @param[in] dev  已打开的从机句柄
 * @param[in] reg  目标寄存器地址（8 bit）
 *
 * @return 0~255  读回的字节值
 * @return -1     失败（errno 有效）
 */
int i2c_smbus_read_byte_data(i2c_dev_t *dev, uint8_t reg)
{
    union i2c_smbus_data d;
    d.byte = 0;

    if (smbus_xfer(dev, I2C_SMBUS_READ, reg, I2C_SMBUS_BYTE_DATA, &d) < 0)
        return -1;
    return d.byte;                 /* 0~255 */
}

/**
 * @brief SMBus Write Byte Data —— 向指定寄存器写 1 字节
 *
 * @param[in] dev  已打开的从机句柄
 * @param[in] reg  目标寄存器地址（8 bit）
 * @param[in] val  待写字节值（0~255）
 *
 * @return 0  成功
 * @return -1 失败（errno 有效）
 */
int i2c_smbus_write_byte_data(i2c_dev_t *dev, uint8_t reg, uint8_t val)
{
    union i2c_smbus_data d;
    d.byte = val;

    if (smbus_xfer(dev, I2C_SMBUS_WRITE, reg, I2C_SMBUS_BYTE_DATA, &d) < 0)
        return -1;
    return 0;
}

/**
 * @brief SMBus Read Word Data —— 从指定寄存器读 2 字节（小端）
 *
 * 【注意】SMBus 规范规定 word 为**小端**（低字节先）。
 * 但部分器件实际是大端输出（如 LM75 温度寄存器），
 * 此时不要用本函数，改用 i2c_read_reg + 手动拼字节。
 *
 * @param[in] dev  已打开的从机句柄
 * @param[in] reg  目标寄存器地址（8 bit，指向 word 低字节）
 *
 * @return 0~65535  读回的小端 word
 * @return -1       失败（errno 有效）
 */
int i2c_smbus_read_word_data(i2c_dev_t *dev, uint8_t reg)
{
    union i2c_smbus_data d;
    d.word = 0;

    if (smbus_xfer(dev, I2C_SMBUS_READ, reg, I2C_SMBUS_WORD_DATA, &d) < 0)
        return -1;
    return d.word;                 /* SMBus 词数据为小端：低字节先 */
}
