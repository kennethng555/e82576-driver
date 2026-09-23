// #include "e82576.h"

// /*
//  * ============================================================
//  * MSI-X
//  * ============================================================
//  */
// void e82576_link_debug_work(struct work_struct *work)
// {
//     struct e82576_device *dev =
//         container_of(to_delayed_work(work),
//             struct e82576_device,
//             link_debug_work);

//     u16 bmcr;
//     u16 bmsr;
//     u16 stat1000;
//     u32 status;
//     u32 eims;
//     u32 ivar;
//     u32 gpie;
//     u32 rxdctl;
//     u32 rdh;
//     u32 rdt;
//     u8 rx_status;

//     e82576_read_phy(dev, dev->phy_address, PHY_BMCR, &bmcr);

//     /*
//      * BMSR is latched-low, so read twice.
//      */
//     e82576_read_phy(dev, dev->phy_address, PHY_BMSR, &bmsr);
//     e82576_read_phy(dev, dev->phy_address, PHY_BMSR, &bmsr);

//     e82576_read_phy(dev, dev->phy_address, PHY_STAT1000, &stat1000);

//     status = e82576_read_reg(dev, E1000_STATUS);
//     eims   = e82576_read_reg(dev, E1000_EIMS);

//     ivar   = e82576_read_reg(dev, E1000_IVAR);
//     gpie   = e82576_read_reg(dev, E1000_GPIE);
//     rxdctl = e82576_read_reg(dev, E1000_RXDCTL(0));
//     rdh    = e82576_read_reg(dev, E1000_RDH(0));
//     rdt    = e82576_read_reg(dev, E1000_RDT(0));

//     if (dev->rx_ring)
//         rx_status = dev->rx_ring[dev->rx_next_to_clean].status;
//     else
//         rx_status = 0xff;

//     dev_info(&dev->pdev->dev,
//              "LINK DEBUG: "
//              "STATUS=0x%08x LU=%d "
//              "BMCR=0x%04x "
//              "BMSR=0x%04x LSTATUS=%d ANEGCOMPLETE=%d "
//              "STAT1000=0x%04x "
//              "GPIE=0x%08x "
//              "IVAR=0x%08x "
//              "EIMS=0x%08x "
//              "RXDCTL=0x%08x "
//              "RDH=%u RDT=%u "
//              "RX_STATUS=0x%02x "
//              "LSC_EN=%d",
//              status,
//              !!(status & E1000_STATUS_LU),
//              bmcr,
//              bmsr,
//              !!(bmsr & BMSR_LSTATUS),
//              !!(bmsr & BMSR_ANEGCOMPLETE),
//              stat1000,
//              gpie,
//              ivar,
//              eims,
//              rxdctl,
//              rdh,
//              rdt,
//              rx_status,
//              !!(eims & E1000_IMS_LSC));

//     dev_info(&dev->pdev->dev,
//          "RX DESC: "
//          "[0]=0x%02x "
//          "[1]=0x%02x "
//          "[2]=0x%02x "
//          "[3]=0x%02x "
//          "[4]=0x%02x "
//          "[5]=0x%02x\n",
//          dev->rx_ring[0].status,
//          dev->rx_ring[1].status,
//          dev->rx_ring[2].status,
//          dev->rx_ring[3].status,
//          dev->rx_ring[4].status,
//          dev->rx_ring[5].status);

//     schedule_delayed_work(
//         &dev->link_debug_work,
//         msecs_to_jiffies(500));
// }

// // static irqreturn_t e82576_msix_handler(
// //     int irq,
// //     void *data)
// // {
// //     struct e82576_device *dev = data;

// //     u32 eicr;
// //     u32 icr;
// //     int ret;

// //     /*
// //      * Determine which MSI-X causes triggered this vector.
// //      */
// //     eicr = e82576_read_reg(dev, E1000_EICR);

// //     dev_info(
// //         &dev->pdev->dev,
// //         "MSI-X: IRQ=%d EICR=0x%08x\n",
// //         irq,
// //         eicr);

// //     if (!eicr)
// //         return IRQ_NONE;

// //     /*
// //      * RX queue 0.
// //      *
// //      * RX processing is deferred to NAPI.
// //      */
// //     if (eicr & E1000_EICR_RXQ0) {

// //         dev_info(
// //             &dev->pdev->dev,
// //             "MSI-X RX: queue 0\n");

// //         /*
// //          * Mask MSI-X vector 0 while NAPI processes
// //          * the RX ring.
// //          *
// //          * RXQ0 and OTHER currently share this vector.
// //          */
// //         e82576_write_reg(
// //             dev,
// //             E1000_EIMC,
// //             BIT(0));

