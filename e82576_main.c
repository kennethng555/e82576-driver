/*
 * e82576_main.c
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
 *   - RX queue 0 MSI-X interrupt
 *   - Linux net_device registration
 *   - ndo_open()
 *   - ndo_stop()
 *   - RX DMA
 *   - TX DMA
 *   - RX/TX descriptor rings
 *
 * NOT IMPLEMENTED YET:
 *   - Full NAPI RX processing
 *   - Packet reception into Linux networking stack
 *   - Full TX completion handling
 *   - Link-status-change MSI-X handling
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

/*
 * ============================================================
 * GET STATUS
 * ============================================================
 */
static void e82576_get_stats64(
    struct net_device *netdev,
    struct rtnl_link_stats64 *stats)
{
    struct e82576_device *dev =
        netdev_priv(netdev);

    *stats = dev->stats;
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

    dev_info(&dev->pdev->dev, "Opening network interface %s\n", netdev->name);

    /*
     * --------------------------------------------------------
     * Initialize DMA descriptor rings.
     * --------------------------------------------------------
     */
    ret = e82576_setup_rings(dev);
    if (ret) {
        dev_err(&dev->pdev->dev, "Failed to initialize DMA rings: %d\n", ret);
        return ret;
    }

    dev_info(&dev->pdev->dev, "OPEN: setup_rings ret=%d rx_ring=%px tx_ring=%px\n",
         ret, dev->rx_ring, dev->tx_ring);

    /*
     * Configure 82576 MSI-X mode.
     */
    // e82576_write_reg(dev, E1000_GPIE, E1000_GPIE_NSICR | E1000_GPIE_PBA);

    /*
    * Map RX queue 0 to MSI-X vector 0.
    */
    e82576_write_reg(dev, E1000_IVAR, E1000_IVAR_VALID | E1000_IVAR_VECTOR(0));

    /*
    * Map OTHER to MSI-X vector 0.
    */
    // e82576_write_reg(dev, E1000_IVAR_MISC, (E1000_IVAR_VALID | E1000_IVAR_VECTOR(0)) << 8);

    // /*
    //  * Vector 0 is the MSI-X vector for "other".
    //  *
    //  * EIAC/EIAM participate in the automatic
    //  * interrupt handling when EIAME is enabled.
    //  * EIAC clears the cause EICR (this should not be enabled)
    //  * EIAM clears the mask EIMS (this should not be enabled)
    //  */
    e82576_write_reg(dev, E1000_EIAC, 0);
    e82576_write_reg(dev, E1000_EIAM, 0);
    e82576_write_reg(dev, E1000_EITR(0), 0);

    /*
     * Enable MSI-X vector 0.
     */
    e82576_write_reg(dev, E1000_EIMS, 0x00000001);
    e82576_write_reg(dev, E1000_IMS, E1000_IMS_RXDW | E1000_IMS_LSC);
    // e82576_write_reg(dev, E1000_IMC, 0xffffffff);
    e82576_flush(dev);

    dev_info(&dev->pdev->dev, "MSI-X configured:\n");
    dev_info(&dev->pdev->dev, "  GPIE      = 0x%08x\n", e82576_read_reg(dev, E1000_GPIE));
    dev_info(&dev->pdev->dev, "  IVAR      = 0x%08x\n", e82576_read_reg(dev, E1000_IVAR));
    dev_info(&dev->pdev->dev, "  IVAR_MISC = 0x%08x\n", e82576_read_reg(dev, E1000_IVAR_MISC));
    dev_info(&dev->pdev->dev, "  EIAC      = 0x%08x\n", e82576_read_reg(dev, E1000_EIAC));
    dev_info(&dev->pdev->dev, "  EIAM      = 0x%08x\n", e82576_read_reg(dev, E1000_EIAM));
    dev_info(&dev->pdev->dev, "  EIMS      = 0x%08x\n", e82576_read_reg(dev, E1000_EIMS));
    dev_info(&dev->pdev->dev, "  IMS       = 0x%08x\n", e82576_read_reg(dev, E1000_IMS));
    dev_info(&dev->pdev->dev, "  MSI-X IRQ = %d\n", dev->msix_irq);

