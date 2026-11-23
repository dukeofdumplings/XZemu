/*
 * Minimal RTL8309M MDIO register-bank model for the Pioneer XDJ-XZ.
 *
 * The Pioneer Linux board file configures the external RMII MAC through
 * rtl8309n_reg_set(bus, 3, 16, 17, 0x000b) and
 * rtl8309n_reg_set(bus, 8, 16, 0, 0x000f).  The GPL rtl8309n driver selects
 * PHY pages by writing PHY8/reg31 bit 15, then selects a MAC page with the
 * lower eight bits of that same selector.  RBP uses the same real MII ioctl
 * transport.  This model preserves that protocol; it is not a generic
 * Realtek switch or a packet-forwarding implementation.
 *
 * TODO(PDJ bring-up): add only guest-traced RTL8309N/LanHub management
 * operations required by original RBP after the basic switch-bank access.
 * In particular, do not fabricate rtk_l2_addr_get() success or forwarding
 * state merely to suppress application error messages.
 */
#include "qemu/osdep.h"
#include "hw/net/imx_fec.h"
#include "hw/net/imx_rtl8309m.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define MII_BMCR 0
#define MII_BMSR 1
#define MII_ADVERTISE 4

#define BMSR_LINK_STATUS (1U << 2)
#define BMSR_ANEGCAPABLE (1U << 3)
#define BMSR_ANEGCOMPLETE (1U << 5)
#define BMSR_10HALF      (1U << 11)
#define BMSR_10FULL      (1U << 12)
#define BMSR_100HALF     (1U << 13)
#define BMSR_100FULL     (1U << 14)

/* MAC page 15 / address 6 LUT access register contract. */
#define RTL8309M_LUT_PHY              6
#define RTL8309M_LUT_CONTROL_REG      16
#define RTL8309M_LUT_COMMAND_REG      18
#define RTL8309M_LUT_DATA_HI_REG      19
#define RTL8309M_LUT_DATA_MIDHI_REG   20
#define RTL8309M_LUT_DATA_MIDLO_REG   21
#define RTL8309M_LUT_DATA_LO_REG      22
#define RTL8309M_LUT_ENABLE            (1U << 0)
#define RTL8309M_LUT_READ              (1U << 12)
#define RTL8309M_LUT_BUSY              (1U << 11)

#define PHY_REG(_s, _page, _phy, _reg) \
    ((_s)->phy_regs[(_page)][(_phy) * IMX_RTL8309M_REGS + (_reg)])
#define MAC_REG(_s, _page, _phy, _reg) \
    ((_s)->mac_regs[(_page)][(_phy) * IMX_RTL8309M_REGS + (_reg)])

static void imx_rtl8309m_apply_link(IMXRTL8309MState *s);

/*
 * rtl8309n_l2_MacToIdx_get(), copied structurally from the Pioneer GPL
 * driver.  The hardware has four entries for each nine-bit hash index.
 */
