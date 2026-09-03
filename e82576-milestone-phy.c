/*
 * e82576.c
 *
 * Minimal Intel 82576 Ethernet driver
 *
 * Current milestone:
 *   - PCI device initialization
 *   - BAR0 MMIO
 *   - Hardware reset
 *   - Proper 82575/82576 NVM synchronization
 *   - EEPROM/NVM MAC address
 *   - PHY discovery through MDIC
 *   - PHY link/speed/duplex detection
 *   - One-vector MSI-X
 *   - Link-status-change interrupt
 *   - Linux net_device registration
 *   - ndo_open()
 *   - ndo_stop()
 *
 * NOT IMPLEMENTED YET:
 *   - RX DMA
 *   - TX DMA
 *   - RX/TX descriptor rings
 *   - NAPI
 *   - Packet transmission/reception
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/ethtool.h>
#include <linux/mii.h>

#define DRIVER_NAME         "e82576"
#define DRIVER_VERSION      "0.8"

#define INTEL_VENDOR_ID     0x8086
#define INTEL_82576_DEVICE  0x10c9


/*
 * ============================================================
 * 82576 REGISTERS
 * ============================================================
 */

#define E1000_CTRL           0x00000
#define E1000_STATUS         0x00008

#define E1000_CTRL_RST       BIT(26)
#define E1000_CTRL_PHY_RST   BIT(31)


/*
 * EEPROM / NVM
 */

#define E1000_EERD           0x00014

#define E1000_EERD_START     BIT(0)
#define E1000_EERD_DONE      BIT(1)

#define E1000_EERD_ADDR_SHIFT 2
#define E1000_EERD_DATA_SHIFT 16


/*
 * MDIC
 */

#define E1000_MDIC           0x00020

#define E1000_MDIC_DATA_MASK  0x0000ffff

#define E1000_MDIC_REG_SHIFT  16
#define E1000_MDIC_PHY_SHIFT  21

#define E1000_MDIC_OP_WRITE   (1U << 26)
#define E1000_MDIC_OP_READ    (2U << 26)

#define E1000_CTRL_SLU      BIT(6)
#define E1000_CTRL_FRCSPD   BIT(11)
#define E1000_CTRL_FRCDPX   BIT(12)
#define E1000_MDIC_READY      BIT(28)
#define E1000_MDIC_INT_EN     BIT(29)
#define E1000_MDIC_ERROR      BIT(30)


/*
 * Legacy interrupt registers.
 */

#define E1000_IMC            0x000D8


/*
 * Extended interrupt registers.
 */

#define E1000_EIMS           0x01524
#define E1000_EIMC           0x01528
#define E1000_EICR           0x01580


/*
 * Link Status Change.
 */

#define E1000_IMS_LSC        BIT(2)


/*
 * ============================================================
 * NVM / SOFTWARE-FIRMWARE SEMAPHORE REGISTERS
 * ============================================================
 *
 * These are important on 82575/82576.
 *
 * The upstream igb driver uses:
 *
 *     SWSM
 *     SW_FW_SYNC
 *
 * Hardware semaphore:
 *
 *     SMBI
 *     SWESMBI
 *
 * Software/firmware resource semaphore:
 *
 *     SW mask  = low 16 bits
 *     FW mask  = high 16 bits
 *
 * For EEPROM access we use EEP_SM.
 */

#define E1000_SWSM              0x05B50

#define E1000_SWSM_SMBI        BIT(0)
#define E1000_SWSM_SWESMBI     BIT(1)

#define E1000_SW_FW_SYNC       0x05B5C

#define E1000_SWFW_PHY0_SM     BIT(0)
#define E1000_SWFW_PHY1_SM     BIT(2)
#define E1000_SWFW_PHY2_SM     BIT(4)
#define E1000_SWFW_PHY3_SM     BIT(6)

#define E1000_SWFW_EEP_SM    BIT(0)


/*
 * PHY registers.
 */

#define PHY_REG_BMCR        0x00
#define PHY_REG_BMSR        0x01
#define PHY_REG_ID1         0x02
#define PHY_REG_ID2         0x03
#define PHY_REG_ANAR        0x04
#define PHY_REG_ANLPAR      0x05
#define PHY_REG_1000_CTRL   0x09
#define PHY_REG_1000_STAT   0x0A

#define PHY_BMCR              MII_BMCR
#define PHY_BMSR              MII_BMSR
#define PHY_ANAR              MII_ADVERTISE
#define PHY_ANLPAR            MII_LPA
#define PHY_CTRL1000          MII_CTRL1000
#define PHY_STAT1000          MII_STAT1000


/*
 * BMCR.
 */

#define PHY_BMCR_RESET        BMCR_RESET
#define PHY_BMCR_AN_ENABLE    BMCR_ANENABLE
#define PHY_BMCR_AN_RESTART   BMCR_ANRESTART
#define PHY_BMCR_SPEED100     BMCR_SPEED100
#define PHY_BMCR_FULLDPLX     BMCR_FULLDPLX


/*
 * BMSR.
 */

#define PHY_BMSR_LINK         BMSR_LSTATUS
#define PHY_BMSR_AN_COMPLETE  BMSR_ANEGCOMPLETE


/*
 * Auto-negotiation.
 */

#define PHY_ADVERTISE_10FULL      ADVERTISE_10FULL
#define PHY_ADVERTISE_100FULL     ADVERTISE_100FULL

#define PHY_CTRL1000_1000FULL     ADVERTISE_1000FULL

#define PHY_LPA_10FULL            LPA_10FULL
#define PHY_LPA_100FULL           LPA_100FULL

#define PHY_STAT1000_LPA_1000FULL LPA_1000FULL


/*
 * ============================================================
 * DEVICE STRUCTURE
 * ============================================================
 */

struct e82576_device {

    struct pci_dev *pdev;
    struct net_device *netdev;
    void __iomem *hw_addr;
    resource_size_t bar0_start;
    resource_size_t bar0_length;

    /* MAC */
    u8 mac_address[ETH_ALEN];

    /* PHY */
    u8 phy_address;
    u16 phy_id1;
    u16 phy_id2;

    /* Link */
    bool link_up;
    int link_speed;
    bool full_duplex;

