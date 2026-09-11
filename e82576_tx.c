#include "e82576.h"


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
         */
        if (!(le32_to_cpu(txd->upper) & E1000_TXD_STAT_DD))
            break;

        /*
         * Unmap the packet buffer.
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
         * Descriptor is available again.
         */
        memset(txd, 0, sizeof(*txd));

        dev->tx_next_to_clean = (dev->tx_next_to_clean + 1) % E82576_NUM_TX_DESC;
    }
}


/*
 * ============================================================
 * TX RING
 * ============================================================
 */

int e82576_setup_tx_ring(
    struct e82576_device *dev)
{
    size_t size;

    size = E82576_NUM_TX_DESC * sizeof(struct e82576_tx_desc);

    /*
     * The descriptor ring must be DMA accessible by the NIC.
     */
    dev->tx_ring = dma_alloc_coherent(&dev->pdev->dev, size, &dev->tx_ring_dma, GFP_KERNEL);
    if (!dev->tx_ring) {
        dev_err(&dev->pdev->dev, "Failed to allocate TX descriptor ring\n");
        return -ENOMEM;
    }

    memset(dev->tx_ring, 0, size);
    memset(dev->tx_buffer, 0, sizeof(dev->tx_buffer));

    dev->tx_next_to_use = 0;
    dev->tx_next_to_clean = 0;

    /*
     * Program descriptor ring base.
     */
    e82576_write_reg(dev, E1000_TDBAL(0), lower_32_bits(dev->tx_ring_dma));
    e82576_write_reg(dev, E1000_TDBAH(0), upper_32_bits(dev->tx_ring_dma));

    /*
     * Ring length is in bytes.
     */
    e82576_write_reg(dev, E1000_TDLEN(0), size);

    /*
     * Hardware starts at descriptor 0.
     */
    e82576_write_reg(dev, E1000_TDH(0), 0);
    e82576_write_reg(dev, E1000_TDT(0), 0);
    e82576_flush(dev);

    dev_info(&dev->pdev->dev, "====================================\n");
    dev_info(&dev->pdev->dev, "TX DMA: ");
    dev_info(&dev->pdev->dev, "TDBAL=0x%08x ", e82576_read_reg(dev, E1000_TDBAL(0)));
    dev_info(&dev->pdev->dev, "TDBAH=0x%08x ", e82576_read_reg(dev, E1000_TDBAH(0)));
    dev_info(&dev->pdev->dev, "TDLEN=%u ", e82576_read_reg(dev, E1000_TDLEN(0)));
    dev_info(&dev->pdev->dev, "TDH=%u ", e82576_read_reg(dev, E1000_TDH(0)));
    dev_info(&dev->pdev->dev, "TDT=%u\n", e82576_read_reg(dev, E1000_TDT(0)));
    dev_info(&dev->pdev->dev, "====================================\n");

    return 0;
}

void e82576_free_tx_ring(
    struct e82576_device *dev)
{
    u16 i;

    if (!dev->tx_ring)
        return;

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

    dma_free_coherent(
        &dev->pdev->dev,
        sizeof(struct e82576_tx_desc) * E82576_NUM_TX_DESC,
        dev->tx_ring,
        dev->tx_ring_dma);

    dev->tx_ring = NULL;
    dev->tx_ring_dma = 0;

    dev->tx_next_to_use = 0;
    dev->tx_next_to_clean = 0;
}


netdev_tx_t e82576_start_xmit(
    struct sk_buff *skb,
    struct net_device *netdev)
{
    struct e82576_device *dev;
    struct e82576_tx_desc *txd;
    struct e82576_tx_buffer *tx_buffer;
    dma_addr_t dma;
    unsigned long flags;
    u16 i;
    u16 next;

    dev = netdev_priv(netdev);

    spin_lock_irqsave(&dev->tx_lock, flags);

    /*
     * Reclaim descriptors that hardware has already completed.
     */
    e82576_clean_tx(dev);

    i = dev->tx_next_to_use;

    next = (i + 1) % E82576_NUM_TX_DESC;

    /*
     * Keep one descriptor empty so that
     * next_to_use == next_to_clean is never ambiguous.
     */
    if (next == dev->tx_next_to_clean) {
        netif_stop_queue(netdev);
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        return NETDEV_TX_BUSY;
    }

    txd = &dev->tx_ring[i];
    tx_buffer = &dev->tx_buffer[i];

    /*
     * Map the skb for DMA.
     */
    dma = dma_map_single(
        &dev->pdev->dev,
        skb->data,
        skb->len,
        DMA_TO_DEVICE);

    if (dma_mapping_error(&dev->pdev->dev, dma)) {
        netdev->stats.tx_dropped++;
        spin_unlock_irqrestore(&dev->tx_lock, flags);
        dev_kfree_skb_any(skb);

        return NETDEV_TX_OK;
    }

    /*
     * Save software ownership information.
     */
    tx_buffer->skb = skb;
    tx_buffer->dma = dma;

    /*
     * Program the descriptor.
     */
    txd->buffer_addr = cpu_to_le64(dma);

    txd->lower =
        cpu_to_le32(skb->len |
            E1000_TXD_CMD_EOP |
            E1000_TXD_CMD_IFCS |
            E1000_TXD_CMD_RS);

    /*
     * Clear status/control bits in upper.
     */
    txd->upper = 0;

    /*
     * Make descriptor visible before advancing TDT.
     */
    dma_wmb();

    /*
     * Advance hardware tail pointer.
     */
    dev->tx_next_to_use = next;

    e82576_write_reg(dev, E1000_TDT(0), dev->tx_next_to_use);
    e82576_flush(dev);

    /*
     * Start cleanup polling.
     */
    schedule_delayed_work(&dev->tx_clean_work, msecs_to_jiffies(1));

    /*
     * Stop the queue if we are now nearly full.
     */
    if (((dev->tx_next_to_use + 1) % E82576_NUM_TX_DESC) == dev->tx_next_to_clean) {
        netif_stop_queue(netdev);
    }

    spin_unlock_irqrestore(&dev->tx_lock, flags);

    return NETDEV_TX_OK;
}

void e82576_tx_clean_work(
    struct work_struct *work)
{
    struct e82576_device *dev;

    dev = container_of(to_delayed_work(work), struct e82576_device, tx_clean_work);

    spin_lock(&dev->tx_lock);
    e82576_clean_tx(dev);
    spin_unlock(&dev->tx_lock);

    /*
     * Continue polling while packets are outstanding.
     */
    if (dev->tx_next_to_clean != dev->tx_next_to_use) {
        schedule_delayed_work(&dev->tx_clean_work, msecs_to_jiffies(1));
    }
}