static unsigned imx_rtl8309m_lut_index(const IMXRTL8309MState *s,
                                       const uint8_t mac[6], unsigned fid)
{
    unsigned index = 0;

    if (MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_CONTROL_REG) &
        (1U << 1)) {
        /* Pioneer driver's hash algorithm 2. */
        index |= (fid & 1) ^ (mac[4] & 1) ^ (mac[3] & 1) ^
                 (mac[2] & 1) ^ (mac[1] & 1) ^ ((mac[1] >> 7) & 1);
        index |= ((mac[5] & 1) ^ ((mac[4] >> 1) & 1) ^
                  ((mac[3] >> 1) & 1) ^ ((mac[2] >> 1) & 1) ^
                  ((mac[1] >> 1) & 1) ^ (mac[0] & 1)) << 1;
        index |= (((mac[5] >> 1) & 1) ^ ((mac[4] >> 2) & 1) ^
                  ((mac[3] >> 2) & 1) ^ ((mac[2] >> 2) & 1) ^
                  ((mac[1] >> 2) & 1) ^ ((mac[0] >> 1) & 1)) << 2;
        index |= (((mac[5] >> 2) & 1) ^ ((mac[4] >> 3) & 1) ^
                  ((mac[3] >> 3) & 1) ^ ((mac[2] >> 3) & 1) ^
                  ((mac[1] >> 3) & 1) ^ ((mac[0] >> 2) & 1)) << 3;
        index |= (((mac[5] >> 3) & 1) ^ ((mac[3] >> 4) & 1) ^
                  ((mac[2] >> 4) & 1) ^ ((mac[1] >> 4) & 1) ^
                  ((mac[0] >> 3) & 1)) << 4;
        index |= (((mac[5] >> 4) & 1) ^ ((mac[4] >> 4) & 1) ^
                  ((mac[2] >> 5) & 1) ^ ((mac[1] >> 5) & 1) ^
                  ((mac[0] >> 4) & 1)) << 5;
        index |= (((mac[5] >> 5) & 1) ^ ((mac[4] >> 5) & 1) ^
                  ((mac[3] >> 5) & 1) ^ ((mac[1] >> 6) & 1) ^
                  ((mac[0] >> 5) & 1)) << 6;
        index |= (((mac[5] >> 6) & 1) ^ ((mac[4] >> 6) & 1) ^
                  ((mac[3] >> 6) & 1) ^ ((mac[2] >> 6) & 1) ^
                  ((mac[0] >> 6) & 1)) << 7;
        index |= (((fid >> 1) & 1) ^ ((mac[5] >> 7) & 1) ^
                  ((mac[1] >> 7) & 1) ^ ((mac[3] >> 7) & 1) ^
                  ((mac[2] >> 7) & 1) ^ ((mac[0] >> 7) & 1)) << 8;
    } else {
        /* Pioneer driver's default hash algorithm 1. */
        index |= ((mac[5] >> 3) ^ (mac[4] >> 4) ^ (mac[3] >> 5) ^
                  (mac[2] >> 6) ^ (mac[1] >> 7)) & 1;
        index |= (((mac[5] >> 4) ^ (mac[4] >> 5) ^ (mac[3] >> 6) ^
                   (mac[2] >> 7) ^ mac[0]) & 1) << 1;
        index |= (((mac[5] >> 5) ^ (mac[4] >> 6) ^ (mac[3] >> 7) ^
                   mac[1] ^ (mac[0] >> 1)) & 1) << 2;
        index |= (((mac[5] >> 6) ^ (mac[4] >> 7) ^ mac[2] ^
                   (mac[1] >> 1) ^ (mac[0] >> 2)) & 1) << 3;
        index |= ((((fid >> 1) & 1) ^ (mac[5] >> 7) ^ mac[3] ^
                   (mac[2] >> 1) ^ (mac[1] >> 2) ^ (mac[0] >> 3)) & 1) << 4;
        index |= (((fid & 1) ^ mac[4] ^ (mac[3] >> 1) ^ (mac[2] >> 2) ^
                   (mac[1] >> 3) ^ (mac[0] >> 4)) & 1) << 5;
        index |= ((mac[5] ^ (mac[4] >> 1) ^ (mac[3] >> 2) ^
                   (mac[2] >> 3) ^ (mac[1] >> 4) ^ (mac[0] >> 5)) & 1) << 6;
        index |= (((mac[5] >> 1) ^ (mac[4] >> 2) ^ (mac[3] >> 3) ^
                   (mac[2] >> 4) ^ (mac[1] >> 5) ^ (mac[0] >> 6)) & 1) << 7;
        index |= (((mac[5] >> 2) ^ (mac[4] >> 3) ^ (mac[3] >> 4) ^
                   (mac[2] >> 5) ^ (mac[1] >> 6) ^ (mac[0] >> 7)) & 1) << 8;
    }

    return index;
}

