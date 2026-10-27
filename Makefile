# I2C 示例 Makefile
#
# 用法：
#   make                编译所有目标
#   make i2c_demo       只编通用 I2C 读写工具
#   make xl9535_demo    只编 XL9535 16-bit GPIO 扩展器 demo
#   make ad7998_demo    只编 AD7998 8-channel 12-bit ADC demo
#   make clean          清理
#
# 说明：
#   - 需要 Linux 环境（WSL / 虚拟机 / 开发板）
#   - Windows MinGW 缺少 <linux/i2c-dev.h>，无法编译
#   - 依赖：内核 i2c-dev 模块已加载，无需额外库

CC      ?= gcc
CFLAGS  += -Wall -Wextra -O2 -g
LDFLAGS ?=

# ---- Rockchip 交叉编译接入（不传参数则保持原生编译行为）----
#   make                                原生编译
#   make RK_CROSS=1 SOC=rk3562 install  交叉编译 → cross-build/out/rk3562/i2c_examples/
# 传了 RK_CROSS 用硬 include（找不到直接报错）；否则 -include（缺失不报错）
ifdef RK_CROSS
include ../cross-build/Makefile.rockchip.cross
else
-include ../cross-build/Makefile.rockchip.cross
endif
CFLAGS  += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

# ---- 通用工具库 ----
UTIL_OBJS = i2c_util.o

# ---- 目标 1：通用 I2C 命令行工具 ----
TARGET_I2C     = i2c_demo
TARGET_I2C_OBJ = i2c_demo.o

# ---- 目标 2：XL9535 16-bit I/O 扩展器 ----
XL9535_OBJS    = xl9535.o xl9535_demo.o
TARGET_XL9535  = xl9535_demo

# ---- 目标 3：AD7998 8-channel 12-bit ADC ----
AD7998_OBJS    = ad7998.o ad7998_demo.o
TARGET_AD7998  = ad7998_demo

all: $(TARGET_I2C) $(TARGET_XL9535) $(TARGET_AD7998)

$(TARGET_I2C): $(TARGET_I2C_OBJ) $(UTIL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(TARGET_I2C_OBJ) $(UTIL_OBJS) $(LDFLAGS)

$(TARGET_XL9535): $(XL9535_OBJS) $(UTIL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(XL9535_OBJS) $(UTIL_OBJS) $(LDFLAGS)

$(TARGET_AD7998): $(AD7998_OBJS) $(UTIL_OBJS)
	$(CC) $(CFLAGS) -o $@ $(AD7998_OBJS) $(UTIL_OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- 头文件依赖 ----
i2c_demo.o:      i2c_demo.c i2c_util.h
i2c_util.o:      i2c_util.c i2c_util.h

xl9535.o:        xl9535.c xl9535.h i2c_util.h
xl9535_demo.o:   xl9535_demo.c xl9535.h i2c_util.h

ad7998.o:        ad7998.c ad7998.h i2c_util.h
ad7998_demo.o:   ad7998_demo.c ad7998.h i2c_util.h

# 原生编译时 OUTPUT_DIR 默认为 bin/；交叉编译时由公共片段设为 cross-build/out/<soc>/i2c_examples/
OUTPUT_DIR ?= bin

install: all
	@mkdir -p $(OUTPUT_DIR)
	@cp $(TARGET_I2C)    $(OUTPUT_DIR)/ && echo "  install $(TARGET_I2C)    -> $(OUTPUT_DIR)/"
	@cp $(TARGET_XL9535) $(OUTPUT_DIR)/ && echo "  install $(TARGET_XL9535) -> $(OUTPUT_DIR)/"
	@cp $(TARGET_AD7998) $(OUTPUT_DIR)/ && echo "  install $(TARGET_AD7998) -> $(OUTPUT_DIR)/"

clean:
	rm -f *.o $(TARGET_I2C) $(TARGET_XL9535) $(TARGET_AD7998)

.PHONY: all clean install
