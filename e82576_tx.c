#include "e82576.h"

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>


/*
 * ============================================================
 * TX CLEANUP
 * ============================================================
 */

static void e82576_clean_tx(
    struct e82576_device *dev)
{
    struct e82576_tx_desc *txd;
    struct e82576_tx_buffer *tx_buffer;
    u16 i;

    while (dev->tx_next_to_clean != dev->tx_next_to_use) {
        i = dev->tx_next_to_clean;

        txd = &dev->tx_ring[i];
        tx_buffer = &dev->tx_buffer[i];

        /*
         * Hardware has not finished with this descriptor yet.
         *
         * DD (Descriptor Done) is written by the NIC when the
         * descriptor has been completely processed.
         */
        if (!(le32_to_cpu(txd->upper) & E1000_TXD_STAT_DD))
            break;

        // dev_info(&dev->pdev->dev,
        //          "TX CLEAN: idx=%u "
        //          "upper=0x%08x "
        //          "TDH=%u "
        //          "TDT=%u\n",
        //          i,
        //          le32_to_cpu(txd->upper),
        //          e82576_read_reg(dev, E1000_TDH(0)),
        //          e82576_read_reg(dev, E1000_TDT(0)));

        /*
         * Unmap and free the skb associated with this descriptor.
         */
        if (tx_buffer->skb) {
            dma_unmap_single(
                &dev->pdev->dev,
                tx_buffer->dma,
                tx_buffer->skb->len,
                DMA_TO_DEVICE);

            dev_kfree_skb_any(tx_buffer->skb);

            tx_buffer->skb = NULL;
            tx_buffer->dma = 0;
        }

        /*
         * Clear the descriptor so that stale status/control
         * bits cannot affect a future transmission.
         */
        memset(txd, 0, sizeof(*txd));

        /*
         * Advance software cleanup index.
         */
        dev->tx_next_to_clean =
            (dev->tx_next_to_clean + 1) % E82576_NUM_TX_DESC;
    }

    /*
     * If the queue was stopped because the ring was full,
     * wake it once descriptors become available again.
     */
    if (netif_queue_stopped(dev->netdev) &&
        dev->tx_next_to_clean != dev->tx_next_to_use) {
        netif_wake_queue(dev->netdev);
    }
}


/*
 * ============================================================
 * TX RING SETUP
 * ============================================================
 */

int e82576_setup_tx_ring(
    struct e82576_device *dev)
{
    size_t size;

    /*
     * One legacy 82576 TX descriptor is 16 bytes.
     */
    size = E82576_NUM_TX_DESC * sizeof(struct e82576_tx_desc);

    dev->tx_ring =
        dma_alloc_coherent(
            &dev->pdev->dev,
            size,
            &dev->tx_ring_dma,
            GFP_KERNEL);

    if (!dev->tx_ring) {
        dev_err(
            &dev->pdev->dev,
            "Failed to allocate TX descriptor ring\n");

        return -ENOMEM;
    }

    memset(dev->tx_ring, 0, size);
    memset(dev->tx_buffer, 0, sizeof(dev->tx_buffer));

    dev->tx_next_to_use = 0;
    dev->tx_next_to_clean = 0;

    dev_info(
        &dev->pdev->dev,
        "TX ring allocated: "
        "cpu=%px dma=%pad "
        "size=%zu "
        "desc_size=%zu "
        "desc_count=%u\n",
        dev->tx_ring,
        &dev->tx_ring_dma,
        size,
        sizeof(struct e82576_tx_desc),
        E82576_NUM_TX_DESC);

    /*
     * --------------------------------------------------------
     * Program TX descriptor ring base.
     * --------------------------------------------------------
     */

    e82576_write_reg(dev, E1000_TDBAL(0), lower_32_bits(dev->tx_ring_dma));
    e82576_write_reg(dev, E1000_TDBAH(0), upper_32_bits(dev->tx_ring_dma));

    /*
     * TDLEN is specified in bytes.
     */
    e82576_write_reg(dev, E1000_TDLEN(0), size);

    /*
     * Hardware and software both start at descriptor 0.
     */
    e82576_write_reg(dev, E1000_TDH(0), 0);
    e82576_write_reg(dev, E1000_TDT(0), 0);

    e82576_flush(dev);

    /*
     * --------------------------------------------------------
     * Verify the programmed registers.
     * --------------------------------------------------------
     */

    dev_info(&dev->pdev->dev, "====================================\n");
    dev_info(&dev->pdev->dev, "TX DMA CONFIGURATION\n");
    dev_info(&dev->pdev->dev, "TDBAL = 0x%08x\n", e82576_read_reg(dev, E1000_TDBAL(0)));
    dev_info(&dev->pdev->dev, "TDBAH = 0x%08x\n", e82576_read_reg(dev, E1000_TDBAH(0)));
    dev_info(&dev->pdev->dev, "TDLEN = %u\n", e82576_read_reg(dev, E1000_TDLEN(0)));
    dev_info(&dev->pdev->dev, "TDH   = %u\n", e82576_read_reg(dev, E1000_TDH(0)));
    dev_info(&dev->pdev->dev, "TDT   = %u\n", e82576_read_reg(dev, E1000_TDT(0)));
    dev_info(&dev->pdev->dev, "====================================\n");

