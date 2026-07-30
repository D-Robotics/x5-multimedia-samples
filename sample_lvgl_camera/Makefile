include ../Makefile.in

CUR_DIR := $(shell pwd)
RELATIVE_PATH := $(shell realpath --relative-to=$(PLATFORM_SAMPLES_DIR) $(CUR_DIR))
TARGET = lvgl_camera
OBJ_DIR := $(CUR_DIR)/build

LVGL_DIR := $(CUR_DIR)/third_party/lvgl

LOCAL_SRCS := \
	$(CUR_DIR)/src/main.c \
	$(CUR_DIR)/src/camera/camera_pipeline.c \
	$(CUR_DIR)/src/camera/media_still.c \
	$(CUR_DIR)/src/camera/rec_mux.c \
	$(CUR_DIR)/src/camera/media_player.c \
	$(CUR_DIR)/src/drm/drm_ui_plane.c \
	$(CUR_DIR)/src/drm/vp_display.c \
	$(CUR_DIR)/src/lvgl_port/lvgl_display.c \
	$(CUR_DIR)/src/lvgl_port/lvgl_input.c \
	$(CUR_DIR)/src/system/sys_status.c \
	$(CUR_DIR)/src/ui/ui_camera.c \
	$(CUR_DIR)/src/ui/ui_gallery.c

LVGL_SRCS := $(shell find $(LVGL_DIR)/src -type f -name '*.c' 2>/dev/null)

UTIL_SRCS := $(wildcard $(PLATFORM_SAMPLES_DIR)/utils/*.c)
VP_SENSOR_SRCS := $(foreach d, \
	$(shell find $(PLATFORM_SAMPLES_DIR)/vp_sensors -maxdepth 1 -type d), \
	$(wildcard $(d)/*.c))

SRCS := $(LOCAL_SRCS) $(LVGL_SRCS) $(UTIL_SRCS) $(VP_SENSOR_SRCS)
OBJS := $(patsubst $(PLATFORM_SAMPLES_DIR)/%.c,$(OBJ_DIR)/%.o,$(SRCS))

CFLAGS += -DLV_CONF_INCLUDE_SIMPLE
CFLAGS += -O2

INCS += -I $(CUR_DIR)
INCS += -I $(CUR_DIR)/config
INCS += -I $(CUR_DIR)/src
INCS += -I $(CUR_DIR)/src/camera
INCS += -I $(CUR_DIR)/src/drm
INCS += -I $(CUR_DIR)/src/lvgl_port
INCS += -I $(CUR_DIR)/src/ui
INCS += -I $(CUR_DIR)/src/system
INCS += -I $(LVGL_DIR)
INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/libdrm/
INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/

LIBS += -lm -ldrm -lmultimedia -lavformat -lavcodec -lavutil -lssl -lcrypto -lz -lpthread

.PHONY: all clean install check_lvgl clean_lvgl_objs

all: $(TARGET)

check_lvgl:
	@test -f $(LVGL_DIR)/lvgl.h || ( \
		echo "LVGL not found. Run: ./setup_lvgl.sh"; exit 1)

$(OBJ_DIR)/%.o: $(PLATFORM_SAMPLES_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(INCS) $(CFLAGS) -c $< -o $@

$(TARGET): check_lvgl $(OBJS)
	$(CC) -o $@ $(OBJS) $(CFLAGS) $(LIBSDIR) $(LIBS)

# Remove stale .o files left in lvgl/src from older builds
clean_lvgl_objs:
	@find $(LVGL_DIR)/src $(LVGL_DIR)/tests -name '*.o' -delete 2>/dev/null || true

clean: clean_lvgl_objs
	rm -rf $(OBJ_DIR) $(TARGET)

install: $(TARGET)
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}
	$(Q)install -m 0775 $(TARGET) \
		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