// //         /*
// //          * Schedule NAPI.
// //          */
// //         if (napi_schedule_prep(&dev->napi))
// //             __napi_schedule(&dev->napi);
// //     }

// //     /*
// //      * OTHER interrupt.
// //      *
// //      * Link status changes are reported through ICR.
// //      */
// //     if (eicr & E1000_EICR_OTHER) {

// //         icr = e82576_read_reg(
// //             dev,
// //             E1000_ICR);

// //         dev_info(
// //             &dev->pdev->dev,
// //             "MSI-X OTHER: ICR=0x%08x\n",
// //             icr);

// //         if (icr & E1000_ICR_LSC) {

// //             ret = e82576_get_link_status(dev);

// //             if (!ret) {

// //                 if (dev->link_up) {

// //                     netif_carrier_on(dev->netdev);

// //                     dev_info(
// //                         &dev->pdev->dev,
// //                         "Carrier ON\n");

// //                 } else {

// //                     netif_carrier_off(dev->netdev);

// //                     dev_info(
// //                         &dev->pdev->dev,
// //                         "Carrier OFF\n");
// //                 }
// //             }
// //         }
// //     }

// //     return IRQ_HANDLED;
// // }

// static irqreturn_t e82576_msix_handler(int irq, void *data)
// {
//     struct e82576_device *dev = data;
//     u32 eicr;

//     eicr = e82576_read_reg(dev, E1000_EICR);

//     dev_info(&dev->pdev->dev,
//              "***** MSI-X IRQ FIRED: irq=%d EICR=0x%08x *****\n",
//              irq, eicr);

//     if (!eicr)
//         return IRQ_NONE;

//     return IRQ_HANDLED;
// }

// int e82576_init_msix(struct e82576_device *dev)
// {
//     int ret;
//     int irq;
//     u32 ivar;

//     dev_info(&dev->pdev->dev, "Initializing MSI-X\n");

//     ret = pci_alloc_irq_vectors(dev->pdev, 1, 1, PCI_IRQ_MSIX);
//     if (ret < 0) {
//         dev_err(&dev->pdev->dev,
//                 "Failed to allocate MSI-X vector: %d\n", ret);
//         return ret;
//     }

//     dev->num_msix_vectors = ret;

//     irq = pci_irq_vector(dev->pdev, 0);
//     if (irq < 0) {
//         ret = irq;
//         goto err_free_vectors;
//     }

//     dev->msix_irq = irq;

//     /*
//      * Enable MSI-X mode.
//      */
//     e82576_write_reg(
//         dev,
//         E1000_GPIE,
//         E1000_GPIE_MSIX_MODE |
//         E1000_GPIE_PBA |
//         E1000_GPIE_EIAME |
//         E1000_GPIE_NSICR);

//     /*
//      * RX queue 0 -> MSI-X vector 0.
//      *
//      * For 82576:
//      *   IVAR row 0
//      *   RX column = offset 0
//      */
//     ivar = (E1000_IVAR_VALID | E1000_IVAR_VECTOR(0));

//     e82576_write_reg(
//         dev,
//         E1000_IVAR,
//         ivar);

//     /*
//      * Mask all extended interrupts while configuring.
//      */
//     e82576_write_reg(dev, E1000_EIMC, 0xffffffff);
//     e82576_flush(dev);

//     /*
//     * Clear stale causes.
//     */
//     e82576_read_reg(dev, E1000_EICR);

//     /*
//     * Register Linux handler while interrupts are still masked.
//     */
//     ret = request_irq(
//         dev->msix_irq,
//         e82576_msix_handler,
//         0,
//         DRIVER_NAME,
//         dev);

//     if (ret) {
//         dev_err(&dev->pdev->dev,
//                 "request_irq() failed: %d\n",
//                 ret);
//         goto err_free_vectors;
//     }

//     dev->msix_enabled = true;

//     /*
//     * Now enable RX queue 0 MSI-X interrupt.
//     */
//     e82576_write_reg(dev, E1000_EIMS, BIT(0));
//     e82576_flush(dev);

//     dev->msix_enabled = true;

//     dev_info(&dev->pdev->dev,
//              "GPIE      = 0x%08x\n",
//              e82576_read_reg(dev, E1000_GPIE));

//     dev_info(&dev->pdev->dev,
//              "IVAR      = 0x%08x\n",
//              e82576_read_reg(dev, E1000_IVAR));