    return 0;
}


/*
 * ============================================================
 * TX RING FREE
 * ============================================================
 */

void e82576_free_tx_ring(
    struct e82576_device *dev)
{
    u16 i;

    if (!dev->tx_ring)
        return;

    /*
     * Release any skb still owned by software.
     */
    for (i = 0; i < E82576_NUM_TX_DESC; i++) {
        struct e82576_tx_buffer *tx_buffer;

        tx_buffer = &dev->tx_buffer[i];

        if (tx_buffer->skb) {
            dma_unmap_single(
                &dev->pdev->dev,
                tx_buffer->dma,
                tx_buffer->skb->len,
                DMA_TO_DEVICE);

            dev_kfree_skb_any(tx_buffer->skb);

            tx_buffer->skb = NULL;
            tx_buffer->dma = 0;
        }
    }

    /*
     * Free the coherent descriptor ring.
     */
    dma_free_coherent(
        &dev->pdev->dev,
        E82576_NUM_TX_DESC *
            sizeof(struct e82576_tx_desc),
        dev->tx_ring,
        dev->tx_ring_dma);

    dev->tx_ring = NULL;
    dev->tx_ring_dma = 0;

    dev->tx_next_to_use = 0;
    dev->tx_next_to_clean = 0;
}


/*
 * ============================================================
 * START TRANSMIT
 * ============================================================
 */

netdev_tx_t e82576_start_xmit(
    struct sk_buff *skb,
    struct net_device *netdev)
{
    struct e82576_device *dev;
    struct e82576_tx_desc *txd;
    struct e82576_tx_buffer *tx_buffer;
    struct ethhdr *eth;
    dma_addr_t dma;
    unsigned long flags;
    u16 i;
    u16 next;

    dev = netdev_priv(netdev);

    /*
     * --------------------------------------------------------
     * Identify the packet being transmitted.
     *
     * Do NOT assume IPv4 here.
     *
     * eth_hdr() works for Ethernet packets regardless of
     * whether the payload is IPv4, IPv6, ARP, etc.
     * --------------------------------------------------------
     */

    eth = eth_hdr(skb);

    // netdev_info(
    //     netdev,
    //     "TX skb: "
    //     "len=%u "
    //     "protocol=0x%04x "
    //     "src=%pM "
    //     "dst=%pM\n",
    //     skb->len,
    //     ntohs(eth->h_proto),
    //     eth->h_source,
    //     eth->h_dest);

    /*
     * --------------------------------------------------------
     * Serialize access to the TX ring.
     * --------------------------------------------------------
     */

    spin_lock_irqsave(&dev->tx_lock, flags);

    /*
     * Reclaim any descriptors that hardware has completed.
     */
    e82576_clean_tx(dev);

    i = dev->tx_next_to_use;

    next = (i + 1) % E82576_NUM_TX_DESC;

    /*
     * Keep one descriptor empty so that
     *
     *     next_to_use == next_to_clean
     *
     * always means the ring is full.
     */
    if (next == dev->tx_next_to_clean) {

        netdev_info(
            netdev,
            "TX ring full: "
            "ntu=%u ntc=%u\n",
            dev->tx_next_to_use,
            dev->tx_next_to_clean);

        netif_stop_queue(netdev);

        spin_unlock_irqrestore(&dev->tx_lock, flags);

        return NETDEV_TX_BUSY;
    }

    txd = &dev->tx_ring[i];
    tx_buffer = &dev->tx_buffer[i];

    /*
     * --------------------------------------------------------
     * Clear descriptor before reuse.
     * --------------------------------------------------------
     */

    memset(txd, 0, sizeof(*txd));

    /*
     * --------------------------------------------------------
     * Map packet data for DMA.
     * --------------------------------------------------------
     */

    dma = dma_map_single(
        &dev->pdev->dev,
        skb->data,
        skb->len,
        DMA_TO_DEVICE);

    if (dma_mapping_error(
            &dev->pdev->dev,
            dma)) {

        netdev_err(
            netdev,
            "TX DMA mapping failed\n");

        netdev->stats.tx_dropped++;

        spin_unlock_irqrestore(
            &dev->tx_lock,
            flags);

        dev_kfree_skb_any(skb);

        return NETDEV_TX_OK;
    }

    /*
     * --------------------------------------------------------
     * Save software ownership information.
     * --------------------------------------------------------
     */

    tx_buffer->skb = skb;
    tx_buffer->dma = dma;