    /* MSI-X */
    int num_msix_vectors;
    int msix_irq;
    bool msix_enabled;
};


/*
 * ============================================================
 * MMIO
 * ============================================================
 */

static inline u32 e82576_read_reg(
    struct e82576_device *dev,
    u32 reg)
{
    return readl(dev->hw_addr + reg);
}

static inline void e82576_write_reg(
    struct e82576_device *dev,
    u32 reg,
    u32 value)
{
    writel(value, dev->hw_addr + reg);
}

static inline void e82576_flush(
    struct e82576_device *dev)
{
    e82576_read_reg(dev, E1000_STATUS);
}

/*
 * ============================================================
 * HARDWARE NVM SEMAPHORE
 * ============================================================
 */

/* Release hardware semaphore. */
static void e82576_put_hw_semaphore(
    struct e82576_device *dev)
{
    u32 swsm;
    swsm = e82576_read_reg(dev, E1000_SWSM);

    /* Clear software semaphore first. */
    swsm &= ~E1000_SWSM_SWESMBI;

    e82576_write_reg(dev, E1000_SWSM, swsm);
    e82576_flush(dev);

    /* Clear SMBI. */

    swsm =
        e82576_read_reg(
            dev,
            E1000_SWSM);


    swsm &= ~E1000_SWSM_SMBI;


    e82576_write_reg(
        dev,
        E1000_SWSM,
        swsm);


    e82576_flush(dev);
}


/*
 * Acquire hardware semaphore.
 */

static int e82576_get_hw_semaphore(
    struct e82576_device *dev)
{
    u32 swsm;

    int i;


    /*
     * First wait for SMBI to become clear.
     *
     * Do not immediately force-clear it.
     *
     * Another function or firmware may legitimately own it.
     */

    for (i = 0; i < 200; i++) {

        swsm =
            e82576_read_reg(
                dev,
                E1000_SWSM);


        if (!(swsm & E1000_SWSM_SMBI))
            break;


        usleep_range(
            500,
            600);
    }


    if (i == 200) {

        swsm =
            e82576_read_reg(
                dev,
                E1000_SWSM);


        dev_err(
            &dev->pdev->dev,
            "NVM hardware semaphore busy: SWSM=0x%08x\n",
            swsm);


        return -EBUSY;
    }


    /*
     * Request SMBI.
     */

    swsm =
        e82576_read_reg(
            dev,
            E1000_SWSM);


    swsm |= E1000_SWSM_SMBI;


    e82576_write_reg(
        dev,
        E1000_SWSM,
        swsm);


    e82576_flush(dev);


    /*
     * Verify that SMBI latched.
     */

    swsm =
        e82576_read_reg(
            dev,
            E1000_SWSM);


    if (!(swsm & E1000_SWSM_SMBI)) {

        dev_err(
            &dev->pdev->dev,
            "Failed to acquire NVM hardware semaphore: SWSM=0x%08x\n",
            swsm);


        return -EBUSY;
    }


    /*
     * Acquire SWESMBI.
     */

    for (i = 0; i < 200; i++) {

        swsm =
            e82576_read_reg(
                dev,
                E1000_SWSM);


        swsm |= E1000_SWSM_SWESMBI;


        e82576_write_reg(
            dev,
            E1000_SWSM,
            swsm);


        e82576_flush(dev);


        swsm =
            e82576_read_reg(
                dev,
                E1000_SWSM);


        if (swsm & E1000_SWSM_SWESMBI)
            return 0;


        usleep_range(
            500,
            600);
    }


    dev_err(
        &dev->pdev->dev,
        "Failed to acquire NVM software semaphore\n");


    e82576_put_hw_semaphore(dev);


    return -EBUSY;
}


/*
 * ============================================================
 * SOFTWARE / FIRMWARE NVM LOCK
 * ============================================================
 */

static int e82576_acquire_nvm(
    struct e82576_device *dev)
{
    u32 swfw;
    u32 swmask;
    u32 fwmask;

    int i;


    swmask = E1000_SWFW_EEP_SM;

    fwmask =
        E1000_SWFW_EEP_SM << 16;


    /*
     * The SW/FW synchronization register is protected by
     * the hardware semaphore.
     */

    for (i = 0; i < 200; i++) {

        int ret;


        ret =
            e82576_get_hw_semaphore(dev);


        if (ret)
            return ret;


        swfw =
            e82576_read_reg(
                dev,
                E1000_SW_FW_SYNC);


        /*
         * Either firmware or another software owner has
         * the EEPROM resource.
         */

        if (!(swfw & (swmask | fwmask)))
            break;


        dev_dbg(
            &dev->pdev->dev,
            "NVM resource busy: SW_FW_SYNC=0x%08x\n",
            swfw);


        e82576_put_hw_semaphore(dev);


        msleep(5);
    }


    if (i == 200) {

        dev_err(
            &dev->pdev->dev,
            "NVM SW/FW synchronization timeout\n");


        return -EBUSY;
    }


    /*
     * Claim the software side of the EEPROM semaphore.
     */

    swfw |= swmask;


    e82576_write_reg(
        dev,
        E1000_SW_FW_SYNC,
        swfw);


    e82576_flush(dev);


    /*
     * We no longer need the hardware semaphore while
     * performing the EERD operation.
     */

    e82576_put_hw_semaphore(dev);


    return 0;
}


/*
 * Release NVM software/firmware ownership.
 */

static void e82576_release_nvm(
    struct e82576_device *dev)
{
    u32 swfw;

    int ret;


    /*
     * We need the hardware semaphore again before
     * modifying SW_FW_SYNC.
     */

    ret =
        e82576_get_hw_semaphore(dev);


    if (ret) {

        dev_err(
            &dev->pdev->dev,
            "Failed to reacquire NVM semaphore during release\n");

        return;
    }


    swfw =
        e82576_read_reg(
            dev,
            E1000_SW_FW_SYNC);


    swfw &= ~E1000_SWFW_EEP_SM;


    e82576_write_reg(
        dev,
        E1000_SW_FW_SYNC,
        swfw);


    e82576_flush(dev);


    e82576_put_hw_semaphore(dev);
}


/*
 * ============================================================
 * NVM READ
 * ============================================================
 */