//     dev_info(&dev->pdev->dev,
//              "EIMS      = 0x%08x\n",
//              e82576_read_reg(dev, E1000_EIMS));

//     dev_info(&dev->pdev->dev,
//              "EICR      = 0x%08x\n",
//              e82576_read_reg(dev, E1000_EICR));

//     dev_info(&dev->pdev->dev,
//              "MSI-X IRQ = %d\n",
//              dev->msix_irq);

//     return 0;

// err_disable_msix:

//     e82576_write_reg(dev, E1000_EIMC, 0xffffffff);
//     e82576_flush(dev);

// err_free_vectors:

//     pci_free_irq_vectors(dev->pdev);

//     dev->num_msix_vectors = 0;
//     dev->msix_irq = -1;

//     return ret;
// }

// // int e82576_init_msix(
// //     struct e82576_device *dev)
// // {
// //     int ret;
// //     int irq;
// //     u32 regval;

// //     dev_info(&dev->pdev->dev, "Initializing MSI-X\n");

// //     /*
// //      * Allocate exactly one MSI-X vector.
// //      * Vector 0 will handle "other" interrupts,
// //      * including link status change.
// //      */
// //     ret = pci_alloc_irq_vectors(dev->pdev, 1, 1, PCI_IRQ_MSIX);
// //     if (ret < 0) {
// //         dev_err(&dev->pdev->dev, "Failed to allocate MSI-X vector: %d\n", ret);
// //         return ret;
// //     }

// //     dev->num_msix_vectors = ret;

// //     irq = pci_irq_vector(dev->pdev, 0);
// //     if (irq < 0) {
// //         ret = irq;
// //         goto err_free_vectors;
// //     }

// //     dev->msix_irq = irq;

// //     /*
// //      * Configure 82576 MSI-X mode.
// //      */
// //     e82576_write_reg(
// //         dev,
// //         E1000_GPIE,
// //         E1000_GPIE_MSIX_MODE |
// //         E1000_GPIE_PBA |
// //         E1000_GPIE_EIAME |
// //         E1000_GPIE_NSICR);

// //     /*
// //     * Map RX queue 0 to MSI-X vector 0.
// //     */
// //     u32 ivar;

// //     ivar = 0;

// //     ivar = E1000_IVAR_VALID | E1000_IVAR_VECTOR(0);
// //     e82576_write_reg(dev, E1000_IVAR, ivar);

// //     /*
// //     * Map OTHER to MSI-X vector 0.
// //     */
// //     e82576_write_reg(
// //         dev,
// //         E1000_IVAR_MISC,
// //         (E1000_IVAR_VALID | E1000_IVAR_VECTOR(0)) << 8);

// //     /*
// //      * Disable extended interrupts while configuring.
// //      */
// //     e82576_write_reg(dev, E1000_EIMC, 0xffffffff);

// //     /*
// //      * Clear pending extended causes.
// //      */
// //     e82576_read_reg(dev, E1000_EICR);

// //     /*
// //      * Vector 0 is the MSI-X vector for "other".
// //      *
// //      * EIAC/EIAM participate in the automatic
// //      * interrupt handling when EIAME is enabled.
// //      * EIAC clears the cause EICR
// //      * EIAM clears the mask EIMS (this should not be enabled)
// //      */
// //     regval = e82576_read_reg(dev, E1000_EIAC);
// //     e82576_write_reg(dev, E1000_EIAC, regval | BIT(0) | BIT(1) | BIT(31));

// //     // regval = e82576_read_reg(dev, E1000_EIAM);
// //     // e82576_write_reg(dev, E1000_EIAM, regval | BIT(0));

// //     // /*
// //     //  * Enable MSI-X vector 0.
// //     //  */
// //     e82576_write_reg(dev, E1000_EIMS, BIT(0) | BIT(1));

// //     /*
// //      * Enable Link Status Change as an interrupt cause.
// //      */
// //     e82576_write_reg(dev, E1000_IMS, E1000_IMS_LSC);
// //     e82576_flush(dev);

// //     /*
// //      * Request Linux IRQ after hardware configuration.
// //      */
// //     ret = request_irq(dev->msix_irq, e82576_msix_handler, 0, DRIVER_NAME, dev);
// //     if (ret) {
// //         dev_err(&dev->pdev->dev, "request_irq() failed: %d\n", ret);
// //         goto err_disable_msix;
// //     }

// //     dev->msix_enabled = true;

