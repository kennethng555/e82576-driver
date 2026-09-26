#include "e82576.h"

/*
 * ============================================================
 * MSI-X
 * ============================================================
 */
void e82576_link_debug_work(struct work_struct *work)
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
    u32 ivar;
    u32 gpie;
    u32 rctl;
    u32 srrctl;
    u32 rxdctl;
    u32 rdh;
    u32 rdt;

    int i;

    /*
     * PHY state
     */
    e82576_read_phy(
        dev,
        dev->phy_address,
        PHY_BMCR,
        &bmcr);

    /*
     * BMSR is latched-low, so read twice.
     */
    e82576_read_phy(
        dev,
        dev->phy_address,
        PHY_BMSR,
        &bmsr);

    e82576_read_phy(
        dev,
        dev->phy_address,
        PHY_BMSR,
        &bmsr);

    e82576_read_phy(
        dev,
        dev->phy_address,
        PHY_STAT1000,
        &stat1000);

    /*
     * MAC / interrupt / RX state
     */
    status = e82576_read_reg(dev, E1000_STATUS);
    eims   = e82576_read_reg(dev, E1000_EIMS);
    ivar   = e82576_read_reg(dev, E1000_IVAR);
    gpie   = e82576_read_reg(dev, E1000_GPIE);

    rctl   = e82576_read_reg(dev, E1000_RCTL);
    srrctl = e82576_read_reg(dev, E1000_SRRCTL(0));
    rxdctl = e82576_read_reg(dev, E1000_RXDCTL(0));
    rdh    = e82576_read_reg(dev, E1000_RDH(0));
    rdt    = e82576_read_reg(dev, E1000_RDT(0));

    dev_info(
        &dev->pdev->dev,
        "LINK DEBUG: "
        "STATUS=0x%08x LU=%d "
        "BMCR=0x%04x "
        "BMSR=0x%04x LSTATUS=%d ANEGCOMPLETE=%d "
        "STAT1000=0x%04x "
        "GPIE=0x%08x "
        "IVAR=0x%08x "
        "EIMS=0x%08x "
        "RCTL=0x%08x "
        "SRRCTL=0x%08x "
        "RXDCTL=0x%08x "
        "RDH=%u RDT=%u",
        status,
        !!(status & E1000_STATUS_LU),
        bmcr,
        bmsr,
        !!(bmsr & BMSR_LSTATUS),
        !!(bmsr & BMSR_ANEGCOMPLETE),
        stat1000,
        gpie,
        ivar,
        eims,
        rctl,
        srrctl,
        rxdctl,
        rdh,
        rdt);

    /*
     * RX ring may not exist while the interface is down
     * or while the ring is being torn down.
     */
    if (!dev->rx_ring) {
        dev_info(
            &dev->pdev->dev,
            "RX DEBUG: rx_ring=NULL\n");

        goto reschedule;
    }

    /*
     * Show the RX ring pointer and DMA address so we can
     * verify that this is the same ring used elsewhere.
     */
    dev_info(
        &dev->pdev->dev,
        "RX DEBUG: "
        "dev=%px "
        "rx_ring=%px "
        "rx_ring_dma=%pad "
        "rx_next_to_clean=%u",
        dev,
        dev->rx_ring,
        &dev->rx_ring_dma,
        dev->rx_next_to_clean);

    /*
     * Scan the entire RX ring.
     *
     * Advanced RX descriptor:
     *
     *   status_error -> wb.upper.status_error
     *   length       -> wb.upper.length
     *   vlan         -> wb.upper.vlan
     *
     * Do NOT interpret read.pkt_addr here. The same
     * descriptor storage is being interpreted as writeback.
     */
    for (i = 0; i < E82576_NUM_RX_DESC; i++) {
        struct e82576_rx_desc *desc;
        u32 status_error;
        u16 length;
        u16 vlan;

        desc = &dev->rx_ring[i];

        status_error = le32_to_cpu(
            READ_ONCE(desc->wb.upper.status_error));

        /*
         * Only print descriptors that hardware has
         * completed.
         */
        if (!(status_error & E1000_RXD_STAT_DD))
            continue;

        length = le16_to_cpu(
            READ_ONCE(desc->wb.upper.length));

        vlan = le16_to_cpu(
            READ_ONCE(desc->wb.upper.vlan));

        dev_info(
            &dev->pdev->dev,
            "RX DESC[%d]: "
            "DD=%d "
            "EOP=%d "
            "STATUS_ERROR=0x%08x "
            "LEN=%u "
            "VLAN=0x%04x",
            i,
            !!(status_error & E1000_RXD_STAT_DD),
            !!(status_error & E1000_RXD_STAT_EOP),
            status_error,
            length,
            vlan);
    }

reschedule:
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
     * Determine which MSI-X causes triggered this vector.
     */
    eicr = e82576_read_reg(dev, E1000_EICR);
    icr = e82576_read_reg(dev, E1000_ICR);

    if (!eicr && (!icr || (icr && E1000_ICR_INTA)))
        return IRQ_NONE;
    
    dev_info(&dev->pdev->dev, "MSI-X: IRQ=%d EICR=0x%08x\n", irq, eicr);
    dev_info(&dev->pdev->dev, "MSI-X OTHER: ICR=0x%08x\n", icr);

    /*
     * RX queue 0.
     *
     * RX processing is deferred to NAPI.
     */
    if (eicr & E1000_EICR_RXQ0) {
        dev_info(&dev->pdev->dev, "MSI-X RX: queue 0\n");

        // /*
        //  * Mask MSI-X vector 0 while NAPI processes
        //  * the RX ring.
        //  *
        //  * RXQ0 and OTHER currently share this vector.
        //  */
        // e82576_write_reg(dev, E1000_EIMC, BIT(0));

        // /*
        //  * Schedule NAPI.
        //  */
        // if (napi_schedule_prep(&dev->napi))
        //     __napi_schedule(&dev->napi);
    }

    /*
     * OTHER interrupt.
     *
     * Link status changes are reported through ICR.
     */
    if (eicr & E1000_EICR_OTHER) {
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

    dev_info(&dev->pdev->dev, "Initializing MSI-X\n");

    /*
     * Allocate exactly one MSI-X vector.
     * Vector 0 will handle "other" interrupts,
     * including link status change.
     */
    ret = pci_alloc_irq_vectors(dev->pdev, 1, 1, PCI_IRQ_MSI);
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
     * Request Linux IRQ after hardware configuration.
     */
    ret = request_irq(dev->msix_irq, e82576_msix_handler, 0, DRIVER_NAME, dev);
    if (ret) {
        dev_err(&dev->pdev->dev, "request_irq() failed: %d\n", ret);
        goto err_free_vectors;
    }

    dev->msix_enabled = true;

    return 0;

err_free_vectors:
    pci_free_irq_vectors(dev->pdev);

    dev->num_msix_vectors = 0;
    dev->msix_irq = -1;

    return ret;
}

void e82576_cleanup_msix(
    struct e82576_device *dev)
{
    if (!dev->msix_enabled)
        return;

    free_irq(dev->msix_irq, dev);
    pci_free_irq_vectors(dev->pdev);

    dev->num_msix_vectors = 0;
    dev->msix_irq = -1;
    dev->msix_enabled = false;
}