static int e82576_read_nvm_word(
    struct e82576_device *dev,
    u16 address,
    u16 *data)
{
    u32 value;

    int timeout;


    /*
     * Start EERD operation.
     */

    value =
        E1000_EERD_START |
        ((u32)address <<
         E1000_EERD_ADDR_SHIFT);


    e82576_write_reg(
        dev,
        E1000_EERD,
        value);


    e82576_flush(dev);


    /*
     * Wait for DONE.
     */

    for (timeout = 0;
         timeout < 10000;
         timeout++) {

        value =
            e82576_read_reg(
                dev,
                E1000_EERD);


        if (value & E1000_EERD_DONE) {

            *data =
                (u16)(
                    (value >>
                     E1000_EERD_DATA_SHIFT) &
                    0xffff);


            return 0;
        }


        udelay(10);
    }


    dev_err(
        &dev->pdev->dev,
        "NVM read timeout: address=0x%04x EERD=0x%08x\n",
        address,
        value);


    return -ETIMEDOUT;
}


/*
 * ============================================================
 * MAC ADDRESS
 * ============================================================
 */

static int e82576_read_mac_address(
    struct e82576_device *dev)
{
    u16 word;

    int i;

    int ret;


    ret =
        e82576_acquire_nvm(dev);


    if (ret) {

        dev_err(
            &dev->pdev->dev,
            "Failed to acquire NVM: %d\n",
            ret);


        return ret;
    }


    for (i = 0; i < 3; i++) {

        ret =
            e82576_read_nvm_word(
                dev,
                i,
                &word);


        if (ret) {

            e82576_release_nvm(dev);

            return ret;
        }


        dev_info(
            &dev->pdev->dev,
            "NVM word %d = 0x%04x\n",
            i,
            word);


        dev->mac_address[i * 2] =
            word & 0xff;


        dev->mac_address[i * 2 + 1] =
            word >> 8;
    }


    e82576_release_nvm(dev);


    if (!is_valid_ether_addr(
            dev->mac_address)) {

        dev_err(
            &dev->pdev->dev,
            "Invalid MAC address %pM\n",
            dev->mac_address);


        return -EINVAL;
    }


    dev_info(
        &dev->pdev->dev,
        "MAC address: %pM\n",
        dev->mac_address);


    return 0;
}


/*
 * ============================================================
 * PHY MDIC READ
 * ============================================================
 */

static int e82576_read_phy(
    struct e82576_device *dev,
    u8 phy,
    u8 reg,
    u16 *data)
{
    u32 mdic;

    int timeout;


    mdic =
        ((u32)reg <<
         E1000_MDIC_REG_SHIFT) |

        ((u32)phy <<
         E1000_MDIC_PHY_SHIFT) |

        E1000_MDIC_OP_READ;


    e82576_write_reg(
        dev,
        E1000_MDIC,
        mdic);


    e82576_flush(dev);


    for (timeout = 0;
         timeout < 1000;
         timeout++) {

        mdic =
            e82576_read_reg(
                dev,
                E1000_MDIC);


        if (mdic & E1000_MDIC_READY)
            break;


        udelay(10);
    }


    if (!(mdic & E1000_MDIC_READY)) {

        dev_err(
            &dev->pdev->dev,
            "PHY read timeout: phy=%u reg=%u MDIC=0x%08x\n",
            phy,
            reg,
            mdic);


        return -ETIMEDOUT;
    }


    if (mdic & E1000_MDIC_ERROR) {

        dev_err(
            &dev->pdev->dev,
            "PHY read error: phy=%u reg=%u MDIC=0x%08x\n",
            phy,
            reg,
            mdic);


        return -EIO;
    }


    *data =
        mdic &
        E1000_MDIC_DATA_MASK;


    return 0;
}


/*
 * ============================================================
 * PHY MDIC WRITE
 * ============================================================
 */

static int e82576_write_phy(
    struct e82576_device *dev,
    u8 phy,
    u8 reg,
    u16 data)
{
    u32 mdic;

    int timeout;


    mdic =
        data |

        ((u32)reg <<
         E1000_MDIC_REG_SHIFT) |

        ((u32)phy <<
         E1000_MDIC_PHY_SHIFT) |

        E1000_MDIC_OP_WRITE;


    e82576_write_reg(
        dev,
        E1000_MDIC,
        mdic);


    e82576_flush(dev);


    for (timeout = 0;
         timeout < 1000;
         timeout++) {

        mdic =
            e82576_read_reg(
                dev,
                E1000_MDIC);


        if (mdic & E1000_MDIC_READY)
            break;


        udelay(10);
    }


    if (!(mdic & E1000_MDIC_READY)) {

        dev_err(
            &dev->pdev->dev,
            "PHY write timeout: phy=%u reg=%u\n",
            phy,
            reg);


        return -ETIMEDOUT;
    }


    if (mdic & E1000_MDIC_ERROR) {

        dev_err(
            &dev->pdev->dev,
            "PHY write error: phy=%u reg=%u MDIC=0x%08x\n",
            phy,
            reg,
            mdic);


        return -EIO;
    }


    return 0;
}


/*
 * ============================================================
 * PHY DISCOVERY
 * ============================================================
 */

static int e82576_find_phy(
    struct e82576_device *dev)
{
    u16 id1;
    u16 id2;

    int ret;


    /*
     * Normal 82576 copper PHY address.
     */

    ret =
        e82576_read_phy(
            dev,
            1,
            PHY_REG_ID1,
            &id1);


    if (!ret) {

        dev->phy_address = 1;


        ret =
            e82576_read_phy(
                dev,
                1,
                PHY_REG_ID2,
                &id2);


        if (ret)
            return ret;


        goto found;
    }


    /*
     * Fallback scan.
     */

    dev_info(
        &dev->pdev->dev,
        "PHY read at address 1 failed, scanning...\n");


    for (dev->phy_address = 1;
         dev->phy_address < 32;
         dev->phy_address++) {

        ret =
            e82576_read_phy(
                dev,
                dev->phy_address,
                PHY_REG_ID1,
                &id1);


        if (ret)
            continue;


        if (id1 == 0xffff ||
            id1 == 0x0000)
            continue;


        ret =
            e82576_read_phy(
                dev,
                dev->phy_address,
                PHY_REG_ID2,
                &id2);


        if (!ret)
            goto found;
    }