static void imx_rtl8309m_lut_read(IMXRTL8309MState *s, unsigned entry)
{
    uint16_t high = 0;

    entry &= IMX_RTL8309M_LUT_ENTRIES - 1;
    if (s->lut_valid[entry]) {
        high = (s->lut_age[entry] & 0x3) << 6;
        high |= (s->lut_port[entry] & 0xf) << 2;
        high |= s->lut_fid[entry] & 0x3;
        MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_DATA_MIDHI_REG) =
            ((uint16_t)s->lut_mac[entry][5] << 8) | s->lut_mac[entry][4];
        MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_DATA_MIDLO_REG) =
            ((uint16_t)s->lut_mac[entry][3] << 8) | s->lut_mac[entry][2];
        MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_DATA_LO_REG) =
            ((uint16_t)s->lut_mac[entry][1] << 8) | s->lut_mac[entry][0];
    } else {
        MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_DATA_MIDHI_REG) = 0;
        MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_DATA_MIDLO_REG) = 0;
        MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_DATA_LO_REG) = 0;
    }
    MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_DATA_HI_REG) = high;
}

static int imx_rtl8309m_lut_lookup(const IMXRTL8309MState *s,
                                    const uint8_t mac[6])
{
    unsigned index = imx_rtl8309m_lut_index(s, mac, 0);
    unsigned way;

    for (way = 0; way < IMX_RTL8309M_LUT_WAYS; way++) {
        unsigned entry = (way << 9) | index;

        if (s->lut_valid[entry] && !memcmp(s->lut_mac[entry], mac, 6)) {
            return s->lut_port[entry];
        }
    }
    return -1;
}

void imx_rtl8309m_learn_frame(IMXRTL8309MState *s, const uint8_t *frame,
                              size_t length, unsigned port)
{
    const uint8_t *mac;
    unsigned index;
    unsigned way;
    unsigned free_entry = IMX_RTL8309M_LUT_ENTRIES;

    if (length < 14 || port >= IMX_RTL8309M_PORTS) {
        return;
    }

    mac = frame + 6; /* Ethernet source address. */
    if (mac[0] & 1) {
        return;
    }

    index = imx_rtl8309m_lut_index(s, mac, 0);
    for (way = 0; way < IMX_RTL8309M_LUT_WAYS; way++) {
        unsigned entry = (way << 9) | index;

        if (s->lut_valid[entry] && !memcmp(s->lut_mac[entry], mac, 6)) {
            free_entry = entry;
            break;
        }
        if (!s->lut_valid[entry] && free_entry == IMX_RTL8309M_LUT_ENTRIES) {
            free_entry = entry;
        }
    }

    if (free_entry == IMX_RTL8309M_LUT_ENTRIES) {
        /* The four-way set is full: preserve its existing learned state. */
        return;
    }

    memcpy(s->lut_mac[free_entry], mac, 6);
    s->lut_valid[free_entry] = 1;
    /* RTL8309N_LUT_AGE100: valid dynamic entry, not a fabricated static one. */
    s->lut_age[free_entry] = 1;
    s->lut_port[free_entry] = port;
    s->lut_fid[free_entry] = 0;
}

static bool imx_rtl8309m_external_port_up(const IMXRTL8309MState *s,
                                           unsigned port)
{
    return port >= IMX_RTL8309M_FIRST_EXTERNAL_PORT &&
        port < IMX_RTL8309M_FIRST_EXTERNAL_PORT +
        IMX_RTL8309M_EXTERNAL_PORTS &&
        (s->external_link_mask &
         (1U << (port - IMX_RTL8309M_FIRST_EXTERNAL_PORT)));
}

static void imx_rtl8309m_send_external(IMXRTL8309MState *s,
                                        unsigned port, const uint8_t *frame,
                                        size_t length)
{
    unsigned index;
    NetClientState *nc;

    if (!imx_rtl8309m_external_port_up(s, port)) {
        return;
    }
    index = port - IMX_RTL8309M_FIRST_EXTERNAL_PORT;
    if (!s->external_nic[index]) {
        return;
    }
    nc = qemu_get_queue(s->external_nic[index]);
    if (qemu_can_send_packet(nc)) {
        qemu_send_packet(nc, frame, length);
    }
}

