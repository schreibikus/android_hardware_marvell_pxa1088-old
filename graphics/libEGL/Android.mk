#
# Copyright (C) 2016 Android For Marvell Project <ctx.xda@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

LOCAL_PATH:= $(call my-dir)
include $(CLEAR_VARS)

# Copy egl.cfg to /system/lib/egl
file := $(TARGET_OUT)/lib/egl/egl.cfg
$(file) : $(LOCAL_PATH)/egl.cfg | $(ACP)
	$(transform-prebuilt-to-target)
ALL_PREBUILT += $(file)


include $(CLEAR_VARS)
LOCAL_PREBUILT_LIBS := \
	libEGL.a \
	libeglapi.a \
	libeglos.a

LOCAL_MODULE_TAGS := optional
include $(BUILD_MULTI_PREBUILT)


include $(CLEAR_VARS)
LOCAL_WHOLE_STATIC_LIBRARIES := \
	libEGL \
	libeglapi \
	libeglos

LOCAL_SHARED_LIBRARIES := libcutils libutils libdl libGAL liblog libhardware

LOCAL_LDLIBS := -ldl -llog -lhardware
LOCAL_MODULE_PATH := $(TARGET_OUT_SHARED_LIBRARIES)/egl
LOCAL_MODULE:= libEGL_MRVL
LOCAL_MODULE_TAGS := optional
LOCAL_PRELINK_MODULE := false
include $(BUILD_SHARED_LIBRARY)