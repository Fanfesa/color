LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := Pearlescent
LOCAL_SRC_FILES := pearlescent.cpp \
    imgui/imgui.cpp \
    imgui/imgui_draw.cpp \
    imgui/imgui_tables.cpp \
    imgui/imgui_widgets.cpp \
    imgui/backends/imgui_impl_opengl3.cpp
LOCAL_C_INCLUDES += $(LOCAL_PATH) $(LOCAL_PATH)/imgui $(LOCAL_PATH)/imgui/backends
LOCAL_CPPFLAGS  += -std=c++17 -O2 -DIMGUI_IMPL_OPENGL_ES2
LOCAL_LDLIBS    := -llog -ldl -lGLESv2 -lEGL
include $(BUILD_SHARED_LIBRARY)