static void imx_rtl8309m_forward_external(IMXRTL8309MState *s,
                                           unsigned ingress,
                                           const uint8_t *frame,
                                           size_t length)
{
    int destination = -1;
    unsigned port;

    if (length >= 6 && !(frame[0] & 1)) {
        destination = imx_rtl8309m_lut_lookup(s, frame);
    }

    if (destination >= IMX_RTL8309M_FIRST_EXTERNAL_PORT &&
        destination < IMX_RTL8309M_FIRST_EXTERNAL_PORT +
        IMX_RTL8309M_EXTERNAL_PORTS) {
        if ((unsigned)destination != ingress) {
            imx_rtl8309m_send_external(s, destination, frame, length);
        }
        return;
    }

    /* Unknown, multicast, and broadcast frames are flooded to live ports. */
    for (port = IMX_RTL8309M_FIRST_EXTERNAL_PORT;
         port < IMX_RTL8309M_FIRST_EXTERNAL_PORT +
         IMX_RTL8309M_EXTERNAL_PORTS; port++) {
        if (port != ingress) {
            imx_rtl8309m_send_external(s, port, frame, length);
        }
    }
}

static unsigned imx_rtl8309m_external_port_number(IMXRTL8309MState *s,
                                                   NetClientState *nc)
{
    unsigned index;

    for (index = 0; index < IMX_RTL8309M_EXTERNAL_PORTS; index++) {
        if (s->external_nic[index] &&
            qemu_get_queue(s->external_nic[index]) == nc) {
            return index + IMX_RTL8309M_FIRST_EXTERNAL_PORT;
        }
    }
    return IMX_RTL8309M_PORTS;
}

static ssize_t imx_rtl8309m_external_receive(NetClientState *nc,
                                              const uint8_t *frame,
                                              size_t length)
{
    IMXRTL8309MState *s = qemu_get_nic_opaque(nc);
    unsigned port = imx_rtl8309m_external_port_number(s, nc);
    int destination = -1;

    if (length < 14 || !imx_rtl8309m_external_port_up(s, port)) {
        return -1;
    }

    imx_rtl8309m_learn_frame(s, frame, length, port);
    if (!(frame[0] & 1)) {
        destination = imx_rtl8309m_lut_lookup(s, frame);
    }

    if (destination < 0 || destination == IMX_RTL8309M_HOST_PORT) {
        if (s->fec && imx_fec_can_receive_from_switch(s->fec)) {
            imx_fec_receive_from_switch(s->fec, frame, length);
        }
    }
    imx_rtl8309m_forward_external(s, port, frame, length);
    return length;
}

static void imx_rtl8309m_external_set_link(NetClientState *nc)
{
    IMXRTL8309MState *s = qemu_get_nic_opaque(nc);
    unsigned port = imx_rtl8309m_external_port_number(s, nc);

    /* The legacy FEC link callback represents the port-1 default backend. */
    if (port == IMX_RTL8309M_FIRST_EXTERNAL_PORT) {
        imx_rtl8309m_set_link(s, !nc->link_down);
    }
}

static NetClientInfo imx_rtl8309m_external_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = imx_rtl8309m_external_receive,
    .link_status_changed = imx_rtl8309m_external_set_link,
};

void imx_rtl8309m_attach_fec(IMXRTL8309MState *s, IMXFECState *fec)
{
    unsigned index;

    s->fec = fec;
    for (index = 0; index < IMX_RTL8309M_EXTERNAL_PORTS; index++) {
        NICInfo *nd = qemu_find_nic_info(TYPE_IMX_RTL8309M,
                                         index == 0, NULL);
        NICConf *conf = &s->external_conf[index];

        if (nd) {
            memcpy(&conf->macaddr, &nd->macaddr, sizeof(conf->macaddr));
            conf->peers.ncs[0] = nd->netdev;
            conf->peers.queues = 1;
            nd->instantiated = 1;
        } else {
            qemu_macaddr_default_if_unset(&conf->macaddr);
        }
        s->external_nic[index] = qemu_new_nic(
            &imx_rtl8309m_external_net_info, conf, TYPE_IMX_RTL8309M,
            NULL, &DEVICE(s)->mem_reentrancy_guard, s);
        qemu_format_nic_info_str(qemu_get_queue(s->external_nic[index]),
                                 conf->macaddr.a);
    }
    imx_rtl8309m_apply_link(s);
}

