/* =========================================================================
 * xl9535_demo.c —— XL9535 / XL9555 16-bit I2C I/O 扩展器 命令行演示
 *
 * 基于 xl9535.h / xl9535.c 驱动，演示真实芯片的典型使用场景：
 *
 *   1. 扫描 /dev/i2c-N 上的 XL9535（默认地址 0x20，A0~A2 可变）
 *   2. 自检：验证芯片应答 + 安全态配置
 *   3. 输出测试：Port0 配置为输出，循环写 0x00/0xFF/0xAA/0x55
 *      接 LED 时能看到"全灭→全亮→交替亮→棋盘亮"四种状态
 *   4. 16-bit 整体读写 —— 演示驱动内部的自动对调正确用法
 *   5. 极性反转 —— 适合低电平有效按键输入
 *
 * 【运行前提】
 *   - 内核已加载 i2c-dev：sudo modprobe i2c-dev
 *   - 硬件：XL9535 供电 2.3~5.5V，SDA/SCL 上拉 4.7k，INT 可悬空
 *   - A0/A1/A2 引脚电平决定地址（默认全 GND = 0x20）
 *   - 输出测试时外部建议接 LED（Pn → 220Ω → LED → GND），
 *     XL9535 低电平输出能力 25mA，足够亮 LED
 *
 * 用法：
 *   扫描总线：
 *     ./xl9535_demo -b 1
 *
 *   自检 + 安全配置（默认 A2=A1=A0=GND → 0x20）：
 *     ./xl9535_demo -b 1 -i
 *
 *   指定地址引脚（A0=1 → 地址 0x21）：
 *     ./xl9535_demo -b 1 -a0 1 -i
 *
 *   输出测试 5 秒（Port0 全输出，循环刷 4 种值）：
 *     ./xl9535_demo -b 1 -o -t 5
 *
 *   配置方向 + 写输出 + 读输入（全 16 引脚整体操作）：
 *     ./xl9535_demo -b 1 -d 0xFF00 -w 0x0055 -r
 *       含义：Port0 输出(0) + Port1 输入(1)；Port0 输出 0x55；读所有输入
 *
 *   极性反转（Port0 bit0 设为反转，适合低有效按键）：
 *     ./xl9535_demo -b 1 -p 0x0100 -r
 *
 *   帮助：
 *     ./xl9535_demo -h
 * ========================================================================= */

#include "xl9535.h"
#include "i2c_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>       /* getopt, usleep */

/* -------------------------------------------------------------------------
 * 命令行解析辅助（复用 i2c_demo.c 里的 parse_num —— 支持 0xHH 和十进制）
 * ------------------------------------------------------------------------- */
/**
 * @brief 把命令行参数字符串解析成整数
 *
 * 支持 "0xHH"（十六进制）和十进制两种写法，范围 0 ~ 0xFFFF。
 * 非法输入直接 exit(1)。
 *
 * @param[in] s    待解析的字符串
 * @param[in] what 参数描述（如 "A0"、"方向掩码"），用于错误提示
 *
 * @return 解析成功返回对应整数值；失败则进程退出
 */
static int parse_num(const char *s, const char *what)
{
    char *end = NULL;
    long v = strtol(s, &end, 0);       /* base=0 → 自动识别 0x 前缀 */
    if (end == s || *end != '\0' || v < 0 || v > 0xFFFF) {
        fprintf(stderr, "错误: %s 不是合法数字: %s\n", what, s);
        exit(1);
    }
    return (int)v;
}

/**
 * @brief 打印本工具的命令行帮助
 *
 * 输出 xl9535_demo 的用法、参数说明、自动对调机制注意事项等。
 *
 * @param[in] prog 程序名（argv[0]）
 *
 * @return 无
 */