    return -ENODEV;


found:

    dev->phy_id1 = id1;

    dev->phy_id2 = id2;


    dev_info(
        &dev->pdev->dev,
        "PHY found at address %u\n",
        dev->phy_address);


    dev_info(
        &dev->pdev->dev,
        "  PHY ID1 = 0x%04x\n",
        dev->phy_id1);


    dev_info(
        &dev->pdev->dev,
        "  PHY ID2 = 0x%04x\n",
        dev->phy_id2);


    return 0;
}


static int e82576_reset_phy_hw(struct e82576_device *dev)
{
    u32 ctrl;
    int timeout;

    dev_info(&dev->pdev->dev,
             "Resetting 82576 PHY through CTRL.PHY_RST\n");

    ctrl = e82576_read_reg(dev, E1000_CTRL);

    ctrl |= E1000_CTRL_PHY_RST;

    e82576_write_reg(dev, E1000_CTRL, ctrl);
    e82576_flush(dev);

    /*
     * The 82576 datasheet specifies that the PHY is internally
     * configured following this reset.
     */
    udelay(100);

    ctrl &= ~E1000_CTRL_PHY_RST;

    e82576_write_reg(dev, E1000_CTRL, ctrl);
    e82576_flush(dev);

    /*
     * Give the PHY time to complete initialization.
     */
    msleep(5);

    /*
     * Verify that MDIO is responding again.
     */
    for (timeout = 0; timeout < 100; timeout++) {
        u16 id1;
        u16 id2;

        if (!e82576_read_phy(dev,
                             dev->phy_address,
                             PHY_REG_ID1,
                             &id1) &&
            !e82576_read_phy(dev,
                             dev->phy_address,
                             PHY_REG_ID2,
                             &id2)) {

            if (id1 != 0xffff &&
                id1 != 0x0000 &&
                id2 != 0xffff &&
                id2 != 0x0000) {

                dev_info(&dev->pdev->dev,
                         "PHY responding after hardware reset: "
                         "ID=0x%04x:0x%04x\n",
                         id1, id2);

                return 0;
            }
        }

        msleep(1);
    }

    dev_err(&dev->pdev->dev,
            "PHY did not respond after hardware reset\n");

    return -ETIMEDOUT;
}


/*
 * ============================================================
 * PHY RESET
 * ============================================================
 */

static int e82576_reset_phy(struct e82576_device *dev)
{
    u16 bmcr;
    int ret;
    int timeout;

    dev_info(&dev->pdev->dev,
             "Resetting PHY through BMCR\n");

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMCR,
                          &bmcr);
    if (ret)
        return ret;

    bmcr |= BMCR_RESET;

    ret = e82576_write_phy(dev,
                           dev->phy_address,
                           PHY_BMCR,
                           bmcr);
    if (ret)
        return ret;

    /*
     * BMCR.RESET is self-clearing.
     */
    for (timeout = 0; timeout < 1000; timeout++) {

        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_BMCR,
                              &bmcr);
        if (ret)
            return ret;

        if (!(bmcr & BMCR_RESET)) {
            dev_info(&dev->pdev->dev,
                     "PHY reset complete: BMCR=0x%04x\n",
                     bmcr);
            return 0;
        }

        udelay(10);
    }

    dev_err(&dev->pdev->dev,
            "PHY reset timeout: BMCR=0x%04x\n",
            bmcr);

    return -ETIMEDOUT;
}


/*
 * ============================================================
 * LINK STATUS
 * ============================================================
 */

static int e82576_get_link_status(struct e82576_device *dev)
{
    u16 bmsr;
    u16 bmcr;
    u16 anar;
    u16 anlpar;
    u16 stat1000;

    bool link;
    bool autoneg;

    int speed = SPEED_UNKNOWN;
    bool full_duplex = false;

    int ret;

    /*
     * BMSR is latched-low.
     */
    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMSR,
                          &bmsr);
    if (ret)
        return ret;

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMSR,
                          &bmsr);
    if (ret)
        return ret;

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMCR,
                          &bmcr);
    if (ret)
        return ret;

    link = !!(bmsr & BMSR_LSTATUS);
    autoneg = !!(bmcr & BMCR_ANENABLE);

    if (!link) {

        dev->link_up = false;
        dev->link_speed = SPEED_UNKNOWN;
        dev->full_duplex = false;

        if (dev->netdev)
            netif_carrier_off(dev->netdev);

        dev_info(&dev->pdev->dev,
                 "Link: DOWN\n");

        return 0;
    }

    /*
     * --------------------------------------------------------
     * Autonegotiated link
     * --------------------------------------------------------
     */

    if (autoneg) {

        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_ANAR,
                              &anar);
        if (ret)
            return ret;

        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_ANLPAR,
                              &anlpar);
        if (ret)
            return ret;

        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_STAT1000,
                              &stat1000);
        if (ret)
            return ret;

        /*
         * 1000BASE-T full duplex.
         */
        if (stat1000 & LPA_1000FULL) {

            speed = SPEED_1000;
            full_duplex = true;
        }

        /*
         * 100BASE-TX full.
         */
        else if ((anar & ADVERTISE_100FULL) &&
                 (anlpar & LPA_100FULL)) {

            speed = SPEED_100;
            full_duplex = true;
        }

        /*
         * 100BASE-TX half.
         */
        else if ((anar & ADVERTISE_100HALF) &&
                 (anlpar & LPA_100HALF)) {

            speed = SPEED_100;
            full_duplex = false;
        }

        /*
         * 10BASE-T full.
         */
        else if ((anar & ADVERTISE_10FULL) &&
                 (anlpar & LPA_10FULL)) {

            speed = SPEED_10;
            full_duplex = true;
        }

        /*
         * 10BASE-T half.
         */
        else if ((anar & ADVERTISE_10HALF) &&
                 (anlpar & LPA_10HALF)) {

            speed = SPEED_10;
            full_duplex = false;
        }
    }

    /*
     * --------------------------------------------------------
     * Forced link
     * --------------------------------------------------------
     */

    else {

        if (bmcr & BMCR_SPEED100)
            speed = SPEED_100;
        else
            speed = SPEED_10;

        full_duplex = !!(bmcr & BMCR_FULLDPLX);
    }

    dev->link_up = true;
    dev->link_speed = speed;
    dev->full_duplex = full_duplex;

    if (dev->netdev)
        netif_carrier_on(dev->netdev);

    dev_info(&dev->pdev->dev,
             "Link: UP\n"
             "  Speed: %d Mbps\n"
             "  Duplex: %s\n"
             "  Autonegotiation: %s\n",
             speed,
             full_duplex ? "Full" : "Half",
             autoneg ? "enabled" : "disabled");

    return 0;
}