    /*
     * --------------------------------------------------------
     * Populate the hardware descriptor.
     * --------------------------------------------------------
     *
     * One descriptor contains the entire skb.
     *
     * EOP:
     *     End of packet.
     *
     * IFCS:
     *     Hardware generates Ethernet FCS.
     *
     * RS:
     *     Request descriptor status/writeback.
     * --------------------------------------------------------
     */

    txd->buffer_addr = cpu_to_le64(dma);
    txd->lower = cpu_to_le32(
            skb->len |
            E1000_TXD_CMD_EOP |
            E1000_TXD_CMD_IFCS |
            E1000_TXD_CMD_RS);

    txd->upper = 0;

    /*
     * Ensure descriptor contents are visible to the device
     * before updating TDT.
     */
    dma_wmb();

    /*
     * --------------------------------------------------------
     * Debug information BEFORE advancing TDT.
     * --------------------------------------------------------
     */

    // netdev_info(
    //     netdev,
    //     "TX[%u] BEFORE TDT: "
    //     "ring_dma=%pad "
    //     "packet_dma=%pad "
    //     "skb=%px "
    //     "len=%u\n",
    //     i,
    //     &dev->tx_ring_dma,
    //     &dma,
    //     skb,
    //     skb->len);

    // netdev_info(
    //     netdev,
    //     "TX[%u] descriptor: "
    //     "buffer_addr=%pad "
    //     "lower=0x%08x "
    //     "upper=0x%08x\n",
    //     i,
    //     &txd->buffer_addr,
    //     le32_to_cpu(txd->lower),
    //     le32_to_cpu(txd->upper));

    // netdev_info(
    //     netdev,
    //     "TX BEFORE TDT: "
    //     "TDBAL=0x%08x "
    //     "TDBAH=0x%08x "
    //     "TDLEN=%u "
    //     "TDH=%u "
    //     "TDT=%u\n",
    //     e82576_read_reg(dev, E1000_TDBAL(0)),
    //     e82576_read_reg(dev, E1000_TDBAH(0)),
    //     e82576_read_reg(dev, E1000_TDLEN(0)),
    //     e82576_read_reg(dev, E1000_TDH(0)),
    //     e82576_read_reg(dev, E1000_TDT(0)));

    /*
     * --------------------------------------------------------
     * Advance software producer index.
     * --------------------------------------------------------
     */

    dev->tx_next_to_use = next;

    /*
     * --------------------------------------------------------
     * Tell hardware that descriptor i is available.
     *
     * This is the point at which the NIC is allowed to DMA
     * from the descriptor and packet buffer.
     * --------------------------------------------------------
     */

    e82576_write_reg(dev, E1000_TDT(0), dev->tx_next_to_use);
    e82576_flush(dev);

    /*
     * --------------------------------------------------------
     * Debug information AFTER advancing TDT.
     * --------------------------------------------------------
     */

    // netdev_info(
    //     netdev,
    //     "TX AFTER TDT: "
    //     "TDH=%u "
    //     "TDT=%u "
    //     "ntu=%u "
    //     "ntc=%u\n",
    //     e82576_read_reg(dev, E1000_TDH(0)),
    //     e82576_read_reg(dev, E1000_TDT(0)),
    //     dev->tx_next_to_use,
    //     dev->tx_next_to_clean);

    /*
     * --------------------------------------------------------
     * Schedule TX completion polling.
     * --------------------------------------------------------
     */

    schedule_delayed_work(&dev->tx_clean_work, msecs_to_jiffies(1));

    /*
     * --------------------------------------------------------
     * Stop queue when only one descriptor remains available.
     * --------------------------------------------------------
     */

    if (((dev->tx_next_to_use + 1) %
         E82576_NUM_TX_DESC) ==
        dev->tx_next_to_clean) {

        netdev_info(
            netdev,
            "TX queue becoming full: "
            "ntu=%u ntc=%u\n",
            dev->tx_next_to_use,
            dev->tx_next_to_clean);

        netif_stop_queue(netdev);
    }

    spin_unlock_irqrestore(
        &dev->tx_lock,
        flags);

    return NETDEV_TX_OK;
}


/*
 * ============================================================
 * TX CLEANUP WORK
 * ============================================================
 */

void e82576_tx_clean_work(
    struct work_struct *work)
{
    struct e82576_device *dev;
    bool pending;

    dev =
        container_of(
            to_delayed_work(work),
            struct e82576_device,
            tx_clean_work);

    spin_lock(&dev->tx_lock);

    e82576_clean_tx(dev);

    pending =
        dev->tx_next_to_clean !=
        dev->tx_next_to_use;

    spin_unlock(&dev->tx_lock);

    /*
     * Keep polling while there are outstanding descriptors.
     */
    if (pending) {
        schedule_delayed_work(
            &dev->tx_clean_work,
            msecs_to_jiffies(1));
    }
}