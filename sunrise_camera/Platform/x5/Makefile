include ../makefile.param
include ./makefile.param

INC_DIR := main api vpp_impl vp_wrap bpu_wrap
INC_DIR += $(GLOBAL_EXTERN_INC_DIR) $(GLOBAL_INSTALL_DIR)
INC := $(patsubst %,-I%/include,$(INC_DIR))
INC += $(patsubst %,-I%/include/libdrm,$(GLOBAL_EXTERN_INC_DIR))
INC += $(patsubst %,-I%,vp_sensors)
LIB := $(patsubst %,-L%/lib,$(INC_DIR))

SRC := $(wildcard *.cpp *.c $(patsubst %,%/src/*.cpp,$(INC_DIR)) $(patsubst %,%/src/*.c,$(INC_DIR)))
SRC += $(wildcard *.c $(patsubst %,%/*.c,vp_sensors) $(patsubst %,%/*/*.c,vp_sensors))
OBJ := $(patsubst %.cpp,%.obj,$(patsubst %.c,%.o,$(SRC)))
DEP := $(patsubst %.obj,%.dep,$(patsubst %.o,%.d,$(OBJ)))

ifeq (${HR_SYSTEM_TYPE},ubuntu)
INC += -I${DEPLOY_SYSTEM_DIR}/usr/include/aarch64-linux-gnu
INC += -I${DEPLOY_SYSTEM_DIR}/usr/aarch64-linux-gnu/include
LIB += --sysroot=${DEPLOY_SYSTEM_DIR}/
LIB += -L${DEPLOY_SYSTEM_DIR}/lib/aarch64-linux-gnu
LIB += -L${DEPLOY_SYSTEM_DIR}/usr/aarch64-linux-gnu/lib
LIB += -L${DEPLOY_SYSTEM_DIR}/usr/lib/aarch64-linux-gnu
LIB += -Wl,-rpath-link=${DEPLOY_SYSTEM_DIR}/lib/aarch64-linux-gnu
LIB += -Wl,-rpath-link=${DEPLOY_SYSTEM_DIR}/usr/aarch64-linux-gnu/lib
LIB += -Wl,-rpath-link=${DEPLOY_SYSTEM_DIR}/usr/lib/aarch64-linux-gnu
endif

ifeq ($(BOARD_BUILD),1)
INC += -I/usr/include/drm/
LIB += -L/usr/hobot/lib/
endif

CC := $(COMPILE_PREFIX)gcc
ifeq ($(BOARD_BUILD),0)
CFLAGS := $(INC) -Werror -O0 -DBSD=1 -fPIC $(CFLAGS_EX) -Wno-error=unused-function -DVERSION=\"$(VERSION)\"
CXX := $(COMPILE_PREFIX)c++
CXXFLAGS := $(CFLAGS)
LINK := $(COMPILE_PREFIX)c++ -o
LIBRARY_LINK := $(COMPILE_PREFIX)c++ -shared -o
else
CFLAGS := $(INC) -O0 -DBSD=1 -fPIC $(CFLAGS_EX) -Wno-error=unused-function -DVERSION=\"$(VERSION)\"
CXX := $(COMPILE_PREFIX)g++
CXXFLAGS := $(CFLAGS)
LINK := $(COMPILE_PREFIX)g++ -o
LIBRARY_LINK := $(COMPILE_PREFIX)g++ -shared -o
endif
LIBRARY_LINK_STATIC := $(COMPILE_PREFIX)ar cr
LIBRARY_LINK_OPTS =
STRIP := $(COMPILE_PREFIX)strip

ifeq (${HR_SYSTEM_TYPE},ubuntu)
CFLAGS += --sysroot=${DEPLOY_SYSTEM_DIR}/
CXXFLAGS += --sysroot=${DEPLOY_SYSTEM_DIR}/
endif

TARGET := libvpp.a

.PHONY : all clean

all : $(TARGET) install

%.o : %.c
	$(CC) $(CFLAGS) -MMD -MP -MF"$(@:%.o=%.d)" -MT"$(@:%.o=%.d)" -c $< -o $@

%.obj : %.cpp
	$(CXX) $(CXXFLAGS) -MMD -MP -MF"$(@:%.obj=%.dep)" -MT"$(@:%.obj=%.dep)" -c $< -o $@

$(TARGET) : $(OBJ)
	$(LIBRARY_LINK_STATIC) $@ $^

install: $(TARGET)
	-mkdir -p $(GLOBAL_INSTALL_DIR)/lib
	-mkdir -p $(GLOBAL_INSTALL_DIR)/test_data
	-mkdir -p $(GLOBAL_INSTALL_DIR)/model_zoom
	-mkdir -p $(GLOBAL_INSTALL_DIR)/gdc_bin
	-mkdir -p $(GLOBAL_INSTALL_DIR)/config
	mv $(TARGET) $(GLOBAL_INSTALL_DIR)/lib
	# cp -rf tools/* $(GLOBAL_INSTALL_DIR)/bin
	cp -rf test_data/* $(GLOBAL_INSTALL_DIR)/test_data
	cp -rf model_zoom/* $(GLOBAL_INSTALL_DIR)/model_zoom
	cp -rf vp_sensors/gdc_bin/* $(GLOBAL_INSTALL_DIR)/gdc_bin
	cp -rf config/* $(GLOBAL_INSTALL_DIR)/config

clean:
	@rm -rf $(OBJ) $(DEP) $(TARGET)

sinclude $(DEP)
