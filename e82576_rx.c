#include "e82576.h"

/*
 * ============================================================
 * RX RING
 * ============================================================
 */

int e82576_setup_rx_ring(
    struct e82576_device *dev)
{
    size_t size;
    int i;

    size =
        E82576_NUM_RX_DESC *
        sizeof(struct e82576_rx_desc);

    dev->rx_ring =
        dma_alloc_coherent(
            &dev->pdev->dev,
            size,
            &dev->rx_ring_dma,
            GFP_KERNEL);

    if (!dev->rx_ring) {
        dev_err(
            &dev->pdev->dev,
            "Failed to allocate RX descriptor ring\n");

        return -ENOMEM;
    }

    memset(
        dev->rx_ring,
        0,
        size);

    memset(
        dev->rx_buffer,
        0,
        sizeof(dev->rx_buffer));

    /*
     * Allocate one skb for every RX descriptor.
     */
    for (i = 0; i < E82576_NUM_RX_DESC; i++) {

        struct sk_buff *skb;
        dma_addr_t dma;
        struct e82576_rx_desc *desc;

        skb = netdev_alloc_skb(
            dev->netdev,
            E82576_RX_BUFFER_SIZE);

        if (!skb) {
            dev_err(
                &dev->pdev->dev,
                "Failed to allocate RX skb %d\n",
                i);

            goto err;
        }

        dma = dma_map_single(
            &dev->pdev->dev,
            skb->data,
            E82576_RX_BUFFER_SIZE,
            DMA_FROM_DEVICE);

        if (dma_mapping_error(
                &dev->pdev->dev,
                dma)) {

            dev_err(
                &dev->pdev->dev,
                "Failed to map RX buffer %d\n",
                i);

            dev_kfree_skb(skb);
            goto err;
        }

        dev->rx_buffer[i].skb = skb;
        dev->rx_buffer[i].dma = dma;

        desc = &dev->rx_ring[i];

        desc->buffer_addr =
            cpu_to_le64(dma);
    }

    /*
     * Hardware owns all RX descriptors.
     */
    dev->rx_next_to_clean = 0;

    /*
     * Program descriptor ring.
     */
    e82576_write_reg(
        dev,
        E1000_RDBAL(0),
        lower_32_bits(dev->rx_ring_dma));

    e82576_write_reg(
        dev,
        E1000_RDBAH(0),
        upper_32_bits(dev->rx_ring_dma));

    e82576_write_reg(
        dev,
        E1000_RDLEN(0),
        size);

    e82576_write_reg(
        dev,
        E1000_RDH(0),
        0);

    /*
     * Tail points to the last descriptor supplied
     * to hardware.
     */
    e82576_write_reg(
        dev,
        E1000_RDT(0),
        E82576_NUM_RX_DESC - 1);

    e82576_flush(dev);

    dev_info(
        &dev->pdev->dev,
        "DMA: "
        "RDBAL=0x%08x "
        "RDBAH=0x%08x "
        "RDLEN=%u "
        "RDH=%u "
        "RDT=%u\n",
        e82576_read_reg(dev, E1000_RDBAL(0)),
        e82576_read_reg(dev, E1000_RDBAH(0)),
        e82576_read_reg(dev, E1000_RDLEN(0)),
        e82576_read_reg(dev, E1000_RDH(0)),
        e82576_read_reg(dev, E1000_RDT(0)));

    return 0;

err:

    while (--i >= 0) {
        if (dev->rx_buffer[i].skb) {

            dma_unmap_single(
                &dev->pdev->dev,
                dev->rx_buffer[i].dma,
                E82576_RX_BUFFER_SIZE,
                DMA_FROM_DEVICE);

            dev_kfree_skb(
                dev->rx_buffer[i].skb);

            dev->rx_buffer[i].skb = NULL;
            dev->rx_buffer[i].dma = 0;
        }
    }

    dma_free_coherent(
        &dev->pdev->dev,
        size,
        dev->rx_ring,
        dev->rx_ring_dma);

    dev->rx_ring = NULL;
    dev->rx_ring_dma = 0;

    return -ENOMEM;
}

void e82576_free_rx_ring(
    struct e82576_device *dev)
{
    size_t size;
    int i;

    for (i = 0; i < E82576_NUM_RX_DESC; i++) {

        if (!dev->rx_buffer[i].skb)
            continue;

        dma_unmap_single(
            &dev->pdev->dev,
            dev->rx_buffer[i].dma,
            E82576_RX_BUFFER_SIZE,
            DMA_FROM_DEVICE);

        dev_kfree_skb(
            dev->rx_buffer[i].skb);

        dev->rx_buffer[i].skb = NULL;
        dev->rx_buffer[i].dma = 0;
    }

    if (!dev->rx_ring)
        return;

    size =
        E82576_NUM_RX_DESC *
        sizeof(struct e82576_rx_desc);

    dma_free_coherent(
        &dev->pdev->dev,
        size,
        dev->rx_ring,
        dev->rx_ring_dma);

    dev->rx_ring = NULL;
    dev->rx_ring_dma = 0;

    dev->rx_next_to_clean = 0;
}

void e82576_enable_dma(
    struct e82576_device *dev)
{
    u32 rxdctl;
    u32 txdctl;

    /*
     * Enable RX descriptor queue 0.
     */
    rxdctl = e82576_read_reg(
        dev,
        E1000_RXDCTL(0));

    rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;

    e82576_write_reg(
        dev,
        E1000_RXDCTL(0),
        rxdctl);


    /*
     * Enable TX descriptor queue 0.
     */
    txdctl = e82576_read_reg(
        dev,
        E1000_TXDCTL(0));

    txdctl |= E1000_TXDCTL_QUEUE_ENABLE;

    e82576_write_reg(
        dev,
        E1000_TXDCTL(0),
        txdctl);


    /*
     * Enable receiver.
     *
     * 2048-byte buffers are the default size encoding.
     */
    e82576_write_reg(
        dev,
        E1000_RCTL,
        E1000_RCTL_EN |
        E1000_RCTL_BAM |
        E1000_RCTL_SECRC);


    /*
     * Enable transmitter.
     */
    e82576_write_reg(
        dev,
        E1000_TCTL,
        E1000_TCTL_EN |
        E1000_TCTL_PSP);

    e82576_flush(dev);
}