# vPE stateless — Stage 1 build.
#
# `make` / `make vpe`   : builds the `vpe` production binary. USE_STUBS=1
#                         (default) links stubs/**/*.c for the ports;
#                         Stage 2 builds with USE_STUBS=0 once real
#                         v_port_*.c bindings exist elsewhere under src/,
#                         so the production binary never ships a stub.
# `make test`           : builds and runs the standalone test suite
#                         (always against the stubs, real local Redis +
#                         real DPDK mempool/ring — plan.md §8.1).
# `make clean`          : remove build artifacts.
#
# Layout: every module directory (under src/ and stubs/) is split into
# <module>/inc (its public header(s)) and <module>/src (its .c file).
# The include path below picks up every module's inc/ automatically —
# adding a new module needs no Makefile edit, same as adding a .c file
# to an existing one.

CC       := gcc
STD      := -std=gnu99
WARN     := -Wall -Wextra
DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
DPDK_LIBS   := $(shell pkg-config --libs libdpdk)

MODULE_DIRS := $(filter %/,$(wildcard src/*/inc/ stubs/*/inc/))
INCLUDES := -Iinclude -Istubs -Itest $(addprefix -I,$(MODULE_DIRS))
CFLAGS   := $(STD) $(WARN) -g -MMD -MP $(INCLUDES) $(DPDK_CFLAGS)
LDLIBS   := $(DPDK_LIBS) -lhiredis -levent -lpthread -lm

USE_STUBS ?= 1

BUILD_DIR := build

rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$(d),$(2)) $(filter $(subst *,%,$(2)),$(d)))

SRC_C := $(call rwildcard,src,*.c)
STUB_C := $(call rwildcard,stubs,*.c)
TEST_C := $(call rwildcard,test,*.c)

ifeq ($(USE_STUBS),1)
VPE_SRC := $(SRC_C) $(STUB_C) main.c
else
VPE_SRC := $(SRC_C) main.c
endif

TEST_SRC := $(SRC_C) $(STUB_C) $(TEST_C)

VPE_OBJ  := $(patsubst %.c,$(BUILD_DIR)/vpe/%.o,$(VPE_SRC))
TEST_OBJ := $(patsubst %.c,$(BUILD_DIR)/test/%.o,$(TEST_SRC))

.PHONY: all vpe test clean

all: vpe

vpe: $(BUILD_DIR)/vpe_bin
	@cp $(BUILD_DIR)/vpe_bin vpe
	@echo "built ./vpe (USE_STUBS=$(USE_STUBS))"

$(BUILD_DIR)/vpe_bin: $(VPE_OBJ)
	$(CC) $(VPE_OBJ) -o $@ $(LDLIBS)

test: $(BUILD_DIR)/vpe_test
	./$(BUILD_DIR)/vpe_test

$(BUILD_DIR)/vpe_test: $(TEST_OBJ)
	$(CC) $(TEST_OBJ) -o $@ $(LDLIBS)

$(BUILD_DIR)/vpe/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/test/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DVPE_TEST_BUILD -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) vpe

-include $(VPE_OBJ:.o=.d)
-include $(TEST_OBJ:.o=.d)