    /*
     * --------------------------------------------------------
     * Enable NAPI before RX DMA starts.
     * --------------------------------------------------------
     */
    napi_enable(&dev->napi);

    /*
     * --------------------------------------------------------
     * Read initial link state.
     * --------------------------------------------------------
     */
    ret = e82576_get_link_status(dev);

    if (ret) {
        dev_err(&dev->pdev->dev, "Unable to read PHY status: %d\n", ret);

        /*
         * Disable NAPI before freeing RX resources.
         */
        napi_disable(&dev->napi);

        goto err_free_rings;
    }

    /*
     * --------------------------------------------------------
     * Set initial carrier state.
     * --------------------------------------------------------
     */
    if (dev->link_up) {
        netif_carrier_on(netdev);
        netif_tx_start_all_queues(netdev);
    } else {
        netif_carrier_off(netdev);
        netif_tx_disable(netdev);
    }

    dev_info(&dev->pdev->dev, "Interface %s opened\n", netdev->name);

    // schedule_delayed_work(&dev->rx_poll_work, msecs_to_jiffies(500));

    return 0;

err_free_rings:
    e82576_free_rx_ring(dev);
    e82576_free_tx_ring(dev);
    return ret;
}


static int e82576_stop(
    struct net_device *netdev)
{
    struct e82576_device *dev = netdev_priv(netdev);

    dev_info(&dev->pdev->dev, "Stopping network interface %s\n", netdev->name);

    /*
     * Stop Linux from giving us more TX work.
     */
    netif_tx_disable(netdev);
    netif_carrier_off(netdev);

    /*
     * --------------------------------------------------------
     * Disable NAPI.
     * --------------------------------------------------------
     */
    napi_disable(&dev->napi);

    /*
     * --------------------------------------------------------
     * Stop TX cleanup work.
     * --------------------------------------------------------
     */
    cancel_delayed_work_sync(&dev->tx_clean_work);

    /*
     * --------------------------------------------------------
     * Stop RX/TX engines.
     * --------------------------------------------------------
     */

    /*
     * Disable packet reception.
     */
    e82576_write_reg(dev, E1000_RCTL, e82576_read_reg(dev, E1000_RCTL) & ~E1000_RCTL_EN);

    /*
     * Disable transmission.
     */
    e82576_write_reg(dev, E1000_TCTL, e82576_read_reg(dev, E1000_TCTL) & ~E1000_TCTL_EN);

    /*
     * Disable RX descriptor queue.
     */
    e82576_write_reg(dev, E1000_RXDCTL(0), e82576_read_reg(dev, E1000_RXDCTL(0)) & ~E1000_RXDCTL_QUEUE_ENABLE);

    /*
     * Disable TX descriptor queue.
     */
    e82576_write_reg(dev, E1000_TXDCTL(0), e82576_read_reg(dev, E1000_TXDCTL(0)) & ~E1000_TXDCTL_QUEUE_ENABLE);
    e82576_flush(dev);

    /*
     * Drain pending interrupt causes.
     */
    e82576_read_reg(dev, E1000_EICR);
    e82576_read_reg(dev, E1000_ICR);

    /*
     * --------------------------------------------------------
     * Release DMA resources.
     * --------------------------------------------------------
     */
    e82576_free_rx_ring(dev);
    e82576_free_tx_ring(dev);

    dev_info(&dev->pdev->dev, "Interface %s stopped\n", netdev->name);

    return 0;
}


