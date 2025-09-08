include ../../Makefile.in
CUR_DIR := $(shell pwd)
RELATIVE_PATH := $(shell realpath --relative-to=$(PLATFORM_SAMPLES_DIR) $(CUR_DIR))

TARGET = mp4_decode_and_display

# 源文件列表
SRCS := mp4_decode_and_display.c codec_helper.c vp_codec.c vp_display.c
OBJS := $(SRCS:.c=.o)

INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/
LIBSDIR += -L ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/lib/
INCS += -I ${HR_BUILD_OUTPUT_DIR}/deploy/system/usr/include/libdrm/
LIBS += -lmultimedia -lavformat -lavcodec -lavutil -lssl -lcrypto -ldrm -lz -lpthread -lm

.PHONY: all clean install

%.o:%.c
	@mkdir -p $(abspath $(dir $@))
	$(CC) -o $@ $(INCS) $(CFLAGS) -c $< -rdynamic -g

$(TARGET):$(OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC)  -o $@ $(OBJS) $(CFLAGS) ${LIBSDIR} $(LIBS) -rdynamic -g

all: ${TARGET}

clean:
	rm -rf ${OBJS} ${TARGET}

install: ${TARGET}
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}
	$(Q)install -m 0775 ${TARGET} \
		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
