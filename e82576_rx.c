#include "e82576.h"

#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>

#define E82576_RX_QUEUE_ENABLE_TIMEOUT 1000

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

    size = E82576_NUM_RX_DESC * sizeof(struct e82576_rx_desc);

    dev->rx_ring = dma_alloc_coherent(&dev->pdev->dev, size, &dev->rx_ring_dma, GFP_KERNEL);

    if (!dev->rx_ring) {
        dev_err(&dev->pdev->dev, "Failed to allocate RX descriptor ring\n");
        return -ENOMEM;
    }

    memset(dev->rx_ring, 0, size);
    memset(dev->rx_buffer, 0, sizeof(dev->rx_buffer));

    /*
     * Allocate one skb for every RX descriptor.
     */
    for (i = 0; i < E82576_NUM_RX_DESC; i++) {
        struct sk_buff *skb;
        dma_addr_t dma;
        struct e82576_rx_desc *desc;

        skb = netdev_alloc_skb(dev->netdev, E82576_RX_BUFFER_SIZE);

        if (!skb) {
            dev_err(&dev->pdev->dev, "Failed to allocate RX skb %d\n", i);
            goto err;
        }

        dma = dma_map_single(&dev->pdev->dev, skb->data, E82576_RX_BUFFER_SIZE, DMA_FROM_DEVICE);
        if (dma_mapping_error(&dev->pdev->dev, dma)) {
            dev_err(&dev->pdev->dev, "Failed to map RX buffer %d\n", i);
            dev_kfree_skb(skb);
            goto err;
        }

        dev->rx_buffer[i].skb = skb;
        dev->rx_buffer[i].dma = dma;

        desc = &dev->rx_ring[i];

        desc->buffer_addr = cpu_to_le64(dma);
    }

    /*
     * Hardware owns all descriptors.
     */
    dev->rx_next_to_clean = 0;

    /*
     * Make sure descriptor writes are visible
     * before handing the ring to hardware.
     */
    dma_wmb();

    /*
     * Program RX descriptor ring.
     */
    e82576_write_reg(dev, E1000_RDBAL(0), lower_32_bits(dev->rx_ring_dma));
    e82576_write_reg(dev, E1000_RDBAH(0), upper_32_bits(dev->rx_ring_dma));
    e82576_write_reg(dev, E1000_RDLEN(0), size);
    e82576_write_reg(dev, E1000_RDH(0), 0);

    /*
     * Hardware owns descriptors 0..N-1.
     */
    // e82576_write_reg(dev, E1000_RDT(0), E82576_NUM_RX_DESC - 1);
    e82576_flush(dev);

    dev_info(&dev->pdev->dev, "====================================\n");
    dev_info(&dev->pdev->dev, "RX DMA CONFIGURATION\n");
    dev_info(&dev->pdev->dev, "RDBAL = 0x%08x\n", e82576_read_reg(dev, E1000_RDBAL(0)));
    dev_info(&dev->pdev->dev, "RDBAH = 0x%08x\n", e82576_read_reg(dev, E1000_RDBAH(0)));
    dev_info(&dev->pdev->dev, "RDLEN = %u\n", e82576_read_reg(dev, E1000_RDLEN(0)));
    dev_info(&dev->pdev->dev, "RDH   = %u\n", e82576_read_reg(dev, E1000_RDH(0)));
    dev_info(&dev->pdev->dev, "RDT   = %u\n", e82576_read_reg(dev, E1000_RDT(0)));
    dev_info(&dev->pdev->dev, "====================================\n");

    dev_info(&dev->pdev->dev, "RX RING: cpu=%px dma=%pad\n", dev->rx_ring, &dev->rx_ring_dma);

    for (int i = 0; i < 4; i++) {
        dev_info(&dev->pdev->dev,
                "RX DESC[%d]: buffer=%016llx status=%02x length=%u\n",
                i,
                le64_to_cpu(dev->rx_ring[i].buffer_addr),
                dev->rx_ring[i].status,
                le16_to_cpu(dev->rx_ring[i].length));
    }

    return 0;

