#include "e82576.h"

/*
 * ============================================================
 * MSI-X
 * ============================================================
 */
static void e82576_link_debug_work(struct work_struct *work)
{
    struct e82576_device *dev =
        container_of(to_delayed_work(work),
            struct e82576_device,
            link_debug_work);

    u16 bmcr;
    u16 bmsr;
    u16 stat1000;
    u32 status;
    u32 eims;
    // u32 eicr;
    // u32 icr;

    e82576_read_phy(dev, dev->phy_address, PHY_BMCR, &bmcr);

    /*
     * BMSR is latched-low, so read twice.
     */
    e82576_read_phy(dev, dev->phy_address, PHY_BMSR, &bmsr);
    e82576_read_phy(dev, dev->phy_address, PHY_BMSR, &bmsr);
    e82576_read_phy(dev, dev->phy_address, PHY_STAT1000, &stat1000);

    status = e82576_read_reg(dev, E1000_STATUS);
    eims   = e82576_read_reg(dev, E1000_EIMS);
    // eicr   = e82576_read_reg(dev, E1000_EICR);
    // icr    = e82576_read_reg(dev, E1000_ICR);

    dev_info(&dev->pdev->dev,
             "LINK DEBUG: "
             "STATUS=0x%08x LU=%d "
             "BMCR=0x%04x "
             "BMSR=0x%04x LSTATUS=%d ANEGCOMPLETE=%d "
             "STAT1000=0x%04x "
             "EIMS=0x%08x LSC_EN=%d",
            //  "EICR=0x%08x LSC=%d"
            //  "ICR=0x%08x LSC=%d",
             status,
             !!(status & E1000_STATUS_LU),
             bmcr,
             bmsr,
             !!(bmsr & BMSR_LSTATUS),
             !!(bmsr & BMSR_ANEGCOMPLETE),
             stat1000,
             eims,
             !!(eims & E1000_IMS_LSC));
            //  eicr,
            //  !!(eicr & E1000_ICR_LSC),
            //  icr,
            //  !!(icr & E1000_ICR_LSC));

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
    eicr = e82576_read_reg(dev, E1000_EICR);

    dev_info(&dev->pdev->dev, "MSI-X: IRQ=%d EICR=0x%08x\n", irq, eicr);
    if (!eicr)
        return IRQ_NONE;

    /*
     * OTHER interrupt.
     */
    if (eicr & BIT(31)) {
        icr = e82576_read_reg(dev, E1000_ICR);

        dev_info(&dev->pdev->dev,"MSI-X OTHER: ICR=0x%08x\n",icr);

        if (icr & E1000_ICR_LSC) {
            ret = e82576_get_link_status(dev);
            if (!ret) {
                if (dev->link_up) {
                    netif_carrier_on(dev->netdev);
                    dev_info(&dev->pdev->dev, "Carrier ON\n");
                } else {
                    netif_carrier_off(dev->netdev);
                    dev_info(&dev->pdev->dev, "Carrier OFF\n");
                }
            }
        }
    }

    return IRQ_HANDLED;
}

int e82576_init_msix(
    struct e82576_device *dev)
{
    int ret;
    int irq;
    u32 regval;

    dev_info(&dev->pdev->dev, "Initializing MSI-X\n");

    /*
     * Allocate exactly one MSI-X vector.
     * Vector 0 will handle "other" interrupts,
     * including link status change.
     */
    ret = pci_alloc_irq_vectors(dev->pdev, 1, 1, PCI_IRQ_MSIX);
    if (ret < 0) {
        dev_err(&dev->pdev->dev, "Failed to allocate MSI-X vector: %d\n", ret);
        return ret;
    }

    dev->num_msix_vectors = ret;

    irq = pci_irq_vector(dev->pdev, 0);
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
    e82576_write_reg(dev, E1000_IVAR_MISC, (0 | E1000_IVAR_VALID) << 8);

    /*
     * Disable extended interrupts while configuring.
     */
    e82576_write_reg(dev, E1000_EIMC, 0xffffffff);

    /*
     * Clear pending extended causes.
     */
    e82576_read_reg(dev, E1000_EICR);

    /*
     * Vector 0 is the MSI-X vector for "other".
     *
     * EIAC/EIAM participate in the automatic
     * interrupt handling when EIAME is enabled.
     * EIAC clears the cause EICR
     * EIAM clears the mask EIMS (this should not be enabled)
     */
    regval = e82576_read_reg(dev, E1000_EIAC);
    e82576_write_reg(dev, E1000_EIAC, regval | BIT(0));

    // regval = e82576_read_reg(dev, E1000_EIAM);
    // e82576_write_reg(dev, E1000_EIAM, regval | BIT(0));

    /*
     * Enable MSI-X vector 0.
     */
    e82576_write_reg(dev, E1000_EIMS, BIT(0));

    /*
     * Enable Link Status Change as an interrupt cause.
     */
    e82576_write_reg(dev, E1000_IMS, E1000_IMS_LSC);
    e82576_flush(dev);

    /*
     * Request Linux IRQ after hardware configuration.
     */
    ret = request_irq(dev->msix_irq, e82576_msix_handler, 0, DRIVER_NAME, dev);
    if (ret) {
        dev_err(&dev->pdev->dev, "request_irq() failed: %d\n", ret);
        goto err_disable_msix;
    }

    dev->msix_enabled = true;

    dev_info(&dev->pdev->dev, "MSI-X configured:\n");
    dev_info(&dev->pdev->dev, "  GPIE      = 0x%08x\n", e82576_read_reg(dev, E1000_GPIE));
    dev_info(&dev->pdev->dev, "  IVAR_MISC = 0x%08x\n", e82576_read_reg(dev, E1000_IVAR_MISC));
    dev_info(&dev->pdev->dev, "  EIAC      = 0x%08x\n", e82576_read_reg(dev, E1000_EIAC));
    dev_info(&dev->pdev->dev, "  EIAM      = 0x%08x\n", e82576_read_reg(dev, E1000_EIAM));
    dev_info(&dev->pdev->dev, "  EIMS      = 0x%08x\n", e82576_read_reg(dev, E1000_EIMS));
    dev_info(&dev->pdev->dev, "  IMS       = 0x%08x\n", e82576_read_reg(dev, E1000_IMS));
    dev_info(&dev->pdev->dev, "  MSI-X IRQ = %d\n", dev->msix_irq);

    return 0;

err_disable_msix:

    e82576_write_reg(dev, E1000_EIMC, 0xffffffff);
    e82576_flush(dev);

err_free_vectors:

    pci_free_irq_vectors(
        dev->pdev);

    dev->num_msix_vectors = 0;
    dev->msix_irq = -1;

    return ret;
}


void e82576_cleanup_msix(
    struct e82576_device *dev)
{
    if (!dev->msix_enabled)
        return;

    e82576_write_reg(dev, E1000_EIMC, 0xffffffff);
    e82576_flush(dev);

    e82576_read_reg(dev, E1000_EICR);

    free_irq(dev->msix_irq, dev);
    pci_free_irq_vectors(dev->pdev);

    dev->num_msix_vectors = 0;
    dev->msix_irq = -1;
    dev->msix_enabled = false;
}