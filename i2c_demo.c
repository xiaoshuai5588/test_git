/* =========================================================================
 * i2c_demo.c —— I2C 用户态访问示例程序（命令行工具）
 *
 * 功能：
 *   - 扫描总线上的设备
 *   - 读任意寄存器（支持多字节块读）
 *   - 写任意寄存器（1 字节）
 *   - 读取 LM75/TMP102 类温度传感器并换算成摄氏度
 *
 * 用法：
 *   ./i2c_demo -s                       扫描默认总线 /dev/i2c-1
 *   ./i2c_demo -b 1 -s                  扫描 /dev/i2c-1
 *   ./i2c_demo -b 1 -a 0x68 -r 0x75     读 MPU6050 的 WHO_AM_I（应返回 0x68）
 *   ./i2c_demo -b 1 -a 0x50 -r 0x00 -n 16   从 EEPROM 连续读 16 字节
 *   ./i2c_demo -b 1 -a 0x68 -w 0x6B -d 0x00 写 1 字节（MPU6050 解除休眠）
 *   ./i2c_demo -b 1 -a 0x48 -t          读 LM75 温度
 *
 * 参数：
 *   -b <bus>   总线号，默认 1（设备节点 /dev/i2c-1）
 *   -a <addr>  7 位从机地址，支持 0x68 / 104 两种写法
 *   -r <reg>   要读的寄存器地址
 *   -n <len>   读取长度，默认 1
 *   -w <reg>   要写的寄存器地址
 *   -d <val>   写入的值（1 字节）
 *   -t         温度模式（默认地址 0x48）
 *   -s         扫描总线
 *   -h         帮助
 *
 * 【运行前提】
 *   1. 内核已加载对应 I2C 适配器驱动（如 i2c-dev）：
 *        sudo modprobe i2c-dev        或  ls /dev/i2c-*
 *   2. 有权限访问设备节点：用 root，或把用户加入 i2c 组：
 *        sudo usermod -aG i2c $USER   （重新登录生效）
 *   3. 硬件接线：SDA/SCL 上拉电阻、共地、地址正确（A0/A1/A2 引脚决定）
 *
 * 【调试技巧】
 *   - 先用 i2cdetect -l 看有哪些总线，再用本程序 -s 扫描
 *   - 用示波器/逻辑分析仪抓 SDA/SCL，对照器件手册时序图
 *   - ENXIO = 没应答（接线/地址/上拉问题），EBUSY = 被内核驱动占用
 * ========================================================================= */

#include "i2c_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>       /* getopt */

/**
 * @brief 打印命令行帮助文本
 *
 * 向 stdout 输出本工具的用法说明和参数列表，供用户参考。
 *
 * @param[in] prog 程序名（argv[0]），用于格式化输出
 *
 * @return 无
 */
static void usage(const char *prog)
{
    printf("用法:\n"
           "  %s -s                       扫描总线(默认 /dev/i2c-1)\n"
           "  %s -b 1 -a 0x68 -r 0x75     读寄存器\n"
           "  %s -b 1 -a 0x68 -r 0x00 -n 8  读 8 字节\n"
           "  %s -b 1 -a 0x68 -w 0x6B -d 0x00  写 1 字节\n"
           "  %s -b 1 -a 0x48 -t          读温度(LM75)\n"
           "\n参数: -b 总线号(默认1)  -a 从机地址  -r 读寄存器  -n 长度\n"
           "      -w 写寄存器  -d 写入值  -t 温度  -s 扫描  -h 帮助\n",
           prog, prog, prog, prog, prog);
}

/**
 * @brief 以十六进制打印缓冲区内容
 *
 * 每行 16 字节，左侧显示行偏移地址。用于直观查看 I2C 读回的寄存器值。
 *
 * @param[in] buf 待打印的数据缓冲区
 * @param[in] len 缓冲区长度（字节）
 *
 * @return 无
 */
static void hexdump(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (i % 16 == 0)
            printf("  %04zx: ", i);
        printf("%02x ", buf[i]);
        if (i % 16 == 15)
            printf("\n");
    }
    if (len % 16 != 0)
        printf("\n");
}