err:

    while (--i >= 0) {
        if (dev->rx_buffer[i].skb) {
            dma_unmap_single(&dev->pdev->dev, dev->rx_buffer[i].dma, E82576_RX_BUFFER_SIZE, DMA_FROM_DEVICE);
            dev_kfree_skb(dev->rx_buffer[i].skb);

            dev->rx_buffer[i].skb = NULL;
            dev->rx_buffer[i].dma = 0;
        }
    }

    dma_free_coherent(&dev->pdev->dev, size, dev->rx_ring, dev->rx_ring_dma);

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

        dma_unmap_single(&dev->pdev->dev, dev->rx_buffer[i].dma, E82576_RX_BUFFER_SIZE, DMA_FROM_DEVICE);

        dev_kfree_skb(dev->rx_buffer[i].skb);

        dev->rx_buffer[i].skb = NULL;
        dev->rx_buffer[i].dma = 0;
    }

    if (!dev->rx_ring)
        return;

    size = E82576_NUM_RX_DESC * sizeof(struct e82576_rx_desc);

    dma_free_coherent(&dev->pdev->dev, size, dev->rx_ring, dev->rx_ring_dma);

    dev->rx_ring = NULL;
    dev->rx_ring_dma = 0;

    dev->rx_next_to_clean = 0;
}

/*
 * ============================================================
 * RX POLLING
 * ============================================================
 */

//  static int e82576_poll_rx(
//     struct e82576_device *dev)
// {
//     u16 i;
//     struct e82576_rx_desc *desc;
//     u8 status;

//     i = dev->rx_next_to_clean;

//     desc = &dev->rx_ring[i];

//     dma_rmb();

//     status = READ_ONCE(desc->status);

//     netdev_info(
//         dev->netdev,
//         "RX POLL: idx=%u status=0x%02x "
//         "RDH=%u RDT=%u\n",
//         i,
//         status,
//         e82576_read_reg(dev, E1000_RDH(0)),
//         e82576_read_reg(dev, E1000_RDT(0)));

//     if (!(status & E1000_RXD_STAT_DD))
//         return 0;

//     /*
//      * Existing RX packet processing goes here.
//      */

//     return 0;
// }

static int e82576_poll_rx(
    struct e82576_device *dev)
{
    int packets = 0;

    while (packets < 64) {

        u16 i;
        struct e82576_rx_desc *desc;
        struct e82576_rx_buffer *rx_buffer;
        struct sk_buff *skb;
        struct sk_buff *new_skb;
        u16 length;
        u8 status;
        u8 errors;

        i = dev->rx_next_to_clean;
        desc = &dev->rx_ring[i];
        rx_buffer = &dev->rx_buffer[i];

        /*
         * Make sure we see the descriptor status
         * written by the NIC.
         */
        dma_rmb();

        status = READ_ONCE(desc->status);

        /*
         * DD = Descriptor Done.
         *
         * If hardware has not completed this
         * descriptor, stop polling.
         */
        if (!(status & E1000_RXD_STAT_DD))
            break;

        length = le16_to_cpu(desc->length);
        errors = desc->errors;
        skb = rx_buffer->skb;

        /*
         * Diagnostic output for the first
         * RX implementation.
         */
        netdev_info(
            dev->netdev,
            "RX[%u]: "
            "status=0x%02x "
            "errors=0x%02x "
            "length=%u "
            "RDT=%u "
            "RDH=%u\n",
            i,
            status,
            errors,
            length,
            e82576_read_reg(dev, E1000_RDT(0)),
            e82576_read_reg(dev, E1000_RDH(0)));

        /*
         * Make the DMA buffer visible to the CPU.
         */
        dma_sync_single_for_cpu(
            &dev->pdev->dev,
            rx_buffer->dma,
            E82576_RX_BUFFER_SIZE,
            DMA_FROM_DEVICE);

        /*
         * Drop packets with hardware RX errors.
         */
        if (errors || length == 0) {

            dev->netdev->stats.rx_dropped++;

            dma_sync_single_for_device(
                &dev->pdev->dev,
                rx_buffer->dma,
                E82576_RX_BUFFER_SIZE,
                DMA_FROM_DEVICE);

            memset(desc, 0, sizeof(*desc));

            desc->buffer_addr = cpu_to_le64(rx_buffer->dma);

            dma_wmb();

            packets++;

            dev->rx_next_to_clean =
                (dev->rx_next_to_clean + 1) %
                E82576_NUM_RX_DESC;

            e82576_write_reg(
                dev,
                E1000_RDT(0),
                (dev->rx_next_to_clean +
                 E82576_NUM_RX_DESC - 1) %
                E82576_NUM_RX_DESC);

            continue;
        }

        /*
         * Allocate a replacement buffer BEFORE
         * giving the descriptor back to hardware.
         */
        new_skb = netdev_alloc_skb(dev->netdev, E82576_RX_BUFFER_SIZE);

        if (!new_skb) {

            dev->netdev->stats.rx_dropped++;

            dma_sync_single_for_device(
                &dev->pdev->dev,
                rx_buffer->dma,
                E82576_RX_BUFFER_SIZE,
                DMA_FROM_DEVICE);

            break;
        }

        /*
         * Copy the received packet out of the
         * DMA buffer.
         *
         * This deliberately avoids page recycling
         * for the first RX milestone.
         */
        skb_put(new_skb, length);

        memcpy(new_skb->data, skb->data, length);

        /*
         * Give the original DMA buffer back
         * to the NIC.
         */
        dma_sync_single_for_device(
            &dev->pdev->dev,
            rx_buffer->dma,
            E82576_RX_BUFFER_SIZE,
            DMA_FROM_DEVICE);

        /*
         * Prepare the new descriptor.
         */
        memset(desc, 0, sizeof(*desc));

        desc->buffer_addr = cpu_to_le64(rx_buffer->dma);

        /*
         * Make descriptor visible before
         * updating RDT.
         */
        dma_wmb();

        /*
         * Ethernet protocol handling.
         */
        new_skb->protocol = eth_type_trans(new_skb, dev->netdev);
        new_skb->ip_summed = CHECKSUM_NONE;

        /*
         * Deliver packet to Linux.
         */
        netif_receive_skb(new_skb);

        dev->netdev->stats.rx_packets++;
        dev->netdev->stats.rx_bytes += length;

        /*
         * Advance software consumer.
         */
        dev->rx_next_to_clean = (dev->rx_next_to_clean + 1) % E82576_NUM_RX_DESC;

        /*
         * Give this descriptor back to hardware.
         *
         * RDT points to the last descriptor
         * owned by hardware.
         */
        e82576_write_reg(
            dev,
            E1000_RDT(0),
            (dev->rx_next_to_clean +
             E82576_NUM_RX_DESC - 1) %
            E82576_NUM_RX_DESC);

        packets++;
    }

