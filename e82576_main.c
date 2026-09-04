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

#include "e82576.h"
#include "e82576_phy.h"

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


/*
 * ============================================================
 * HARDWARE NVM SEMAPHORE
 * ============================================================
 *
 * This is the important fix for:
 *
 *     NVM hardware semaphore busy
 *
 * We first acquire SMBI.
 *
 * Then we acquire SWESMBI.
 *
 * This follows the synchronization model used by Intel's
 * upstream igb implementation for this hardware family.
 */


/*
 * Release hardware semaphore.
 */

static void e82576_put_hw_semaphore(
    struct e82576_device *dev)
{
    u32 swsm;


    swsm =
        e82576_read_reg(
            dev,
            E1000_SWSM);


    /*
     * Clear software semaphore first.
     */

    swsm &= ~E1000_SWSM_SWESMBI;


    e82576_write_reg(
        dev,
        E1000_SWSM,
        swsm);


    e82576_flush(dev);


    /*
     * Clear SMBI.
     */

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
 * MSI-X
 * ============================================================
 */

static void e82576_link_debug_work(struct work_struct *work)
{
    struct e82576_device *dev =
        container_of(
            to_delayed_work(work),
            struct e82576_device,
            link_debug_work);

    u16 bmcr;
    u16 bmsr;
    u16 stat1000;
    u32 status;
    u32 eims;

    e82576_read_phy(dev, dev->phy_address,
                    PHY_BMCR, &bmcr);

    /*
     * BMSR is latched-low, so read twice.
     */
    e82576_read_phy(dev, dev->phy_address,
                    PHY_BMSR, &bmsr);

    e82576_read_phy(dev, dev->phy_address,
                    PHY_BMSR, &bmsr);

    e82576_read_phy(dev, dev->phy_address,
                    PHY_STAT1000, &stat1000);

    status = e82576_read_reg(dev, E1000_STATUS);
    eims   = e82576_read_reg(dev, E1000_EIMS);

    dev_info(&dev->pdev->dev,
             "LINK DEBUG: "
             "STATUS=0x%08x LU=%d "
             "BMCR=0x%04x "
             "BMSR=0x%04x LSTATUS=%d ANEGCOMPLETE=%d "
             "STAT1000=0x%04x "
             "EIMS=0x%08x LSC_EN=%d\n",
             status,
             !!(status & E1000_STATUS_LU),
             bmcr,
             bmsr,
             !!(bmsr & BMSR_LSTATUS),
             !!(bmsr & BMSR_ANEGCOMPLETE),
             stat1000,
             eims,
             !!(eims & E1000_IMS_LSC));

    schedule_delayed_work(
        &dev->link_debug_work,
        msecs_to_jiffies(500));
}

static irqreturn_t e82576_msix_handler(
    int irq,
    void *data)
{
    struct e82576_device *dev = data;

    u32 eicr;
    u32 icr;
    int ret;

    /*
     * First determine which MSI-X cause
     * caused this vector.
     */
    eicr = e82576_read_reg(
        dev,
        E1000_EICR);

    dev_info(
        &dev->pdev->dev,
        "MSI-X: IRQ=%d EICR=0x%08x\n",
        irq,
        eicr);

    if (!eicr)
        return IRQ_NONE;

    /*
     * OTHER interrupt.
     */
    if (eicr & BIT(31)) {

        icr = e82576_read_reg(
            dev,
            E1000_ICR);

        dev_info(
            &dev->pdev->dev,
            "MSI-X OTHER: ICR=0x%08x\n",
            icr);

        if (icr & E1000_ICR_LSC) {

            ret = e82576_get_link_status(dev);

            dev_info(
                &dev->pdev->dev,
                "LSC: link_up=%d ret=%d\n",
                dev->link_up,
                ret);

            if (!ret) {
                if (dev->link_up) {
                    netif_carrier_on(dev->netdev);
                    dev_info(
                        &dev->pdev->dev,
                        "Carrier ON\n");
                } else {
                    netif_carrier_off(dev->netdev);
                    dev_info(
                        &dev->pdev->dev,
                        "Carrier OFF\n");
                }
            }
        }
    }

    return IRQ_HANDLED;
}


static int e82576_init_msix(
    struct e82576_device *dev)
{
    int ret;
    int irq;
    u32 regval;

    dev_info(
        &dev->pdev->dev,
        "Initializing MSI-X\n");