/*
 * ============================================================
 * HARDWARE RESET
 * ============================================================
 */

static int e82576_reset_hw(
    struct e82576_device *dev)
{
    u32 ctrl;

    int timeout;


    /*
     * Disable interrupts before reset.
     */

    e82576_write_reg(
        dev,
        E1000_IMC,
        0xffffffff);


    e82576_write_reg(
        dev,
        E1000_EIMC,
        0xffffffff);


    e82576_flush(dev);


    /*
     * MAC reset.
     */

    ctrl =
        e82576_read_reg(
            dev,
            E1000_CTRL);


    ctrl |= E1000_CTRL_RST;


    e82576_write_reg(
        dev,
        E1000_CTRL,
        ctrl);


    e82576_flush(dev);


    for (timeout = 0;
         timeout < 1000;
         timeout++) {

        ctrl =
            e82576_read_reg(
                dev,
                E1000_CTRL);


        if (!(ctrl & E1000_CTRL_RST))
            break;


        udelay(10);
    }


    if (ctrl & E1000_CTRL_RST) {

        dev_err(
            &dev->pdev->dev,
            "MAC reset timeout\n");


        return -ETIMEDOUT;
    }


    msleep(10);


    return 0;
}


static int e82576_configure_phy(struct e82576_device *dev)
{
    u16 anar;
    u16 ctrl1000;
    u16 bmcr;
    int ret;

    dev_info(&dev->pdev->dev,
             "Configuring PHY for auto-negotiation\n");

    /*
     * --------------------------------------------------------
     * Read current BMCR.
     * --------------------------------------------------------
     */

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMCR,
                          &bmcr);
    if (ret)
        return ret;

    dev_info(&dev->pdev->dev,
             "Initial BMCR = 0x%04x\n",
             bmcr);

    /*
     * Remove states that prevent normal operation.
     */
    bmcr &= ~(BMCR_PDOWN |
              BMCR_ISOLATE |
              BMCR_LOOPBACK);

    /*
     * Enable auto-negotiation.
     */
    bmcr |= BMCR_ANENABLE;

    ret = e82576_write_phy(dev,
                           dev->phy_address,
                           PHY_BMCR,
                           bmcr);
    if (ret)
        return ret;

    /*
     * Read back.
     */
    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMCR,
                          &bmcr);
    if (ret)
        return ret;

    dev_info(&dev->pdev->dev,
             "BMCR after enabling PHY = 0x%04x\n",
             bmcr);

    /*
     * --------------------------------------------------------
     * Configure ANAR.
     * --------------------------------------------------------
     */

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_ANAR,
                          &anar);
    if (ret)
        return ret;

    anar &= ~(ADVERTISE_10HALF |
              ADVERTISE_10FULL |
              ADVERTISE_100HALF |
              ADVERTISE_100FULL);

    anar |= ADVERTISE_CSMA |
            ADVERTISE_10HALF |
            ADVERTISE_10FULL |
            ADVERTISE_100HALF |
            ADVERTISE_100FULL;

    ret = e82576_write_phy(dev,
                           dev->phy_address,
                           PHY_ANAR,
                           anar);
    if (ret)
        return ret;

    /*
     * --------------------------------------------------------
     * Configure 1000BASE-T.
     * --------------------------------------------------------
     */

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_CTRL1000,
                          &ctrl1000);
    if (ret)
        return ret;

    ctrl1000 &= ~(ADVERTISE_1000FULL |
                  ADVERTISE_1000HALF);

    ctrl1000 |= ADVERTISE_1000FULL;

    ret = e82576_write_phy(dev,
                           dev->phy_address,
                           PHY_CTRL1000,
                           ctrl1000);
    if (ret)
        return ret;

    /*
     * --------------------------------------------------------
     * Restart auto-negotiation.
     * --------------------------------------------------------
     */

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMCR,
                          &bmcr);
    if (ret)
        return ret;

    bmcr |= BMCR_ANENABLE;
    bmcr |= BMCR_ANRESTART;

    /*
     * Make absolutely sure PHY is not powered down.
     */
    bmcr &= ~(BMCR_PDOWN |
              BMCR_ISOLATE |
              BMCR_LOOPBACK);

    ret = e82576_write_phy(dev,
                           dev->phy_address,
                           PHY_BMCR,
                           bmcr);
    if (ret)
        return ret;

    /*
     * Read back.
     */
    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMCR,
                          &bmcr);
    if (ret)
        return ret;

    dev_info(&dev->pdev->dev,
             "PHY auto-negotiation restarted: BMCR=0x%04x\n",
             bmcr);

    return 0;
}


