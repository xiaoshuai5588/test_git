/* =========================================================================
 * ad7998_demo.c —— AD7998 8-channel 12-bit I2C ADC 命令行演示
 *
 * 依赖：i2c_util.h / i2c_util.c + ad7998.h / ad7998.c
 *
 * 默认硬件配置：
 *   - 供电 VDD = 5V（或 3.3V 但参考必须 ≤ 3.3V）
 *   - 参考电压 REFIN = 4.096V （接高精度参考源，如 REF02）
 *   - LSB = 4.096 / 4096 = 0.001 V = 1 mV，所以 raw / 1000 = 伏特
 *   - AS 引脚浮空 → I2C 地址 0x20
 *
 * 用法：
 *   扫描总线：
 *     ./ad7998_demo -b 1
 *
 *   自检 + 配置验证：
 *     ./ad7998_demo -b 1 -i
 *
 *   读 VIN3 单次：
 *     ./ad7998_demo -b 1 -c 3
 *
 *   读 VIN1 + VIN3 + VIN5 序列：
 *     ./ad7998_demo -b 1 -s 1,3,5
 *
 *   连续采样 VIN2 100 次，打印统计：
 *     ./ad7998_demo -b 1 -c 2 -n 100
 *
 *   连续采样 8 路全通道 50 次循环（每轮输出一行 8 通道电压）：
 *     ./ad7998_demo -b 1 -s 1,2,3,4,5,6,7,8 -n 50
 *
 *   使用非默认地址（AS=VDD → 0x22）：
 *     ./ad7998_demo -b 1 -a 0x22 -c 1
 *
 *   帮助：
 *     ./ad7998_demo -h
 * ========================================================================= */

#include "ad7998.h"
#include "i2c_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>       /* getopt, usleep */
#include <ctype.h>        /* isdigit */

/**
 * @brief 把逗号分隔的通道列表字符串解析成通道号数组
 *
 * 支持 "1,3,5"、"VIN1,VIN3"、"vin1+vin3" 等写法（跳过非数字前缀）。
 * 最多解析 max 个通道，范围 1..AD7998_NUM_CHANNELS。
 * 非法输入直接 exit(1)。
 *
 * @param[in]  s    待解析的字符串（逗号分隔）
 * @param[out] out  解析得到的通道号数组（调用方分配，容量 >= max）
 * @param[in]  max  数组容量上限
 *
 * @return 成功返回解析到的通道个数（1..max）；失败则进程退出
 */
static int parse_channel_list(const char *s, int *out, int max)
{
    int n = 0;
    char *dup = strdup(s);
    if (!dup) { errno = ENOMEM; exit(1); }

    char *tok = strtok(dup, ",");
    while (tok && n < max) {
        /* 允许 "1"、"VIN1"、"vin1" 等写法 */
        char *p = tok;
        while (*p && !isdigit(*p)) p++;   /* 跳过 "VIN"/"vin" 前缀 */
        if (!*p) {
            fprintf(stderr, "错误: 无法解析通道号 '%s'\n", tok);
            free(dup); exit(1);
        }
        int v = atoi(p);
        if (v < 1 || v > AD7998_NUM_CHANNELS) {
            fprintf(stderr, "错误: 通道号 %d 超出范围 1..%d\n", v, AD7998_NUM_CHANNELS);
            free(dup); exit(1);
        }
        out[n++] = v;
        tok = strtok(NULL, ",");
    }
    free(dup);
    if (n == 0) {
        fprintf(stderr, "错误: -s 后面需要至少 1 个通道号\n");
        exit(1);
    }
    return n;
}

/**
 * @brief 打印本工具的命令行帮助
 *
 * 输出 ad7998_demo 的用法、参数说明、硬件配置提示。
 *
 * @param[in] prog 程序名（argv[0]）
 *
 * @return 无
 */