    if (packets)
        e82576_flush(dev);

    return packets;
}

void e82576_rx_poll_work(
    struct work_struct *work)
{
    struct e82576_device *dev =
        container_of(
            to_delayed_work(work),
            struct e82576_device,
            rx_poll_work);

    while (1) {

        u16 idx = dev->rx_next_to_clean;

        struct e82576_rx_desc *desc =
            &dev->rx_ring[idx];

        u8 status =
            READ_ONCE(desc->status);

        /*
         * Hardware has not completed this descriptor.
         */
        if (!(status & E1000_RXD_STAT_DD))
            break;

        /*
         * At this point we own the descriptor.
         */
        {
            struct sk_buff *skb;
            struct sk_buff *new_skb;
            dma_addr_t new_dma;
            unsigned int length;

            skb =
                dev->rx_buffer[idx].skb;

            length =
                le16_to_cpu(desc->length);

            dev_info(
                &dev->pdev->dev,
                "RX PACKET: idx=%u status=0x%02x length=%u\n",
                idx,
                status,
                length);

            /*
             * Remove the DMA mapping because Linux
             * will now own the received packet.
             */
            dma_unmap_single(
                &dev->pdev->dev,
                dev->rx_buffer[idx].dma,
                E82576_RX_BUFFER_SIZE,
                DMA_FROM_DEVICE);

            /*
             * Tell skb how much data was received.
             */
            skb_put(skb, length);

            /*
             * Set protocol based on Ethernet header.
             */
            skb->protocol =
                eth_type_trans(
                    skb,
                    dev->netdev);

            /*
             * Give packet to Linux.
             */
            netif_receive_skb(skb);

            /*
             * Allocate replacement buffer.
             */
            new_skb =
                netdev_alloc_skb(
                    dev->netdev,
                    E82576_RX_BUFFER_SIZE);

            if (!new_skb) {

                dev_err(
                    &dev->pdev->dev,
                    "RX skb allocation failed at idx=%u\n",
                    idx);

                /*
                 * We cannot safely return this descriptor
                 * to hardware without a replacement buffer.
                 */
                break;
            }

            new_dma =
                dma_map_single(
                    &dev->pdev->dev,
                    new_skb->data,
                    E82576_RX_BUFFER_SIZE,
                    DMA_FROM_DEVICE);

            if (dma_mapping_error(
                    &dev->pdev->dev,
                    new_dma)) {

                dev_kfree_skb(new_skb);

                dev_err(
                    &dev->pdev->dev,
                    "RX DMA mapping failed at idx=%u\n",
                    idx);

                break;
            }

            /*
             * Install replacement buffer.
             */
            dev->rx_buffer[idx].skb =
                new_skb;

            dev->rx_buffer[idx].dma =
                new_dma;

            desc->buffer_addr =
                cpu_to_le64(new_dma);

            /*
             * Clear hardware writeback fields.
             */
            desc->length = 0;
            desc->checksum = 0;
            desc->status = 0;
            desc->errors = 0;
            desc->special = 0;

            /*
             * Make descriptor writes visible before
             * returning descriptor ownership.
             */
            dma_wmb();

            /*
             * Return this descriptor to hardware.
             */
            e82576_write_reg(
                dev,
                E1000_RDT(0),
                idx);

            /*
             * Move to next descriptor.
             */
            dev->rx_next_to_clean =
                (idx + 1) %
                E82576_NUM_RX_DESC;
        }
    }

