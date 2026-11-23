/*
 * SABRELITE Board System emulation.
 *
 * Copyright (c) 2015 Jean-Christophe Dubois <jcd@tribudubois.net>
 *
 * This code is licensed under the GPL, version 2 or later.
 * See the file `COPYING' in the top level directory.
 *
 * It (partially) emulates a sabrelite board, with a Freescale
 * i.MX6 SoC
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/fsl-imx6.h"
#include "hw/arm/boot.h"
#include "hw/misc/imx6_m16_erp_diag.h"
#include "hw/misc/imx6_xz_mixer_diag.h"
#include "hw/misc/imx6_xz_jog_diag.h"
#include "hw/misc/imx6_tusb4020bi.h"
#include "hw/ssi/imx_spi.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/i2c/i2c.h"
#include "monitor/qdev.h"
#include "qemu/error-report.h"
#include "system/qtest.h"
#include "system/reset.h"

struct SabreliteMachineState {
    MachineState parent_obj;

    FslIMX6State soc;
    CanBusState *canbus[FSL_IMX6_NUM_CANS];
    char *xz_m16_erp_mode;
    char *xz_mixer_mode;
    char *xz_jog_mode;
    bool xz_jog_trace_diagnostic_enable;
    char *xz_usb_overcurrent_mode;
    bool xz_tusb4020bi;
    bool xz_rtl8309m;
    bool xz_rtl8309m_trace;
    IMX6XZMixerDiagState *xz_mixer;
    IMX6XZJogDiagState *xz_jog[2];
    bool xz_mixer_activated;
    qemu_irq xz_usb_overcurrent[2];
};

#define TYPE_SABRELITE_MACHINE MACHINE_TYPE_NAME("sabrelite")
OBJECT_DECLARE_SIMPLE_TYPE(SabreliteMachineState, SABRELITE_MACHINE)

static struct arm_boot_info sabrelite_binfo = {
    /* DDR memory start */
    .loader_start = FSL_IMX6_MMDC_ADDR,
    /* No board ID, we boot from DT tree */
    .board_id = -1,
};

/* No need to do any particular setup for secondary boot */
static void sabrelite_write_secondary(ARMCPU *cpu,
                                      const struct arm_boot_info *info)
{
}

/* Secondary cores are reset through SRC device */
static void sabrelite_reset_secondary(ARMCPU *cpu,
                                      const struct arm_boot_info *info)
{
}

static char *sabrelite_get_xz_m16_erp_mode(Object *obj, Error **errp)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(obj);

    return g_strdup(s->xz_m16_erp_mode);
}

static void sabrelite_set_xz_m16_erp_mode(Object *obj, const char *value,
                                           Error **errp)
{
    static const char *const modes[] = {
        "off", "no-request", "withhold-ready", "withhold-reply",
        "zero-reply", "bad-crc", "bad-header", "accept",
        "panel-first-reports", "panel-first-reports-tempo-center",
    };
    SabreliteMachineState *s = SABRELITE_MACHINE(obj);
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(modes); i++) {
        if (!strcmp(value, modes[i])) {
            g_free(s->xz_m16_erp_mode);
            s->xz_m16_erp_mode = g_strdup(value);
            return;
        }
    }
    error_setg(errp, "invalid xz-m16-erp mode '%s'", value);
}

static char *sabrelite_get_xz_mixer_mode(Object *obj, Error **errp)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(obj);

    return g_strdup(s->xz_mixer_mode);
}

static void sabrelite_set_xz_mixer_mode(Object *obj, const char *value,
                                         Error **errp)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(obj);

    if (!strcmp(value, "off") || !strcmp(value, "synthetic-menu") ||
        !strcmp(value, "deferred-synthetic-menu")) {
        g_free(s->xz_mixer_mode);
        s->xz_mixer_mode = g_strdup(value);
        return;
    }
    error_setg(errp, "invalid xz-mixer mode '%s'", value);
}

static char *sabrelite_get_xz_jog_mode(Object *obj, Error **errp)
{
    return g_strdup(SABRELITE_MACHINE(obj)->xz_jog_mode);
}

