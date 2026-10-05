MODEL_DIR ?= model
BUILD_DIR ?= build-model
CHIPYARD ?=
RISCV_PREFIX ?= $(CHIPYARD)/.conda-env/riscv-tools
SUPPORT := $(CHIPYARD)/generators/testchipip/src/main/resources/testchipip/csrc
MODEL_SOURCES := $(wildcard $(MODEL_DIR)/TestHarness*.cpp)
MODEL_OBJECTS := $(patsubst $(MODEL_DIR)/%.cpp,$(BUILD_DIR)/%.o,$(MODEL_SOURCES))
NATIVE_NAMES := mm testchip_htif testchip_tsi uart
NATIVE_OBJECTS := $(addprefix $(BUILD_DIR)/,$(addsuffix .o,$(NATIVE_NAMES)))
CPPFLAGS := -I$(MODEL_DIR) -I$(SUPPORT) -I$(RISCV_PREFIX)/include
CXXFLAGS ?= -O1 -std=c++17 -Wno-format

all: $(BUILD_DIR)/emulator

$(BUILD_DIR)/TestHarness%.o: $(MODEL_DIR)/TestHarness%.cpp $(MODEL_DIR)/TestHarness.h
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: $(SUPPORT)/%.cc
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/%.o: %.cpp $(MODEL_DIR)/TestHarness.h
	@mkdir -p $(@D)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

$(BUILD_DIR)/emulator: $(MODEL_OBJECTS) $(NATIVE_OBJECTS) $(BUILD_DIR)/main.o $(BUILD_DIR)/blackboxes.o
	$(CXX) $^ $(RISCV_PREFIX)/lib/libfesvr.a -lgmp -lpthread -o $@

.PHONY: all
