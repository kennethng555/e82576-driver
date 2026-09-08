#include "e82576.h"

/*
 * ============================================================
 * TX RING
 * ============================================================
 */

int e82576_setup_tx_ring(
    struct e82576_device *dev)
{
    size_t size;

    size = E82576_NUM_TX_DESC *
           sizeof(struct e82576_tx_desc);

    /*
     * The descriptor ring must be DMA accessible by the NIC.
     */
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

    memset(
        dev->tx_ring,
        0,
        size);

    memset(
        dev->tx_buffer,
        0,
        sizeof(dev->tx_buffer));

    dev->tx_next_to_use = 0;
    dev->tx_next_to_clean = 0;

    /*
     * Program descriptor ring base.
     */
    e82576_write_reg(
        dev,
        E1000_TDBAL(0),
        lower_32_bits(dev->tx_ring_dma));

    e82576_write_reg(
        dev,
        E1000_TDBAH(0),
        upper_32_bits(dev->tx_ring_dma));

    /*
     * Ring length is in bytes.
     */
    e82576_write_reg(
        dev,
        E1000_TDLEN(0),
        size);

    /*
     * Hardware starts at descriptor 0.
     */
    e82576_write_reg(
        dev,
        E1000_TDH(0),
        0);

    e82576_write_reg(
        dev,
        E1000_TDT(0),
        0);

    e82576_flush(dev);

    dev_info(
        &dev->pdev->dev,
        "DMA: "
        "TDBAL=0x%08x "
        "TDBAH=0x%08x "
        "TDLEN=%u "
        "TDH=%u "
        "TDT=%u\n",
        e82576_read_reg(dev, E1000_TDBAL(0)),
        e82576_read_reg(dev, E1000_TDBAH(0)),
        e82576_read_reg(dev, E1000_TDLEN(0)),
        e82576_read_reg(dev, E1000_TDH(0)),
        e82576_read_reg(dev, E1000_TDT(0)));

    return 0;
}

void e82576_free_tx_ring(
    struct e82576_device *dev)
{
    size_t size;

    if (!dev->tx_ring)
        return;

    size =
        E82576_NUM_TX_DESC *
        sizeof(struct e82576_tx_desc);

    dma_free_coherent(
        &dev->pdev->dev,
        size,
        dev->tx_ring,
        dev->tx_ring_dma);

    dev->tx_ring = NULL;
    dev->tx_ring_dma = 0;

    memset(
        dev->tx_buffer,
        0,
        sizeof(dev->tx_buffer));

    dev->tx_next_to_use = 0;
    dev->tx_next_to_clean = 0;
}


netdev_tx_t e82576_start_xmit(
    struct sk_buff *skb,
    struct net_device *netdev)
{
    struct e82576_device *dev = netdev_priv(netdev);

    dev_warn_ratelimited(&dev->pdev->dev, "TX not implemented yet\n");
    dev_kfree_skb(skb);

    netdev->stats.tx_dropped++;

    return NETDEV_TX_OK;
}