static const struct net_device_ops e82576_netdev_ops = {
    .ndo_open       = e82576_open,
    .ndo_stop       = e82576_stop,
    .ndo_start_xmit = e82576_start_xmit,
    .ndo_get_stats64 = e82576_get_stats64,
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

    dev_info(&pdev->dev, "82576 probe\n");

    /*
     * --------------------------------------------------------
     * Enable PCI device.
     * --------------------------------------------------------
     */
    ret = pci_enable_device_mem(pdev);

    if (ret) {
        dev_err(&pdev->dev, "pci_enable_device_mem() failed: %d\n", ret);
        return ret;
    }

    /*
     * --------------------------------------------------------
     * Configure DMA mask.
     * --------------------------------------------------------
     */
    ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));

    if (ret) {
        dev_warn(&pdev->dev, "64-bit DMA unavailable, trying 32-bit\n");

        ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));

        if (ret) {
            dev_err(&pdev->dev, "No usable DMA configuration\n");
            goto err_disable_device;
        }
    }

    /*
     * Enable PCI bus mastering.
     */
    pci_set_master(pdev);

    /*
     * --------------------------------------------------------
     * Request BAR0.
     * --------------------------------------------------------
     */
    ret = pci_request_region(pdev, 0, DRIVER_NAME);

    if (ret) {
        dev_err(&pdev->dev, "Failed to request BAR0: %d\n", ret);
        goto err_disable_device;
    }

    /*
     * --------------------------------------------------------
     * Allocate net_device.
     * --------------------------------------------------------
     */
    netdev = alloc_etherdev(sizeof(struct e82576_device));

    if (!netdev) {
        ret = -ENOMEM;
        goto err_release_region;
    }

    dev = netdev_priv(netdev);

    spin_lock_init(&dev->tx_lock);
    INIT_DELAYED_WORK(&dev->tx_clean_work, e82576_tx_clean_work);

    /*
     * RX polling work remains disabled.
     *
     * RX processing will eventually be handled by NAPI.
     */
    // INIT_DELAYED_WORK(&dev->rx_poll_work, e82576_rx_poll_work);

    dev->pdev = pdev;
    dev->netdev = netdev;
    dev->msix_irq = -1;

    dev->bar0_start = pci_resource_start(pdev, 0);
    dev->bar0_length = pci_resource_len(pdev, 0);

    /*
     * --------------------------------------------------------
     * Map BAR0.
     * --------------------------------------------------------
     */
    dev->hw_addr = pci_iomap(pdev, 0, 0);

    if (!dev->hw_addr) {
        dev_err(&pdev->dev, "Failed to map BAR0\n");
        ret = -ENOMEM;
        goto err_free_netdev;
    }

    pci_set_drvdata(pdev, dev);

    /*
     * --------------------------------------------------------
     * Register NAPI.
     * --------------------------------------------------------
     */
    netif_napi_add(netdev, &dev->napi, e82576_poll);

    /*
     * --------------------------------------------------------
     * Hardware reset.
     * --------------------------------------------------------
     *
     * Establish a known device state before touching NVM.
     */
    ret = e82576_reset_hw(dev);

    if (ret) {
        dev_err(&pdev->dev, "Hardware reset failed: %d\n", ret);
        goto err_unmap;
    }

    /*
     * --------------------------------------------------------
     * Read MAC address from NVM.
     * --------------------------------------------------------
     */
    ret = e82576_read_mac_address(dev);

    if (ret) {
        dev_err(&pdev->dev, "Failed to read MAC address: %d\n", ret);
        goto err_unmap;
    }

    /*
     * Set Linux MAC address.
     */
    eth_hw_addr_set(netdev, dev->mac_address);

    /*
     * --------------------------------------------------------
     * Initialize PHY.
     * --------------------------------------------------------
     */
    ret = e82576_init_phy(dev);

    if (ret) {
        dev_err(&pdev->dev, "PHY initialization failed: %d\n", ret);
        goto err_unmap;
    }

    /*
     * --------------------------------------------------------
     * Link debug worker.
     * --------------------------------------------------------
     */
    // INIT_DELAYED_WORK(&dev->link_debug_work, e82576_link_debug_work);

    /*
     * --------------------------------------------------------
     * MSI-X.
     * --------------------------------------------------------
     *
     * MSI-X is configured and the Linux IRQ handler is
     * registered, but all hardware interrupt causes remain
     * masked until ndo_open().
     */
    ret = e82576_init_msix(dev);

    if (ret) {
        dev_err(&pdev->dev, "MSI-X initialization failed: %d\n", ret);
        goto err_unmap;
    }

    /*
     * --------------------------------------------------------
     * net_device.
     * --------------------------------------------------------
     */
    netdev->netdev_ops = &e82576_netdev_ops;

    dev_info(
        &pdev->dev,
        "netdev_ops=%px start_xmit=%px\n",
        netdev->netdev_ops,
        netdev->netdev_ops->ndo_start_xmit);

    netif_carrier_off(netdev);

    /*
     * --------------------------------------------------------
     * Register network interface.
     * --------------------------------------------------------
     */
    ret = register_netdev(netdev);

    if (ret) {
        dev_err(&pdev->dev, "register_netdev() failed: %d\n", ret);
        goto err_msix;
    }

    dev_info(&pdev->dev, "====================================\n");
    dev_info(&pdev->dev, "82576 initialization successful\n");
    dev_info(&pdev->dev, "Interface: %s\n",  netdev->name);
    dev_info(&pdev->dev, "MAC: %pM\n", dev->mac_address);
    dev_info(&pdev->dev, "PHY address: %u\n", dev->phy_address);
    dev_info(&pdev->dev, "Link: %s\n", dev->link_up ? "UP" : "DOWN");
    dev_info(&pdev->dev, "MSI-X IRQ: %d\n", dev->msix_irq);
    dev_info(&pdev->dev, "====================================\n");

    // schedule_delayed_work(&dev->link_debug_work, msecs_to_jiffies(500));

    return 0;


