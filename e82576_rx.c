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
    srrctl |= (E82576_RX_BUFFER_SIZE / 1024) << E1000_SRRCTL_BSIZEPKT_SHIFT;

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

    

    size = E82576_NUM_RX_DESC * sizeof(struct e82576_rx_desc);

    dev->rx_ring = dma_alloc_coherent(&dev->pdev->dev, size, &dev->rx_ring_dma, GFP_KERNEL);

    if (!dev->rx_ring) {
        dev_err(&dev->pdev->dev, "Failed to allocate RX descriptor ring\n");
        return -ENOMEM;
    }

    memset(dev->rx_ring, 0, size);
    memset(dev->rx_buffer, 0, sizeof(dev->rx_buffer));

    /*
     * Allocate one packet buffer for every RX descriptor.
     */
    for (i = 0; i < E82576_NUM_RX_DESC; i++) {
        struct sk_buff *skb;
        dma_addr_t dma;
        struct e82576_rx_desc *desc;

        skb = netdev_alloc_skb(dev->netdev, E82576_RX_MAX_FRAME_SIZE);

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

        /*
         * -----------------------------------------------------
         * Advanced RX descriptor READ format
         * -----------------------------------------------------
         *
         * pkt_addr = packet buffer DMA address
         *
         * hdr_addr = zero because we are using a single packet
         *            buffer rather than header splitting.
         */

        desc->read.pkt_addr = cpu_to_le64(dma);
        desc->read.hdr_addr = 0;
    }

    /*
     * Hardware owns all descriptors.
     */
    dev->rx_next_to_clean = 0;
    dev->rx_skb = NULL;

    /*
     * ---------------------------------------------------------
     * Program RX descriptor ring.
     * ---------------------------------------------------------
     */

    e82576_write_reg(dev, E1000_RDBAL(0), lower_32_bits(dev->rx_ring_dma));
    e82576_write_reg(dev, E1000_RDBAH(0), upper_32_bits(dev->rx_ring_dma));
    e82576_write_reg(dev, E1000_RDLEN(0), size);

    /*
     * Make descriptor writes visible before giving the ring
     * to the NIC.
     */
    dma_wmb();

    /*
     * Hardware starts at descriptor 0.
     */
    e82576_write_reg(dev, E1000_RDH(0), 0);

    /*
     * Give hardware descriptors 0 through N-1.
     */
    e82576_write_reg(dev, E1000_RDT(0), E82576_NUM_RX_DESC - 1);

    e82576_flush(dev);

    /*
     * ---------------------------------------------------------
     * Read back RX configuration.
     * ---------------------------------------------------------
     */

    dev_info(&dev->pdev->dev, "====================================\n");
    dev_info(&dev->pdev->dev, "RX DMA CONFIGURATION\n");
    dev_info(&dev->pdev->dev, "RDBAL  = 0x%08x\n", e82576_read_reg(dev, E1000_RDBAL(0)));
    dev_info(&dev->pdev->dev, "RDBAH  = 0x%08x\n", e82576_read_reg(dev, E1000_RDBAH(0)));
    dev_info(&dev->pdev->dev, "RDLEN  = %u\n", e82576_read_reg(dev, E1000_RDLEN(0)));
    dev_info(&dev->pdev->dev, "RDH    = %u\n", e82576_read_reg(dev, E1000_RDH(0)));
    dev_info(&dev->pdev->dev, "RDT    = %u\n", e82576_read_reg(dev, E1000_RDT(0)));
    dev_info(&dev->pdev->dev, "SRRCTL = 0x%08x\n", e82576_read_reg(dev, E1000_SRRCTL(0)));
    dev_info(&dev->pdev->dev, "DRXMXOD = 0x%08x\n", e82576_read_reg(dev, E1000_DRXMXOD));
    dev_info(&dev->pdev->dev, "RXDCTL = 0x%08x\n", e82576_read_reg(dev, E1000_RXDCTL(0)));
    dev_info(&dev->pdev->dev, "RCTL   = 0x%08x\n", e82576_read_reg(dev, E1000_RCTL));
    dev_info(&dev->pdev->dev, "====================================\n");

    /*
     * Ring information.
     */
    dev_info(&dev->pdev->dev, "RX RING: cpu=%px dma=%pad\n", dev->rx_ring, &dev->rx_ring_dma);

    /*
     * Verify the first four descriptors.
     *
     * These are still in the READ format at this point.
     */
    for (i = 0; i < 4; i++) {
        struct e82576_rx_desc *desc = &dev->rx_ring[i];

        dev_info(&dev->pdev->dev, "RX[%d]: ", i);
        dev_info(&dev->pdev->dev, "pkt_addr=%016llx ", 
            (unsigned long long)le64_to_cpu(desc->read.pkt_addr));
        dev_info(&dev->pdev->dev, "sw_dma=%016llx ", 
            (unsigned long long)dev->rx_buffer[i].dma);
        dev_info(&dev->pdev->dev, "hdr_addr=%016llx ", 
            (unsigned long long)le64_to_cpu(desc->read.hdr_addr));
        dev_info(&dev->pdev->dev, "skb=%px\n", dev->rx_buffer[i].skb);
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

    /*
     * Drop any packet that was being assembled from
     * multiple RX descriptors.
     */
    if (dev->rx_skb) {
        dev_kfree_skb_any(dev->rx_skb);
        dev->rx_skb = NULL;
    }

    /*
     * Free every descriptor-owned RX buffer.
     */
    for (i = 0; i < E82576_NUM_RX_DESC; i++)
        e82576_free_rx_buffer(dev, i);

    /*
     * Free the descriptor ring.
     */
    if (dev->rx_ring) {
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
    }

    dev->rx_next_to_clean = 0;
}