/**
 * @brief 把命令行参数字符串解析成整数
 *
 * 支持 "0x68"（十六进制）、"0X68"、"104"（十进制）等写法，内部用 strtol(base=0)。
 * 范围限制 0 ~ 0xFFFF。非法输入直接 exit(1)。
 *
 * @param[in] s    待解析的字符串
 * @param[in] what 该数字的描述（如 "总线号"、"从机地址"），用于错误提示
 *
 * @return 解析成功返回对应整数值；失败则进程退出
 */
static int parse_num(const char *s, const char *what)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);
    if (end == s || *end != '\0' || v < 0 || v > 0xFFFF) {
        fprintf(stderr, "错误: %s 不是合法数字: %s\n", what, s);
        exit(1);
    }
    return (int)v;
}

/**
 * @brief 把 errno 翻译成可读性错误信息并给出排查建议
 *
 * 根据当前 errno 值（ENXIO / EBUSY / EACCES / ENOENT / EOPNOTSUPP 等），
 * 向 stderr 输出对应的中文解释和排查方向。
 *
 * @param[in] what 出错的动作描述（如 "打开设备"、"读寄存器"）
 * @param[in] path I2C 设备节点路径（如 "/dev/i2c-1"）
 * @param[in] addr 出错的从机地址（7-bit），可为 0
 *
 * @return 无
 */
static void report_error(const char *what, const char *path, int addr)
{
    /* 先保存 errno：printf/strerror 等库函数可能改写它，必须在调用前取走 */
    int err = errno;

    fprintf(stderr, "错误: %s 失败: %s\n", what, strerror(err));
    switch (err) {
    case ENXIO:
        fprintf(stderr, "  该地址(0x%02x)无应答：检查接线、地址引脚、上拉电阻\n", addr);
        break;
    case EBUSY:
        fprintf(stderr, "  地址 0x%02x 已被内核驱动占用：先卸载驱动或换地址\n", addr);
        break;
    case EACCES:
    case EPERM:
        fprintf(stderr, "  权限不足：用 sudo，或把用户加入 i2c 组\n");
        break;
    case ENOENT:
        fprintf(stderr, "  设备节点 %s 不存在：确认 i2c-dev 已加载(ls /dev/i2c-*)\n", path);
        break;
    case EOPNOTSUPP:
        fprintf(stderr, "  该适配器不支持此操作：换用 SMBus 接口或换适配器\n");
        break;
    default:
        break;
    }
}

/**
 * @brief I2C 用户态命令行工具主函数
 *
 * 功能：总线扫描、任意寄存器读/写、LM75/TMP102 温度读取。
 *
 * 编译：
 *   gcc -Wall -O2 i2c_demo.c i2c_util.c -o i2c_demo
 *
 * 运行前提：
 *   - 内核加载 i2c-dev：sudo modprobe i2c-dev
 *   - 权限：sudo，或 sudo usermod -aG i2c $USER
 *
 * 典型用法：
 *   ./i2c_demo -s                        扫描默认总线 /dev/i2c-1
 *   ./i2c_demo -b 1 -a 0x68 -r 0x75      读 MPU6050 WHO_AM_I
 *   ./i2c_demo -b 1 -a 0x48 -t           读 LM75 温度
 *   ./i2c_demo -b 1 -a 0x68 -w 0x6B -d 0x00  写 1 字节
 *
 * @param[in] argc 参数个数
 * @param[in] argv 参数数组
 *
 * @return 0 成功，非 0 失败
 */
