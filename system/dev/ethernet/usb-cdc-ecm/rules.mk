# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE_TYPE := driver

MODULE_STATIC_LIBS := system/ulib/ddk system/dev/lib/usb \
                      system/ulib/pretty system/ulib/sync

MODULE_LIBS := system/ulib/driver system/ulib/zircon system/ulib/c

MODULE := $(LOCAL_DIR)

MODULE_SRCS := $(LOCAL_DIR)/usb-cdc-ecm.c

MODULE_BANJO_LIBS := \
    system/banjo/ddk-protocol-ethernet \

include make/module.mk