    /*
     * Continue polling.
     */
    schedule_delayed_work(
        &dev->rx_poll_work,
        msecs_to_jiffies(10));
}

void e82576_enable_dma(
    struct e82576_device *dev)
{
    u32 rxdctl;
    u32 txdctl;
    u32 rctl;
    u32 tctl;
    int i;

    /*
     * ------------------------------------------------------------
     * RX
     * ------------------------------------------------------------
     */

    /*
     * Enable receiver.
     */
    rctl = e82576_read_reg(dev, E1000_RCTL);
    rctl |= E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC;

    e82576_write_reg(dev, E1000_RCTL, rctl);

    /*
     * Enable RX descriptor queue 0.
     */
    rxdctl = e82576_read_reg(dev, E1000_RXDCTL(0));
    rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;

    e82576_write_reg(dev, E1000_RXDCTL(0), rxdctl);
    e82576_flush(dev);

    /*
     * Intel requires us to poll RXDCTL.ENABLE until the
     * hardware reports that the queue is actually enabled.
     */
    for (i = 0; i < E82576_RX_QUEUE_ENABLE_TIMEOUT; i++) {
        rxdctl = e82576_read_reg(dev, E1000_RXDCTL(0));
        if (rxdctl & E1000_RXDCTL_QUEUE_ENABLE)
            break;

        udelay(1);
    }

    if (!(rxdctl & E1000_RXDCTL_QUEUE_ENABLE)) {
        dev_err(&dev->pdev->dev, "RX queue failed to enable: RXDCTL=0x%08x\n", rxdctl);
        return;
    }

    /*
     * Now that RXDCTL.ENABLE has been observed set,
     * give the hardware ownership of descriptors 0..N-1.
     */
    e82576_write_reg(dev, E1000_RDT(0), E82576_NUM_RX_DESC - 1);
    e82576_flush(dev);

    /*
     * ------------------------------------------------------------
     * TX
     * ------------------------------------------------------------
     */

    txdctl = e82576_read_reg(dev, E1000_TXDCTL(0));
    txdctl |= E1000_TXDCTL_QUEUE_ENABLE;

    e82576_write_reg(dev, E1000_TXDCTL(0), txdctl);

    /*
     * Enable transmitter.
     */
    tctl = e82576_read_reg(dev, E1000_TCTL);
    tctl |= E1000_TCTL_EN | E1000_TCTL_PSP;

    e82576_write_reg(dev, E1000_TCTL, tctl);
    e82576_flush(dev);

    dev_info(&dev->pdev->dev, "====================================\n");
    dev_info(&dev->pdev->dev, "DMA enabled: ");
    dev_info(&dev->pdev->dev, "RXDCTL=0x%08x ", e82576_read_reg(dev, E1000_RXDCTL(0)));
    dev_info(&dev->pdev->dev, "RCTL=0x%08x ", e82576_read_reg(dev, E1000_RCTL));
    dev_info(&dev->pdev->dev, "RDH=%u\n", e82576_read_reg(dev, E1000_RDH(0)));
    dev_info(&dev->pdev->dev, "RDT=%u\n", e82576_read_reg(dev, E1000_RDT(0)));
    dev_info(&dev->pdev->dev, "TXDCTL=0x%08x ", e82576_read_reg(dev, E1000_TXDCTL(0)));
    dev_info(&dev->pdev->dev, "TCTL=0x%08x\n", e82576_read_reg(dev, E1000_TCTL));
    dev_info(&dev->pdev->dev, "====================================\n");

}