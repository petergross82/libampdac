# Fallback build when CMake is not installed.
# Prefer: cmake -S . -B build && cmake --build build

NATIVE := native
CXX ?= g++
CXXFLAGS ?= -std=c++17 -fPIC -O2 -Wall -Wextra -Werror=return-type -Wno-unused-parameter -D_GNU_SOURCE
INCLUDES := -Iinclude -Icompat -Ithird_party -I$(NATIVE)
LDFLAGS ?= -lpthread -lusb-1.0
# Distro packages without -dev only ship the SONAME symlink.
LIBUSB_SO ?= /usr/lib/x86_64-linux-gnu/libusb-1.0.so.0

SRCS := \
	src/ampdac.cpp \
	src/ampdac_android_log.cpp \
	src/ampdac_kernel.cpp \
	$(NATIVE)/usb_session_transport.cpp \
	$(NATIVE)/usb_teardown.cpp \
	$(NATIVE)/uac2_descriptor_parser.cpp \
	$(NATIVE)/uac2_dsd_detector.cpp \
	$(NATIVE)/uac2_clock_control.cpp \
	$(NATIVE)/usb_device_controller.cpp \
	$(NATIVE)/usb_iso_transfer_pool.cpp \
	$(NATIVE)/spsc_ring_buffer.cpp \
	$(NATIVE)/pcm_wire_formatter.cpp \
	$(NATIVE)/usb_playback_state.cpp \
	$(NATIVE)/usb_producer_coordinator.cpp \
	$(NATIVE)/dop_formatter.cpp \
	$(NATIVE)/native_dsd_formatter.cpp \
	$(NATIVE)/dsd_playback_manager.cpp \
	$(NATIVE)/libusb_event_thread.cpp \
	$(NATIVE)/decoder_to_ring_bridge.cpp

BUILD := build
OBJS := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(SRCS)))
HDRS := $(wildcard include/*.h) $(wildcard src/*.h) $(wildcard $(NATIVE)/*.h)

DEADBEEF_INC ?= plugins/deadbeef/include
DEADBEEF_DIR ?= $(HOME)/deadbeef-1.10.3

.PHONY: all clean plugin install-deadbeef
all: $(BUILD)/libampdac.so $(BUILD)/ampdac-cli plugin

$(BUILD):
	mkdir -p $(BUILD)

# Map basename → source path
$(BUILD)/ampdac.o: src/ampdac.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(BUILD)/ampdac_android_log.o: src/ampdac_android_log.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(BUILD)/ampdac_kernel.o: src/ampdac_kernel.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@
$(BUILD)/%.o: $(NATIVE)/%.cpp $(HDRS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -c $< -o $@

$(BUILD)/libampdac.so: $(OBJS)
	$(CXX) -shared -o $@ $^ $(LIBUSB_SO) -lpthread

$(BUILD)/ampdac-cli: examples/ampdac_cli.cpp $(BUILD)/libampdac.so
	$(CXX) $(CXXFLAGS) -Iinclude -o $@ examples/ampdac_cli.cpp -L$(BUILD) -lampdac -Wl,-rpath,'$$ORIGIN'

$(BUILD)/ampdac_output.o: plugins/deadbeef/ampdac_output.c | $(BUILD)
	$(CC) -std=c11 -fPIC -O2 -Wall -Wextra -D_GNU_SOURCE \
		-Iinclude -I$(DEADBEEF_INC) -c $< -o $@

$(BUILD)/ddb_out_ampdac.so: $(BUILD)/ampdac_output.o $(OBJS)
	$(CXX) -shared -o $@ $^ $(LIBUSB_SO) -lpthread

plugin: $(BUILD)/ddb_out_ampdac.so

install-deadbeef: plugin
	rm -f $(DEADBEEF_DIR)/plugins/libampdac.so
	install -m 755 $(BUILD)/ddb_out_ampdac.so $(DEADBEEF_DIR)/plugins/ddb_out_ampdac.so

clean:
	rm -rf $(BUILD)