static void usage(const char *prog)
{
    printf(
        "用法:\n"
        "  %s -b 1                       扫描总线并列出 XL9535\n"
        "  %s -b 1 -i                    自检 + 安全配置（所有引脚 INPUT）\n"
        "  %s -b 1 -o -t 5               输出测试：Port0 刷 0x00/0xFF/0xAA/0x55，持续 5s\n"
        "  %s -b 1 -d 0xFF00             配置方向（低字节=Port0, 高字节=Port1; 0=输出 1=输入）\n"
        "  %s -b 1 -w 0x0055             写输出（低字节=Port0, 高字节=Port1）\n"
        "  %s -b 1 -r                    读所有输入引脚电平\n"
        "  %s -b 1 -p 0x0100             极性反转掩码（1=反转该引脚输入极性）\n"
        "  %s -b 1 -a0 1 -a1 0 -a2 0     指定硬件地址引脚电平（默认全 0 → 0x20）\n"
        "  %s -h                         帮助\n"
        "\n"
        "说明:\n"
        "  - XL9535 地址 = 0x20 + (A2<<2 | A1<<1 | A0)，A0/A1/A2 接 GND=0, VCC=1\n"
        "  - 方向位: 0 = OUTPUT, 1 = INPUT（这跟某些 I/O 扩展器相反，别搞反！）\n"
        "  - 16-bit 参数如 0xFF00: 低字节操作 Port0(P00~P07), 高字节操作 Port1(P10~P17)\n"
        "  - 上电默认所有引脚为 INPUT（高阻），Output 默认 0xFF（全高）\n"
        "  - 自动对调机制: 连续写 Output 时先写 Port1 再写 Port0，芯片自动对调\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/**
 * @brief 以直观格式打印 8-bit 端口值
 *
 * 显示十六进制 + 每一位对应的 Px0~Px7 引脚标签和 H/L 电平，
 * 方便肉眼观察 GPIO 状态。
 *
 * @param[in] name 端口标签（如 "Port0"）
 * @param[in] val  8-bit 端口值
 *
 * @return 无
 */
static void print_port_bits(const char *name, uint8_t val)
{
    printf("  %s = 0x%02X  (二进制:", name, val);
    for (int bit = 7; bit >= 0; bit--) {
        printf("%d", (val >> bit) & 1);
    }
    printf(")  ");
    /* 引脚标签：高位 Px7 ... 低位 Px0 */
    for (int bit = 7; bit >= 0; bit--) {
        if ((val >> bit) & 1)
            printf("Px%d=H ", bit);
        else
            printf("Px%d=L ", bit);
    }
    printf("\n");
}

/**
 * @brief 把 errno 翻译成可读错误信息并给出排查方向
 *
 * 针对 XL9535 操作失败时（ENXIO / EBUSY / EACCES 等），输出中文提示。
 *
 * @param[in] what 出错动作描述（如 "写 Port0 输出"）
 * @param[in] path I2C 设备节点路径
 *
 * @return 无
 */
static void report_errno(const char *what, const char *path)
{
    int err = errno;
    fprintf(stderr, "错误: %s 失败: %s\n", what, strerror(err));
    switch (err) {
    case ENXIO:
        fprintf(stderr, "  该地址无应答：检查 A0/A1/A2 引脚、SDA/SCL 上拉、供电\n");
        break;
    case EBUSY:
        fprintf(stderr, "  该地址已被内核驱动占用：先卸载驱动或换地址\n");
        break;
    case EACCES:
    case EPERM:
        fprintf(stderr, "  权限不足：用 sudo，或把用户加入 i2c 组\n");
        break;
    case ENOENT:
        fprintf(stderr, "  设备节点 %s 不存在：确认 i2c-dev 已加载\n", path);
        break;
    case EOPNOTSUPP:
        fprintf(stderr, "  适配器不支持此操作：换支持 I2C_RDWR 的适配器\n");
        break;
    default:
        break;
    }
}

/**
 * @brief 执行 XL9535 Port0 输出测试循环
 *
 * 把 Port0 全部设为输出，依次循环写入 0x00→0xFF→0xAA→0x55，
 * 每个值停 500ms，并读回 Input Port 验证。结束后恢复安全态（全部输入）。
 * 外部接 LED 可直观看到四种状态变化。
 *
 * @param[in,out] dev     已打开的 XL9535 设备句柄
 * @param[in]     seconds 测试总时长（秒）
 *
 * @return 0 成功，-1 失败
 */
static int run_output_test(i2c_dev_t *dev, int seconds)
{
    const uint8_t pattern[] = { 0x00, 0xFF, 0xAA, 0x55 };
    const int num_patterns = sizeof(pattern) / sizeof(pattern[0]);
    int count = seconds * 2;    /* 每状态 500ms */
    if (count <= 0) count = 4;

    /* Step 1: 把 Port0 设为全输出 (Config0 = 0x00) */
    if (xl9535_set_dir_port0(dev, 0x00) < 0) {
        report_errno("配置 Port0 方向", dev->path);
        return -1;
    }
    /* Step 2: 把 Port1 设为全输入 (Config1 = 0xFF —— 安全，不驱动其他设备) */
    if (xl9535_set_dir_port1(dev, 0xFF) < 0) {
        report_errno("配置 Port1 方向", dev->path);
        return -1;
    }

    printf("[输出测试] Port0 设为 OUTPUT, Port1 设为 INPUT\n");
    printf("[输出测试] 循环写 0x00→0xFF→0xAA→0x55，每 500ms 切换，共 %d 次 → %d 秒\n",
           count, count * 500 / 1000);
    printf("          接 LED(Pn→220Ω→GND) 时:\n");
    printf("            0x00 → 全灭（低电平输出，LED 反接会亮）\n");
    printf("            0xFF → 全亮（高电平输出，LED 正极接 Pn 会亮）\n");
    printf("            0xAA → P07,P05,P03,P01 亮；P06,P04,P02,P00 灭\n");
    printf("            0x55 → P06,P04,P02,P00 亮；P07,P05,P03,P01 灭\n");

    for (int i = 0; i < count; i++) {
        uint8_t val = pattern[i % num_patterns];

        /* 【这里演示了 xl9535_write_port0 的"安全"用法】
         * 每次调用只写 1 字节 + STOP，不受芯片"寄存器对自动对调"影响。
         * 如果写成 write_port0 再 write_port1，第二次调用的 STOP 也会
         * 把第一次的事务切断，所以两次独立 write 也完全安全 ——
         * 自动对调只在**同一次 START-STOP 事务内连续写**时才触发。 */
        if (xl9535_write_port0(dev, val) < 0) {
            report_errno("写 Port0 输出", dev->path);
            xl9535_configure_all(dev, 0xFFFF);    /* 失败也要恢复安全态 */
            return -1;
        }

        /* 同时读回 Input Port 验证（Output 引脚也能被 Input Port 读到） */
        int in0 = xl9535_read_port0(dev);
        if (in0 >= 0) {
            printf("  [%02d/%02d] Port0 out=0x%02X  (read back=0x%02X)  %s\n",
                   i + 1, count, val, in0,
                   (in0 == val) ? "OK" : "MISMATCH!");
        } else {
            printf("  [%02d/%02d] Port0 out=0x%02X  (读回失败: %s)\n",
                   i + 1, count, val, strerror(errno));
        }

        usleep(500 * 1000);    /* 500 ms */
    }

    /* Step 3: 恢复安全态 —— 所有引脚重新设为 INPUT（高阻） */
    printf("[输出测试] 恢复安全态: 所有引脚 → INPUT (高阻)\n");
    xl9535_configure_all(dev, 0xFFFF);
    return 0;
}

/**
 * @brief XL9535 命令行演示工具主函数
 *
 * 功能：总线扫描、芯片自检、方向/输出/极性配置、输入读取、Port0 输出测试。
 *
 * 编译：
 *   gcc -Wall -O2 xl9535_demo.c xl9535.c i2c_util.c -o xl9535_demo
 *
 * 运行前提：
 *   - 内核加载 i2c-dev：sudo modprobe i2c-dev
 *   - XL9535 供电 2.3~5.5V，SDA/SCL 4.7k 上拉，A0/A1/A2 接好
 *
 * 典型用法：
 *   ./xl9535_demo -b 1                    扫描
 *   ./xl9535_demo -b 1 -i                 自检
 *   ./xl9535_demo -b 1 -o -t 5            输出测试 5 秒
 *   ./xl9535_demo -b 1 -d 0xFF00 -w 0x0055 -r  方向+输出+读输入
 *
 * @param[in] argc 参数个数
 * @param[in] argv 参数数组
 *
 * @return 0 成功，非 0 失败
 */
int main(int argc, char **argv)
{
    int bus   = 1;                  /* 默认 /dev/i2c-1 */
    int a2 = 0, a1 = 0, a0 = 0;      /* 默认 A2=A1=A0=GND → 地址 0x20 */

    int do_selftest = 0;
    int do_output   = 0;
    int do_read     = 0;
    int do_scan     = 0;

    int dir16 = -1;      /* 未指定方向 */
    int out16 = -1;      /* 未指定输出值 */
    int pol16 = -1;      /* 未指定极性 */

    int out_test_seconds = 3;

    int opt;
    /* getopt: 冒号=需要参数, 无冒号=纯开关 */
    while ((opt = getopt(argc, argv, "b:a0:a1:a2:iot:d:w:p:h")) != -1) {
        switch (opt) {
        case 'b': bus = parse_num(optarg, "总线号");             break;
        case 'a0': a0 = parse_num(optarg, "A0");                 break;
        case 'a1': a1 = parse_num(optarg, "A1");                 break;
        case 'a2': a2 = parse_num(optarg, "A2");                 break;
        case 'i': do_selftest = 1;                               break;
        case 'o': do_output = 1;                                 break;
        case 'r': do_read = 1;                                   break;
        case 't': out_test_seconds = parse_num(optarg, "秒数");  break;
        case 'd': dir16 = parse_num(optarg, "方向掩码");         break;
        case 'w': out16 = parse_num(optarg, "输出值");           break;
        case 'p': pol16 = parse_num(optarg, "极性掩码");         break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    /* 构造设备节点路径 */
    char path[64];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);

    /* ---- 模式 0：扫描 ----
     * 没有指定任何操作时默认扫描（跟 -s 效果一样）。
     * XL9535 系列固定地址族 0x20~0x27，如果扫描结果里有这些，
     * 大概率就是 XL9535/XL9555（但不能 100% 确认，其他器件可能撞地址）。 */
    if (!do_selftest && !do_output && !do_read && dir16 < 0 &&
        out16 < 0 && pol16 < 0) {
        int n = i2c_scan(path);
        if (n < 0) {
            report_errno("打开总线", path);
            return 1;
        }
        /* 检查扫描结果里有没有 XL9535 地址族 */
        printf("\nXL9535 地址族: 0x20~0x27 (A2,A1,A0 决定)\n");
        printf("当前配置 A2=%d A1=%d A0=%d → 目标地址 0x%02X\n",
               a2, a1, a0, xl9535_addr(a2, a1, a0));
        if (n == 0) {
            printf("警告: 没扫到任何设备 —— 检查接线/SDA/SCL 上拉/供电\n");
        }
        return 0;
    }

    /* ---- 以下所有模式都需要先打开 XL9535 ---- */
    i2c_dev_t dev;
    int addr = xl9535_addr(a2, a1, a0);
    printf("打开 %s, XL9535 地址 0x%02X (A2=%d A1=%d A0=%d)...\n", path, addr, a2, a1, a0);

    if (xl9535_open(&dev, path, a2, a1, a0) < 0) {
        report_errno("打开 XL9535", path);
        fprintf(stderr, "  提示: XL9535 默认地址 0x20, 如使用不同 A0/A1/A2 引脚, 用 -a0/-a1/-a2 指定\n");
        return 1;
    }

    int rc = 0;

    /* ---- 模式 1：自检 ---- */
    if (do_selftest) {
        /* safe_inputs=1: 读完后把所有引脚重新设为高阻输入（安全默认） */
        if (xl9535_selftest(&dev, 1) < 0) {
            rc = 1;
            goto done;
        }
    }

    /* ---- 模式 2：配置方向 ---- */
    if (dir16 >= 0) {
        printf("配置方向: 0x%04X (0=OUTPUT, 1=INPUT)\n", dir16);
        printf("  Port0(P00~P07) bit 0..7 = 0x%02X\n", (dir16 & 0xFF));
        printf("  Port1(P10~P17) bit 8..15 = 0x%02X\n", (dir16 >> 8) & 0xFF);

        /* 【这里演示了一次性写两个 Config 寄存器 —— 利用自动对调】
         * xl9535_set_dir_both 内部：先发 Config1(0x07)+port1_mask,
         * 再发 1 字节 port0_mask → 自动进入 Config0(0x06)。
         * 一次 START/STOP 搞定两个寄存器，比单独调用两次快。 */
        if (xl9535_configure_all(&dev, (uint16_t)dir16) < 0) {
            report_errno("配置方向", dev.path);
            rc = 1;
            goto done;
        }
        printf("  → 成功\n");
    }

    /* ---- 模式 3：写输出 ---- */
    if (out16 >= 0) {
        printf("写输出: 0x%04X\n", out16);
        print_port_bits("  Port0", (uint8_t)(out16 & 0xFF));
        print_port_bits("  Port1", (uint8_t)((out16 >> 8) & 0xFF));

        /* 【这里也用了自动对调】
         * xl9535_write_all_outputs → xl9535_write_outputs16:
         * 先发 Output1(0x03)+out1, 再发 out0 → 自动进 Output0(0x02)。 */
        if (xl9535_write_all_outputs(&dev, (uint16_t)out16) < 0) {
            report_errno("写输出", dev.path);
            rc = 1;
            goto done;
        }
        printf("  → 成功\n");
    }

    /* ---- 模式 4：极性反转 ---- */
    if (pol16 >= 0) {
        uint8_t pol0 = (uint8_t)(pol16 & 0xFF);
        uint8_t pol1 = (uint8_t)((pol16 >> 8) & 0xFF);
        printf("极性反转掩码: Port0=0x%02X, Port1=0x%02X (1=反转极性)\n", pol0, pol1);
        if (xl9535_set_pol_port0(&dev, pol0) < 0 ||
            xl9535_set_pol_port1(&dev, pol1) < 0) {
            report_errno("设置极性", dev.path);
            rc = 1;
            goto done;
        }
        printf("  → 成功（适合低电平有效按键 —— 按下时读到 1）\n");
    }

    /* ---- 模式 5：读输入 ---- */
    if (do_read) {
        int val = xl9535_read_all_inputs(&dev);
        if (val < 0) {
            report_errno("读输入", dev.path);
            rc = 1;
            goto done;
        }
        printf("读所有输入:\n");
        print_port_bits("  Port0", (uint8_t)(val & 0xFF));
        print_port_bits("  Port1", (uint8_t)((val >> 8) & 0xFF));
        printf("  16-bit raw = 0x%04X\n", (unsigned)val);
    }

    /* ---- 模式 6：输出测试循环 ---- */
    if (do_output) {
        if (run_output_test(&dev, out_test_seconds) < 0) {
            rc = 1;
            goto done;
        }
    }

done:
    xl9535_close(&dev);
    if (rc == 0) {
        printf("完成。\n");
    }
    return rc;
}
