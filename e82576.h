#ifndef E82576_H
#define E82576_H

#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/types.h>
#include <linux/bitops.h>
#include <linux/io.h>
#include <linux/mii.h>

/*
 * TX/RX
*/
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <linux/dma-mapping.h>


#define DRIVER_NAME         "e82576"
#define DRIVER_VERSION      "0.8"

#define INTEL_VENDOR_ID     0x8086
#define INTEL_82576_DEVICE  0x10c9


/*
 * ============================================================
 * 82576 REGISTERS
 * ============================================================
 */

#define E1000_CTRL           0x00000
#define E1000_STATUS         0x00008

#define E1000_CTRL_RST       BIT(26)
#define E1000_CTRL_PHY_RST   BIT(31)


/*
 * ============================================================
 * EEPROM / NVM
 * ============================================================
 */

#define E1000_EERD           0x00014

#define E1000_EERD_START     BIT(0)
#define E1000_EERD_DONE      BIT(1)

#define E1000_EERD_ADDR_SHIFT 2
#define E1000_EERD_DATA_SHIFT 16


/*
 * ============================================================
 * MDIC
 * ============================================================
 */

#define E1000_MDIC           0x00020

#define E1000_MDIC_DATA_MASK 0x0000ffff

#define E1000_MDIC_REG_SHIFT 16
#define E1000_MDIC_PHY_SHIFT 21

#define E1000_MDIC_OP_WRITE  (1U << 26)
#define E1000_MDIC_OP_READ   (2U << 26)

#define E1000_MDIC_READY     BIT(28)
#define E1000_MDIC_INT_EN    BIT(29)
#define E1000_MDIC_ERROR     BIT(30)


/*
 * ============================================================
 * MAC COPPER CONFIGURATION
 * ============================================================
 */

#define E1000_CTRL_SLU       BIT(6)
#define E1000_CTRL_FRCSPD    BIT(11)
#define E1000_CTRL_FRCDPX    BIT(12)


/*
 * ============================================================
 * INTERRUPTS
 * ============================================================
 */

#define E1000_ICR       0x000C0
#define E1000_ICS       0x000C8
#define E1000_IMS       0x000D0
#define E1000_IMC       0x000D8

#define E1000_EICS      0x01520
#define E1000_EIMS      0x01524
#define E1000_EIMC      0x01528
#define E1000_EIAC      0x0152C
#define E1000_EIAM      0x01530
#define E1000_GPIE      0x01514
#define E1000_EICR      0x01580

#define E1000_EICR_OTHER      BIT(31)

#define E1000_IVAR_MISC 0x01740

#define E1000_ICR_LSC    BIT(2)
#define E1000_IMS_LSC    BIT(2)
#define E1000_IVAR_VALID 0x80

#define E1000_GPIE_NSICR    0x00000001
#define E1000_GPIE_MSIX_MODE 0x00000010
#define E1000_GPIE_EIAME    0x40000000
#define E1000_GPIE_PBA      0x80000000

#define E1000_STATUS_LU 0x00000002
#define BMSR_LSTATUS 0x0004


/*
 * ============================================================
 * SOFTWARE / FIRMWARE SEMAPHORES
 * ============================================================
 */

#define E1000_SWSM            0x05B50

#define E1000_SWSM_SMBI       BIT(0)
#define E1000_SWSM_SWESMBI    BIT(1)

#define E1000_SW_FW_SYNC      0x05B5C

#define E1000_SWFW_PHY0_SM    BIT(0)
#define E1000_SWFW_PHY1_SM    BIT(2)
#define E1000_SWFW_PHY2_SM    BIT(4)
#define E1000_SWFW_PHY3_SM    BIT(6)

#define E1000_SWFW_EEP_SM     BIT(0)


/*
 * ============================================================
 * PHY REGISTERS
 * ============================================================
 */

#define PHY_REG_ID1           MII_PHYSID1
#define PHY_REG_ID2           MII_PHYSID2