int main(int argc, char **argv)
{
    int bus      = 1;         /* 默认总线 1 */
    int addr     = -1;        /* 未指定；由动作决定默认值 */
    int rd_reg   = -1, rd_len = 1;
    int wr_reg   = -1, wr_val = -1;
    int do_scan  = 0, do_temp = 0;
    int opt;

    /* 冒号表示该选项需要参数 */
    while ((opt = getopt(argc, argv, "b:a:r:n:w:d:sth")) != -1) {
        switch (opt) {
        case 'b': bus     = parse_num(optarg, "总线号");   break;
        case 'a': addr    = parse_num(optarg, "从机地址"); break;
        case 'r': rd_reg  = parse_num(optarg, "寄存器");   break;
        case 'n': rd_len  = parse_num(optarg, "长度");     break;
        case 'w': wr_reg  = parse_num(optarg, "寄存器");   break;
        case 'd': wr_val  = parse_num(optarg, "写入值");   break;
        case 't': do_temp = 1;                             break;
        case 's': do_scan = 1;                             break;
        case 'h':
        default:  usage(argv[0]); return (opt == 'h') ? 0 : 1;
        }
    }

    char path[64];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);

    /* ---- 模式 1：扫描 ---- */
    if (do_scan) {
        int n = i2c_scan(path);
        if (n < 0) {
            report_error("打开总线", path, 0);
            return 1;
        }
        return 0;
    }

    /* ---- 模式 2：温度（默认 LM75 地址 0x48） ---- */
    if (do_temp) {
        if (addr < 0)
            addr = 0x48;

        i2c_dev_t dev;
        if (i2c_open(&dev, path, addr) < 0) {
            report_error("打开设备", path, addr);
            return 1;
        }

        /* LM75 温度寄存器 0x00 输出 2 字节，格式为"高字节先"的 11 位
         * 二进制补码（低 5 位为小数位）。所以这里必须用原生 i2c_read_reg：
         * 用 SMBus read_word 会被内核按小端重组，导致高低字节颠倒。 */
        uint8_t b[2];
        if (i2c_read_reg(&dev, 0x00, b, sizeof(b)) < 0) {
            report_error("读温度寄存器", path, addr);
            i2c_close(&dev);
            return 1;
        }

        int16_t raw = (int16_t)((b[0] << 8) | b[1]);
        /* 右移 5 位取 11 位有效数据，每个 LSB = 0.125°C */
        double celsius = (raw >> 5) * 0.125;
        printf("温度: %.3f °C  (raw=0x%04x)\n", celsius, (unsigned)raw);

        i2c_close(&dev);
        return 0;
    }

    /* ---- 模式 3/4：读 / 写寄存器 ---- */
    if (rd_reg < 0 && wr_reg < 0) {
        usage(argv[0]);
        return 1;
    }
    if (addr < 0) {
        fprintf(stderr, "错误: 读/写寄存器必须用 -a 指定从机地址\n");
        return 1;
    }
    if (rd_len <= 0 || rd_len > I2C_MAX_XFER) {
        fprintf(stderr, "错误: -n 必须在 1~%d 之间\n", I2C_MAX_XFER);
        return 1;
    }
    if (wr_reg >= 0 && (wr_val < 0 || wr_val > 0xFF)) {
        fprintf(stderr, "错误: -d 写入值必须在 0~255 之间\n");
        return 1;
    }

    i2c_dev_t dev;
    if (i2c_open(&dev, path, addr) < 0) {
        report_error("打开设备", path, addr);
        return 1;
    }

    if (wr_reg >= 0) {
        uint8_t val = (uint8_t)wr_val;
        if (i2c_write_reg(&dev, (uint8_t)wr_reg, &val, 1) < 0) {
            report_error("写寄存器", path, addr);
            i2c_close(&dev);
            return 1;
        }
        printf("写成功: addr=0x%02x reg=0x%02x val=0x%02x\n",
               addr, wr_reg, val);
    }

    if (rd_reg >= 0) {
        uint8_t buf[I2C_MAX_XFER];
        if (i2c_read_reg(&dev, (uint8_t)rd_reg, buf, (size_t)rd_len) < 0) {
            report_error("读寄存器", path, addr);
            i2c_close(&dev);
            return 1;
        }
        printf("读成功: addr=0x%02x reg=0x%02x len=%d\n", addr, rd_reg, rd_len);
        hexdump(buf, (size_t)rd_len);
    }

    i2c_close(&dev);
    return 0;
}
