/*
 * Copyright (c) 2015 Jean-Christophe Dubois <jcd@tribudubois.net>
 *
 * i.MX6 SOC emulation.
 *
 * Based on hw/arm/fsl-imx31.c
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/fsl-imx6.h"
#include "hw/misc/unimp.h"
#include "hw/usb/imx-usb-phy.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "system/system.h"
#include "chardev/char.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "target/arm/cpu-qom.h"

#define IMX6_ESDHC_CAPABILITIES     0x057834b4

#define NAME_SIZE 20

static void fsl_imx6_init(Object *obj)
{
    FslIMX6State *s = FSL_IMX6(obj);
    char name[NAME_SIZE];
    int i;

    object_initialize_child(obj, "a9mpcore", &s->a9mpcore, TYPE_A9MPCORE_PRIV);

    object_initialize_child(obj, "ccm", &s->ccm, TYPE_IMX6_CCM);

    object_initialize_child(obj, "src", &s->src, TYPE_IMX6_SRC);

    object_initialize_child(obj, "apbh", &s->apbh, TYPE_IMX6_APBH_DMA);
    object_initialize_child(obj, "sdma", &s->sdma, TYPE_IMX6_SDMA);
    object_initialize_child(obj, "audmux", &s->audmux, TYPE_IMX6_AUDMUX);
    for (i = 0; i < FSL_IMX6_NUM_SSIS; i++) {
        snprintf(name, NAME_SIZE, "ssi%d", i + 1);
        object_initialize_child(obj, name, &s->ssi[i], TYPE_IMX6_SSI);
    }
    object_initialize_child(obj, "gpmi-reset", &s->gpmi_reset,
                            TYPE_IMX6_GPMI_RESET);
    imx6_apbh_dma_set_gpmi(&s->apbh, &s->gpmi_reset);
    object_initialize_child(obj, "bch", &s->bch, TYPE_IMX6_BCH);
    imx6_apbh_dma_set_bch(&s->apbh, &s->bch);
    imx6_bch_set_gpmi(&s->bch, &s->gpmi_reset);

    object_initialize_child(obj, "gc2000-diagnostic", &s->gc2000_diag,
                            TYPE_IMX6_GC2000_DIAG);
    object_initialize_child(obj, "gc320-diagnostic", &s->gc320_diag,
                            TYPE_IMX6_GC320_DIAG);
    object_initialize_child(obj, "openvg-reset-diagnostic",
                            &s->openvg_reset_diag,
                            TYPE_IMX6_OPENVG_RESET_DIAG);
    object_initialize_child(obj, "ipu1-trace-diagnostic", &s->ipu1_trace,
                            TYPE_IMX6_IPU1_TRACE);
    object_initialize_child(obj, "xdj-scanout-diagnostic", &s->xdj_scanout,
                            TYPE_IMX6_XDJ_SCANOUT);

    object_initialize_child(obj, "snvs", &s->snvs, TYPE_IMX7_SNVS);

    for (i = 0; i < FSL_IMX6_NUM_UARTS; i++) {
        snprintf(name, NAME_SIZE, "uart%d", i + 1);
        object_initialize_child(obj, name, &s->uart[i], TYPE_IMX_SERIAL);
    }

    object_initialize_child(obj, "gpt", &s->gpt, TYPE_IMX6_GPT);

    for (i = 0; i < FSL_IMX6_NUM_EPITS; i++) {
        snprintf(name, NAME_SIZE, "epit%d", i + 1);
        object_initialize_child(obj, name, &s->epit[i], TYPE_IMX_EPIT);
    }

    for (i = 0; i < FSL_IMX6_NUM_I2CS; i++) {
        snprintf(name, NAME_SIZE, "i2c%d", i + 1);
        object_initialize_child(obj, name, &s->i2c[i], TYPE_IMX_I2C);
    }

    for (i = 0; i < FSL_IMX6_NUM_GPIOS; i++) {
        snprintf(name, NAME_SIZE, "gpio%d", i + 1);
        object_initialize_child(obj, name, &s->gpio[i], TYPE_IMX_GPIO);
    }

    for (i = 0; i < FSL_IMX6_NUM_USDHCS; i++) {
        snprintf(name, NAME_SIZE, "sdhc%d", i + 1);
        object_initialize_child(obj, name, &s->usdhc[i], TYPE_IMX_USDHC);
    }

    for (i = 0; i < FSL_IMX6_NUM_USB_PHYS; i++) {
        snprintf(name, NAME_SIZE, "usbphy%d", i);
        object_initialize_child(obj, name, &s->usbphy[i], TYPE_IMX_USBPHY);
    }
    for (i = 0; i < FSL_IMX6_NUM_USBS; i++) {
        snprintf(name, NAME_SIZE, "usb%d", i);
        object_initialize_child(obj, name, &s->usb[i], TYPE_CHIPIDEA);
    }

    for (i = 0; i < FSL_IMX6_NUM_ECSPIS; i++) {
        snprintf(name, NAME_SIZE, "spi%d", i + 1);
        object_initialize_child(obj, name, &s->spi[i], TYPE_IMX_SPI);
    }
    for (i = 0; i < FSL_IMX6_NUM_CANS; i++) {
        snprintf(name, NAME_SIZE, "flexcan%d", i + 1);
        object_initialize_child(obj, name, &s->flexcan[i], TYPE_CAN_FLEXCAN2);
    }
    for (i = 0; i < FSL_IMX6_NUM_WDTS; i++) {
        snprintf(name, NAME_SIZE, "wdt%d", i);
        object_initialize_child(obj, name, &s->wdt[i], TYPE_IMX2_WDT);
    }


    object_initialize_child(obj, "eth", &s->eth, TYPE_IMX_ENET);
    object_initialize_child(obj, "rtl8309m", &s->rtl8309m,
                            TYPE_IMX_RTL8309M);

    object_initialize_child(obj, "pcie", &s->pcie, TYPE_DESIGNWARE_PCIE_HOST);
    object_initialize_child(obj, "pcie4-msi-irq", &s->pcie4_msi_irq,
                            TYPE_OR_IRQ);
}

static void fsl_imx6_realize(DeviceState *dev, Error **errp)
{
    MachineState *ms = MACHINE(qdev_get_machine());
    FslIMX6State *s = FSL_IMX6(dev);
    uint16_t i;
    qemu_irq irq;
    unsigned int smp_cpus = ms->smp.cpus;
    DeviceState *mpcore = DEVICE(&s->a9mpcore);
    DeviceState *gic;
    char name[NAME_SIZE];

    if (smp_cpus > FSL_IMX6_NUM_CPUS) {
        error_setg(errp, "%s: Only %d CPUs are supported (%d requested)",
                   TYPE_FSL_IMX6, FSL_IMX6_NUM_CPUS, smp_cpus);
        return;
    }

    for (i = 0; i < smp_cpus; i++) {
        snprintf(name, NAME_SIZE, "cpu%d", i);
        object_initialize_child(OBJECT(dev), name, &s->cpu[i],
                                ARM_CPU_TYPE_NAME("cortex-a9"));
    }

    for (i = 0; i < smp_cpus; i++) {

        /* On uniprocessor, the CBAR is set to 0 */
        if (smp_cpus > 1) {
            object_property_set_int(OBJECT(&s->cpu[i]), "reset-cbar",
                                    FSL_IMX6_A9MPCORE_ADDR, &error_abort);
        }

        /* All CPU but CPU 0 start in power off mode */
        if (i) {
            object_property_set_bool(OBJECT(&s->cpu[i]), "start-powered-off",
                                     true, &error_abort);
        }

        if (!qdev_realize(DEVICE(&s->cpu[i]), NULL, errp)) {
            return;
        }
    }

    object_property_set_int(OBJECT(mpcore), "num-cpu", smp_cpus, &error_abort);

    object_property_set_int(OBJECT(mpcore), "num-irq",
                            FSL_IMX6_MAX_IRQ + GIC_INTERNAL, &error_abort);

    if (!sysbus_realize(SYS_BUS_DEVICE(mpcore), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(mpcore), 0, FSL_IMX6_A9MPCORE_ADDR);

    gic = mpcore;
    for (i = 0; i < smp_cpus; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i,
                           qdev_get_gpio_in(DEVICE(&s->cpu[i]), ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(gic), i + smp_cpus,
                           qdev_get_gpio_in(DEVICE(&s->cpu[i]), ARM_CPU_FIQ));
    }

    /* L2 cache controller */
    sysbus_create_simple("l2x0", FSL_IMX6_PL310_ADDR, NULL);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ccm), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ccm), 0, FSL_IMX6_CCM_ADDR);

    /* TYPE_IMX6_SRC contains a dot and cannot be addressed by -global. */
    s->src.nand_boot_mode = s->nand_boot_mode;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->src), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->src), 0, FSL_IMX6_SRC_ADDR);

    /* Phase-A XZ model: APBH channel-zero handoff plus GPMI/BCH reset only. */
    sysbus_realize(SYS_BUS_DEVICE(&s->apbh), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->apbh), 0,
                    FSL_IMX6_APBH_BRIDGE_DMA_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->apbh), 0,
                       qdev_get_gpio_in(gic, FSL_IMX6_APBH_BRIDGE_DMA_IRQ));
    sysbus_realize(SYS_BUS_DEVICE(&s->gpmi_reset), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpmi_reset), 0, FSL_IMX6_GPMI_ADDR);
    sysbus_realize(SYS_BUS_DEVICE(&s->bch), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->bch), 0, FSL_IMX6_BCH_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->bch), 0,
                       qdev_get_gpio_in(gic, FSL_IMX6_BCH_IRQ));

    /* Pioneer Linux polls INTR[0] for setup, then uses cyclic PCM channels. */
    s->sdma.cyclic_diagnostic_enable = s->sdma_cyclic_diagnostic_enable;
    s->sdma.cyclic_audio_enable = s->sdma_cyclic_audio_enable;
    s->sdma.cyclic_audio_catchup_enable =
        s->sdma_cyclic_audio_catchup_enable;
    s->sdma.cyclic_static_metadata_fastpath =
        s->sdma_cyclic_static_metadata_fastpath;
    s->sdma.performance_diagnostic_enable =
        s->sdma_performance_diagnostic_enable;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->sdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->sdma), 0, FSL_IMX6_SDMA_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->sdma), 0,
                       qdev_get_gpio_in(gic, FSL_IMX6_SDMA_IRQ));

    /* Pioneer d810-audio-rev8 configures AUDMUX and SSI1/SSI2 before SDMA. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->audmux), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->audmux), 0, FSL_IMX6_AUDMUX_ADDR);
    for (i = 0; i < FSL_IMX6_NUM_SSIS; i++) {
        static const hwaddr ssi_addr[FSL_IMX6_NUM_SSIS] = {
            FSL_IMX6_SSI1_ADDR, FSL_IMX6_SSI2_ADDR,
        };

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->ssi[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->ssi[i]), 0, ssi_addr[i]);
    }
    s->sdma.ssi[0] = &s->ssi[0];
    s->sdma.ssi[1] = &s->ssi[1];

    /*
     * Realize the embedded diagnostics so QOM has no unrealized child; leave
     * their windows absent unless the corresponding explicit gate is set.
     */
    s->gc2000_diag.diagnostic_reference_identity =
        s->gc2000_diagnostic_reference_identity;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gc2000_diag), errp)) {
        return;
    }
    s->gc320_diag.diagnostic_gc320_model =
        s->gc320_diagnostic_gc320_model;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gc320_diag), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->openvg_reset_diag), errp)) {
        return;
    }
    s->ipu1_trace.bg23_eof_diagnostic_enable =
        s->ipu1_bg23_eof_diagnostic_enable;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ipu1_trace), errp)) {
        return;
    }
    s->xdj_scanout.diagnostic_enable = s->xdj_scanout_diagnostic_enable ||
        s->ipu1_bg23_cpmem_scanout_diagnostic_enable;
    s->xdj_scanout.bg23_cpmem_diagnostic_enable =
        s->ipu1_bg23_cpmem_scanout_diagnostic_enable;
    s->xdj_scanout.ipu1_trace = &s->ipu1_trace;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->xdj_scanout), errp)) {
        return;
    }

    /* Explicit diagnostic gate; no Vivante MMIO window exists by default. */
    if (s->gc2000_diagnostic_enable) {
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->gc2000_diag), 0,
                        FSL_IMX6_GPU_3D_ADDR);
    }

    /* Independent opt-in 2D diagnostic device; no shared 3D state or IRQ. */
    if (s->gc320_diagnostic_enable) {
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->gc320_diag), 0,
                        FSL_IMX6_GPU_2D_ADDR);
    }

    /* H15: source-bounded reset state only; no identity, FE, DMA, or IRQ. */
    if (s->openvg_reset_diagnostic_enable) {
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->openvg_reset_diag), 0,
                        FSL_IMX6_OPENVG_ADDR);
    }

    /* E0 is trace-only.  B1 is a separate, explicit BG23 experiment. */
    if (s->ipu1_trace_diagnostic_enable ||
        s->ipu1_bg23_eof_diagnostic_enable ||
        s->ipu1_bg23_cpmem_scanout_diagnostic_enable) {
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->ipu1_trace), 0,
                        FSL_IMX6_IPU_1_ADDR);
    }
    if (s->ipu1_bg23_eof_diagnostic_enable) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->ipu1_trace), 0,
                           qdev_get_gpio_in(gic, FSL_IMX6_IPU1_SYNC_IRQ));
    }

    /* Initialize all UARTs */
    for (i = 0; i < FSL_IMX6_NUM_UARTS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } serial_table[FSL_IMX6_NUM_UARTS] = {
            { FSL_IMX6_UART1_ADDR, FSL_IMX6_UART1_IRQ },
            { FSL_IMX6_UART2_ADDR, FSL_IMX6_UART2_IRQ },
            { FSL_IMX6_UART3_ADDR, FSL_IMX6_UART3_IRQ },
            { FSL_IMX6_UART4_ADDR, FSL_IMX6_UART4_IRQ },
            { FSL_IMX6_UART5_ADDR, FSL_IMX6_UART5_IRQ },
        };

        qdev_prop_set_chr(DEVICE(&s->uart[i]), "chardev", serial_hd(i));

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart[i]), errp)) {
            return;
        }

        sysbus_mmio_map(SYS_BUS_DEVICE(&s->uart[i]), 0, serial_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart[i]), 0,
                           qdev_get_gpio_in(gic, serial_table[i].irq));
    }

    s->gpt.ccm = IMX_CCM(&s->ccm);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpt), errp)) {
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpt), 0, FSL_IMX6_GPT_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpt), 0,
                       qdev_get_gpio_in(gic, FSL_IMX6_GPT_IRQ));

    /* Initialize all EPIT timers */
    for (i = 0; i < FSL_IMX6_NUM_EPITS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } epit_table[FSL_IMX6_NUM_EPITS] = {
            { FSL_IMX6_EPIT1_ADDR, FSL_IMX6_EPIT1_IRQ },
            { FSL_IMX6_EPIT2_ADDR, FSL_IMX6_EPIT2_IRQ },
        };

        s->epit[i].ccm = IMX_CCM(&s->ccm);

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->epit[i]), errp)) {
            return;
        }

        sysbus_mmio_map(SYS_BUS_DEVICE(&s->epit[i]), 0, epit_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->epit[i]), 0,
                           qdev_get_gpio_in(gic, epit_table[i].irq));
    }

    /* Initialize all I2C */
    for (i = 0; i < FSL_IMX6_NUM_I2CS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } i2c_table[FSL_IMX6_NUM_I2CS] = {
            { FSL_IMX6_I2C1_ADDR, FSL_IMX6_I2C1_IRQ },
            { FSL_IMX6_I2C2_ADDR, FSL_IMX6_I2C2_IRQ },
            { FSL_IMX6_I2C3_ADDR, FSL_IMX6_I2C3_IRQ }
        };

        if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c[i]), errp)) {
            return;
        }

        sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c[i]), 0, i2c_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c[i]), 0,
                           qdev_get_gpio_in(gic, i2c_table[i].irq));
    }

    /* Initialize all GPIOs */
    for (i = 0; i < FSL_IMX6_NUM_GPIOS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq_low;
            unsigned int irq_high;
        } gpio_table[FSL_IMX6_NUM_GPIOS] = {
            {
                FSL_IMX6_GPIO1_ADDR,
                FSL_IMX6_GPIO1_LOW_IRQ,
                FSL_IMX6_GPIO1_HIGH_IRQ
            },
            {
                FSL_IMX6_GPIO2_ADDR,
                FSL_IMX6_GPIO2_LOW_IRQ,
                FSL_IMX6_GPIO2_HIGH_IRQ
            },
            {
                FSL_IMX6_GPIO3_ADDR,
                FSL_IMX6_GPIO3_LOW_IRQ,
                FSL_IMX6_GPIO3_HIGH_IRQ
            },
            {
                FSL_IMX6_GPIO4_ADDR,
                FSL_IMX6_GPIO4_LOW_IRQ,
                FSL_IMX6_GPIO4_HIGH_IRQ
            },
            {
                FSL_IMX6_GPIO5_ADDR,
                FSL_IMX6_GPIO5_LOW_IRQ,
                FSL_IMX6_GPIO5_HIGH_IRQ
            },
            {
                FSL_IMX6_GPIO6_ADDR,
                FSL_IMX6_GPIO6_LOW_IRQ,
                FSL_IMX6_GPIO6_HIGH_IRQ
            },
            {
                FSL_IMX6_GPIO7_ADDR,
                FSL_IMX6_GPIO7_LOW_IRQ,
                FSL_IMX6_GPIO7_HIGH_IRQ
            },
        };

        object_property_set_bool(OBJECT(&s->gpio[i]), "has-edge-sel", true,
                                 &error_abort);
        object_property_set_bool(OBJECT(&s->gpio[i]), "has-upper-pin-irq",
                                 true, &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio[i]), errp)) {
            return;
        }

        sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio[i]), 0, gpio_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio[i]), 0,
                           qdev_get_gpio_in(gic, gpio_table[i].irq_low));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio[i]), 1,
                           qdev_get_gpio_in(gic, gpio_table[i].irq_high));
    }

    /* Initialize all SDHC */
    for (i = 0; i < FSL_IMX6_NUM_USDHCS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } esdhc_table[FSL_IMX6_NUM_USDHCS] = {
            { FSL_IMX6_uSDHC1_ADDR, FSL_IMX6_uSDHC1_IRQ },
            { FSL_IMX6_uSDHC2_ADDR, FSL_IMX6_uSDHC2_IRQ },
            { FSL_IMX6_uSDHC3_ADDR, FSL_IMX6_uSDHC3_IRQ },
            { FSL_IMX6_uSDHC4_ADDR, FSL_IMX6_uSDHC4_IRQ },
        };

        /* UHS-I SDIO3.0 SDR104 1.8V ADMA */
        object_property_set_uint(OBJECT(&s->usdhc[i]), "capareg",
                                 IMX6_ESDHC_CAPABILITIES, &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->usdhc[i]), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->usdhc[i]), 0, esdhc_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->usdhc[i]), 0,
                           qdev_get_gpio_in(gic, esdhc_table[i].irq));
    }

    /* USB */
    for (i = 0; i < FSL_IMX6_NUM_USB_PHYS; i++) {
        sysbus_realize(SYS_BUS_DEVICE(&s->usbphy[i]), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->usbphy[i]), 0,
                        FSL_IMX6_USBPHY1_ADDR + i * 0x1000);
    }
    for (i = 0; i < FSL_IMX6_NUM_USBS; i++) {
        static const int FSL_IMX6_USBn_IRQ[] = {
            FSL_IMX6_USB_OTG_IRQ,
            FSL_IMX6_USB_HOST1_IRQ,
            FSL_IMX6_USB_HOST2_IRQ,
            FSL_IMX6_USB_HOST3_IRQ,
        };

        sysbus_realize(SYS_BUS_DEVICE(&s->usb[i]), &error_abort);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->usb[i]), 0,
                        FSL_IMX6_USBOH3_USB_ADDR + i * 0x200);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->usb[i]), 0,
                           qdev_get_gpio_in(gic, FSL_IMX6_USBn_IRQ[i]));
    }

    /* Initialize all ECSPI */
    for (i = 0; i < FSL_IMX6_NUM_ECSPIS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } spi_table[FSL_IMX6_NUM_ECSPIS] = {
            { FSL_IMX6_eCSPI1_ADDR, FSL_IMX6_ECSPI1_IRQ },
            { FSL_IMX6_eCSPI2_ADDR, FSL_IMX6_ECSPI2_IRQ },
            { FSL_IMX6_eCSPI3_ADDR, FSL_IMX6_ECSPI3_IRQ },
            { FSL_IMX6_eCSPI4_ADDR, FSL_IMX6_ECSPI4_IRQ },
            { FSL_IMX6_eCSPI5_ADDR, FSL_IMX6_ECSPI5_IRQ },
        };

        /* Initialize the SPI */
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->spi[i]), errp)) {
            return;
        }

        sysbus_mmio_map(SYS_BUS_DEVICE(&s->spi[i]), 0, spi_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->spi[i]), 0,
                           qdev_get_gpio_in(gic, spi_table[i].irq));
    }

    /* Initialize all FLEXCANs */
    for (i = 0; i < FSL_IMX6_NUM_CANS; i++) {
        static const struct {
            hwaddr addr;
            unsigned int irq;
        } flexcan_table[FSL_IMX6_NUM_CANS] = {
            { FSL_IMX6_CAN1_ADDR, FSL_IMX6_FLEXCAN1_IRQ },
            { FSL_IMX6_CAN2_ADDR, FSL_IMX6_FLEXCAN2_IRQ },
        };

        object_property_set_link(OBJECT(&s->flexcan[i]), "clock-control-module",
                                 OBJECT(&s->ccm), &error_abort);
        object_property_set_link(OBJECT(&s->flexcan[i]), "canbus",
                                 OBJECT(s->canbus[i]), &error_abort);

        sysbus_realize(SYS_BUS_DEVICE(&s->flexcan[i]), &error_abort);

        sysbus_mmio_map(SYS_BUS_DEVICE(&s->flexcan[i]), 0, flexcan_table[i].addr);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->flexcan[i]), 0,
                           qdev_get_gpio_in(gic, flexcan_table[i].irq));
    }

    object_property_set_uint(OBJECT(&s->eth), "phy-num", s->phy_num,
                             &error_abort);
    object_property_set_bool(OBJECT(&s->rtl8309m), "trace-enable",
                             s->xdj_rtl8309m_trace_enable, &error_abort);
    /*
     * This is a SoC child and therefore participates in qdev reset even
     * when its MDIO mux is disabled.  Realize it unconditionally; only the
     * FEC attachment below is opt-in.
     */
    if (!qdev_realize(DEVICE(&s->rtl8309m), NULL, errp)) {
        return;
    }
    /*
     * With the XDJ switch enabled, the default QEMU NIC belongs to external
     * switch port 1, not directly to the FEC CPU port.  This preserves the
     * traditional one-backend launch while allowing ports 2 and 3 to be
     * attached independently through additional imx-rtl8309m NIC entries.
     */
    if (!s->xdj_rtl8309m_enable ||
        !imx_rtl8309m_dataplane_enabled(&s->rtl8309m)) {
        qemu_configure_nic_device(DEVICE(&s->eth), true, NULL);
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->eth), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->eth), 0, FSL_IMX6_ENET_ADDR);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->eth), 0,
                       qdev_get_gpio_in(gic, FSL_IMX6_ENET_MAC_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->eth), 1,
                       qdev_get_gpio_in(gic, FSL_IMX6_ENET_MAC_1588_IRQ));
    if (s->xdj_rtl8309m_enable) {
        imx_fec_set_rtl8309m(&s->eth, &s->rtl8309m);
        if (imx_rtl8309m_dataplane_enabled(&s->rtl8309m)) {
            imx_rtl8309m_attach_fec(&s->rtl8309m, &s->eth);
        }
    }

    /*
     * SNVS
     */
    sysbus_realize(SYS_BUS_DEVICE(&s->snvs), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->snvs), 0, FSL_IMX6_SNVSHP_ADDR);

    /*
     * Watchdog
     */
    for (i = 0; i < FSL_IMX6_NUM_WDTS; i++) {
        static const hwaddr FSL_IMX6_WDOGn_ADDR[FSL_IMX6_NUM_WDTS] = {
            FSL_IMX6_WDOG1_ADDR,
            FSL_IMX6_WDOG2_ADDR,
        };
        static const int FSL_IMX6_WDOGn_IRQ[FSL_IMX6_NUM_WDTS] = {
            FSL_IMX6_WDOG1_IRQ,
            FSL_IMX6_WDOG2_IRQ,
        };

        object_property_set_bool(OBJECT(&s->wdt[i]), "pretimeout-support",
                                 true, &error_abort);
        sysbus_realize(SYS_BUS_DEVICE(&s->wdt[i]), &error_abort);

        sysbus_mmio_map(SYS_BUS_DEVICE(&s->wdt[i]), 0, FSL_IMX6_WDOGn_ADDR[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->wdt[i]), 0,
                           qdev_get_gpio_in(gic, FSL_IMX6_WDOGn_IRQ[i]));
    }

    /*
     * PCIe
     */
    sysbus_realize(SYS_BUS_DEVICE(&s->pcie), &error_abort);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pcie), 0, FSL_IMX6_PCIe_REG_ADDR);

    object_property_set_int(OBJECT(&s->pcie4_msi_irq), "num-lines", 2,
                            &error_abort);
    qdev_realize(DEVICE(&s->pcie4_msi_irq), NULL, &error_abort);

    irq = qdev_get_gpio_in(DEVICE(&s->a9mpcore), FSL_IMX6_PCIE4_MSI_IRQ);
    qdev_connect_gpio_out(DEVICE(&s->pcie4_msi_irq), 0, irq);

    irq = qdev_get_gpio_in(DEVICE(&s->a9mpcore), FSL_IMX6_PCIE1_IRQ);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pcie), 0, irq);
    irq = qdev_get_gpio_in(DEVICE(&s->a9mpcore), FSL_IMX6_PCIE2_IRQ);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pcie), 1, irq);
    irq = qdev_get_gpio_in(DEVICE(&s->a9mpcore), FSL_IMX6_PCIE3_IRQ);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pcie), 2, irq);
    irq = qdev_get_gpio_in(DEVICE(&s->pcie4_msi_irq), 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pcie), 3, irq);
    irq = qdev_get_gpio_in(DEVICE(&s->pcie4_msi_irq), 1);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->pcie), 4, irq);

    /*
     * PCIe PHY
     */
    create_unimplemented_device("pcie-phy", FSL_IMX6_PCIe_ADDR,
                                FSL_IMX6_PCIe_SIZE);

    /* ROM memory */
    if (!memory_region_init_rom(&s->rom, OBJECT(dev), "imx6.rom",
                                FSL_IMX6_ROM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), FSL_IMX6_ROM_ADDR,
                                &s->rom);

    /* CAAM memory */
    if (!memory_region_init_rom(&s->caam, OBJECT(dev), "imx6.caam",
                                FSL_IMX6_CAAM_MEM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), FSL_IMX6_CAAM_MEM_ADDR,
                                &s->caam);

    /* OCRAM memory */
    if (!memory_region_init_ram(&s->ocram, NULL, "imx6.ocram",
                                FSL_IMX6_OCRAM_SIZE, errp)) {
        return;
    }
    memory_region_add_subregion(get_system_memory(), FSL_IMX6_OCRAM_ADDR,
                                &s->ocram);

    /* internal OCRAM (256 KB) is aliased over 1 MB */
    memory_region_init_alias(&s->ocram_alias, OBJECT(dev), "imx6.ocram_alias",
                             &s->ocram, 0, FSL_IMX6_OCRAM_ALIAS_SIZE);
    memory_region_add_subregion(get_system_memory(), FSL_IMX6_OCRAM_ALIAS_ADDR,
                                &s->ocram_alias);
}