static void sabrelite_set_xz_jog_mode(Object *obj, const char *value,
                                       Error **errp)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(obj);

    if (!strcmp(value, "off") || !strcmp(value, "synthetic-play-cue")) {
        g_free(s->xz_jog_mode);
        s->xz_jog_mode = g_strdup(value);
        return;
    }
    error_setg(errp, "invalid xz-jog mode '%s'", value);
}

static bool sabrelite_get_xz_jog_trace_diagnostic_enable(Object *obj,
                                                          Error **errp)
{
    return SABRELITE_MACHINE(obj)->xz_jog_trace_diagnostic_enable;
}

static void sabrelite_set_xz_jog_trace_diagnostic_enable(Object *obj,
                                                          bool value,
                                                          Error **errp)
{
    SABRELITE_MACHINE(obj)->xz_jog_trace_diagnostic_enable = value;
}

static void sabrelite_activate_xz_jog(SabreliteMachineState *s,
                                      unsigned index, Error **errp)
{
    static const char *const spi_names[] = { "spi2", "spi3" };
    /* ECSPI2 is JOG LCD 2 (Right/Deck 2); ECSPI3 is LCD 1 (Left/Deck 1). */
    static const char *const ids[] = { "xz-jog-right", "xz-jog-left" };
    Object *spi_dev;
    DeviceState *jog;
    SSIBus *spi_bus;

    if (s->xz_jog[index]) {
        return;
    }
    spi_dev = object_resolve_path_component(OBJECT(&s->soc), spi_names[index]);
    if (!spi_dev) {
        error_setg(errp, "XZ jog diagnostic: ECSPI%u is unavailable", index + 2);
        return;
    }
    /* Only XZ jog endpoints require manual-transfer CS framing.  Keep the
     * established ECSPI4/5 panel and mixer behaviour untouched. */
    IMX_SPI(spi_dev)->deassert_cs_on_tc = true;
    spi_bus = (SSIBus *)qdev_get_child_bus(DEVICE(spi_dev), "spi");
    if (!spi_bus) {
        error_setg(errp, "XZ jog diagnostic: ECSPI%u bus is unavailable", index + 2);
        return;
    }
    jog = qdev_new(TYPE_IMX6_XZ_JOG_DIAG);
    qdev_set_id(jog, g_strdup(ids[index]), errp);
    if (*errp) {
        object_unref(OBJECT(jog));
        return;
    }
    qdev_prop_set_uint8(jog, "cs", 0);
    qdev_prop_set_bit(jog, "diagnostic-enable",
                      s->xz_jog_trace_diagnostic_enable);
    qdev_realize_and_unref(jog, BUS(spi_bus), errp);
    if (*errp) {
        return;
    }
    s->xz_jog[index] = IMX6_XZ_JOG_DIAG(jog);
    sysbus_connect_irq(SYS_BUS_DEVICE(spi_dev), 1,
                       qdev_get_gpio_in_named(jog, SSI_GPIO_CS, 0));
    /* Original XDJ-XZ: Right ECSPI2 -> GPIO3_24; Left ECSPI3 -> GPIO4_24. */
    qdev_connect_gpio_out_named(jog, "ready", 0,
        qdev_get_gpio_in(DEVICE(&s->soc.gpio[index + 2]), 23));
    imx6_xz_jog_diag_set_ready(s->xz_jog[index], true);
}

/*
 * Attach only on an explicit board request.  In deferred mode this is not
 * called during machine construction: until activation, ECSPI5 and GPIO1_12
 * remain exactly as they are on the default, no-responder machine.
 */
