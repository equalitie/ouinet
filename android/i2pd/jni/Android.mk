LOCAL_PATH := $(call my-dir)

# export BOOST_PATH, OPENSSL_PATH and IFADDRS_PATH before
# build libi2pd
include $(CLEAR_VARS)
LOCAL_MODULE := libi2pd
LOCAL_CPP_FEATURES := rtti exceptions
LOCAL_C_INCLUDES += $(IFADDRS_PATH) $(LIB_SRC_PATH) $(BOOST_PATH)/include $(OPENSSL_PATH)/include
LOCAL_SRC_FILES := $(IFADDRS_PATH)/ifaddrs.c \
	$(wildcard $(LIB_SRC_PATH)/*.cpp)
include $(BUILD_STATIC_LIBRARY)

# build libi2pd_client
include $(CLEAR_VARS)
LOCAL_MODULE := libi2pd_client
LOCAL_CPP_FEATURES := rtti exceptions
LOCAL_C_INCLUDES += $(LIB_SRC_PATH) $(LIB_CLIENT_SRC_PATH) $(BOOST_PATH)/include $(OPENSSL_PATH)/include
LOCAL_SRC_FILES := $(wildcard $(LIB_CLIENT_SRC_PATH)/*.cpp)
include $(BUILD_STATIC_LIBRARY)