static const Property fsl_imx6_properties[] = {
    DEFINE_PROP_UINT32("fec-phy-num", FslIMX6State, phy_num, 0),
    DEFINE_PROP_BOOL("xdj-rtl8309m-enable", FslIMX6State,
                     xdj_rtl8309m_enable, false),
    DEFINE_PROP_BOOL("xdj-rtl8309m-trace-enable", FslIMX6State,
                     xdj_rtl8309m_trace_enable, false),
    DEFINE_PROP_BOOL("gc2000-diagnostic-enable", FslIMX6State,
                     gc2000_diagnostic_enable, false),
    DEFINE_PROP_BOOL("gc2000-diagnostic-reference-identity", FslIMX6State,
                     gc2000_diagnostic_reference_identity, false),
    DEFINE_PROP_BOOL("gc320-diagnostic-enable", FslIMX6State,
                     gc320_diagnostic_enable, false),
    DEFINE_PROP_BOOL("gc320-diagnostic-gc320-model", FslIMX6State,
                     gc320_diagnostic_gc320_model, false),
    DEFINE_PROP_BOOL("openvg-reset-diagnostic-enable", FslIMX6State,
                     openvg_reset_diagnostic_enable, false),
    DEFINE_PROP_BOOL("ipu1-trace-diagnostic-enable", FslIMX6State,
                     ipu1_trace_diagnostic_enable, false),
    DEFINE_PROP_BOOL("ipu1-bg23-eof-diagnostic-enable", FslIMX6State,
                     ipu1_bg23_eof_diagnostic_enable, false),
    DEFINE_PROP_BOOL("ipu1-bg23-cpmem-scanout-diagnostic-enable",
                     FslIMX6State,
                     ipu1_bg23_cpmem_scanout_diagnostic_enable, false),
    DEFINE_PROP_BOOL("xdj-scanout-diagnostic-enable", FslIMX6State,
                     xdj_scanout_diagnostic_enable, false),
    DEFINE_PROP_BOOL("sdma-cyclic-diagnostic-enable", FslIMX6State,
                     sdma_cyclic_diagnostic_enable, false),
    DEFINE_PROP_BOOL("sdma-cyclic-audio-enable", FslIMX6State,
                     sdma_cyclic_audio_enable, false),
    DEFINE_PROP_BOOL("sdma-cyclic-audio-catchup-enable", FslIMX6State,
                     sdma_cyclic_audio_catchup_enable, false),
    DEFINE_PROP_BOOL("sdma-cyclic-static-metadata-fastpath", FslIMX6State,
                     sdma_cyclic_static_metadata_fastpath, false),
    DEFINE_PROP_BOOL("sdma-performance-diagnostic-enable", FslIMX6State,
                     sdma_performance_diagnostic_enable, false),
    DEFINE_PROP_BOOL("nand-boot-mode", FslIMX6State, nand_boot_mode, false),
    DEFINE_PROP_LINK("canbus0", FslIMX6State, canbus[0], TYPE_CAN_BUS,
                     CanBusState *),
    DEFINE_PROP_LINK("canbus1", FslIMX6State, canbus[1], TYPE_CAN_BUS,
                     CanBusState *),
};

static void fsl_imx6_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_props(dc, fsl_imx6_properties);
    dc->realize = fsl_imx6_realize;
    dc->desc = "i.MX6 SOC";
    /* Reason: Uses serial_hd() in the realize() function */
    dc->user_creatable = false;
}

static const TypeInfo fsl_imx6_type_info = {
    .name = TYPE_FSL_IMX6,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(FslIMX6State),
    .instance_init = fsl_imx6_init,
    .class_init = fsl_imx6_class_init,
};

static void fsl_imx6_register_types(void)
{
    type_register_static(&fsl_imx6_type_info);
}

type_init(fsl_imx6_register_types)
