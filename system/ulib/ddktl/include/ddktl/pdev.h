// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <ddktl/i2c-channel.h>
#include <ddktl/mmio.h>
#include <ddktl/protocol/gpio.h>
#include <ddktl/protocol/platform/device.h>

#include <lib/zx/bti.h>
#include <lib/zx/interrupt.h>
#include <optional>
#include <zircon/types.h>

namespace ddk {

class PDev : public PDevProtocolProxy {

public:
    PDev(pdev_protocol_t* proto)
        : PDevProtocolProxy(proto){};

    ~PDev() = default;

    // Prints out information about the platform device.
    void ShowInfo();

    zx_status_t MapMmio(uint32_t index, std::optional<MmioBuffer>* mmio);

    zx_status_t GetInterrupt(uint32_t index, zx::interrupt* out) {
        return PDevProtocolProxy::GetInterrupt(index, 0, out);
    }

    std::optional<I2cChannel> GetI2c(uint32_t index);
    std::optional<GpioProtocolProxy> GetGpio(uint32_t index);
};

} // namespace ddk
