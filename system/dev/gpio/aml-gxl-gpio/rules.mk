# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := driver

MODULE_SRCS += \
    $(LOCAL_DIR)/aml-gxl-gpio.cpp \
    $(LOCAL_DIR)/binding.c \

MODULE_STATIC_LIBS := \
    system/ulib/ddk \
    system/ulib/ddktl \
    system/ulib/fbl \
    system/ulib/zx \
    system/ulib/zxcpp \

MODULE_LIBS := \
    system/ulib/c \
    system/ulib/driver \
    system/ulib/zircon \

MODULE_BANJO_LIBS := \
    system/banjo/ddk-protocol-gpio \
    system/banjo/ddk-protocol-gpio-impl \
    system/banjo/ddk-protocol-platform-bus \
    system/banjo/ddk-protocol-platform-device \

MODULE_HEADER_DEPS := system/dev/lib/amlogic

include make/module.mk