static int e82576_wait_for_autoneg(struct e82576_device *dev)
{
    u16 bmcr;
    u16 bmsr;
    u16 anar;
    u16 anlpar;
    u16 ctrl1000;
    u16 stat1000;

    int ret;
    int timeout;

    dev_info(&dev->pdev->dev,
             "Waiting for PHY auto-negotiation...\n");

    for (timeout = 0; timeout < 50; timeout++) {

        /*
         * BMCR
         */
        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_BMCR,
                              &bmcr);
        if (ret)
            return ret;

        /*
         * BMSR must be read twice because link status and
         * related status bits can be latched-low.
         */
        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_BMSR,
                              &bmsr);
        if (ret)
            return ret;

        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_BMSR,
                              &bmsr);
        if (ret)
            return ret;

        /*
         * Advertisement.
         */
        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_ANAR,
                              &anar);
        if (ret)
            return ret;

        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_ANLPAR,
                              &anlpar);
        if (ret)
            return ret;

        /*
         * 1000BASE-T.
         */
        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_CTRL1000,
                              &ctrl1000);
        if (ret)
            return ret;

        ret = e82576_read_phy(dev,
                              dev->phy_address,
                              PHY_STAT1000,
                              &stat1000);
        if (ret)
            return ret;

        dev_info(&dev->pdev->dev,
                 "AN state [%d/50]: "
                 "BMCR=0x%04x "
                 "BMSR=0x%04x "
                 "ANAR=0x%04x "
                 "ANLPAR=0x%04x "
                 "CTRL1000=0x%04x "
                 "STAT1000=0x%04x\n",
                 timeout,
                 bmcr,
                 bmsr,
                 anar,
                 anlpar,
                 ctrl1000,
                 stat1000);

        /*
         * Auto-negotiation complete.
         */
        if (bmsr & BMSR_ANEGCOMPLETE) {

            dev_info(&dev->pdev->dev,
                     "====================================\n"
                     "PHY AUTO-NEGOTIATION COMPLETE\n"
                     "====================================\n");

            return 0;
        }

        msleep(100);
    }

    dev_warn(&dev->pdev->dev,
             "PHY auto-negotiation timeout\n");

    return -ETIMEDOUT;
}

static int e82576_check_link(struct e82576_device *dev)
{
    u16 bmsr;
    u16 anlpar;
    u16 stat1000;
    u16 ctrl1000;

    int ret;

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_REG_BMSR,
                          &bmsr);
    if (ret)
        return ret;

    /*
     * BMSR link status is latched low.
     */
    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_REG_BMSR,
                          &bmsr);
    if (ret)
        return ret;

    if (!(bmsr & BMSR_LSTATUS)) {

        dev->link_up = false;
        dev->link_speed = SPEED_UNKNOWN;
        dev->full_duplex = false;

        dev_info(&dev->pdev->dev,
                 "Link: DOWN\n");

        return 0;
    }

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_REG_ANLPAR,
                          &anlpar);
    if (ret)
        return ret;

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_REG_1000_STAT,
                          &stat1000);
    if (ret)
        return ret;

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_REG_1000_CTRL,
                          &ctrl1000);
    if (ret)
        return ret;

    /*
     * 1000BASE-T full duplex.
     */
    if ((stat1000 & LPA_1000FULL) &&
        (ctrl1000 & ADVERTISE_1000FULL)) {

        dev->link_speed = SPEED_1000;
        dev->full_duplex = true;
    }
    else if (anlpar & LPA_100FULL) {

        dev->link_speed = SPEED_100;
        dev->full_duplex = true;
    }
    else if (anlpar & LPA_10FULL) {

        dev->link_speed = SPEED_10;
        dev->full_duplex = true;
    }
    else {

        dev->link_speed = SPEED_UNKNOWN;
        dev->full_duplex = false;
    }

    dev->link_up = true;

    dev_info(&dev->pdev->dev,
             "Link: UP\n"
             "  Speed: %d Mbps\n"
             "  Duplex: %s\n",
             dev->link_speed,
             dev->full_duplex ? "Full" : "Half");

    return 0;
}

static void e82576_dump_phy_status(struct e82576_device *dev)
{
    u16 bmcr = 0;
    u16 bmsr = 0;
    u16 anar = 0;
    u16 anlpar = 0;
    u16 ctrl1000 = 0;
    u16 stat1000 = 0;

    e82576_read_phy(dev, dev->phy_address,
                    PHY_BMCR, &bmcr);

    e82576_read_phy(dev, dev->phy_address,
                    PHY_BMSR, &bmsr);

    e82576_read_phy(dev, dev->phy_address,
                    PHY_ANAR, &anar);

    e82576_read_phy(dev, dev->phy_address,
                    PHY_ANLPAR, &anlpar);

    e82576_read_phy(dev, dev->phy_address,
                    PHY_CTRL1000, &ctrl1000);

    e82576_read_phy(dev, dev->phy_address,
                    PHY_STAT1000, &stat1000);

    dev_info(&dev->pdev->dev,
             "\n"
             "========== PHY STATUS ==========\n"
             "PHY address : %u\n"
             "PHY ID1     : 0x%04x\n"
             "PHY ID2     : 0x%04x\n"
             "BMCR [0]    : 0x%04x\n"
             "BMSR [1]    : 0x%04x\n"
             "ANAR [4]    : 0x%04x\n"
             "ANLPAR [5]  : 0x%04x\n"
             "1000CTRL[9] : 0x%04x\n"
             "1000STAT[10]: 0x%04x\n"
             "================================\n",
             dev->phy_address,
             dev->phy_id1,
             dev->phy_id2,
             bmcr,
             bmsr,
             anar,
             anlpar,
             ctrl1000,
             stat1000);
}

static int e82576_init_phy(struct e82576_device *dev)
{
    int ret;

    dev_info(&dev->pdev->dev,
             "Initializing PHY\n");

    /*
     * --------------------------------------------------------
     * Discover PHY
     * --------------------------------------------------------
     */

    ret = e82576_find_phy(dev);

    if (ret) {
        dev_err(&dev->pdev->dev,
                "PHY discovery failed: %d\n",
                ret);
        return ret;
    }

    /*
     * --------------------------------------------------------
     * Do NOT issue BMCR_RESET.
     *
     * 82576 PHY reset is controlled through the MAC.
     * --------------------------------------------------------
     */

    dev_info(&dev->pdev->dev,
             "Skipping Clause-22 BMCR PHY reset\n");

    msleep(100);

    /*
     * --------------------------------------------------------
     * Configure PHY
     * --------------------------------------------------------
     */

    ret = e82576_configure_phy(dev);

    if (ret) {
        dev_err(&dev->pdev->dev,
                "PHY configuration failed: %d\n",
                ret);
        return ret;
    }

    /*
     * --------------------------------------------------------
     * Dump PHY state.
     * --------------------------------------------------------
     */

    dev_info(&dev->pdev->dev,
             "PHY state immediately after configuration:\n");

    e82576_dump_phy_status(dev);

    /*
     * --------------------------------------------------------
     * Wait for auto-negotiation.
     * --------------------------------------------------------
     */

    ret = e82576_wait_for_autoneg(dev);

    if (ret == -ETIMEDOUT) {

        dev_warn(&dev->pdev->dev,
                 "PHY auto-negotiation did not complete\n");

        e82576_dump_phy_status(dev);

        dev->link_up = false;
        dev->link_speed = SPEED_UNKNOWN;
        dev->full_duplex = false;

        return 0;
    }

    if (ret) {
        dev_err(&dev->pdev->dev,
                "PHY auto-negotiation access failed: %d\n",
                ret);
        return ret;
    }

    ret = e82576_get_link_status(dev);

    if (ret) {
        dev_err(&dev->pdev->dev,
                "PHY status read failed: %d\n",
                ret);
        return ret;
    }

    dev_info(&dev->pdev->dev,
             "PHY initialization complete\n");

    return 0;
}


