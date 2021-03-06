# Copyright 2018 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := driver

MODULE_SRCS += \
    $(LOCAL_DIR)/aml-gpu.c \
    $(LOCAL_DIR)/aml-s905d2g.c \

MODULE_STATIC_LIBS := system/ulib/ddk system/ulib/sync system/dev/lib/amlogic system/ulib/fidl

MODULE_LIBS := system/ulib/driver system/ulib/c system/ulib/zircon

MODULE_BANJO_LIBS := \
    system/banjo/ddk-protocol-gpio \
    system/banjo/ddk-protocol-iommu \
    system/banjo/ddk-protocol-platform-bus \
    system/banjo/ddk-protocol-platform-device \
    system/banjo/ddk-protocol-scpi \

MODULE_FIDL_LIBS := \
    system/fidl/fuchsia-gpu-clock \

MODULE_HEADER_DEPS := \
    system/dev/lib/amlogic

include make/module.mk