void imx_rtl8309m_receive_from_host(IMXRTL8309MState *s,
                                    const uint8_t *frame, size_t length)
{
    if (length < 14) {
        return;
    }
    imx_rtl8309m_learn_frame(s, frame, length, IMX_RTL8309M_HOST_PORT);
    imx_rtl8309m_forward_external(s, IMX_RTL8309M_HOST_PORT, frame, length);
}

static void imx_rtl8309m_trace(const IMXRTL8309MState *s, const char *op,
                                unsigned phy, unsigned reg, uint16_t value)
{
    unsigned page;

    if (!s->trace_enable) {
        return;
    }

    /* PHY8/reg31 is the global bank selector, not a port-page entry. */
    page = s->phy_register_page && phy < IMX_RTL8309M_PHYS
           ? s->phy_page[phy] : s->mac_page;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "[imx-rtl8309m] %s bank=%s page=%u phy=%u reg=%u value=0x%04x\n",
                  op, s->phy_register_page ? "phy" : "mac",
                  page,
                  phy, reg, value);
}

static void imx_rtl8309m_apply_link(IMXRTL8309MState *s)
{
    unsigned phy;

    for (phy = 0; phy < IMX_RTL8309M_PHYS; phy++) {
        bool external_port = phy >= IMX_RTL8309M_FIRST_EXTERNAL_PORT &&
            phy < IMX_RTL8309M_FIRST_EXTERNAL_PORT +
                  IMX_RTL8309M_EXTERNAL_PORTS;
        bool port_up = external_port &&
            (s->external_link_mask &
             (1U << (phy - IMX_RTL8309M_FIRST_EXTERNAL_PORT)));

        /* Port 1 is the only port currently attached to QEMU's net backend. */
        if (phy == IMX_RTL8309M_FIRST_EXTERNAL_PORT) {
            port_up = port_up && s->link_up;
        }

        if (port_up) {
            PHY_REG(s, 0, phy, MII_BMSR) |= BMSR_LINK_STATUS |
                BMSR_ANEGCAPABLE | BMSR_ANEGCOMPLETE |
                BMSR_10HALF | BMSR_10FULL | BMSR_100HALF | BMSR_100FULL;
        } else {
            PHY_REG(s, 0, phy, MII_BMSR) &=
                ~(BMSR_LINK_STATUS | BMSR_ANEGCOMPLETE);
        }
    }
}

void imx_rtl8309m_set_link(IMXRTL8309MState *s, bool link_up)
{
    s->link_up = link_up;
    imx_rtl8309m_apply_link(s);
}

bool imx_rtl8309m_dataplane_enabled(const IMXRTL8309MState *s)
{
    return s->dataplane_enable;
}

/*
 * PHY6 remains the existing FEC PHY when the RTL selector is in PHY mode.
 * In MAC mode PHY6 is a valid RTL8309M switch-register address, as required
 * by the original page/register ABI.
 */
bool imx_rtl8309m_handles(const IMXRTL8309MState *s, unsigned phy,
                          unsigned reg)
{
    if (phy >= IMX_RTL8309M_PORTS) {
        return false;
    }

    /*
     * Before the exact selector write, leave Linux's standard FEC PHY scan
     * completely untouched: only the selector itself belongs to this model.
     * The original board code makes that PHY8/reg31 write before using any
     * RTL8309N register.  This avoids falsely advertising a zero-ID PHY.
     */
    if (!s->active) {
        return phy == 8 && reg == 31;
    }

    if (!s->phy_register_page) {
        return true;
    }

    return phy != 6;
}

