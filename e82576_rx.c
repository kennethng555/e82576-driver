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

int e82576_setup_rx_ring(struct e82576_device *dev)
{
    size_t size;
    int i;
    u32 drxmxod;
    u32 srrctl;

    size = E82576_NUM_RX_DESC * sizeof(struct e82576_rx_desc);

    dev->rx_ring = dma_alloc_coherent(&dev->pdev->dev,
                                      size,
                                      &dev->rx_ring_dma,
                                      GFP_KERNEL);

    if (!dev->rx_ring) {
        dev_err(&dev->pdev->dev,
                "Failed to allocate RX descriptor ring\n");
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
            dev_err(&dev->pdev->dev,
                    "Failed to allocate RX skb %d\n", i);
            goto err;
        }

        dma = dma_map_single(&dev->pdev->dev,
                             skb->data,
                             E82576_RX_BUFFER_SIZE,
                             DMA_FROM_DEVICE);

        if (dma_mapping_error(&dev->pdev->dev, dma)) {
            dev_err(&dev->pdev->dev,
                    "Failed to map RX buffer %d\n", i);

            dev_kfree_skb(skb);
            goto err;
        }

        dev->rx_buffer[i].skb = skb;
        dev->rx_buffer[i].dma = dma;

        desc = &dev->rx_ring[i];

        desc->buffer_addr = cpu_to_le64(dma);
        desc->length = 0;
        desc->checksum = 0;
        desc->status = 0;
        desc->errors = 0;
        desc->special = 0;
    }

    /*
     * Hardware owns all descriptors.
     */
    dev->rx_next_to_clean = 0;

    /*
     * Make descriptor writes visible before giving the ring
     * to the NIC.
     */
    dma_wmb();

    /*
     * ---------------------------------------------------------
     * Configure RX descriptor format / packet buffer size.
     * ---------------------------------------------------------
     */

    srrctl = 0;

    /*
     * BSIZEPKT is expressed in 1 KiB units.
     *
     * 2048 byte RX buffer -> value 2.
     */
    srrctl |= (E82576_RX_BUFFER_SIZE / 1024)
              << E1000_SRRCTL_BSIZEPKT_SHIFT;

    /*
     * Use advanced RX descriptors.
     */
    srrctl |= E1000_SRRCTL_DESCTYPE_ADV;

    e82576_write_reg(dev, E1000_SRRCTL(0), srrctl);

    /*
     * ---------------------------------------------------------
     * Configure maximum DMA read request.
     * ---------------------------------------------------------
     */

    drxmxod = e82576_read_reg(dev, E1000_DRXMXOD);

    drxmxod &= ~E1000_DRXMXOD_MAX_BYTES_REQ_MASK;
    drxmxod |= 0x10;

    e82576_write_reg(dev, E1000_DRXMXOD, drxmxod);

    /*
     * ---------------------------------------------------------
     * Program RX descriptor ring.
     * ---------------------------------------------------------
     */

    e82576_write_reg(dev,
                     E1000_RDBAL(0),
                     lower_32_bits(dev->rx_ring_dma));

    e82576_write_reg(dev,
                     E1000_RDBAH(0),
                     upper_32_bits(dev->rx_ring_dma));

    e82576_write_reg(dev,
                     E1000_RDLEN(0),
                     size);

    /*
     * Hardware starts at descriptor 0.
     */
    e82576_write_reg(dev, E1000_RDH(0), 0);

    /*
     * Give hardware descriptors 0 through N-1.
     */
    e82576_write_reg(dev,
                     E1000_RDT(0),
                     E82576_NUM_RX_DESC - 1);

    e82576_flush(dev);

    /*
     * ---------------------------------------------------------
     * Read back RX configuration.
     * ---------------------------------------------------------
     */

    dev_info(&dev->pdev->dev,
             "====================================\n");

    dev_info(&dev->pdev->dev,
             "RX DMA CONFIGURATION\n");

    dev_info(&dev->pdev->dev,
             "RDBAL  = 0x%08x\n",
             e82576_read_reg(dev, E1000_RDBAL(0)));

    dev_info(&dev->pdev->dev,
             "RDBAH  = 0x%08x\n",
             e82576_read_reg(dev, E1000_RDBAH(0)));

    dev_info(&dev->pdev->dev,
             "RDLEN  = %u\n",
             e82576_read_reg(dev, E1000_RDLEN(0)));

    dev_info(&dev->pdev->dev,
             "RDH    = %u\n",
             e82576_read_reg(dev, E1000_RDH(0)));

    dev_info(&dev->pdev->dev,
             "RDT    = %u\n",
             e82576_read_reg(dev, E1000_RDT(0)));

    dev_info(&dev->pdev->dev,
             "SRRCTL = 0x%08x\n",
             e82576_read_reg(dev, E1000_SRRCTL(0)));

    dev_info(&dev->pdev->dev,
             "DRXMXOD = 0x%08x\n",
             e82576_read_reg(dev, E1000_DRXMXOD));

    dev_info(&dev->pdev->dev,
             "RXDCTL = 0x%08x\n",
             e82576_read_reg(dev, E1000_RXDCTL(0)));

    dev_info(&dev->pdev->dev,
             "RCTL   = 0x%08x\n",
             e82576_read_reg(dev, E1000_RCTL));

    dev_info(&dev->pdev->dev,
             "====================================\n");

    /*
     * Ring information.
     */
    dev_info(&dev->pdev->dev,
             "RX RING: cpu=%px dma=%pad\n",
             dev->rx_ring,
             &dev->rx_ring_dma);

    /*
     * Verify the first four descriptors.
     */
    for (i = 0; i < 4; i++) {
        struct e82576_rx_desc *desc = &dev->rx_ring[i];

        dev_info(&dev->pdev->dev,
                 "RX[%d]: "
                 "buffer=%016llx "
                 "sw_dma=%016llx "
                 "skb=%px "
                 "status=0x%02x "
                 "length=%u\n",
                 i,
                 (unsigned long long)
                     le64_to_cpu(desc->buffer_addr),
                 (unsigned long long)
                     dev->rx_buffer[i].dma,
                 dev->rx_buffer[i].skb,
                 desc->status,
                 le16_to_cpu(desc->length));
    }

    return 0;