static void sabrelite_activate_xz_mixer(SabreliteMachineState *s,
                                        Error **errp)
{
    Object *spi_dev;
    DeviceState *mixer;
    SSIBus *spi_bus;

    if (s->xz_mixer) {
        return;
    }

    spi_dev = object_resolve_path_component(OBJECT(&s->soc), "spi5");
    if (!spi_dev) {
        error_setg(errp, "XZ mixer diagnostic: ECSPI5 is unavailable");
        return;
    }
    spi_bus = (SSIBus *)qdev_get_child_bus(DEVICE(spi_dev), "spi");
    if (!spi_bus) {
        error_setg(errp, "XZ mixer diagnostic: ECSPI5 bus is unavailable");
        return;
    }

    mixer = qdev_new(TYPE_IMX6_XZ_MIXER_DIAG);
    qdev_set_id(mixer, g_strdup("xz-mixer"), errp);
    if (*errp) {
        object_unref(OBJECT(mixer));
        return;
    }
    qdev_prop_set_uint8(mixer, "cs", 0);
    qdev_realize_and_unref(mixer, BUS(spi_bus), errp);
    if (*errp) {
        return;
    }

    s->xz_mixer = IMX6_XZ_MIXER_DIAG(mixer);
    /* imx_spi SysBus IRQ 1 is ECSPI5 CS0 (IRQ 0 is controller IRQ). */
    sysbus_connect_irq(SYS_BUS_DEVICE(spi_dev), 1,
                       qdev_get_gpio_in_named(mixer, SSI_GPIO_CS, 0));
    qdev_connect_gpio_out_named(mixer, "ready", 0,
                                qdev_get_gpio_in(DEVICE(&s->soc.gpio[0]), 11));
    /* GPIO1_12 is active low on the original XDJ-XZ board. */
    imx6_xz_mixer_diag_set_ready(s->xz_mixer, true);
}

static bool sabrelite_get_xz_mixer_activate(Object *obj, Error **errp)
{
    return SABRELITE_MACHINE(obj)->xz_mixer_activated;
}

static void sabrelite_set_xz_mixer_activate(Object *obj, bool value,
                                             Error **errp)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(obj);

    if (!value) {
        error_setg(errp, "XZ mixer diagnostic cannot be detached at runtime");
        return;
    }
    if (strcmp(s->xz_mixer_mode, "deferred-synthetic-menu")) {
        error_setg(errp, "xz-mixer-activate requires deferred-synthetic-menu");
        return;
    }
    if (s->xz_mixer_activated) {
        return;
    }
    sabrelite_activate_xz_mixer(s, errp);
    if (!*errp) {
        s->xz_mixer_activated = true;
    }
}

static char *sabrelite_get_xz_usb_overcurrent_mode(Object *obj,
                                                    Error **errp)
{
    return g_strdup(SABRELITE_MACHINE(obj)->xz_usb_overcurrent_mode);
}

static void sabrelite_set_xz_usb_overcurrent_mode(Object *obj,
                                                    const char *value,
                                                    Error **errp)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(obj);

    if (strcmp(value, "normal") && strcmp(value, "a1-low") &&
        strcmp(value, "a2-low")) {
        error_setg(errp, "invalid xz-usb-overcurrent mode '%s'", value);
        return;
    }

    g_free(s->xz_usb_overcurrent_mode);
    s->xz_usb_overcurrent_mode = g_strdup(value);
}

static void sabrelite_apply_xz_usb_overcurrent(SabreliteMachineState *s)
{
    /* GPIO126/GPIO204: physical idle samples show active-low OC deasserted. */
    qemu_set_irq(s->xz_usb_overcurrent[0],
                 strcmp(s->xz_usb_overcurrent_mode, "a1-low") != 0);
    qemu_set_irq(s->xz_usb_overcurrent[1],
                 strcmp(s->xz_usb_overcurrent_mode, "a2-low") != 0);
}

static bool sabrelite_get_xz_tusb4020bi(Object *obj, Error **errp)
{
    return SABRELITE_MACHINE(obj)->xz_tusb4020bi;
}

static void sabrelite_set_xz_tusb4020bi(Object *obj, bool value,
                                         Error **errp)
{
    SABRELITE_MACHINE(obj)->xz_tusb4020bi = value;
}

static bool sabrelite_get_xz_rtl8309m(Object *obj, Error **errp)
{
    return SABRELITE_MACHINE(obj)->xz_rtl8309m;
}

static void sabrelite_set_xz_rtl8309m(Object *obj, bool value,
                                       Error **errp)
{
    SABRELITE_MACHINE(obj)->xz_rtl8309m = value;
}

static bool sabrelite_get_xz_rtl8309m_trace(Object *obj, Error **errp)
{
    return SABRELITE_MACHINE(obj)->xz_rtl8309m_trace;
}

static void sabrelite_set_xz_rtl8309m_trace(Object *obj, bool value,
                                             Error **errp)
{
    SABRELITE_MACHINE(obj)->xz_rtl8309m_trace = value;
}

