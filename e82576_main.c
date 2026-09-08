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
     * Initialize DMA descriptor rings.
     */
    ret = e82576_setup_rings(dev);

    if (ret) {
        dev_err(
            &dev->pdev->dev,
            "Failed to initialize DMA rings: %d\n",
            ret);

        return ret;
    }


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


    ret = e82576_get_link_status(dev);

    if (ret) {

        dev_err(
            &dev->pdev->dev,
            "Unable to read PHY status: %d\n",
            ret);

        e82576_free_rx_ring(dev);
        e82576_free_tx_ring(dev);

        return ret;
    }


    /*
     * TX queue will be enabled when we implement
     * ndo_start_xmit().
     */
    netif_tx_start_all_queues(netdev);


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


    /*
     * Stop Linux from giving us more packets.
     */
    netif_tx_disable(netdev);

    netif_carrier_off(netdev);


    /*
     * Disable LSC interrupt.
     */
    e82576_write_reg(
        dev,
        E1000_IMC,
        E1000_IMS_LSC);


    /*
     * Disable MSI-X vector 0.
     */
    e82576_write_reg(
        dev,
        E1000_EIMC,
        BIT(0));


    /*
     * Stop RX/TX engines.
     */
    e82576_write_reg(
        dev,
        E1000_RCTL,
        0);

    e82576_write_reg(
        dev,
        E1000_TCTL,
        0);


    /*
     * Disable descriptor queues.
     */
    e82576_write_reg(
        dev,
        E1000_RXDCTL(0),
        e82576_read_reg(
            dev,
            E1000_RXDCTL(0)) &
        ~E1000_RXDCTL_QUEUE_ENABLE);

    e82576_write_reg(
        dev,
        E1000_TXDCTL(0),
        e82576_read_reg(
            dev,
            E1000_TXDCTL(0)) &
        ~E1000_TXDCTL_QUEUE_ENABLE);

    e82576_flush(dev);


    /*
     * Release DMA resources.
     */
    e82576_free_rx_ring(dev);
    e82576_free_tx_ring(dev);


    return 0;
}

static const struct net_device_ops e82576_netdev_ops = 
{
    .ndo_open = e82576_open,
    .ndo_stop = e82576_stop,
    .ndo_start_xmit = e82576_start_xmit,
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
    
    ret = pci_enable_device_mem(pdev);
    if (ret) {
        dev_err(&pdev->dev, "pci_enable_device_mem() failed: %d\n", ret);
        return ret;
    }

    pci_set_master(pdev);

    ret = pci_request_region(pdev, 0, DRIVER_NAME);
    if (ret) {
        dev_err(&pdev->dev, "Failed to request BAR0: %d\n", ret);
        goto err_disable_device;
    }

    netdev = alloc_etherdev(sizeof(struct e82576_device));
    if (!netdev) {
        ret = -ENOMEM;
        goto err_release_region;
    }

    dev = netdev_priv(netdev);

    dev->pdev = pdev;
    dev->netdev = netdev;
    dev->msix_irq = -1;
    dev->bar0_start = pci_resource_start(pdev, 0);
    dev->bar0_length = pci_resource_len(pdev, 0);
    dev->hw_addr = pci_iomap(pdev, 0, 0);

    if (!dev->hw_addr) {
        dev_err(&pdev->dev, "Failed to map BAR0\n");
        ret = -ENOMEM;
        goto err_free_netdev;
    }

    pci_set_drvdata(pdev, dev);

    /*
     * Hardware reset first.
     *
     * This establishes a known device state before
     * touching NVM.
     */
    ret = e82576_reset_hw(dev);
    if (ret) {
        dev_err(&pdev->dev, "Hardware reset failed: %d\n", ret);
        goto err_unmap;
    }

    /*
     * Read MAC.
     *
     * This now uses the proper NVM synchronization.
     */
    ret = e82576_read_mac_address(dev);
    if (ret) {
        dev_err(&pdev->dev, "Failed to read MAC address: %d\n", ret);
        goto err_unmap;
    }

    /*
     * Set Linux MAC address.
     *
     * eth_hw_addr_set() is preferable on current kernels
     * to writing netdev->dev_addr directly.
     */
    eth_hw_addr_set(netdev, dev->mac_address);

    /*
     * PHY.
     */
    ret = e82576_init_phy(dev);
    if (ret) {
        dev_err(&pdev->dev, "PHY initialization failed: %d\n", ret);
        goto err_unmap;
    }

    // INIT_DELAYED_WORK(&dev->link_debug_work, e82576_link_debug_work);

    /*
     * MSI-X.
     */

    ret = e82576_init_msix(dev);
    if (ret) {
        dev_err(&pdev->dev, "MSI-X initialization failed: %d\n", ret);
        goto err_unmap;
    }

    /*
     * net_device.
     */
    netdev->netdev_ops = &e82576_netdev_ops;

    netif_carrier_off(netdev);

    ret = register_netdev(netdev);
    if (ret) {
        dev_err(&pdev->dev, "register_netdev() failed: %d\n", ret);
        goto err_msix;
    }

    dev_info(&pdev->dev, "====================================\n");
    dev_info(&pdev->dev, "82576 initialization successful\n");
    dev_info(&pdev->dev, "Interface: %s\n", netdev->name);
    dev_info(&pdev->dev, "MAC: %pM\n", dev->mac_address);
    dev_info(&pdev->dev, "PHY address: %u\n", dev->phy_address);
    dev_info(&pdev->dev, "Link: %s\n", dev->link_up ? "UP" : "DOWN");
    dev_info(&pdev->dev, "MSI-X IRQ: %d\n", dev->msix_irq);
    dev_info(&pdev->dev, "====================================\n");

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
    unregister_netdev(dev->netdev);
    e82576_cleanup_msix(dev);
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
        PCI_DEVICE(INTEL_VENDOR_ID, INTEL_82576_DEVICE)
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

static struct pci_driver e82576_driver = 
{
    .name = DRIVER_NAME,
    .id_table = e82576_pci_ids,
    .probe = e82576_probe,
    .remove = e82576_remove,
};


module_pci_driver(e82576_driver);
MODULE_AUTHOR("Custom 82576 Driver Development");
MODULE_DESCRIPTION("Minimal Intel 82576 Ethernet driver");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRIVER_VERSION);
