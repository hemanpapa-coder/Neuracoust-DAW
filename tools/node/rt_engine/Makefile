CC ?= gcc
BUILD_DIR ?= build
MODULE_DIR ?= modules

CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Wformat=2 -Wshadow -Wstrict-prototypes
CFLAGS += -fno-math-errno -fomit-frame-pointer -DNDEBUG
MODULE_CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Wformat=2 -Wshadow -fPIC -fno-math-errno -fomit-frame-pointer -DNDEBUG
LDFLAGS ?=
LDLIBS ?= -lm -lpthread -ldl

TARGET := $(BUILD_DIR)/neuracoust-rt-engine
SOURCES := src/na_rt_engine.c
MODULE_4001E := $(BUILD_DIR)/na_4001e.so
MODULE_DM2C := $(BUILD_DIR)/na_dm2c.so
MODULE_COMPRESSOR101 := $(BUILD_DIR)/na_compressor101.so
MODULE_COMPRESSOR201 := $(BUILD_DIR)/na_compressor201.so
MODULE_COMPRESSOR99 := $(BUILD_DIR)/na_compressor99.so
MODULE_GATE_EXPANDER99 := $(BUILD_DIR)/na_gate_expander99.so
MODULE_MIRAGE991 := $(BUILD_DIR)/na_mirage991.so
MODULE_MIRAGE8 := $(BUILD_DIR)/na_mirage8.so
MODULE_MIRAGE901 := $(BUILD_DIR)/na_mirage901.so
MODULE_COAIR2026 := $(BUILD_DIR)/na_coair2026.so
MODULE_QF2D := $(BUILD_DIR)/na_qf2d.so
MODULE_COMP_LIMITER_340 := $(BUILD_DIR)/na_comp_limiter_340.so
MODULE_SPACE_SCULPTOR := $(BUILD_DIR)/na_space_sculptor.so

.PHONY: all modules clean check check-4001e check-dm2c check-compressor101 check-compressor201 check-compressor99 check-gate-expander99 check-mirage991 check-mirage8 check-mirage901 check-coair2026 check-qf2d check-comp-limiter-340 check-space-sculptor

all: $(TARGET) modules

modules: $(MODULE_4001E) $(MODULE_DM2C) $(MODULE_COMPRESSOR101) $(MODULE_COMPRESSOR201) $(MODULE_COMPRESSOR99) $(MODULE_GATE_EXPANDER99) $(MODULE_MIRAGE991) $(MODULE_MIRAGE8) $(MODULE_MIRAGE901) $(MODULE_COAIR2026) $(MODULE_QF2D) $(MODULE_COMP_LIMITER_340) $(MODULE_SPACE_SCULPTOR)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TARGET): $(SOURCES) include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -Iinclude $(SOURCES) -o $@ $(LDFLAGS) $(LDLIBS)

$(MODULE_4001E): $(MODULE_DIR)/na_4001e.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_DM2C): $(MODULE_DIR)/na_dm2c.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_COMPRESSOR101): $(MODULE_DIR)/na_compressor101.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_COMPRESSOR201): $(MODULE_DIR)/na_compressor201.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_COMPRESSOR99): $(MODULE_DIR)/na_compressor99.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_GATE_EXPANDER99): $(MODULE_DIR)/na_gate_expander99.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_MIRAGE991): $(MODULE_DIR)/na_mirage991.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_MIRAGE8): $(MODULE_DIR)/na_mirage8.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_MIRAGE901): $(MODULE_DIR)/na_mirage901.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_COAIR2026): $(MODULE_DIR)/na_coair2026.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_QF2D): $(MODULE_DIR)/na_qf2d.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_COMP_LIMITER_340): $(MODULE_DIR)/na_comp_limiter_340.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

$(MODULE_SPACE_SCULPTOR): $(MODULE_DIR)/na_space_sculptor.c include/na_rt_plugin.h | $(BUILD_DIR)
	$(CC) $(MODULE_CFLAGS) -Iinclude -shared $< -o $@ -lm

check: $(TARGET)
	$(TARGET) --self-test

check-4001e: $(TARGET) $(MODULE_4001E)
	$(TARGET) --module $(MODULE_4001E) --self-test

check-dm2c: $(TARGET) $(MODULE_DM2C)
	$(TARGET) --module $(MODULE_DM2C) --self-test

check-compressor101: $(TARGET) $(MODULE_COMPRESSOR101)
	$(TARGET) --module $(MODULE_COMPRESSOR101) --self-test

check-compressor201: $(TARGET) $(MODULE_COMPRESSOR201)
	$(TARGET) --module $(MODULE_COMPRESSOR201) --self-test

check-compressor99: $(TARGET) $(MODULE_COMPRESSOR99)
	$(TARGET) --module $(MODULE_COMPRESSOR99) --self-test

check-gate-expander99: $(TARGET) $(MODULE_GATE_EXPANDER99)
	$(TARGET) --module $(MODULE_GATE_EXPANDER99) --self-test

check-mirage991: $(TARGET) $(MODULE_MIRAGE991)
	$(TARGET) --module $(MODULE_MIRAGE991) --self-test

check-mirage8: $(TARGET) $(MODULE_MIRAGE8)
	$(TARGET) --module $(MODULE_MIRAGE8) --self-test

check-mirage901: $(TARGET) $(MODULE_MIRAGE901)
	$(TARGET) --module $(MODULE_MIRAGE901) --self-test

check-coair2026: $(TARGET) $(MODULE_COAIR2026)
	$(TARGET) --module $(MODULE_COAIR2026) --self-test

check-qf2d: $(TARGET) $(MODULE_QF2D)
	$(TARGET) --module $(MODULE_QF2D) --self-test

check-comp-limiter-340: $(TARGET) $(MODULE_COMP_LIMITER_340)
	$(TARGET) --module $(MODULE_COMP_LIMITER_340) --self-test

check-space-sculptor: $(TARGET) $(MODULE_SPACE_SCULPTOR)
	$(TARGET) --module $(MODULE_SPACE_SCULPTOR) --self-test

clean:
	rm -rf $(BUILD_DIR)
