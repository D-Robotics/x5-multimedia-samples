include ../../Makefile.in
CUR_DIR := $(shell pwd)
RELATIVE_PATH := $(shell realpath --relative-to=$(PLATFORM_SAMPLES_DIR) $(CUR_DIR))

TARGETS := single_pipe_vin_isp_vse_process \
           single_pipe_vin_isp_vse_encode \
           single_pipe_vin_isp_vse_hdmi

.PHONY: all clean install $(TARGETS)

all: $(TARGETS)

EXCLUDE_MAIN_SRCS := $(CUR_DIR)/single_pipe_vin_isp_vse_process.c \
                     $(CUR_DIR)/single_pipe_vin_isp_vse_encode.c \
                     $(CUR_DIR)/single_pipe_vin_isp_vse_hdmi.c
EXCLUDE_LOCAL_COMMON_SRCS := $(CUR_DIR)/unix_socket.c \
                             $(CUR_DIR)/ping_pang_file_saver.c
EXCLUDE_SRCS := $(EXCLUDE_MAIN_SRCS) $(EXCLUDE_LOCAL_COMMON_SRCS)

ALL_GLOBAL_SRCS := $(foreach cf, $(SRC_PATH), $(wildcard $(cf)/*.c))
GLOBAL_COMMON_SRCS := $(filter-out $(EXCLUDE_SRCS), $(ALL_GLOBAL_SRCS))

LOCAL_COMMON_SRCS := $(CUR_DIR)/unix_socket.c \
                     $(CUR_DIR)/ping_pang_file_saver.c

VP_DISPLAY_SRC := $(CUR_DIR)/../common/vp_display.c
LOCAL_COMMON_SRCS += $(VP_DISPLAY_SRC)

COMMON_SRCS := $(LOCAL_COMMON_SRCS) $(GLOBAL_COMMON_SRCS)
COMMON_OBJS := $(COMMON_SRCS:.c=.o)

PROCESS_MAIN_SRC := $(CUR_DIR)/single_pipe_vin_isp_vse_process.c
ENCODE_MAIN_SRC := $(CUR_DIR)/single_pipe_vin_isp_vse_encode.c
HDMI_MAIN_SRC := $(CUR_DIR)/single_pipe_vin_isp_vse_hdmi.c
PROCESS_OBJ := $(PROCESS_MAIN_SRC:.c=.o)
ENCODE_OBJ := $(ENCODE_MAIN_SRC:.c=.o)
HDMI_OBJ := $(HDMI_MAIN_SRC:.c=.o)

LIBS += -lm -ldrm -lmultimedia
INCS += -I $(CUR_DIR)/../common/
INCS += -I $(CUR_DIR)
INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/libdrm/
INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/

%.o: %.c
	@mkdir -p $(abspath $(dir $@))
	$(CC) $(INCS) $(CFLAGS) -c $< -o $@

single_pipe_vin_isp_vse_process: $(PROCESS_OBJ) $(COMMON_OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC) -O0 -o $@ $(PROCESS_OBJ) $(COMMON_OBJS) $(CFLAGS) $(LIBSDIR) $(LIBS)

single_pipe_vin_isp_vse_encode: $(ENCODE_OBJ) $(COMMON_OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC) -O0 -o $@ $(ENCODE_OBJ) $(COMMON_OBJS) $(CFLAGS) $(LIBSDIR) $(LIBS)

single_pipe_vin_isp_vse_hdmi: $(HDMI_OBJ) $(COMMON_OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC) -O0 -o $@ $(HDMI_OBJ) $(COMMON_OBJS) $(CFLAGS) $(LIBSDIR) $(LIBS)

clean:
	rm -rf $(COMMON_OBJS) $(PROCESS_OBJ) $(ENCODE_OBJ) $(HDMI_OBJ)
	rm -rf $(TARGETS)
	rm -rf install

install: $(TARGETS)
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}
	$(Q)install -m 0775 $(TARGETS) \
		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/