/*
 * ============================================================
 * MSI-X
 * ============================================================
 */

static irqreturn_t e82576_msix_handler(
    int irq,
    void *data)
{
    struct e82576_device *dev = data;

    u32 icr;


    icr =
        e82576_read_reg(
            dev,
            E1000_EICR);


    if (!icr)
        return IRQ_NONE;


    dev_info(
        &dev->pdev->dev,
        "MSI-X interrupt: EICR=0x%08x\n",
        icr);


    if (icr & E1000_IMS_LSC) {

        dev_info(
            &dev->pdev->dev,
            "MSI-X: Link Status Change\n");


        e82576_get_link_status(dev);
    }


    return IRQ_HANDLED;
}


static int e82576_init_msix(
    struct e82576_device *dev)
{
    int ret;

    int irq;


    dev_info(
        &dev->pdev->dev,
        "Initializing MSI-X\n");


    ret =
        pci_alloc_irq_vectors(
            dev->pdev,
            1,
            1,
            PCI_IRQ_MSIX);


    if (ret < 0) {

        dev_err(
            &dev->pdev->dev,
            "Failed to allocate MSI-X vector: %d\n",
            ret);


        return ret;
    }


    dev->num_msix_vectors = ret;


    irq =
        pci_irq_vector(
            dev->pdev,
            0);


    if (irq < 0) {

        ret = irq;

        goto err_free_vectors;
    }


    dev->msix_irq = irq;


    ret =
        request_irq(
            dev->msix_irq,
            e82576_msix_handler,
            0,
            DRIVER_NAME,
            dev);


    if (ret) {

        dev_err(
            &dev->pdev->dev,
            "request_irq() failed: %d\n",
            ret);


        goto err_free_vectors;
    }


    dev->msix_enabled = true;


    /*
     * Clear pending causes.
     */

    e82576_read_reg(
        dev,
        E1000_EICR);


    /*
     * Enable LSC.
     */

    e82576_write_reg(
        dev,
        E1000_EIMS,
        E1000_IMS_LSC);


    e82576_flush(dev);


    dev_info(
        &dev->pdev->dev,
        "MSI-X initialized: IRQ=%d\n",
        dev->msix_irq);


    return 0;


err_free_vectors:

    pci_free_irq_vectors(
        dev->pdev);


    dev->num_msix_vectors = 0;

    dev->msix_irq = -1;


    return ret;
}


static void e82576_cleanup_msix(
    struct e82576_device *dev)
{
    if (!dev->msix_enabled)
        return;


    e82576_write_reg(
        dev,
        E1000_EIMC,
        0xffffffff);


    e82576_flush(dev);


    e82576_read_reg(
        dev,
        E1000_EICR);


    free_irq(
        dev->msix_irq,
        dev);


    pci_free_irq_vectors(
        dev->pdev);


    dev->num_msix_vectors = 0;

    dev->msix_irq = -1;

    dev->msix_enabled = false;
}


/*
 * ============================================================
 * NET DEVICE
 * ============================================================
 */

static int e82576_open(
    struct net_device *netdev)
{
    struct e82576_device *dev =
        netdev_priv(netdev);

    int ret;


    dev_info(
        &dev->pdev->dev,
        "Opening network interface %s\n",
        netdev->name);


    e82576_write_reg(
        dev,
        E1000_EIMC,
        0xffffffff);


    e82576_read_reg(
        dev,
        E1000_EICR);


    e82576_write_reg(
        dev,
        E1000_EIMS,
        E1000_IMS_LSC);


    e82576_flush(dev);


    ret =
        e82576_get_link_status(dev);


    if (ret) {

        dev_err(
            &dev->pdev->dev,
            "Unable to read PHY status: %d\n",
            ret);


        return ret;
    }


    netif_tx_disable(netdev);


    if (dev->link_up)
        netif_carrier_on(netdev);
    else
        netif_carrier_off(netdev);


    dev_info(
        &dev->pdev->dev,
        "Interface %s opened\n",
        netdev->name);


    return 0;
}


static int e82576_stop(
    struct net_device *netdev)
{
    struct e82576_device *dev =
        netdev_priv(netdev);


    dev_info(
        &dev->pdev->dev,
        "Stopping network interface %s\n",
        netdev->name);


    netif_carrier_off(netdev);


    e82576_write_reg(
        dev,
        E1000_EIMC,
        E1000_IMS_LSC);


    e82576_flush(dev);


    e82576_read_reg(
        dev,
        E1000_EICR);


    netif_tx_disable(netdev);


    return 0;
}


static netdev_tx_t e82576_start_xmit(
    struct sk_buff *skb,
    struct net_device *netdev)
{
    struct e82576_device *dev =
        netdev_priv(netdev);


    dev_warn_ratelimited(
        &dev->pdev->dev,
        "TX not implemented yet\n");


    dev_kfree_skb(skb);


    netdev->stats.tx_dropped++;


    return NETDEV_TX_OK;
}


static const struct net_device_ops e82576_netdev_ops = {

    .ndo_open =
        e82576_open,

    .ndo_stop =
        e82576_stop,

    .ndo_start_xmit =
        e82576_start_xmit,
};


/*
 * ============================================================
 * PCI PROBE
 * ============================================================
 */