int e82576_refill_rx_buffer(
    struct e82576_device *dev,
    u16 index)
{
    struct e82576_rx_buffer *buffer;
    struct e82576_rx_desc *desc;
    struct sk_buff *skb;
    dma_addr_t dma;

    buffer = &dev->rx_buffer[index];
    desc = &dev->rx_ring[index];

    /*
     * The descriptor must not already own an skb.
     */
    if (buffer->skb || buffer->dma) {
        dev_err(
            &dev->pdev->dev,
            "RX refill: idx=%u still has buffer\n",
            index);

        return -EINVAL;
    }

    skb = netdev_alloc_skb(
        dev->netdev,
        E82576_RX_MAX_FRAME_SIZE);

    if (!skb) {
        dev_err(
            &dev->pdev->dev,
            "RX refill: failed to allocate skb for idx=%u\n",
            index);

        return -ENOMEM;
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
            "RX refill: DMA mapping failed for idx=%u\n",
            index);

        dev_kfree_skb_any(skb);

        return -ENOMEM;
    }

    buffer->skb = skb;
    buffer->dma = dma;

    /*
     * Program the hardware descriptor.
     */
    desc->read.pkt_addr =
        cpu_to_le64(dma);

    desc->read.hdr_addr = 0;

    /*
     * Make descriptor writes visible to the NIC.
     */
    dma_wmb();

    return 0;
}

// /*
//  * ============================================================
//  * RX POLLING
//  * ============================================================
//  */

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
        u32 status_error;
        u16 length;
        bool dd;
        bool eop;
        u8 *buf;

        /*
         * Make sure descriptor contents written by the NIC
         * are visible to the CPU.
         */
        dma_rmb();

        /*
         * IMPORTANT:
         *
         * The 82576 RX queue is using the ADVANCED RX descriptor
         * format.
         *
         * Advanced RX writeback layout:
         *
         *   bytes 0-7   : lower dword / RSS / packet info
         *   bytes 8-11  : status_error
         *   bytes 12-13 : packet length
         *   bytes 14-15 : VLAN
         */

        status_error = le32_to_cpu(*(__le32 *)((u8 *)desc + 8));
        length = le16_to_cpu(*(__le16 *)((u8 *)desc + 12));

        /*
         * Advanced descriptor status bits:
         *
         * bit 0 = DD
         * bit 1 = EOP
         */
        dd  = status_error & BIT(0);
        eop = status_error & BIT(1);

        if (!dd)
            continue;

        dev_info(&dev->pdev->dev,
                 "RXD[%d]: DD=1 EOP=%d "
                 "STATUS_ERROR=0x%08x LEN=%u\n",
                 i,
                 eop,
                 status_error,
                 length);

        /*
         * Dump the complete raw 16-byte descriptor.
         */
        dev_info(&dev->pdev->dev,
            "RXD[%d] RAW: "
            "%02x %02x %02x %02x "
            "%02x %02x %02x %02x "
            "%02x %02x %02x %02x "
            "%02x %02x %02x %02x\n",
            i,
            ((u8 *)desc)[0],  ((u8 *)desc)[1],
            ((u8 *)desc)[2],  ((u8 *)desc)[3],
            ((u8 *)desc)[4],  ((u8 *)desc)[5],
            ((u8 *)desc)[6],  ((u8 *)desc)[7],
            ((u8 *)desc)[8],  ((u8 *)desc)[9],
            ((u8 *)desc)[10], ((u8 *)desc)[11],
            ((u8 *)desc)[12], ((u8 *)desc)[13],
            ((u8 *)desc)[14], ((u8 *)desc)[15]);

        /*
         * Make sure an skb exists for this descriptor.
         */
        if (!dev->rx_buffer[i].skb) {
            dev_info(&dev->pdev->dev,
                     "RXD[%d]: DD=1 but skb is NULL\n",
                     i);
            continue;
        }

        buf = dev->rx_buffer[i].skb->data;

        /*
         * Don't trust a bogus descriptor length enough to
         * read beyond our allocated RX buffer.
         */
        if (length > E82576_RX_BUFFER_SIZE) {
            dev_info(&dev->pdev->dev,
                     "RXD[%d]: INVALID LEN=%u "
                     "(buffer size=%u)\n",
                     i,
                     length,
                     E82576_RX_BUFFER_SIZE);
            continue;
        }

        /*
         * Dump received packet data.
         */
        print_hex_dump(KERN_INFO,
                       "RX DATA: ",
                       DUMP_PREFIX_OFFSET,
                       16,
                       1,
                       buf,
                       min_t(u16, length, 64),
                       true);

        /*
         * Check for the packet sent by send_packet.py:
         *
         * Destination:
         *   00:1b:22:57:1d:64
         *
         * Source:
         *   00:e0:4c:29:16:24
         *
         * Payload:
         *   "hello 82576"
         *
         * Ethernet header = 14 bytes
         * Payload          = 11 bytes
         * Minimum frame    = 25 bytes
         */
        if (eop &&
            length >= 25 &&
            !memcmp(buf,
                    "\x00\x1b\x22\x57\x1d\x64",
                    ETH_ALEN) &&
            !memcmp(buf + ETH_ALEN,
                    "\x00\xe0\x4c\x29\x16\x24",
                    ETH_ALEN) &&
            !memcmp(buf + 14,
                    "hello 82576",
                    11)) {

            dev_info(&dev->pdev->dev,
                     "***** FOUND send_packet.py PACKET "
                     "IN RXD[%d] *****\n",
                     i);
        }
    }

