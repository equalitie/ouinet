#APP_ABI := all
#APP_ABI := armeabi-v7a x86
#APP_ABI := x86
#APP_ABI := x86_64
APP_ABI := armeabi-v7a
#can be android-3 but will fail for x86 since arch-x86 is not present at ndkroot/platforms/android-3/ . libz is taken from there.
APP_PLATFORM := android-14

NDK_TOOLCHAIN_VERSION := clang
APP_STL := c++_static

# Enable c++11 extensions in source code
APP_CPPFLAGS += -std=c++11 -fvisibility=default -fPIE

APP_CPPFLAGS += -DANDROID_BINARY -DANDROID -D__ANDROID__ 
APP_LDFLAGS += -rdynamic -fPIE -pie
ifeq ($(TARGET_ARCH_ABI),armeabi-v7a)
APP_CPPFLAGS += -DANDROID_ARM7A
endif

# don't change me
I2PD_SRC_PATH = $(PWD)/../../src/ouiservice/i2p/i2pd

LIB_SRC_PATH = $(I2PD_SRC_PATH)/libi2pd
LIB_CLIENT_SRC_PATH = $(I2PD_SRC_PATH)/libi2pd_client