// //     dev_info(&dev->pdev->dev, "MSI-X configured:\n");
// //     dev_info(&dev->pdev->dev, "  GPIE      = 0x%08x\n", e82576_read_reg(dev, E1000_GPIE));
// //     dev_info(&dev->pdev->dev, "  IVAR      = 0x%08x\n", e82576_read_reg(dev, E1000_IVAR));
// //     dev_info(&dev->pdev->dev, "  IVAR_MISC = 0x%08x\n", e82576_read_reg(dev, E1000_IVAR_MISC));
// //     dev_info(&dev->pdev->dev, "  EIAC      = 0x%08x\n", e82576_read_reg(dev, E1000_EIAC));
// //     dev_info(&dev->pdev->dev, "  EIAM      = 0x%08x\n", e82576_read_reg(dev, E1000_EIAM));
// //     dev_info(&dev->pdev->dev, "  EIMS      = 0x%08x\n", e82576_read_reg(dev, E1000_EIMS));
// //     dev_info(&dev->pdev->dev, "  IMS       = 0x%08x\n", e82576_read_reg(dev, E1000_IMS));
// //     dev_info(&dev->pdev->dev, "  MSI-X IRQ = %d\n", dev->msix_irq);

// //     return 0;

// // err_disable_msix:

// //     e82576_write_reg(dev, E1000_EIMC, 0xffffffff);
// //     e82576_flush(dev);

// // err_free_vectors:

// //     pci_free_irq_vectors(
// //         dev->pdev);

// //     dev->num_msix_vectors = 0;
// //     dev->msix_irq = -1;

// //     return ret;
// // }


// void e82576_cleanup_msix(
//     struct e82576_device *dev)
// {
//     if (!dev->msix_enabled)
//         return;

//     e82576_write_reg(dev, E1000_EIMC, 0xffffffff);
//     e82576_flush(dev);

//     e82576_read_reg(dev, E1000_EICR);

//     free_irq(dev->msix_irq, dev);
//     pci_free_irq_vectors(dev->pdev);

//     dev->num_msix_vectors = 0;
//     dev->msix_irq = -1;
//     dev->msix_enabled = false;
// }

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
 * MSI-X / LINK DEBUG
 * ============================================================
 *
 * This worker intentionally does NOT read EICR.
 *
 * EICR should be consumed by the actual MSI-X handler while
 * we are debugging interrupt delivery.
 */
void e82576_link_debug_work(struct work_struct *work)
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
    u32 eicr;
    u32 ivar;
    u32 gpie;
    u32 rxdctl;
    u32 rdh;
    u32 rdt;

    /*
     * Read PHY state.
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
     * Read interrupt / RX state.
     *
     * IMPORTANT:
     * Do NOT read EICR here.
     *
     * The MSI-X handler is the only code path that should
     * consume EICR during interrupt debugging.
     */
    status = e82576_read_reg(
        dev,
        E1000_STATUS);

    eims = e82576_read_reg(
        dev,
        E1000_EIMS);

    eicr = e82576_read_reg(
        dev,
        E1000_EICR);

    ivar = e82576_read_reg(
        dev,
        E1000_IVAR);

    gpie = e82576_read_reg(
        dev,
        E1000_GPIE);

    rxdctl = e82576_read_reg(
        dev,
        E1000_RXDCTL(0));

    rdh = e82576_read_reg(
        dev,
        E1000_RDH(0));

    rdt = e82576_read_reg(
        dev,
        E1000_RDT(0));

    dev_info(
        &dev->pdev->dev,
        "LINK DEBUG: "
        "STATUS=0x%08x LU=%d "
        "BMCR=0x%04x "
        "BMSR=0x%04x "
        "LSTATUS=%d "
        "ANEGCOMPLETE=%d "
        "STAT1000=0x%04x "
        "GPIE=0x%08x "
        "IVAR=0x%08x "
        "EIMS=0x%08x "
        "EICR=0x%08x "
        "RXDCTL=0x%08x "
        "RDH=%u "
        "RDT=%u",
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
        eicr,
        rxdctl,
        rdh,
        rdt);

    /*
     * Print a few RX descriptors.
     *
     * This is useful for determining whether RX DMA completed
     * even when the MSI-X handler is not invoked.
     */
    if (dev->rx_ring) {
        dev_info(
            &dev->pdev->dev,
            "RX DESC STATUS: "
            "[0]=0x%02x "
            "[1]=0x%02x "
            "[2]=0x%02x "
            "[3]=0x%02x "
            "[4]=0x%02x "
            "[5]=0x%02x",
            dev->rx_ring[0].status,
            dev->rx_ring[1].status,
            dev->rx_ring[2].status,
            dev->rx_ring[3].status,
            dev->rx_ring[4].status,
            dev->rx_ring[5].status);
    }

    schedule_delayed_work(
        &dev->link_debug_work,
        msecs_to_jiffies(500));
}