#define PHY_BMCR              MII_BMCR
#define PHY_BMSR              MII_BMSR
#define PHY_ANAR              MII_ADVERTISE
#define PHY_ANLPAR            MII_LPA
#define PHY_CTRL1000          MII_CTRL1000
#define PHY_STAT1000          MII_STAT1000


/*
 * ============================================================
 * MANC
 * ============================================================
 */

#define E1000_MANC                     0x05820
#define E1000_MANC_BLK_PHY_RST_ON_IDE  BIT(18)


/*
 * ============================================================
 * TX/RX DESCRIPTORS
 * ============================================================
 */

#define E82576_NUM_TX_DESC       64
#define E82576_NUM_RX_DESC       64
#define E82576_RX_BUFFER_SIZE    2048
#define E82576_TX_RING_SIZE 256


/*
 * Software state associated with a TX descriptor.
 */

struct e82576_tx_buffer {
    struct sk_buff *skb;
    dma_addr_t dma;
};


/*
 * Software state associated with an RX descriptor.
 */

struct e82576_rx_buffer {
    struct sk_buff *skb;
    dma_addr_t dma;
};


/*
 * ============================================================
 * DEVICE STRUCTURE
 * ============================================================
 */

struct e82576_device {

    struct pci_dev *pdev;
    struct net_device *netdev;
    void __iomem *hw_addr;
    struct delayed_work link_debug_work;

    resource_size_t bar0_start;
    resource_size_t bar0_length;


    /*
     * MAC.
     */

    u8 mac_address[ETH_ALEN];


    /*
     * PHY.
     */

    u8 phy_address;

    u16 phy_id1;
    u16 phy_id2;


    /*
     * Link.
     */

    bool link_up;
    int link_speed;
    bool full_duplex;


    /*
     * MSI-X.
     */

    int num_msix_vectors;
    int msix_irq;
    bool msix_enabled;


    /*
     * TX ring.
     */

    struct e82576_tx_desc *tx_ring;
    dma_addr_t tx_ring_dma;

    struct e82576_tx_buffer tx_buffer[E82576_NUM_TX_DESC];

    u16 tx_next_to_use;
    u16 tx_next_to_clean;

    spinlock_t tx_lock;

    struct delayed_work tx_clean_work;


    /*
     * RX ring.
     */

    struct e82576_rx_desc *rx_ring;
    dma_addr_t rx_ring_dma;

    struct e82576_rx_buffer rx_buffer[E82576_NUM_RX_DESC];

    u16 rx_next_to_clean;

    struct delayed_work rx_poll_work;
};


/*
 * ============================================================
 * RX/TX DMA REGISTERS
 * ============================================================
 */

#define E1000_RCTL       0x00100
#define E1000_TCTL       0x00400

#define E1000_RDBAL(_n)  (0x0C000 + ((_n) * 0x40))
#define E1000_RDBAH(_n)  (0x0C004 + ((_n) * 0x40))
#define E1000_RDLEN(_n)  (0x0C008 + ((_n) * 0x40))
#define E1000_SRRCTL(_n) (0x0C00C + ((_n) * 0x40))
#define E1000_RDH(_n)    (0x0C010 + ((_n) * 0x40))
#define E1000_RDT(_n)    (0x0C018 + ((_n) * 0x40))
#define E1000_RXDCTL(_n) (0x0C028 + ((_n) * 0x40))

#define E1000_TDBAL(_n)  (0x0E000 + ((_n) * 0x40))
#define E1000_TDBAH(_n)  (0x0E004 + ((_n) * 0x40))
#define E1000_TDLEN(_n)  (0x0E008 + ((_n) * 0x40))
#define E1000_TDH(_n)    (0x0E010 + ((_n) * 0x40))
#define E1000_TDT(_n)    (0x0E018 + ((_n) * 0x40))
#define E1000_TXDCTL(_n) (0x0E028 + ((_n) * 0x40))


/*
 * Receive Control.
 */

