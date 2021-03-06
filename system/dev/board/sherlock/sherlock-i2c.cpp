// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/platform-defs.h>
#include <ddk/protocol/platform/bus.h>
#include <ddktl/protocol/gpioimpl.h>

#include <soc/aml-t931/t931-gpio.h>
#include <soc/aml-t931/t931-hw.h>

#include "sherlock.h"

namespace sherlock {

static const pbus_mmio_t i2c_mmios[] = {
    {
        .base = T931_I2C_AOBUS_BASE,
        .length = 0x20,
    },
    {
        .base = T931_I2C2_BASE,
        .length = 0x20,
    },
    {
        .base = T931_I2C3_BASE,
        .length = 0x20,
    },
};

static const pbus_irq_t i2c_irqs[] = {
    {
        .irq = T931_I2C_AO_0_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
    {
        .irq = T931_I2C2_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
    {
        .irq = T931_I2C3_IRQ,
        .mode = ZX_INTERRUPT_MODE_EDGE_HIGH,
    },
};

static pbus_dev_t i2c_dev = []() {
    pbus_dev_t dev;
    dev.name = "gpio";
    dev.vid = PDEV_VID_AMLOGIC;
    dev.pid = PDEV_PID_GENERIC;
    dev.did = PDEV_DID_AMLOGIC_I2C;
    dev.mmio_list = i2c_mmios;
    dev.mmio_count = countof(i2c_mmios);
    dev.irq_list = i2c_irqs;
    dev.irq_count = countof(i2c_irqs);
    return dev;
}();

zx_status_t Sherlock::I2cInit() {

    ddk::GpioImplProtocolProxy gpio_impl(&gpio_impl_);

    // setup pinmux for our I2C busses
    //i2c_ao_0
    gpio_impl.SetAltFunction(T931_GPIOAO(2), 1);
    gpio_impl.SetAltFunction(T931_GPIOAO(3), 1);
    //i2c2
    gpio_impl.SetAltFunction(T931_GPIOZ(14), 3);
    gpio_impl.SetAltFunction(T931_GPIOZ(15), 3);
    //i2c3
    gpio_impl.SetAltFunction(T931_GPIOA(14), 2);
    gpio_impl.SetAltFunction(T931_GPIOA(15), 2);

    zx_status_t status = pbus_.ProtocolDeviceAdd(ZX_PROTOCOL_I2C_IMPL, &i2c_dev);
    if (status != ZX_OK) {
        zxlogf(ERROR, "%s: ProtocolDeviceAdd failed %d\n", __func__, status);
        return status;
    }

    return ZX_OK;
}

} // namespace sherlock
