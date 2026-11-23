/*
 * Minimal RTL8309M MDIO register-bank model for the XDJ-XZ FEC path.
 *
 * The device has no guest MMIO window.  It sits behind the i.MX6 FEC MDIO
 * controller and implements only the paged register ownership exercised by
 * the original Pioneer board code and RBP's rtl8309n library.
 */
#ifndef HW_NET_IMX_RTL8309M_H
#define HW_NET_IMX_RTL8309M_H

#include "hw/core/qdev.h"
#include "qom/object.h"
#include "net/net.h"

typedef struct IMXFECState IMXFECState;

#define TYPE_IMX_RTL8309M "imx-rtl8309m"
OBJECT_DECLARE_SIMPLE_TYPE(IMXRTL8309MState, IMX_RTL8309M)

#define IMX_RTL8309M_PAGES 19
#define IMX_RTL8309M_PORTS 9
#define IMX_RTL8309M_PHYS 8
#define IMX_RTL8309M_REGS 32
/* The RTL8309N LUT is four-way with a nine-bit hash index. */
#define IMX_RTL8309M_LUT_ENTRIES 2048
#define IMX_RTL8309M_LUT_WAYS 4
#define IMX_RTL8309M_HOST_PORT 0
#define IMX_RTL8309M_FIRST_EXTERNAL_PORT 1
/*
 * The XDJ-XZ exposes three external LINK jacks.  The original sources do
 * not establish their rear-panel-label-to-RTL-port mapping, so these remain
 * deliberately anonymous logical ports 1..3 until a physical observation
 * proves that mapping.
 */
#define IMX_RTL8309M_EXTERNAL_PORTS 3

struct IMXRTL8309MState {
    DeviceState parent_obj;

    /* Page 0..18, PHY 0..7, MII register 0..31. */
    uint16_t phy_regs[IMX_RTL8309M_PAGES]
                     [IMX_RTL8309M_PHYS * IMX_RTL8309M_REGS];
    /* Page 0..18, switch register PHY/address 0..8, register 0..31. */
    uint16_t mac_regs[IMX_RTL8309M_PAGES]
                     [IMX_RTL8309M_PORTS * IMX_RTL8309M_REGS];
    uint8_t phy_page[IMX_RTL8309M_PHYS];
    uint8_t mac_page;
    bool phy_register_page;
    bool active;
    /* FEC's QEMU backend controls logical external port 1. */
    bool link_up;
    /* One bit per logical external port 1..3; default: port 1 only. */
    uint8_t external_link_mask;
    bool dataplane_enable;
    bool trace_enable;

    /* QEMU network frontends for the three logical external LINK ports. */
    NICConf external_conf[IMX_RTL8309M_EXTERNAL_PORTS];
    NICState *external_nic[IMX_RTL8309M_EXTERNAL_PORTS];
    IMXFECState *fec;

    /*
     * Source-MAC learning state.  This is intentionally a small hardware
     * FDB, not a Pro DJ Link peer database: the latter remains guest-owned.
     */
    uint8_t lut_mac[IMX_RTL8309M_LUT_ENTRIES][6];
    uint8_t lut_valid[IMX_RTL8309M_LUT_ENTRIES];
    uint8_t lut_age[IMX_RTL8309M_LUT_ENTRIES];
    uint8_t lut_port[IMX_RTL8309M_LUT_ENTRIES];
    uint8_t lut_fid[IMX_RTL8309M_LUT_ENTRIES];
};

/* Called by the FEC MDIO mux. */
bool imx_rtl8309m_handles(const IMXRTL8309MState *s, unsigned phy,
                          unsigned reg);
uint16_t imx_rtl8309m_mdio_read(IMXRTL8309MState *s, unsigned phy,
                                unsigned reg);
void imx_rtl8309m_mdio_write(IMXRTL8309MState *s, unsigned phy,
                             unsigned reg, uint16_t value);
void imx_rtl8309m_set_link(IMXRTL8309MState *s, bool link_up);
bool imx_rtl8309m_dataplane_enabled(const IMXRTL8309MState *s);

/* Learn a source address as it enters the switch through a modeled port. */
void imx_rtl8309m_learn_frame(IMXRTL8309MState *s, const uint8_t *frame,
                              size_t length, unsigned port);

/* Called after the guest FEC has been realized. */
void imx_rtl8309m_attach_fec(IMXRTL8309MState *s, IMXFECState *fec);
/* Guest FEC TX enters the switch at the CPU/host port. */
void imx_rtl8309m_receive_from_host(IMXRTL8309MState *s,
                                    const uint8_t *frame, size_t length);

#endif /* HW_NET_IMX_RTL8309M_H */