static void sabrelite_reset(MachineState *machine, ResetType type)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(machine);

    qemu_devices_reset(type);
    sabrelite_apply_xz_usb_overcurrent(s);
}

static void sabrelite_init(MachineState *machine)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(machine);

    /* Check the amount of memory is compatible with the SOC */
    if (machine->ram_size > FSL_IMX6_MMDC_SIZE) {
        error_report("RAM size " RAM_ADDR_FMT " above max supported (%08x)",
                     machine->ram_size, FSL_IMX6_MMDC_SIZE);
        exit(1);
    }

    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_FSL_IMX6);

    /* Ethernet PHY address is 6 */
    object_property_set_int(OBJECT(&s->soc), "fec-phy-num", 6, &error_fatal);
    object_property_set_bool(OBJECT(&s->soc), "xdj-rtl8309m-enable",
                             s->xz_rtl8309m, &error_fatal);
    object_property_set_bool(OBJECT(&s->soc), "xdj-rtl8309m-trace-enable",
                             s->xz_rtl8309m_trace, &error_fatal);

    for (int i = 0; i < FSL_IMX6_NUM_CANS; i++) {
        g_autofree char *bus_name = g_strdup_printf("canbus%d", i);

        object_property_set_link(OBJECT(&s->soc), bus_name,
                                 OBJECT(s->canbus[i]), &error_fatal);
    }

    qdev_realize(DEVICE(&s->soc), NULL, &error_fatal);

    if (s->xz_tusb4020bi) {
        /* Pioneer’s setup_usbhub_4ch.sh talks to I2C1 / Linux i2c-0. */
        i2c_slave_create_simple(s->soc.i2c[0].bus,
                                TYPE_IMX6_XDJ_TUSB4020BI, 0x44);
    }

    /* GPIO4_IO30 (126) and GPIO7_IO12 (204), external USB-A OC inputs. */
    s->xz_usb_overcurrent[0] =
        qdev_get_gpio_in(DEVICE(&s->soc.gpio[3]), 30);
    s->xz_usb_overcurrent[1] =
        qdev_get_gpio_in(DEVICE(&s->soc.gpio[6]), 12);
    sabrelite_apply_xz_usb_overcurrent(s);

    memory_region_add_subregion(get_system_memory(), FSL_IMX6_MMDC_ADDR,
                                machine->ram);

    if (strcmp(s->xz_m16_erp_mode, "off")) {
        Object *spi_dev = object_resolve_path_component(OBJECT(&s->soc), "spi4");
        DeviceState *erp;
        SSIBus *spi_bus;

        if (!spi_dev) {
            error_report("XZ ERP diagnostic: ECSPI4 is unavailable");
            exit(1);
        }
        spi_bus = (SSIBus *)qdev_get_child_bus(DEVICE(spi_dev), "spi");
        if (!spi_bus) {
            error_report("XZ ERP diagnostic: ECSPI4 bus is unavailable");
            exit(1);
        }
        if (!strcmp(s->xz_m16_erp_mode, "panel-first-reports") ||
            !strcmp(s->xz_m16_erp_mode, "panel-first-reports-tempo-center")) {
            static const char *const panel_kinds[] = { "erp", "pnl" };
            unsigned cs;

            /*
             * The original i.MX SPI driver uses internal CS and releases it
             * when a queued panel message drains.  This edge is the report
             * boundary for ERP/PNL; without it, a short pre-RBP exchange can
             * skew every later 20-byte PNL frame.
             */
            IMX_SPI(spi_dev)->deassert_cs_on_tc = true;

            for (cs = 0; cs < ARRAY_SIZE(panel_kinds); cs++) {
                erp = qdev_new(TYPE_IMX6_M16_ERP_DIAG);
                qdev_prop_set_string(erp, "mode",
                    cs == 0 ? s->xz_m16_erp_mode : "panel-first-reports");
                qdev_prop_set_string(erp, "panel-kind", panel_kinds[cs]);
                qdev_prop_set_uint8(erp, "cs", cs);
                qdev_set_id(erp, g_strdup(cs == 0 ? "xz-erp" : "xz-pnl"),
                            &error_fatal);
                qdev_realize_and_unref(erp, BUS(spi_bus), &error_fatal);
                /* imx_spi IRQ 1 + CS drives the ECSPI4 chip-select line. */
                sysbus_connect_irq(SYS_BUS_DEVICE(spi_dev), 1 + cs,
                    qdev_get_gpio_in_named(erp, SSI_GPIO_CS, 0));
            }
        } else {
            erp = qdev_new(TYPE_IMX6_M16_ERP_DIAG);
            qdev_prop_set_string(erp, "mode", s->xz_m16_erp_mode);
            qdev_realize_and_unref(erp, BUS(spi_bus), &error_fatal);
            qdev_connect_gpio_out_named(erp, "ready", 0,
                qdev_get_gpio_in(DEVICE(&s->soc.gpio[0]), 16));
            qdev_connect_gpio_out(DEVICE(&s->soc.gpio[0]), 18,
                qdev_get_gpio_in_named(erp, "request", 0));
        }
    }

    if (!strcmp(s->xz_mixer_mode, "synthetic-menu")) {
        sabrelite_activate_xz_mixer(s, &error_fatal);
        s->xz_mixer_activated = true;
    }
    if (!strcmp(s->xz_jog_mode, "synthetic-play-cue")) {
        sabrelite_activate_xz_jog(s, 0, &error_fatal);
        sabrelite_activate_xz_jog(s, 1, &error_fatal);
    }

    {
        /*
         * TODO: Ideally we would expose the chip select and spi bus on the
         * SoC object using alias properties; then we would not need to
         * directly access the underlying spi device object.
         */
        /* Add the sst25vf016b NOR FLASH memory to first SPI */
        Object *spi_dev;

        spi_dev = object_resolve_path_component(OBJECT(&s->soc), "spi1");
        if (spi_dev) {
            SSIBus *spi_bus;

            spi_bus = (SSIBus *)qdev_get_child_bus(DEVICE(spi_dev), "spi");
            if (spi_bus) {
                DeviceState *flash_dev;
                qemu_irq cs_line;
                DriveInfo *dinfo = drive_get(IF_MTD, 0, 0);

                flash_dev = qdev_new("sst25vf016b");
                if (dinfo) {
                    qdev_prop_set_drive_err(flash_dev, "drive",
                                            blk_by_legacy_dinfo(dinfo),
                                            &error_fatal);
                }
                qdev_realize_and_unref(flash_dev, BUS(spi_bus), &error_fatal);

                cs_line = qdev_get_gpio_in_named(flash_dev, SSI_GPIO_CS, 0);
                qdev_connect_gpio_out(DEVICE(&s->soc.gpio[2]), 19, cs_line);
            }
        }
    }

    sabrelite_binfo.ram_size = machine->ram_size;
    sabrelite_binfo.secure_boot = true;
    sabrelite_binfo.write_secondary_boot = sabrelite_write_secondary;
    sabrelite_binfo.secondary_cpu_reset_hook = sabrelite_reset_secondary;

    if (!qtest_enabled()) {
        arm_load_kernel(&s->soc.cpu[0], machine, &sabrelite_binfo);
    }
}