err_msix:
    e82576_cleanup_msix(dev);

err_unmap:
    if (dev->hw_addr) {
        pci_iounmap(pdev, dev->hw_addr);
        dev->hw_addr = NULL;
    }

err_free_netdev:
    free_netdev(netdev);

err_release_region:
    pci_release_region(pdev, 0);

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

    dev = pci_get_drvdata(pdev);
    if (!dev)
        return;

    dev_info(&pdev->dev, "Removing e82576\n");

    /*
     * unregister_netdev() calls ndo_stop() if the interface
     * is currently up.
     */
    unregister_netdev(dev->netdev);

    /*
     * Stop the periodic debug worker.
     */
    // cancel_delayed_work_sync(&dev->link_debug_work);

    /*
     * Remove MSI-X handler.
     */
    e82576_cleanup_msix(dev);

    /*
     * Unmap BAR0.
     */
    if (dev->hw_addr) {
        pci_iounmap(pdev, dev->hw_addr);
        dev->hw_addr = NULL;
    }

    pci_release_region(pdev, 0);
    pci_clear_master(pdev);
    pci_disable_device(pdev);
    free_netdev(dev->netdev);
    pci_set_drvdata(pdev, NULL);
    dev_info(&pdev->dev, "e82576 removed\n");
}


/*
 * ============================================================
 * PCI DEVICE TABLE
 * ============================================================
 */
static const struct pci_device_id e82576_pci_ids[] =
{
    {
        PCI_DEVICE(
            INTEL_VENDOR_ID,
            INTEL_82576_DEVICE)
    },

    {
        0,
    }
};

MODULE_DEVICE_TABLE(pci, e82576_pci_ids);


/*
 * ============================================================
 * PCI DRIVER
 * ============================================================
 */
static struct pci_driver e82576_driver =
{
    .name     = DRIVER_NAME,
    .id_table = e82576_pci_ids,
    .probe    = e82576_probe,
    .remove   = e82576_remove,
};


module_pci_driver(e82576_driver);
MODULE_AUTHOR("Custom 82576 Driver Development");
MODULE_DESCRIPTION("Minimal Intel 82576 Ethernet driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
