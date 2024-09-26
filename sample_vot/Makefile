include ../Makefile.in
CUR_DIR := $(shell pwd)
RELATIVE_PATH := $(shell realpath --relative-to=$(PLATFORM_SAMPLES_DIR) $(CUR_DIR))

install:
	$(Q)install -d ${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}
	$(Q)install -m 0775 insmode_driver.sh \
		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/
	$(Q)cp -raf ${CUR_DIR}/resource \
		${PLATFORM_SAMPLES_DEPLOY_DIR}/${RELATIVE_PATH}/