uint16_t imx_rtl8309m_mdio_read(IMXRTL8309MState *s, unsigned phy,
                                unsigned reg)
{
    uint16_t value;

    reg &= IMX_RTL8309M_REGS - 1;

    if (phy >= IMX_RTL8309M_PORTS) {
        return UINT16_MAX;
    }

    if (phy == 8 && reg == 31) {
        value = s->phy_register_page ? 0x8000 : s->mac_page;
        imx_rtl8309m_trace(s, "read", phy, reg, value);
        return value;
    }

    if (s->phy_register_page) {
        if (phy >= IMX_RTL8309M_PHYS) {
            return UINT16_MAX;
        }
        if (reg == 31) {
            value = 0x8000 | s->phy_page[phy];
        } else {
            value = PHY_REG(s, s->phy_page[phy], phy, reg);
        }
    } else {
        value = MAC_REG(s, s->mac_page, phy, reg);
    }

    imx_rtl8309m_trace(s, "read", phy, reg, value);
    return value;
}

void imx_rtl8309m_mdio_write(IMXRTL8309MState *s, unsigned phy,
                             unsigned reg, uint16_t value)
{
    reg &= IMX_RTL8309M_REGS - 1;

    if (phy >= IMX_RTL8309M_PORTS) {
        return;
    }

    if (phy == 8 && reg == 31) {
        s->active = true;
        s->phy_register_page = (value & 0x8000) != 0;
        if (!s->phy_register_page) {
            s->mac_page = value & 0xff;
            if (s->mac_page >= IMX_RTL8309M_PAGES) {
                s->mac_page = 0;
            }
        }
        imx_rtl8309m_trace(s, "write", phy, reg, value);
        return;
    }

    if (s->phy_register_page) {
        if (phy >= IMX_RTL8309M_PHYS) {
            return;
        }
        if (reg == 31) {
            s->phy_page[phy] = value & 0xff;
            if (s->phy_page[phy] >= IMX_RTL8309M_PAGES) {
                s->phy_page[phy] = 0;
            }
            imx_rtl8309m_trace(s, "write", phy, reg, value);
            return;
        }
        PHY_REG(s, s->phy_page[phy], phy, reg) = value;
        imx_rtl8309m_trace(s, "write", phy, reg, value);
        return;
    }

    MAC_REG(s, s->mac_page, phy, reg) = value;

    /*
     * rtl8309n_l2_ucastEntry_get() uses MAC page 15/PHY6 as a small LUT
     * command engine.  A read command completes synchronously here: QEMU
     * needs no fake delay, but must clear the driver's busy bit and provide
     * the requested entry data before it polls.
     */
    if (s->mac_page == 15 && phy == RTL8309M_LUT_PHY &&
        reg == RTL8309M_LUT_COMMAND_REG &&
        (value & (RTL8309M_LUT_READ | RTL8309M_LUT_BUSY)) ==
        (RTL8309M_LUT_READ | RTL8309M_LUT_BUSY) &&
        (MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_CONTROL_REG) &
         RTL8309M_LUT_ENABLE)) {
        imx_rtl8309m_lut_read(s, value & 0x7ff);
        MAC_REG(s, 15, RTL8309M_LUT_PHY, RTL8309M_LUT_COMMAND_REG) =
            value & ~RTL8309M_LUT_BUSY;
    }
    imx_rtl8309m_trace(s, "write", phy, reg, value);
}

static void imx_rtl8309m_reset(DeviceState *dev)
{
    IMXRTL8309MState *s = IMX_RTL8309M(dev);
    unsigned phy;

    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    memset(s->mac_regs, 0, sizeof(s->mac_regs));
    memset(s->phy_page, 0, sizeof(s->phy_page));
    memset(s->lut_mac, 0, sizeof(s->lut_mac));
    memset(s->lut_valid, 0, sizeof(s->lut_valid));
    memset(s->lut_age, 0, sizeof(s->lut_age));
    memset(s->lut_port, 0, sizeof(s->lut_port));
    memset(s->lut_fid, 0, sizeof(s->lut_fid));
    s->mac_page = 0;
    s->phy_register_page = false;
    s->active = false;

    for (phy = 0; phy < IMX_RTL8309M_PHYS; phy++) {
        /* Conventional autonegotiating 100BASE-TX full-duplex defaults. */
        PHY_REG(s, 0, phy, MII_BMCR) = 0x1140;
        PHY_REG(s, 0, phy, MII_BMSR) = 0x7869;
        PHY_REG(s, 0, phy, MII_ADVERTISE) = 0x01e1;
    }
    imx_rtl8309m_apply_link(s);
}