static void usage(const char *prog)
{
    printf(
        "用法:\n"
        "  %s -b 1                       扫描总线 (显示 AD7998 地址族 0x20~0x24)\n"
        "  %s -b 1 -i                    自检 + 打印当前 Config Register\n"
        "  %s -b 1 -c 3                  单次读取 VIN3\n"
        "  %s -b 1 -c 3 -n 100           连续采样 VIN3 100 次 + 统计\n"
        "  %s -b 1 -s 1,3,5              序列模式读 VIN1+VIN3+VIN5\n"
        "  %s -b 1 -s 1,2,3,4,5,6,7,8    8 通道全扫\n"
        "  %s -b 1 -s 1,2,3,4,5,6,7,8 -n 20   全通道连续采样 20 轮\n"
        "  %s -b 1 -a 0x22 -c 1          非默认地址 (AD7998-0 + AS=VDD → 0x22)\n"
        "  %s -h                         帮助\n"
        "\n"
        "参数:\n"
        "  -b <bus>   I2C 总线号 (默认 1, 设备节点 /dev/i2c-N)\n"
        "  -a <addr>  AD7998 7-bit 地址 (默认 0x20, AS Float)\n"
        "  -c <ch>    单次通道号 1..8\n"
        "  -s <list>  序列模式通道列表, 逗号分隔, 如 '1,3,5,8'\n"
        "  -n <cnt>   连续采样次数 (默认 1, 仅当 -c 或 -s 时有效)\n"
        "  -i         自检: 读 Config Register + 校验 FLTR 位\n"
        "  -h         帮助\n"
        "\n"
        "硬件:\n"
        "  参考电压 REFIN = 4.096V (本驱动默认值, LSB=1mV)\n"
        "  模拟输入范围 0V ~ REFIN\n"
        "  AS 引脚: Float=0x20, AGND=0x21(AD7998-0), VDD=0x22(AD7998-0)\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/**
 * @brief 对单个通道连续采样并输出统计结果
 *
 * 连续读取指定通道 count 次，每次打印一行，最后汇总成功率、
 * 平均/最小/最大电压、峰峰值、通道不匹配次数、Alert 标志次数。
 *
 * @param[in,out] dev     已打开的 AD7998 设备句柄
 * @param[in]     channel 通道号（1..8，VIN1~VIN8）
 * @param[in]     count   采样次数
 *
 * @return 0 全部成功，1 有失败
 */
static int run_continuous_single(i2c_dev_t *dev, int channel, int count)
{
    printf("连续采样 VIN%d, 共 %d 次\n", channel, count);

    double sum = 0;
    double min_v = 1e9, max_v = -1e9;
    int errors = 0, bad_ch = 0, alerts = 0;

    for (int i = 0; i < count; i++) {
        int raw, ch, alert;
        double volts;

        if (ad7998_read_channel(dev, channel, &raw, &ch, &alert, &volts) < 0) {
            errors++;
            fprintf(stderr, "  [%d/%d] 读失败: %s\n", i + 1, count, strerror(errno));
            continue;
        }

        if (ch != channel) bad_ch++;
        if (alert) alerts++;

        sum += volts;
        if (volts < min_v) min_v = volts;
        if (volts > max_v) max_v = volts;

        /* 首次打印格式说明 + 每次一行结果 */
        if (i == 0)
            printf("  #次  raw(12bit)  CH  Alert    电压(V)\n");
        printf("  %3d  0x%03X  %d  %d  %8.4f\n", i + 1, raw, ch, alert, volts);
    }

    /* 统计 */
    printf("\n== 统计 (VIN%d, %d 次, VREF=%.3fV) ==\n", channel, count, AD7998_VREF_V);
    printf("  成功率: %d/%d (%d%%)\n", count - errors, count,
           count > 0 ? (100 * (count - errors) / count) : 0);
    printf("  平均电压: %.4f V\n", sum / (count - errors > 0 ? count - errors : 1));
    printf("  最小: %.4f V  最大: %.4f V  峰峰值: %.4f V\n",
           min_v, max_v, max_v - min_v);
    printf("  读回通道不匹配: %d 次  Alert_Flag=1: %d 次\n", bad_ch, alerts);

    if (max_v > AD7998_VREF_V || min_v < 0) {
        printf("  ⚠️ 检测到超出输入范围 (0 ~ %.3fV), 检查接线或参考电压\n", AD7998_VREF_V);
    }

    return errors == 0 ? 0 : 1;
}

/**
 * @brief 序列模式连续采样多个通道并统计
 *
 * 先构造 config_mask，然后连续 rounds 轮序列读取，
 * 每轮打印所有通道电压，最后汇总各通道统计信息。
 *
 * @param[in,out] dev       已打开的 AD7998 设备句柄
 * @param[in]     channels  通道号数组（1..8）
 * @param[in]     nch       通道个数
 * @param[in]     rounds    连续采样轮数
 *
 * @return 0 全部成功，1 有失败
 */
static int run_continuous_sequence(i2c_dev_t *dev, const int *channels, int nch, int rounds)
{
    /* 构造 config_mask */
    uint16_t mask = 0;
    for (int i = 0; i < nch; i++) mask |= AD7998_CH_BIT(channels[i] - 1);

    printf("序列模式连续采样, 通道: ");
    for (int i = 0; i < nch; i++) printf("VIN%d%s", channels[i], i < nch - 1 ? "+" : "");
    printf(", %d 轮\n", rounds);
    printf("Config Mask = 0x%04X, VREF = %.3fV\n", mask, AD7998_VREF_V);

    /* 逐通道累积统计 */
    double sum[8]   = {0};
    double min_v[8] = {1e9,1e9,1e9,1e9,1e9,1e9,1e9,1e9};
    double max_v[8] = {-1e9,-1e9,-1e9,-1e9,-1e9,-1e9,-1e9,-1e9};
    int err_count = 0;

    /* 分配一次采样的缓冲区 */
    int *raws = (int *)malloc(sizeof(int) * nch);
    int *chs  = (int *)malloc(sizeof(int) * nch);
    if (!raws || !chs) { errno = ENOMEM; return -1; }

    for (int r = 0; r < rounds; r++) {
        int got = 0;
        if (ad7998_read_sequence(dev, mask, raws, chs, &got) < 0) {
            err_count++;
            fprintf(stderr, "  轮 %d: 序列读失败: %s\n", r + 1, strerror(errno));
            continue;
        }
        if (got != nch) {
            err_count++;
            fprintf(stderr, "  轮 %d: 期望 %d 通道, 实际 %d\n", r + 1, nch, got);
            continue;
        }

        /* 打印首轮所有值 + 更新统计 */
        if (r == 0) {
            printf("  #轮  ");
            for (int i = 0; i < got; i++) printf("  VIN%2d", chs[i]);
            printf("\n");
        }

        printf("  %3d", r + 1);
        for (int i = 0; i < got; i++) {
            double v = (double)raws[i] * AD7998_VREF_V / 4096.0;
            int idx = chs[i] - 1;     /* CH1→索引0, ..., CH8→索引7 */
            sum[idx] += v;
            if (v < min_v[idx]) min_v[idx] = v;
            if (v > max_v[idx]) max_v[idx] = v;
            printf("  %7.4f", v);
        }
        printf("\n");
    }

    /* 汇总 */
    printf("\n== 统计 (%d 轮, VREF=%.3fV) ==\n", rounds, AD7998_VREF_V);
    printf("  成功率: %d/%d (%d%%)\n", rounds - err_count, rounds,
           rounds > 0 ? (100 * (rounds - err_count) / rounds) : 0);
    printf("  %-6s %10s %10s %10s %10s\n", "通道", "平均(V)", "最小(V)", "最大(V)", "峰峰值");
    for (int i = 0; i < nch; i++) {
        int ch = channels[i];
        int idx = ch - 1;
        int good = rounds - err_count;
        if (good > 0 && min_v[idx] != 1e9) {
            printf("  VIN%-4d %10.4f %10.4f %10.4f %10.4f\n",
                   ch, sum[idx] / good, min_v[idx], max_v[idx], max_v[idx] - min_v[idx]);
        } else {
            printf("  VIN%-4d       (无数据)\n", ch);
        }
    }

    free(raws); free(chs);
    return err_count == 0 ? 0 : 1;
}

/**
 * @brief AD7998 硬件自检
 *
 * 1) 读取 Configuration Register 确认芯片应答并打印各字段；
 * 2) 触发一次 VIN1 单次转换验证转换通路。
 *
 * @param[in,out] dev 已打开的 AD7998 设备句柄
 *
 * @return 0 成功，-1 失败
 */
static int run_selftest(i2c_dev_t *dev)
{
    /* 读 Config Register —— 验证芯片应答 + FLTR=1 */
    int cfg = ad7998_read_config(dev);
    if (cfg < 0) {
        fprintf(stderr, "自检失败: 读 Config Register 失败: %s\n", strerror(errno));
        return -1;
    }

    printf("AD7998[%s @ 0x%02X] 自检通过\n", dev->path, dev->addr);
    printf("  Config Register = 0x%04X\n", (unsigned)cfg);
    printf("  FLTR  (D3)      = %d (%s)\n", (cfg >> 3) & 1,
           (cfg & (1 << 3)) ? "开启 SDA/SCL 滤波" : "旁路滤波");
    printf("  ALERT_EN (D2)   = %d\n", (cfg >> 2) & 1);
    printf("  CH8..CH1 (D11..D4) = 0x%03X (通道选位)\n", (unsigned)((cfg >> 4) & 0xFF));
    printf("  VREF 设置 = %.3f V\n", AD7998_VREF_V);
    printf("  LSB = %.3f mV  (12-bit ADC)\n", AD7998_VREF_V * 1000.0 / 4096.0);
    printf("  输入范围 0V ~ %.3fV\n", AD7998_VREF_V);

    /* 触发一次 VIN1 读 —— 确认转换通路也通 */
    int raw, ch, alert;
    double volts;
    if (ad7998_read_channel(dev, 1, &raw, &ch, &alert, &volts) < 0) {
        fprintf(stderr, "  ⚠️ VIN1 单次读失败: %s (但 Config 读写正常, 芯片基本 OK)\n",
                strerror(errno));
    } else {
        printf("  VIN1 单次读 = 0x%03X = %.4f V (CH=%d, Alert=%d)\n", raw, volts, ch, alert);
    }
    return 0;
}

/**
 * @brief 把 errno 翻译成可读性错误信息（AD7998 专用）
 *
 * 针对 AD7998 操作失败（ENXIO / EBUSY / EACCES / EOPNOTSUPP），
 * 给出 AS 引脚配置和排查提示。
 *
 * @param[in] what 出错动作描述
 * @param[in] path I2C 设备节点路径（当前用于宏展开，未直接使用）
 *
 * @return 无
 */
static void report_errno(const char *what, const char *path)
{
    int err = errno;
    fprintf(stderr, "错误: %s 失败: %s\n", what, strerror(err));
    switch (err) {
    case ENXIO:
        fprintf(stderr, "  该地址无应答: 检查 AS 引脚电平、SDA/SCL 上拉、VDD 供电、REFIN\n");
        fprintf(stderr, "  AD7998-0 + AS=Float → 0x20, AGND=0x21, VDD=0x22\n");
        break;
    case EBUSY:
        fprintf(stderr, "  地址被内核驱动占用: 先卸载驱动或换 AS 引脚\n");
        break;
    case EACCES: case EPERM:
        fprintf(stderr, "  权限不足: 用 sudo 或 sudo usermod -aG i2c $USER\n");
        break;
    case EOPNOTSUPP:
        fprintf(stderr, "  适配器不支持 I2C_RDWR: 换支持完整 I2C 的适配器\n");
        break;
    default: break;
    }
    (void)path;
}

/**
 * @brief AD7998 8 通道 12-bit ADC 命令行演示工具主函数
 *
 * 功能：总线扫描、自检、单通道连续采样、序列模式多通道采样+统计。
 *
 * 编译：
 *   gcc -Wall -O2 ad7998_demo.c ad7998.c i2c_util.c -o ad7998_demo -lm
 *
 * 运行前提：
 *   - 内核加载 i2c-dev：sudo modprobe i2c-dev
 *   - 硬件：VDD=5V（或 3.3V），REFIN=4.096V 高精度参考，SDA/SCL 上拉
 *   - AS 引脚：Float→0x20(默认), AGND→0x21, VDD→0x22
 *
 * 典型用法：
 *   ./ad7998_demo -b 1                      扫描
 *   ./ad7998_demo -b 1 -i                   自检
 *   ./ad7998_demo -b 1 -c 3 -n 100          读 VIN3 100 次 + 统计
 *   ./ad7998_demo -b 1 -s 1,2,3,4,5,6,7,8   8 通道全扫
 *
 * @param[in] argc 参数个数
 * @param[in] argv 参数数组
 *
 * @return 0 成功，非 0 失败
 */
int main(int argc, char **argv)
{
    int bus       = 1;          /* 默认 /dev/i2c-1 */
    int addr      = AD7998_ADDR_DEFAULT;  /* 默认 0x20, AS Float */
    int do_selftest = 0;

    int single_ch = -1;         /* 单次通道号 */
    int channels[8];            /* 序列模式通道数组 */
    int nch = 0;                /* 序列通道数 */
    int rounds = 1;             /* 采样次数 */

    int opt;
    while ((opt = getopt(argc, argv, "b:a:c:s:n:ih")) != -1) {
        switch (opt) {
        case 'b': bus   = strtol(optarg, NULL, 0); break;
        case 'a': addr  = strtol(optarg, NULL, 0); break;
        case 'c': single_ch = strtol(optarg, NULL, 0); break;
        case 's': nch = parse_channel_list(optarg, channels, 8); break;
        case 'n': rounds = strtol(optarg, NULL, 0); break;
        case 'i': do_selftest = 1; break;
        case 'h':
        default:  usage(argv[0]); return (opt == 'h') ? 0 : 1;
        }
    }

    /* 构造路径 */
    char path[64];
    snprintf(path, sizeof(path), "/dev/i2c-%d", bus);

    /* ---- 模式 0: 扫描 ---- */
    if (!do_selftest && single_ch < 0 && nch == 0) {
        int n = i2c_scan(path);
        if (n < 0) {
            report_errno("打开总线", path);
            return 1;
        }
        printf("\nAD7998 地址族 (7-bit): 0x20~0x24\n");
        printf("  AS Float    → 0x20\n");
        printf("  AS AGND     → 0x21 (AD7998-0), 0x23 (AD7998-1)\n");
        printf("  AS VDD      → 0x22 (AD7998-0), 0x24 (AD7998-1)\n");
        printf("当前配置 addr=0x%02X (可用 -a 改)\n", addr);
        return 0;
    }

    /* ---- 打开 AD7998 ---- */
    i2c_dev_t dev;
    printf("打开 %s, AD7998 @ 0x%02X...\n", path, addr);
    if (ad7998_open(&dev, path, addr) < 0) {
        report_errno("打开 AD7998", path);
        return 1;
    }

    int rc = 0;

    /* ---- 模式 1: 自检 ---- */
    if (do_selftest) {
        if (run_selftest(&dev) < 0) rc = 1;
    }

    /* ---- 模式 2: 单次通道连续采样 ---- */
    if (single_ch >= 0) {
        if (single_ch < 1 || single_ch > AD7998_NUM_CHANNELS) {
            fprintf(stderr, "错误: 通道号 %d 超出 1..%d\n", single_ch, AD7998_NUM_CHANNELS);
            rc = 1; goto done;
        }
        if (rounds <= 0) rounds = 1;
        rc = run_continuous_single(&dev, single_ch, rounds);
    }

    /* ---- 模式 3: 序列模式 ---- */
    if (nch > 0) {
        if (rounds <= 0) rounds = 1;
        rc = run_continuous_sequence(&dev, channels, nch, rounds);
    }

done:
    ad7998_close(&dev);
    printf("退出。\n");
    return rc;
}
