LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := Pearlescent
LOCAL_SRC_FILES := pearlescent.cpp
LOCAL_C_INCLUDES += $(LOCAL_PATH)
LOCAL_CPPFLAGS  += -std=c++17 -O2
LOCAL_LDLIBS    := -llog -ldl
include $(BUILD_SHARED_LIBRARY)