#define E1000_RCTL_EN       BIT(1)
#define E1000_RCTL_SBP      BIT(2)
#define E1000_RCTL_UPE      BIT(3)
#define E1000_RCTL_MPE      BIT(4)
#define E1000_RCTL_LPE      BIT(5)
#define E1000_RCTL_BAM      BIT(15)
#define E1000_RCTL_SECRC    BIT(26)


/*
 * Transmit Control.
 */

#define E1000_TCTL_EN       BIT(1)
#define E1000_TCTL_PSP      BIT(3)


/*
 * Queue enable.
 */

#define E1000_RXDCTL_QUEUE_ENABLE BIT(25)
#define E1000_TXDCTL_QUEUE_ENABLE BIT(25)


/*
 * Legacy transmit descriptor.
 */

#define E1000_TXD_CMD_EOP   BIT(24)
#define E1000_TXD_CMD_IFCS  BIT(25)
#define E1000_TXD_CMD_RS    BIT(27)
#define E1000_TXD_STAT_DD   BIT(0)

/*
 * RX descriptor status bits.
 * bit 0 = DD   Descriptor Done
 * bit 1 = EOP  End of Packet
 * bit 2 = IXSM
 * bit 3 = VP
 * bit 4 = UDPCS
 * bit 5 = TCPCS
 * bit 6 = IPCS
 * bit 7 = PIF
 */
#define E1000_RXD_STAT_DD   BIT(0)
#define E1000_RXD_STAT_EOP  BIT(1)


struct e82576_tx_desc {
    __le64 buffer_addr;
    __le32 lower;
    __le32 upper;
};

struct e82576_rx_desc {
    __le64 buffer_addr;
    __le16 length;
    __le16 checksum;
    u8 status;
    u8 errors;
    __le16 special;
};


/*
 * ============================================================
 * MMIO
 * ============================================================
 */

static inline u32 e82576_read_reg(
    struct e82576_device *dev,
    u32 reg)
{
    return readl(dev->hw_addr + reg);
}


static inline void e82576_write_reg(
    struct e82576_device *dev,
    u32 reg,
    u32 value)
{
    writel(value, dev->hw_addr + reg);
}


static inline void e82576_flush(
    struct e82576_device *dev)
{
    e82576_read_reg(dev, E1000_STATUS);
}

/* hw */
int e82576_reset_hw(struct e82576_device *dev);

/* nvm */
void e82576_put_hw_semaphore(struct e82576_device *dev);
int e82576_get_hw_semaphore(struct e82576_device *dev);
int e82576_acquire_nvm(struct e82576_device *dev);
void e82576_release_nvm(struct e82576_device *dev);
int e82576_read_nvm_word(struct e82576_device *dev,u16 address, u16 *data);
int e82576_read_mac_address(struct e82576_device *dev);

/* phy */
int e82576_init_phy(struct e82576_device *dev);
int e82576_read_phy(struct e82576_device *dev, u8 phy, u8 reg, u16 *data);
int e82576_write_phy(struct e82576_device *dev, u8 phy, u8 reg, u16 data);
int e82576_find_phy(struct e82576_device *dev);
int e82576_reset_phy_hw(struct e82576_device *dev);
int e82576_get_link_status(struct e82576_device *dev);
int e82576_configure_phy(struct e82576_device *dev);
int e82576_wait_for_autoneg(struct e82576_device *dev);
void e82576_dump_phy_status(struct e82576_device *dev);

/* irq */
int e82576_init_msix(struct e82576_device *dev);
void e82576_cleanup_msix(struct e82576_device *dev);

/* tx */
int e82576_setup_tx_ring(struct e82576_device *dev);
void e82576_free_tx_ring(struct e82576_device *dev);
netdev_tx_t e82576_start_xmit(struct sk_buff *skb, struct net_device *netdev);
void e82576_tx_clean_work(struct work_struct *work);

/* rx */
int e82576_setup_rx_ring(struct e82576_device *dev);
void e82576_free_rx_ring(struct e82576_device *dev);
void e82576_rx_poll_work(struct work_struct *work);
void e82576_enable_dma(struct e82576_device *dev);

/* ring */
int e82576_setup_rings(struct e82576_device *dev);

#endif /* E82576_H */