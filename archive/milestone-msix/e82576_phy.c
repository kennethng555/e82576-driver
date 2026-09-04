#include "e82576_phy.h"

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/ethtool.h>
#include <linux/mii.h>

/*
 * ============================================================
 * PHY MDIC READ
 * ============================================================
 */

int e82576_read_phy(
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
 * LINK STATUS
 * ============================================================
 */

int e82576_get_link_status(struct e82576_device *dev)
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

        // dev_info(&dev->pdev->dev,
        //          "AN state [%d/50]: "
        //          "BMCR=0x%04x "
        //          "BMSR=0x%04x "
        //          "ANAR=0x%04x "
        //          "ANLPAR=0x%04x "
        //          "CTRL1000=0x%04x "
        //          "STAT1000=0x%04x\n",
        //          timeout,
        //          bmcr,
        //          bmsr,
        //          anar,
        //          anlpar,
        //          ctrl1000,
        //          stat1000);

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
                          PHY_BMSR,
                          &bmsr);
    if (ret)
        return ret;

    /*
     * BMSR link status is latched low.
     */
    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_BMSR,
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

    ret = e82576_read_phy(dev,
                          dev->phy_address,
                          PHY_CTRL1000,
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

void e82576_dump_phy_status(struct e82576_device *dev)
{
    u16 bmcr = 0;
    u16 bmsr = 0;
    u16 anar = 0;
    u16 anlpar = 0;
    u16 ctrl1000 = 0;
    u16 stat1000 = 0;
    u32 status = e82576_read_reg(dev, E1000_STATUS);

    dev_info(&dev->pdev->dev,
            "MAC LINK: STATUS=0x%08x LU=%d\n",
            status,
            !!(status & E1000_STATUS_LU));
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

int e82576_init_phy(struct e82576_device *dev)
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