static int e82576_probe(
    struct pci_dev *pdev,
    const struct pci_device_id *id)
{
    struct net_device *netdev;

    struct e82576_device *dev;

    int ret;


    dev_info(
        &pdev->dev,
        "82576 probe\n");


    ret =
        pci_enable_device_mem(pdev);


    if (ret) {

        dev_err(
            &pdev->dev,
            "pci_enable_device_mem() failed: %d\n",
            ret);


        return ret;
    }


    pci_set_master(pdev);


    ret =
        pci_request_region(
            pdev,
            0,
            DRIVER_NAME);


    if (ret) {

        dev_err(
            &pdev->dev,
            "Failed to request BAR0: %d\n",
            ret);


        goto err_disable_device;
    }


    netdev =
        alloc_etherdev(
            sizeof(struct e82576_device));


    if (!netdev) {

        ret = -ENOMEM;

        goto err_release_region;
    }


    dev =
        netdev_priv(netdev);


    dev->pdev = pdev;

    dev->netdev = netdev;

    dev->msix_irq = -1;


    dev->bar0_start =
        pci_resource_start(
            pdev,
            0);


    dev->bar0_length =
        pci_resource_len(
            pdev,
            0);


    dev->hw_addr =
        pci_iomap(
            pdev,
            0,
            0);


    if (!dev->hw_addr) {

        dev_err(
            &pdev->dev,
            "Failed to map BAR0\n");


        ret = -ENOMEM;

        goto err_free_netdev;
    }


    pci_set_drvdata(
        pdev,
        dev);


    /*
     * Hardware reset first.
     *
     * This establishes a known device state before
     * touching NVM.
     */

    ret =
        e82576_reset_hw(dev);


    if (ret) {

        dev_err(
            &pdev->dev,
            "Hardware reset failed: %d\n",
            ret);


        goto err_unmap;
    }


    /*
     * Read MAC.
     *
     * This now uses the proper NVM synchronization.
     */

    ret =
        e82576_read_mac_address(dev);


    if (ret) {

        dev_err(
            &pdev->dev,
            "Failed to read MAC address: %d\n",
            ret);


        goto err_unmap;
    }


    /*
     * Set Linux MAC address.
     *
     * eth_hw_addr_set() is preferable on current kernels
     * to writing netdev->dev_addr directly.
     */

    eth_hw_addr_set(
        netdev,
        dev->mac_address);


    /*
     * PHY.
     */

    ret =
        e82576_init_phy(dev);


    if (ret) {

        dev_err(
            &pdev->dev,
            "PHY initialization failed: %d\n",
            ret);


        goto err_unmap;
    }


    /*
     * MSI-X.
     */

    ret =
        e82576_init_msix(dev);


    if (ret) {

        dev_err(
            &pdev->dev,
            "MSI-X initialization failed: %d\n",
            ret);


        goto err_unmap;
    }


    /*
     * net_device.
     */

    netdev->netdev_ops =
        &e82576_netdev_ops;


    netif_carrier_off(netdev);


    ret =
        register_netdev(netdev);


    if (ret) {

        dev_err(
            &pdev->dev,
            "register_netdev() failed: %d\n",
            ret);


        goto err_msix;
    }


    dev_info(
        &pdev->dev,
        "====================================\n");


    dev_info(
        &pdev->dev,
        "82576 initialization successful\n");


    dev_info(
        &pdev->dev,
        "Interface: %s\n",
        netdev->name);


    dev_info(
        &pdev->dev,
        "MAC: %pM\n",
        dev->mac_address);


    dev_info(
        &pdev->dev,
        "PHY address: %u\n",
        dev->phy_address);


    dev_info(
        &pdev->dev,
        "Link: %s\n",
        dev->link_up ?
            "UP" :
            "DOWN");


    dev_info(
        &pdev->dev,
        "MSI-X IRQ: %d\n",
        dev->msix_irq);


    dev_info(
        &pdev->dev,
        "====================================\n");


    return 0;


err_msix:

    e82576_cleanup_msix(dev);


err_unmap:

    if (dev->hw_addr) {

        pci_iounmap(
            pdev,
            dev->hw_addr);


        dev->hw_addr = NULL;
    }


err_free_netdev:

    free_netdev(netdev);


err_release_region:

    pci_release_region(
        pdev,
        0);


err_disable_device:

    pci_clear_master(pdev);

    pci_disable_device(pdev);


    return ret;
}


/*
 * ============================================================
 * PCI REMOVE
 * ============================================================
 */

static void e82576_remove(
    struct pci_dev *pdev)
{
    struct e82576_device *dev;


    dev =
        pci_get_drvdata(pdev);


    if (!dev)
        return;


    dev_info(
        &pdev->dev,
        "Removing e82576\n");


    unregister_netdev(
        dev->netdev);


    e82576_cleanup_msix(dev);


    if (dev->hw_addr) {

        pci_iounmap(
            pdev,
            dev->hw_addr);


        dev->hw_addr = NULL;
    }


    pci_release_region(
        pdev,
        0);


    pci_clear_master(pdev);

    pci_disable_device(pdev);


    free_netdev(
        dev->netdev);


    pci_set_drvdata(
        pdev,
        NULL);


    dev_info(
        &pdev->dev,
        "e82576 removed\n");
}


/*
 * ============================================================
 * PCI DEVICE TABLE
 * ============================================================
 */

static const struct pci_device_id e82576_pci_ids[] = {

    {
        PCI_DEVICE(
            INTEL_VENDOR_ID,
            INTEL_82576_DEVICE)
    },

    {
        0,
    }
};


MODULE_DEVICE_TABLE(
    pci,
    e82576_pci_ids);


/*
 * ============================================================
 * PCI DRIVER
 * ============================================================
 */

static struct pci_driver e82576_driver = {

    .name =
        DRIVER_NAME,

    .id_table =
        e82576_pci_ids,

    .probe =
        e82576_probe,

    .remove =
        e82576_remove,
};


module_pci_driver(
    e82576_driver);


MODULE_AUTHOR(
    "Custom 82576 Driver Development");

MODULE_DESCRIPTION(
    "Minimal Intel 82576 Ethernet driver");

MODULE_LICENSE(
    "GPL");

MODULE_VERSION(
    DRIVER_VERSION);