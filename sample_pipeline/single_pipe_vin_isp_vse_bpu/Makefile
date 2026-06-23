include ../../Makefile.in
CUR_DIR := $(shell pwd)
RELATIVE_PATH := $(shell realpath --relative-to=$(PLATFORM_SAMPLES_DIR) $(CUR_DIR))
COMMON_DIR := $(PLATFORM_SAMPLES_DIR)/sample_pipeline/common

TARGET = single_pipe_vin_isp_vse_bpu

SRCS_C := $(foreach cf, $(SRC_PATH), $(wildcard $(cf)/*.c))

OBJS_C := $(SRCS_C:.c=.o)
OBJS := $(OBJS_C)

INCS +=

LIBS += -ldnn -lcnn_intf -lhbrt_bayes_aarch64

.PHONY: all clean install

%.o:%.c
	@mkdir -p $(abspath $(dir $@))
	$(CC) $(INCS) $(CFLAGS) -c $< $(LIBS) $(LIBSDIR) -o $@

$(TARGET):$(OBJS)
	@mkdir -p $(abspath $(dir $@))
	$(CC) -O0 -o $@ $(OBJS) $(CFLAGS) ${LIBSDIR} $(LIBS)

all: ${TARGET}

clean:
	rm -rf ${OBJS} ${TARGET} ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}

install: ${TARGET}
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/model_zoom
	$(Q)install -m 0775 ${TARGET} ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
	$(Q)install -m 0664 ${CUR_DIR}/model_zoom/*.bin ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/model_zoom/
