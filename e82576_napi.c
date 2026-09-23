#include "e82576.h"

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

        u16 idx;
        struct e82576_rx_desc *desc;
        u8 status;
        u16 length;
        struct sk_buff *skb;
        struct sk_buff *new_skb;
        dma_addr_t new_dma;

        idx = dev->rx_next_to_clean;

        desc = &dev->rx_ring[idx];

        status = READ_ONCE(desc->status);

        /*
         * Hardware has not completed this descriptor.
         */
        if (!(status & E1000_RXD_STAT_DD))
            break;

        /*
         * Read descriptor contents before recycling it.
         */
        length = le16_to_cpu(READ_ONCE(desc->length));

        skb = dev->rx_buffer[idx].skb;

        dev_info(
            &dev->pdev->dev,
            "RX PACKET: idx=%u status=0x%02x length=%u\n",
            idx,
            status,
            length);

        /*
         * Remove the DMA mapping from the buffer
         * that is being handed to Linux.
         */
        dma_unmap_single(
            &dev->pdev->dev,
            dev->rx_buffer[idx].dma,
            E82576_RX_BUFFER_SIZE,
            DMA_FROM_DEVICE);

        /*
         * Tell the skb how much packet data it contains.
         */
        skb_put(
            skb,
            length);

        /*
         * Determine the Ethernet protocol.
         */
        skb->protocol =
            eth_type_trans(
                skb,
                dev->netdev);

        /*
         * Give the packet to the Linux networking stack.
         */
        netif_receive_skb(skb);

        /*
         * Allocate a replacement buffer.
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

            break;
        }

        /*
         * Map replacement buffer for DMA.
         */
        new_dma =
            dma_map_single(
                &dev->pdev->dev,
                new_skb->data,
                E82576_RX_BUFFER_SIZE,
                DMA_FROM_DEVICE);

        if (dma_mapping_error(
                &dev->pdev->dev,
                new_dma)) {

            dev_err(
                &dev->pdev->dev,
                "RX DMA mapping failed at idx=%u\n",
                idx);

            dev_kfree_skb(new_skb);

            break;
        }

        /*
         * Install replacement buffer.
         */
        dev->rx_buffer[idx].skb =
            new_skb;

        dev->rx_buffer[idx].dma =
            new_dma;

        /*
         * Give hardware the replacement buffer.
         */
        desc->buffer_addr =
            cpu_to_le64(new_dma);

        /*
         * Clear descriptor writeback fields.
         */
        desc->length = 0;
        desc->checksum = 0;
        desc->status = 0;
        desc->errors = 0;
        desc->special = 0;

        /*
         * Ensure descriptor contents are visible
         * before returning ownership to hardware.
         */
        dma_wmb();

        /*
         * Return descriptor to hardware.
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

        work_done++;
    }

    /*
     * We processed fewer packets than the budget,
     * so the RX ring is currently drained.
     */
    if (work_done < budget) {

        /*
         * Complete the NAPI cycle.
         */
        if (napi_complete_done(napi, work_done)) {

            /*
             * Re-enable MSI-X vector 0.
             *
             * RX queue 0 and OTHER currently share
             * this vector.
             */
            e82576_write_reg(
                dev,
                E1000_EIMS,
                BIT(0));

            e82576_flush(dev);
        }
    }

    return work_done;
}