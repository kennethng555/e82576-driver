#include "e82576.h"

static void e82576_rx_record_error(
    struct e82576_device *dev,
    u32 status_error,
    u16 length)
{
    dev->stats.rx_errors++;

    if (status_error & E1000_RXD_ERR_CE)
        dev->stats.rx_crc_errors++;

    if (status_error & E1000_RXD_ERR_RXE)
        dev->stats.rx_fifo_errors++;

    if (length == 0)
        dev->stats.rx_length_errors++;
}

int e82576_poll(
    struct napi_struct *napi,
    int budget)
{
    struct e82576_device *dev =
        container_of(
            napi,
            struct e82576_device,
            napi);

    int work_done = 0;

    while (work_done < budget) {
        u16 index;
        struct e82576_rx_desc *desc;
        struct e82576_rx_buffer *buffer;
        struct sk_buff *skb;

        u32 status_error;
        u16 length;

        index = dev->rx_next_to_clean;

        desc = &dev->rx_ring[index];
        buffer = &dev->rx_buffer[index];

        skb = buffer->skb;

        /*
         * Make descriptor writes performed by the NIC
         * visible to the CPU.
         */
        dma_rmb();

        status_error =
            le32_to_cpu(
                *(__le32 *)((u8 *)desc + 8));

        /*
         * Hardware has not completed this descriptor.
         */
        if (!(status_error & E1000_RXD_STAT_DD))
            break;

        length =
            le16_to_cpu(
                *(__le16 *)((u8 *)desc + 12));

        // dev_info(
        //     &dev->pdev->dev,
        //     "RX: idx=%u DD=%u EOP=%u len=%u\n",
        //     index,
        //     !!(status_error & E1000_RXD_STAT_DD),
        //     !!(status_error & E1000_RXD_STAT_EOP),
        //     length);

        /*
         * We no longer need the DMA mapping because
         * hardware has finished writing this buffer.
         */
        dma_unmap_single(
            &dev->pdev->dev,
            buffer->dma,
            E82576_RX_BUFFER_SIZE,
            DMA_FROM_DEVICE);

        buffer->dma = 0;

        /*
         * Sanity check.
         */
        if (!skb) {
            dev_err(
                &dev->pdev->dev,
                "NAPI RX: idx=%u has no skb\n",
                index);

            dev->stats.rx_dropped++;

            /*
             * Refill the descriptor before returning
             * ownership to hardware.
             */
            if (e82576_refill_rx_buffer(dev, index)) {
                dev_err(
                    &dev->pdev->dev,
                    "RX refill failed: idx=%u\n",
                    index);
                break;
            }

            e82576_write_reg(
                dev,
                E1000_RDT(0),
                index);

            dev->rx_next_to_clean++;

            if (dev->rx_next_to_clean ==
                E82576_NUM_RX_DESC)
                dev->rx_next_to_clean = 0;

            work_done++;

            continue;
        }

        /*
         * Handle RX errors.
         */
        if (status_error & E1000_RXD_ERR_RXE) {

            e82576_rx_record_error(
                dev,
                status_error,
                length);

            /*
             * Drop any partially assembled packet.
             */
            if (dev->rx_skb) {
                dev_kfree_skb_any(dev->rx_skb);
                dev->rx_skb = NULL;
            }

            /*
             * Drop the current descriptor's skb.
             */
            dev_kfree_skb_any(skb);
            buffer->skb = NULL;

            /*
             * Give the descriptor a new buffer.
             */
            if (e82576_refill_rx_buffer(dev, index)) {
                dev_err(
                    &dev->pdev->dev,
                    "RX refill failed after error: idx=%u\n",
                    index);
                break;
            }

            /*
             * Return descriptor ownership to hardware.
             */
            e82576_write_reg(
                dev,
                E1000_RDT(0),
                index);

            dev->rx_next_to_clean++;

            if (dev->rx_next_to_clean ==
                E82576_NUM_RX_DESC)
                dev->rx_next_to_clean = 0;

            work_done++;

            continue;
        }

        /*
         * ----------------------------------------------------
         * Packet assembly
         * ----------------------------------------------------
         *
         * The first descriptor's skb becomes the packet skb.
         * Subsequent descriptors are copied into it.
         */
        if (!dev->rx_skb) {

            /*
             * First fragment of a new packet.
             */
            dev->rx_skb = skb;
            buffer->skb = NULL;

            /*
             * Add this fragment to the packet.
             */
            skb_put(
                dev->rx_skb,
                length);

        } else {

            /*
             * Subsequent fragment.
             */
            if (skb_tailroom(dev->rx_skb) < length) {

                dev_err(
                    &dev->pdev->dev,
                    "RX packet too large: "
                    "idx=%u len=%u tailroom=%u\n",
                    index,
                    length,
                    skb_tailroom(dev->rx_skb));

                dev->stats.rx_length_errors++;

                /*
                 * Drop the packet being assembled.
                 */
                dev_kfree_skb_any(dev->rx_skb);
                dev->rx_skb = NULL;

                /*
                 * Drop the current fragment skb.
                 */
                dev_kfree_skb_any(skb);
                buffer->skb = NULL;

                /*
                 * Refill this descriptor.
                 */
                if (e82576_refill_rx_buffer(dev, index)) {
                    dev_err(
                        &dev->pdev->dev,
                        "RX refill failed: idx=%u\n",
                        index);
                    break;
                }

                /*
                 * Return descriptor ownership to hardware.
                 */
                e82576_write_reg(
                    dev,
                    E1000_RDT(0),
                    index);

                dev->rx_next_to_clean++;

                if (dev->rx_next_to_clean ==
                    E82576_NUM_RX_DESC)
                    dev->rx_next_to_clean = 0;

                work_done++;

                continue;
            }

            /*
             * Append the fragment.
             */
            memcpy(
                skb_put(
                    dev->rx_skb,
                    length),
                skb->data,
                length);

            /*
             * This descriptor's skb is no longer
             * needed after its data has been copied.
             */
            dev_kfree_skb_any(skb);
            buffer->skb = NULL;
        }

        /*
         * Give this descriptor a fresh DMA buffer.
         */
        if (e82576_refill_rx_buffer(dev, index)) {
            dev_err(
                &dev->pdev->dev,
                "RX refill failed: idx=%u\n",
                index);

            /*
             * The descriptor is not returned to hardware.
             */
            break;
        }

        /*
         * Return descriptor ownership to hardware.
         */
        e82576_write_reg(
            dev,
            E1000_RDT(0),
            index);

        /*
         * Advance to the next descriptor.
         */
        dev->rx_next_to_clean++;

        if (dev->rx_next_to_clean ==
            E82576_NUM_RX_DESC)
            dev->rx_next_to_clean = 0;

        work_done++;

        /*
         * ----------------------------------------------------
         * EOP
         * ----------------------------------------------------
         *
         * EOP means this descriptor contains the final
         * fragment of the current packet.
         */
        if (status_error & E1000_RXD_STAT_EOP) {
            struct sk_buff *packet;

            packet = dev->rx_skb;
            dev->rx_skb = NULL;

            /*
             * Set the Ethernet protocol.
             */
            packet->protocol =
                eth_type_trans(
                    packet,
                    dev->netdev);

            /*
             * Hardware checksum verification is not
             * implemented yet.
             */
            packet->ip_summed = CHECKSUM_NONE;

            /*
             * Hand the complete packet to GRO.
             */
            napi_gro_receive(
                napi,
                packet);

            /*
             * Driver statistics.
             */
            dev->stats.rx_packets++;
            dev->stats.rx_bytes += packet->len;
        }
    }

    /*
     * If we processed everything available, complete
     * the NAPI poll and re-enable RX interrupts.
     */
    if (work_done < budget) {

        napi_complete_done(
            napi,
            work_done);

        e82576_write_reg(
            dev,
            E1000_EIMS,
            E1000_EICR_RXQ0);
    }

    return work_done;
}