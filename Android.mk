LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_CPP_EXTENSION := .cpp .cc
LOCAL_MODULE := BulletTimeSA64
LOCAL_SRC_FILES := main.cpp mod/logger.cpp mod/config.cpp mod/flogger.cpp
LOCAL_CFLAGS += -O2 -DNDEBUG -std=c++17 -fno-exceptions -fno-rtti
LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_LDLIBS += -llog
include $(BUILD_SHARED_LIBRARY)