/*
 * ============================================================
 * MSI-X INTERRUPT HANDLER
 * ============================================================
 *
 * Current purpose:
 *
 *   Determine whether RX queue 0 actually generates an MSI-X
 *   interrupt and whether EICR reports the expected cause.
 *
 * NAPI scheduling is intentionally NOT done yet.
 */
static irqreturn_t e82576_msix_handler(
    int irq,
    void *data)
{
    struct e82576_device *dev = data;
    u32 eicr;

    /*
     * EICR is read only from the actual interrupt handler.
     */
    eicr = e82576_read_reg(
        dev,
        E1000_EICR);

    dev_info(
        &dev->pdev->dev,
        "***** MSI-X IRQ FIRED: "
        "irq=%d EICR=0x%08x *****\n",
        irq,
        eicr);

    if (!eicr)
        return IRQ_NONE;

    /*
     * RX/NAPI processing will be added after we establish
     * that MSI-X delivery works.
     */
    return IRQ_HANDLED;
}


/*
 * ============================================================
 * MSI-X INITIALIZATION
 * ============================================================
 *
 * MSI-X is configured here, but interrupts remain MASKED.
 *
 * The RX interrupt is enabled later by ndo_open(), after the
 * RX ring and RX DMA engine are running.
 */
int e82576_init_msix(
    struct e82576_device *dev)
{
    int ret;
    int irq;
    u32 ivar;

    dev_info(
        &dev->pdev->dev,
        "Initializing MSI-X\n");

    /*
     * Allocate exactly one MSI-X vector.
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

    /*
     * Obtain Linux IRQ number for MSI-X vector 0.
     */
    irq = pci_irq_vector(
        dev->pdev,
        0);

    if (irq < 0) {
        ret = irq;
        goto err_free_vectors;
    }

    dev->msix_irq = irq;

    /*
     * --------------------------------------------------------
     * Configure 82576 MSI-X mode.
     * --------------------------------------------------------
     */
    e82576_write_reg(
        dev,
        E1000_GPIE,
        E1000_GPIE_MSIX_MODE |
        E1000_GPIE_PBA |
        E1000_GPIE_EIAME |
        E1000_GPIE_NSICR);

    /*
     * --------------------------------------------------------
     * RX queue 0 -> MSI-X vector 0
     *
     * For 82576:
     *
     *   IVAR row    = 0
     *   RX queue 0  = low byte
     *   vector      = 0
     *   VALID       = 1
     * --------------------------------------------------------
     */
    ivar =
        E1000_IVAR_VALID |
        E1000_IVAR_VECTOR(0);

    e82576_write_reg(
        dev,
        E1000_IVAR,
        ivar);

    /*
     * --------------------------------------------------------
     * Keep ALL extended interrupt causes masked.
     * --------------------------------------------------------
     *
     * RXQ0 will be enabled later by ndo_open().
     */
    e82576_write_reg(
        dev,
        E1000_EIMC,
        0xffffffff);

    e82576_flush(dev);

    /*
     * Clear any stale pending extended interrupt causes.
     *
     * Do this before installing the handler.
     */
    e82576_read_reg(
        dev,
        E1000_EICR);

    /*
     * --------------------------------------------------------
     * Register Linux IRQ while hardware interrupts remain
     * masked.
     * --------------------------------------------------------
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

    /*
     * Hardware interrupts are intentionally still disabled.
     *
     * ndo_open() will enable RXQ0 after the RX ring and DMA
     * engine have been initialized.
     */
    dev_info(
        &dev->pdev->dev,
        "MSI-X configured:\n");

    dev_info(
        &dev->pdev->dev,
        "  GPIE      = 0x%08x\n",
        e82576_read_reg(
            dev,
            E1000_GPIE));

    dev_info(
        &dev->pdev->dev,
        "  IVAR      = 0x%08x\n",
        e82576_read_reg(
            dev,
            E1000_IVAR));

    dev_info(
        &dev->pdev->dev,
        "  EIMS      = 0x%08x\n",
        e82576_read_reg(
            dev,
            E1000_EIMS));

    dev_info(
        &dev->pdev->dev,
        "  MSI-X IRQ = %d\n",
        dev->msix_irq);

    return 0;


err_disable_msix:

    e82576_write_reg(
        dev,
        E1000_EIMC,
        0xffffffff);

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