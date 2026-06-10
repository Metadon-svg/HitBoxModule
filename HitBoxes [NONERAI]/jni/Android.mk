LOCAL_PATH := $(call my-dir)
MAIN_LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)

LOCAL_MODULE := HitBoxes

LOCAL_CFLAGS += -Wno-format-security -fvisibility=hidden -ffunction-sections -fdata-sections -w
LOCAL_CFLAGS += -fno-rtti -fexceptions -fpermissive -Oz

LOCAL_CPPFLAGS += -Wno-format-security -fvisibility=hidden -ffunction-sections -fdata-sections -w
LOCAL_CPPFLAGS += -fno-rtti -fexceptions -fpermissive -Oz -std=c++17
LOCAL_CPPFLAGS += -Wno-c++17-narrowing -fms-extensions -DNDEBUG

LOCAL_LDFLAGS += -Wl,--gc-sections -Wl,--strip-all -llog

FILE_LIST := $(wildcard $(LOCAL_PATH)/*.cpp)
FILE_LIST += $(wildcard $(LOCAL_PATH)/*.c)
FILE_LIST += $(wildcard $(LOCAL_PATH)/KittyMemory/*.cpp)

LOCAL_C_INCLUDES += $(wildcard $(LOCAL_PATH)/)
LOCAL_C_INCLUDES += $(wildcard $(LOCAL_PATH)/KittyMemory/)
LOCAL_SRC_FILES := $(FILE_LIST:$(LOCAL_PATH)/%=%)

include $(BUILD_SHARED_LIBRARY)