reschedule:
    schedule_delayed_work(&dev->rx_poll_work,
                          msecs_to_jiffies(500));
}

int e82576_enable_dma(struct e82576_device *dev)
{
    u32 rxdctl;
    u32 txdctl;
    u32 rctl;
    u32 tctl;
    int i;

    /*
     * ------------------------------------------------------------
     * RX MAC
     * ------------------------------------------------------------
     */

    rctl = e82576_read_reg(dev, E1000_RCTL);

    rctl |= E1000_RCTL_EN | E1000_RCTL_LPE;

    e82576_write_reg(dev, E1000_RCTL, rctl);
    e82576_flush(dev);

    /*
     * ------------------------------------------------------------
     * RXDCTL
     * ------------------------------------------------------------
     */

    rxdctl = e82576_read_reg(dev, E1000_RXDCTL(0));

    rxdctl &= ~0x0007ffff;

    rxdctl |= 8;          /* PTHRESH = 8 */
    rxdctl |= 8 << 8;     /* HTHRESH = 8 */
    rxdctl |= 1 << 16;    /* WTHRESH = 1 */
    rxdctl |= E1000_RXDCTL_QUEUE_ENABLE;

    e82576_write_reg(dev, E1000_RXDCTL(0), rxdctl);
    e82576_flush(dev);

    /*
     * Wait for RXDCTL.ENABLE to become active.
     */
    for (i = 0; i < E82576_RX_QUEUE_ENABLE_TIMEOUT; i++) {
        rxdctl = e82576_read_reg(dev, E1000_RXDCTL(0));
        if (rxdctl & E1000_RXDCTL_QUEUE_ENABLE)
            break;

        udelay(1);
    }

    if (!(rxdctl & E1000_RXDCTL_QUEUE_ENABLE)) {
        dev_err(&dev->pdev->dev, "RX queue failed to enable: RXDCTL=0x%08x\n", rxdctl);
        return -EIO;
    }

    /*
     * ------------------------------------------------------------
     * TXDCTL
     * ------------------------------------------------------------
     */

    txdctl = e82576_read_reg(dev, E1000_TXDCTL(0));

    txdctl |= E1000_TXDCTL_QUEUE_ENABLE;

    e82576_write_reg(dev, E1000_TXDCTL(0), txdctl);
    e82576_flush(dev);

    /*
     * ------------------------------------------------------------
     * TX MAC
     * ------------------------------------------------------------
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

    return 0;
}

void e82576_free_rx_buffer(
    struct e82576_device *dev,
    u16 index)
{
    struct e82576_rx_buffer *buffer;

    buffer = &dev->rx_buffer[index];

    if (buffer->dma) {
        dma_unmap_single(
            &dev->pdev->dev,
            buffer->dma,
            E82576_RX_BUFFER_SIZE,
            DMA_FROM_DEVICE);

        buffer->dma = 0;
    }

    if (buffer->skb) {
        dev_kfree_skb_any(buffer->skb);
        buffer->skb = NULL;
    }
}