static const VMStateDescription vmstate_imx_rtl8309m = {
    .name = TYPE_IMX_RTL8309M,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_2DARRAY(phy_regs, IMXRTL8309MState,
                               IMX_RTL8309M_PAGES,
                               IMX_RTL8309M_PHYS * IMX_RTL8309M_REGS),
        VMSTATE_UINT16_2DARRAY(mac_regs, IMXRTL8309MState,
                               IMX_RTL8309M_PAGES,
                               IMX_RTL8309M_PORTS * IMX_RTL8309M_REGS),
        VMSTATE_UINT8_ARRAY(phy_page, IMXRTL8309MState,
                            IMX_RTL8309M_PHYS),
        VMSTATE_UINT8(mac_page, IMXRTL8309MState),
        VMSTATE_BOOL(phy_register_page, IMXRTL8309MState),
        VMSTATE_BOOL(active, IMXRTL8309MState),
        VMSTATE_BOOL(link_up, IMXRTL8309MState),
        VMSTATE_UINT8(external_link_mask, IMXRTL8309MState),
        VMSTATE_UINT8_2DARRAY(lut_mac, IMXRTL8309MState,
                              IMX_RTL8309M_LUT_ENTRIES, 6),
        VMSTATE_UINT8_ARRAY(lut_valid, IMXRTL8309MState,
                            IMX_RTL8309M_LUT_ENTRIES),
        VMSTATE_UINT8_ARRAY(lut_age, IMXRTL8309MState,
                            IMX_RTL8309M_LUT_ENTRIES),
        VMSTATE_UINT8_ARRAY(lut_port, IMXRTL8309MState,
                            IMX_RTL8309M_LUT_ENTRIES),
        VMSTATE_UINT8_ARRAY(lut_fid, IMXRTL8309MState,
                            IMX_RTL8309M_LUT_ENTRIES),
        VMSTATE_END_OF_LIST()
    }
};

static void imx_rtl8309m_init(Object *obj)
{
    IMXRTL8309MState *s = IMX_RTL8309M(obj);

    /* A QEMU NIC is link-up unless its backend explicitly says otherwise. */
    s->link_up = true;
    s->external_link_mask = 1;
}

static const Property imx_rtl8309m_properties[] = {
    /*
     * Three independently visible logical external links.  This is topology
     * only: no physical rear-panel labels or peer protocol are inferred.
     */
    DEFINE_PROP_UINT8("external-link-mask", IMXRTL8309MState,
                      external_link_mask, 1),
    DEFINE_PROP_BOOL("dataplane-enable", IMXRTL8309MState,
                     dataplane_enable, false),
    DEFINE_PROP_BOOL("trace-enable", IMXRTL8309MState, trace_enable, false),
};

static void imx_rtl8309m_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_imx_rtl8309m;
    device_class_set_legacy_reset(dc, imx_rtl8309m_reset);
    device_class_set_props(dc, imx_rtl8309m_properties);
    dc->desc = "XDJ-XZ RTL8309M MDIO register-bank model";
    dc->user_creatable = false;
}

static const TypeInfo imx_rtl8309m_info = {
    .name = TYPE_IMX_RTL8309M,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(IMXRTL8309MState),
    .instance_init = imx_rtl8309m_init,
    .class_init = imx_rtl8309m_class_init,
};

static void imx_rtl8309m_register_types(void)
{
    type_register_static(&imx_rtl8309m_info);
}

type_init(imx_rtl8309m_register_types)