err:

    while (--i >= 0) {
        if (dev->rx_buffer[i].skb) {
            dma_unmap_single(&dev->pdev->dev,
                             dev->rx_buffer[i].dma,
                             E82576_RX_BUFFER_SIZE,
                             DMA_FROM_DEVICE);

            dev_kfree_skb(dev->rx_buffer[i].skb);

            dev->rx_buffer[i].skb = NULL;
            dev->rx_buffer[i].dma = 0;
        }
    }

    dma_free_coherent(&dev->pdev->dev,
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

// /*
//  * ============================================================
//  * RX POLLING
//  * ============================================================
//  */

// static int e82576_poll_rx(
//     struct e82576_device *dev)
// {
//     int packets = 0;

//     while (packets < 64) {

//         u16 i;
//         struct e82576_rx_desc *desc;
//         struct e82576_rx_buffer *rx_buffer;
//         struct sk_buff *skb;
//         struct sk_buff *new_skb;
//         u16 length;
//         u8 status;
//         u8 errors;

//         i = dev->rx_next_to_clean;
//         desc = &dev->rx_ring[i];
//         rx_buffer = &dev->rx_buffer[i];

//         /*
//          * Make sure we see the descriptor status
//          * written by the NIC.
//          */
//         dma_rmb();

//         status = READ_ONCE(desc->status);

//         /*
//          * DD = Descriptor Done.
//          *
//          * If hardware has not completed this
//          * descriptor, stop polling.
//          */
//         if (!(status & E1000_RXD_STAT_DD))
//             break;

//         length = le16_to_cpu(desc->length);
//         errors = desc->errors;
//         skb = rx_buffer->skb;

//         /*
//          * Diagnostic output for the first
//          * RX implementation.
//          */
//         netdev_info(
//             dev->netdev,
//             "RX[%u]: "
//             "status=0x%02x "
//             "errors=0x%02x "
//             "length=%u "
//             "RDT=%u "
//             "RDH=%u\n",
//             i,
//             status,
//             errors,
//             length,
//             e82576_read_reg(dev, E1000_RDT(0)),
//             e82576_read_reg(dev, E1000_RDH(0)));

//         /*
//          * Make the DMA buffer visible to the CPU.
//          */
//         dma_sync_single_for_cpu(
//             &dev->pdev->dev,
//             rx_buffer->dma,
//             E82576_RX_BUFFER_SIZE,
//             DMA_FROM_DEVICE);

//         /*
//          * Drop packets with hardware RX errors.
//          */
//         if (errors || length == 0) {

//             dev->netdev->stats.rx_dropped++;

//             dma_sync_single_for_device(
//                 &dev->pdev->dev,
//                 rx_buffer->dma,
//                 E82576_RX_BUFFER_SIZE,
//                 DMA_FROM_DEVICE);

//             memset(desc, 0, sizeof(*desc));

//             desc->buffer_addr = cpu_to_le64(rx_buffer->dma);

//             dma_wmb();

//             packets++;

//             dev->rx_next_to_clean =
//                 (dev->rx_next_to_clean + 1) %
//                 E82576_NUM_RX_DESC;

//             e82576_write_reg(
//                 dev,
//                 E1000_RDT(0),
//                 (dev->rx_next_to_clean +
//                  E82576_NUM_RX_DESC - 1) %
//                 E82576_NUM_RX_DESC);

//             continue;
//         }

//         /*
//          * Allocate a replacement buffer BEFORE
//          * giving the descriptor back to hardware.
//          */
//         new_skb = netdev_alloc_skb(dev->netdev, E82576_RX_BUFFER_SIZE);

//         if (!new_skb) {

//             dev->netdev->stats.rx_dropped++;

//             dma_sync_single_for_device(
//                 &dev->pdev->dev,
//                 rx_buffer->dma,
//                 E82576_RX_BUFFER_SIZE,
//                 DMA_FROM_DEVICE);

//             break;
//         }

//         /*
//          * Copy the received packet out of the
//          * DMA buffer.
//          *
//          * This deliberately avoids page recycling
//          * for the first RX milestone.
//          */
//         skb_put(new_skb, length);

//         memcpy(new_skb->data, skb->data, length);

//         /*
//          * Give the original DMA buffer back
//          * to the NIC.
//          */
//         dma_sync_single_for_device(
//             &dev->pdev->dev,
//             rx_buffer->dma,
//             E82576_RX_BUFFER_SIZE,
//             DMA_FROM_DEVICE);

//         /*
//          * Prepare the new descriptor.
//          */
//         memset(desc, 0, sizeof(*desc));

//         desc->buffer_addr = cpu_to_le64(rx_buffer->dma);

//         /*
//          * Make descriptor visible before
//          * updating RDT.
//          */
//         dma_wmb();

//         /*
//          * Ethernet protocol handling.
//          */
//         new_skb->protocol = eth_type_trans(new_skb, dev->netdev);
//         new_skb->ip_summed = CHECKSUM_NONE;

//         /*
//          * Deliver packet to Linux.
//          */
//         netif_receive_skb(new_skb);

//         dev->netdev->stats.rx_packets++;
//         dev->netdev->stats.rx_bytes += length;

//         /*
//          * Advance software consumer.
//          */
//         dev->rx_next_to_clean = (dev->rx_next_to_clean + 1) % E82576_NUM_RX_DESC;

//         /*
//          * Give this descriptor back to hardware.
//          *
//          * RDT points to the last descriptor
//          * owned by hardware.
//          */
//         e82576_write_reg(
//             dev,
//             E1000_RDT(0),
//             (dev->rx_next_to_clean +
//              E82576_NUM_RX_DESC - 1) %
//             E82576_NUM_RX_DESC);

//         packets++;
//     }

//     if (packets)
//         e82576_flush(dev);

//     return packets;
// }

// void e82576_rx_poll_work(
//     struct work_struct *work)
// {
//     struct e82576_device *dev =
//         container_of(
//             to_delayed_work(work),
//             struct e82576_device,
//             rx_poll_work);

//     while (1) {

//         u16 idx = dev->rx_next_to_clean;

//         struct e82576_rx_desc *desc =
//             &dev->rx_ring[idx];

//         u8 status =
//             READ_ONCE(desc->status);

//         /*
//          * Hardware has not completed this descriptor.
//          */
//         if (!(status & E1000_RXD_STAT_DD))
//             break;

//         /*
//          * At this point we own the descriptor.
//          */
//         {
//             struct sk_buff *skb;
//             struct sk_buff *new_skb;
//             dma_addr_t new_dma;
//             unsigned int length;

//             skb =
//                 dev->rx_buffer[idx].skb;

//             length =
//                 le16_to_cpu(desc->length);

//             // dev_info(
//             //     &dev->pdev->dev,
//             //     "RX PACKET: idx=%u status=0x%02x length=%u\n",
//             //     idx,
//             //     status,
//             //     length);

//             /*
//              * Remove the DMA mapping because Linux
//              * will now own the received packet.
//              */
//             dma_unmap_single(
//                 &dev->pdev->dev,
//                 dev->rx_buffer[idx].dma,
//                 E82576_RX_BUFFER_SIZE,
//                 DMA_FROM_DEVICE);

//             /*
//              * Tell skb how much data was received.
//              */
//             skb_put(skb, length);

//             /*
//              * Set protocol based on Ethernet header.
//              */
//             skb->protocol =
//                 eth_type_trans(
//                     skb,
//                     dev->netdev);

//             /*
//              * Give packet to Linux.
//              */
//             netif_receive_skb(skb);

//             /*
//              * Allocate replacement buffer.
//              */
//             new_skb =
//                 netdev_alloc_skb(
//                     dev->netdev,
//                     E82576_RX_BUFFER_SIZE);

//             if (!new_skb) {

//                 dev_err(
//                     &dev->pdev->dev,
//                     "RX skb allocation failed at idx=%u\n",
//                     idx);

//                 /*
//                  * We cannot safely return this descriptor
//                  * to hardware without a replacement buffer.
//                  */
//                 break;
//             }

//             new_dma =
//                 dma_map_single(
//                     &dev->pdev->dev,
//                     new_skb->data,
//                     E82576_RX_BUFFER_SIZE,
//                     DMA_FROM_DEVICE);

//             if (dma_mapping_error(
//                     &dev->pdev->dev,
//                     new_dma)) {

//                 dev_kfree_skb(new_skb);

//                 dev_err(
//                     &dev->pdev->dev,
//                     "RX DMA mapping failed at idx=%u\n",
//                     idx);

//                 break;
//             }

//             /*
//              * Install replacement buffer.
//              */
//             dev->rx_buffer[idx].skb =
//                 new_skb;

//             dev->rx_buffer[idx].dma =
//                 new_dma;

//             desc->buffer_addr =
//                 cpu_to_le64(new_dma);

//             /*
//              * Clear hardware writeback fields.
//              */
//             desc->length = 0;
//             desc->checksum = 0;
//             desc->status = 0;
//             desc->errors = 0;
//             desc->special = 0;

//             /*
//              * Make descriptor writes visible before
//              * returning descriptor ownership.
//              */
//             dma_wmb();

//             /*
//              * Return this descriptor to hardware.
//              */
//             e82576_write_reg(
//                 dev,
//                 E1000_RDT(0),
//                 idx);

//             /*
//              * Move to next descriptor.
//              */
//             dev->rx_next_to_clean =
//                 (idx + 1) %
//                 E82576_NUM_RX_DESC;
//         }
//     }

//     // /*
//     //  * Continue polling.
//     //  */
//     // schedule_delayed_work(
//     //     &dev->rx_poll_work,
//     //     msecs_to_jiffies(10));
// }

void e82576_rx_poll_work(struct work_struct *work)
{
    struct e82576_device *dev =
        container_of(to_delayed_work(work),
                     struct e82576_device,
                     rx_poll_work);

    int i;

    if (!dev->rx_ring)
        goto reschedule;

    for (i = 0; i < E82576_NUM_RX_DESC; i++) {
        struct e82576_rx_desc *desc = &dev->rx_ring[i];
        u8 *p = (u8 *)desc;

        /*
         * Make sure descriptor contents are visible to the CPU
         * after the NIC has DMA'd them.
         */
        dma_rmb();

        /*
         * Legacy 82576 RX descriptor:
         *
         *   bytes 0-7   = buffer address
         *   bytes 8-9   = packet length
         *   byte  12    = status
         */
        u8 status = p[12];

        bool dd  = status & E1000_RXD_STAT_DD;
        bool eop = status & E1000_RXD_STAT_EOP;

        /*
         * Nothing to process if the NIC hasn't completed
         * this descriptor.
         */
        if (!dd)
            continue;

        u16 length = (u16)p[8] |
                     ((u16)p[9] << 8);

        dev_info(&dev->pdev->dev,
                 "RXD[%d]: STATUS=0x%02x DD=%d EOP=%d LEN=%u\n",
                 i,
                 status,
                 dd,
                 eop,
                 length);

        /*
         * Dump the completed descriptor.
         */
        dev_info(&dev->pdev->dev,
                 "RXD[%d] RAW: "
                 "%02x %02x %02x %02x "
                 "%02x %02x %02x %02x "
                 "%02x %02x %02x %02x "
                 "%02x %02x %02x %02x\n",
                 i,
                 p[0],  p[1],  p[2],  p[3],
                 p[4],  p[5],  p[6],  p[7],
                 p[8],  p[9],  p[10], p[11],
                 p[12], p[13], p[14], p[15]);

        if (eop) {
            dev_info(&dev->pdev->dev,
                     "RXD[%d]: COMPLETE FRAME! DD=1 EOP=1 LEN=%u\n",
                     i, length);
        } else {
            dev_info(&dev->pdev->dev,
                     "RXD[%d]: DD=1 but EOP=0 "
                     "(frame may continue)\n",
                     i);
        }
    }

reschedule:
    schedule_delayed_work(&dev->rx_poll_work,
                          msecs_to_jiffies(1));
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

    rctl |= E1000_RCTL_EN |
            E1000_RCTL_BAM |
            E1000_RCTL_UPE |
            E1000_RCTL_MPE |
            E1000_RCTL_SECRC;

    e82576_write_reg(dev, E1000_RCTL, rctl);
    e82576_flush(dev);

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
    dev_info(&dev->pdev->dev,
        "e82576: RX CONFIG:\n"
        "  SRRCTL  = 0x%08x\n"
        "  RDBAL   = 0x%08x\n"
        "  RDBAH   = 0x%08x\n"
        "  RDLEN   = %u\n"
        "  DRXMXOD = 0x%08x\n",
        e82576_read_reg(dev, E1000_SRRCTL(0)),
        e82576_read_reg(dev, E1000_RDBAL(0)),
        e82576_read_reg(dev, E1000_RDBAH(0)),
        e82576_read_reg(dev, E1000_RDLEN(0)),
        e82576_read_reg(dev, E1000_DRXMXOD));
    dev_info(&dev->pdev->dev, "====================================\n");

}