static void sabrelite_machine_instance_init(Object *obj)
{
    SabreliteMachineState *s = SABRELITE_MACHINE(obj);

    s->xz_m16_erp_mode = g_strdup("off");
    object_property_add_str(obj, "xz-m16-erp",
                            sabrelite_get_xz_m16_erp_mode,
                            sabrelite_set_xz_m16_erp_mode);
    object_property_set_description(obj, "xz-m16-erp",
        "XDJ-XZ ERP updater diagnostic responder: off (default), "
        "negative handshake modes, accept, panel-first-reports, or "
        "panel-first-reports-tempo-center. The panel modes are synthetic "
        "normal-RBP diagnostics, not a physical M16C replay.");

    s->xz_mixer_mode = g_strdup("off");
    object_property_add_str(obj, "xz-mixer",
                            sabrelite_get_xz_mixer_mode,
                            sabrelite_set_xz_mixer_mode);
    object_property_set_description(obj, "xz-mixer",
        "XDJ-XZ mixer diagnostic responder: off (default), synthetic-menu, "
        "or deferred-synthetic-menu. "
        "synthetic-menu is not a physical mixer-ready frame replay.");
    object_property_add_bool(obj, "xz-mixer-activate",
                             sabrelite_get_xz_mixer_activate,
                             sabrelite_set_xz_mixer_activate);
    object_property_set_description(obj, "xz-mixer-activate",
        "Runtime activation for deferred-synthetic-menu only. The responder is "
        "absent before activation, preserving the default ECSPI5/GPIO state.");

    s->xz_jog_mode = g_strdup("off");
    object_property_add_str(obj, "xz-jog", sabrelite_get_xz_jog_mode,
                            sabrelite_set_xz_jog_mode);
    object_property_set_description(obj, "xz-jog",
        "XDJ-XZ jog diagnostic responder: off (default) or "
        "synthetic-play-cue. The latter only supplies RBP-decoded PLAY/CUE "
        "receive fields and is not a physical jog MCU replay.");
    object_property_add_bool(obj, "xz-jog-trace-diagnostic-enable",
                             sabrelite_get_xz_jog_trace_diagnostic_enable,
                             sabrelite_set_xz_jog_trace_diagnostic_enable);
    object_property_set_description(obj, "xz-jog-trace-diagnostic-enable",
        "Emit bounded, opt-in XDJ-XZ jog responder CS/model-frame telemetry. "
        "It observes transport only and does not alter SPI responses.");

    s->xz_usb_overcurrent_mode = g_strdup("normal");
    object_property_add_str(obj, "xz-usb-overcurrent",
                            sabrelite_get_xz_usb_overcurrent_mode,
                            sabrelite_set_xz_usb_overcurrent_mode);
    object_property_set_description(obj, "xz-usb-overcurrent",
        "XDJ-XZ USB-A overcurrent inputs: normal (both idle-high), a1-low, "
        "or a2-low for diagnostics.");

    object_property_add_bool(obj, "xz-tusb4020bi",
                             sabrelite_get_xz_tusb4020bi,
                             sabrelite_set_xz_tusb4020bi);
    object_property_set_description(obj, "xz-tusb4020bi",
        "Attach the XDJ-XZ TUSB4020BI SMBus configuration target to I2C1. "
        "The separately requested xdj-tusb4020bi USB device provides its "
        "upstream/downstream USB topology.");

    object_property_add_bool(obj, "xz-rtl8309m",
                             sabrelite_get_xz_rtl8309m,
                             sabrelite_set_xz_rtl8309m);
    object_property_set_description(obj, "xz-rtl8309m",
        "Enable the XDJ-XZ RTL8309M paged MDIO register-bank model behind "
        "the i.MX6 FEC. PHY6 remains the FEC link PHY; the model handles "
        "the original Pioneer PHY8/page-selector switch management path.");
    object_property_add_bool(obj, "xz-rtl8309m-trace",
                             sabrelite_get_xz_rtl8309m_trace,
                             sabrelite_set_xz_rtl8309m_trace);
    object_property_set_description(obj, "xz-rtl8309m-trace",
        "Emit compact guest MDIO transactions from the opt-in RTL8309M model.");

    object_property_add_link(obj, "canbus0", TYPE_CAN_BUS,
                             (Object **)&s->canbus[0],
                             object_property_allow_set_link,
                             0);

    object_property_add_link(obj, "canbus1", TYPE_CAN_BUS,
                             (Object **)&s->canbus[1],
                             object_property_allow_set_link,
                             0);
}

static void sabrelite_machine_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Freescale i.MX6 Quad SABRE Lite Board (Cortex-A9)";
    mc->init = sabrelite_init;
    mc->reset = sabrelite_reset;
    mc->max_cpus = FSL_IMX6_NUM_CPUS;
    mc->ignore_memory_transaction_failures = true;
    mc->default_ram_id = "sabrelite.ram";
    mc->auto_create_sdcard = true;
}

static const TypeInfo sabrelite_machine_init_typeinfo = {
    .name          = TYPE_SABRELITE_MACHINE,
    .parent        = TYPE_MACHINE,
    .class_init    = sabrelite_machine_class_init,
    .instance_init = sabrelite_machine_instance_init,
    .instance_size = sizeof(SabreliteMachineState),
    .abstract      = false,
    .interfaces    = arm_machine_interfaces,
};

static void sabrelite_machine_init_register_types(void)
{
    type_register_static(&sabrelite_machine_init_typeinfo);
}

type_init(sabrelite_machine_init_register_types)
