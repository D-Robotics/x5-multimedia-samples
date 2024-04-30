CURRENT_PATH = $(shell pwd)
EXCLUDED_DIRS := chip_base_test sunrise_camera
EXCLUDED_DIRS_FLAGS := $(foreach dir,$(EXCLUDED_DIRS), ! -path "*$(dir)*")
SUB_FOLDERS := $(shell find $(CURRENT_PATH) -name "Makefile" $(EXCLUDED_DIRS_FLAGS) | sed 's/Makefile//g' | sed '/samples\/$$/d')
.PHONY:all clean

all:
	@echo ${SUB_FOLDERS}
	@for dir in ${SUB_FOLDERS}; do \
		make -C $$dir ||exit; \
	done
	@echo build all samples

clean:
	@for dir in ${SUB_FOLDERS}; do \
		make -C $$dir clean ||exit; \
	done
	@echo clean all samples