    /*
     * Allocate exactly one MSI-X vector.
     * Vector 0 will handle "other" interrupts,
     * including link status change.
     */
    ret = pci_alloc_irq_vectors(
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

    irq = pci_irq_vector(
        dev->pdev,
        0);

    if (irq < 0) {
        ret = irq;
        goto err_free_vectors;
    }

    dev->msix_irq = irq;

    /*
     * Configure 82576 MSI-X mode.
     */
    e82576_write_reg(
        dev,
        E1000_GPIE,
        E1000_GPIE_MSIX_MODE |
        E1000_GPIE_PBA |
        E1000_GPIE_EIAME |
        E1000_GPIE_NSICR);

    /*
     * Route "other" causes to MSI-X vector 0.
     *
     * Bits 8:14 = vector number
     * Bit 15     = valid
     */
    e82576_write_reg(
        dev,
        E1000_IVAR_MISC,
        (0 | E1000_IVAR_VALID) << 8);

    /*
     * Disable extended interrupts while configuring.
     */
    e82576_write_reg(
        dev,
        E1000_IMC,
        0xffffffff);

    /*
     * Clear pending extended causes.
     */
    e82576_read_reg(
        dev,
        E1000_ICR);

    /*
     * Vector 0 is the MSI-X vector for "other".
     *
     * EIAC/EIAM participate in the automatic
     * interrupt handling when EIAME is enabled.
     */
    regval = e82576_read_reg(
        dev,
        E1000_EIAC);

    e82576_write_reg(
        dev,
        E1000_EIAC,
        regval | BIT(0));

    regval = e82576_read_reg(
        dev,
        E1000_EIAM);

    e82576_write_reg(
        dev,
        E1000_EIAM,
        regval | BIT(0));

    /*
     * Enable MSI-X vector 0.
     */
    e82576_write_reg(
        dev,
        E1000_EIMS,
        BIT(0));

    /*
     * Enable Link Status Change as an interrupt cause.
     */
    e82576_write_reg(
        dev,
        E1000_IMS,
        E1000_IMS_LSC);

    e82576_flush(dev);

    /*
     * Request Linux IRQ after hardware configuration.
     */
    ret = request_irq(
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

        goto err_disable_msix;
    }

    dev->msix_enabled = true;

    dev_info(
        &dev->pdev->dev,
        "MSI-X configured:\n");

    dev_info(
        &dev->pdev->dev,
        "  GPIE      = 0x%08x\n",
        e82576_read_reg(dev, E1000_GPIE));

    dev_info(
        &dev->pdev->dev,
        "  IVAR_MISC = 0x%08x\n",
        e82576_read_reg(dev, E1000_IVAR_MISC));

    dev_info(
        &dev->pdev->dev,
        "  EIAC      = 0x%08x\n",
        e82576_read_reg(dev, E1000_EIAC));

    dev_info(
        &dev->pdev->dev,
        "  EIAM      = 0x%08x\n",
        e82576_read_reg(dev, E1000_EIAM));

    dev_info(
        &dev->pdev->dev,
        "  EIMS      = 0x%08x\n",
        e82576_read_reg(dev, E1000_EIMS));

    dev_info(
        &dev->pdev->dev,
        "  IMS       = 0x%08x\n",
        e82576_read_reg(dev, E1000_IMS));

    dev_info(
        &dev->pdev->dev,
        "  MSI-X IRQ = %d\n",
        dev->msix_irq);

    return 0;

err_disable_msix:

    e82576_write_reg(
        dev,
        E1000_IMC,
        0xffffffff);

    e82576_flush(dev);

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
        E1000_IMC,
        0xffffffff);


    e82576_flush(dev);


    e82576_read_reg(
        dev,
        E1000_ICR);


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


    /*
    * Enable MSI-X vector 0.
    */
    e82576_write_reg(
        dev,
        E1000_EIMS,
        BIT(0));

    /*
    * Enable Link Status Change interrupt cause.
    */
    e82576_write_reg(
        dev,
        E1000_IMS,
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

    /*
    * Start temporary link-state diagnostics.
    */
    // schedule_delayed_work(
    //     &dev->link_debug_work,
    //     msecs_to_jiffies(500));

    // dev_info(
    //     &dev->pdev->dev,
    //     "Interface %s opened\n",
    //     netdev->name);

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

    // cancel_delayed_work_sync(
    // &dev->link_debug_work);


    netif_carrier_off(netdev);


    e82576_write_reg(
        dev,
        E1000_IMC,
        E1000_IMS_LSC);


    e82576_flush(dev);


    e82576_read_reg(
        dev,
        E1000_ICR);


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

    // INIT_DELAYED_WORK(
    // &dev->link_debug_work,
    // e82576_link_debug_work);


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
