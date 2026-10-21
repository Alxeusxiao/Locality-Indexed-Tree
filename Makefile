# =====================================================================
# LI-Tree 实验工程
#
#   make help       列出可用目标
#   make            构建全部程序到 build/
#   make run-small  生成 10 万条数据，跑一遍演示 + 基准自检
#   make run-bench  按 README 的口径复现完整实验（100 万条）
#   make clean      清理 build/
# =====================================================================

CC     := gcc
SRCDIR := src
BUILD  := build

# ---- 平台差异 --------------------------------------------------------
ifeq ($(OS),Windows_NT)
  EXE        := .exe
  MKDIR      := if not exist "$(BUILD)" mkdir "$(BUILD)"
  MKDIR_DATA := if not exist "$(BUILD)\data" mkdir "$(BUILD)\data"
  RM_DIR     := if exist "$(BUILD)" rmdir /S /Q "$(BUILD)"
  # MinGW 的 msvcrt printf 不支持 %lld / %llu，需要打开 ANSI printf 实现
  STDIO_FLAG := -D__USE_MINGW_ANSI_STDIO=1
else
  EXE        :=
  MKDIR      := mkdir -p "$(BUILD)"
  MKDIR_DATA := mkdir -p "$(BUILD)/data"
  RM_DIR     := rm -rf "$(BUILD)"
  STDIO_FLAG :=
endif

CFLAGS ?= -std=gnu11 $(STDIO_FLAG) -O2 -Wall -Wextra -Wshadow
LDLIBS ?= -lm

# ---- 目标 ------------------------------------------------------------
CORE_NAMES := timer dataset searcher litree thread
CORE_OBJS  := $(addprefix $(BUILD)/,$(addsuffix .o,$(CORE_NAMES)))

APPS     := li-gen li-demo li-bench
APP_BINS := $(addprefix $(BUILD)/,$(addsuffix $(EXE),$(APPS)))

DATA1M := $(BUILD)/data/keys_1m.txt
DATA5M := $(BUILD)/data/keys_5m.txt

.PHONY: all help clean run-small run-bench run-bench-5m run-bench-mt

all: $(APP_BINS)

help:
	@echo 可用目标:
	@echo   make              构建 $(APP_BINS)
	@echo   make run-small    10 万条数据 + 演示 + 基准自检
	@echo   make run-bench    复现文档中的完整实验(100 万条)
	@echo   make run-bench-5m 复现 500 万条的两方案对比
	@echo   make run-bench-mt 多核扩展性测试(1/2/4/8/16 线程)
	@echo   make clean        清理 $(BUILD)/

$(BUILD):
	@$(MKDIR)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/li-gen$(EXE): $(BUILD)/gen.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/li-demo$(EXE): $(BUILD)/demo.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

$(BUILD)/li-bench$(EXE): $(BUILD)/bench.o $(CORE_OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDLIBS)

# ---- 自检 / 复现 -----------------------------------------------------
run-small: all
	$(BUILD)/li-gen$(EXE) --out $(BUILD)/small.txt --count 100000 --dist nd --seed 1 --check
	$(BUILD)/li-demo$(EXE) --data $(BUILD)/small.txt --keys 100000 --scheme 1
	$(BUILD)/li-bench$(EXE) --data $(BUILD)/small.txt --keys 100000 --queries 200000 --scheme 1

run-bench: all
	@$(MKDIR_DATA)
	$(BUILD)/li-gen$(EXE) --out $(DATA1M) --count 1000000 --dist nd --seed 1 --check
	$(BUILD)/li-demo$(EXE) --data $(DATA1M) --keys 1000000 --scheme 1
	$(BUILD)/li-bench$(EXE) --data $(DATA1M) --keys 1000000 --queries 100000 --scheme 1 --cap 150 --repeat 3
	$(BUILD)/li-bench$(EXE) --data $(DATA1M) --keys 1000000 --queries 100000 --scheme 2 --repeat 3

run-bench-5m: all
	@$(MKDIR_DATA)
	$(BUILD)/li-gen$(EXE) --out $(DATA5M) --count 5000000 --dist nd --seed 1 --check
	$(BUILD)/li-bench$(EXE) --data $(DATA5M) --keys 5000000 --queries 20000 --scheme 1 --cap 150 --repeat 3
	$(BUILD)/li-bench$(EXE) --data $(DATA5M) --keys 5000000 --queries 20000 --scheme 2 --repeat 3

# 多核扩展性：同一份 100 万条数据，1/2/4/8/16 线程各跑一遍
run-bench-mt: all
	@$(MKDIR_DATA)
	$(BUILD)/li-gen$(EXE) --out $(DATA1M) --count 1000000 --dist nd --seed 1 --check
	$(BUILD)/li-bench$(EXE) --data $(DATA1M) --keys 1000000 --queries 100000 --scheme 1 --cap 150 --repeat 3 --threads 1
	$(BUILD)/li-bench$(EXE) --data $(DATA1M) --keys 1000000 --queries 100000 --scheme 1 --cap 150 --repeat 3 --threads 2
	$(BUILD)/li-bench$(EXE) --data $(DATA1M) --keys 1000000 --queries 100000 --scheme 1 --cap 150 --repeat 3 --threads 4
	$(BUILD)/li-bench$(EXE) --data $(DATA1M) --keys 1000000 --queries 100000 --scheme 1 --cap 150 --repeat 3 --threads 8
	$(BUILD)/li-bench$(EXE) --data $(DATA1M) --keys 1000000 --queries 100000 --scheme 1 --cap 150 --repeat 3 --threads 16

clean:
	@$(RM_DIR)

-include $(wildcard $(BUILD)/*.d)
