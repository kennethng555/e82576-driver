#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/if_ether.h>
#include <linux/delay.h>
#include <linux/bitfield.h>

#define DRIVER_NAME "e82576"

#define INTEL_VENDOR_ID       0x8086
#define INTEL_82576_DEVICE_ID 0x10c9


/*
 * ============================================================
 * 82576 Registers
 * ============================================================
 */

#define E1000_CTRL       0x00000
#define E1000_STATUS     0x00008
#define E1000_EERD       0x00014
#define E1000_CTRL_EXT   0x00018
#define E1000_MDIC       0x00020
#define E1000_IMC        0x000D8

#define E1000_RCTL       0x00100
#define E1000_TCTL       0x00400

#define E1000_EICR       0x01580

#define E1000_RAL0       0x05400
#define E1000_RAH0       0x05404


/*
 * ============================================================
 * CTRL bits
 * ============================================================
 */

#define E1000_CTRL_RST   BIT(26)


/*
 * ============================================================
 * RCTL/TCTL bits
 * ============================================================
 */

#define E1000_RCTL_EN    BIT(1)
#define E1000_TCTL_PSP   BIT(3)


/*
 * ============================================================
 * EERD
 * ============================================================
 *
 * EEPROM read request:
 *
 * bit 0     START
 * bits 15:8 DATA
 */

#define E1000_EERD_START       BIT(0)
#define E1000_EERD_DONE        BIT(1)
#define E1000_EERD_ADDR_SHIFT  2
#define E1000_EERD_DATA_MASK   GENMASK(31, 16)


/*
 * ============================================================
 * MDIC
 * ============================================================
 *
 * PHY register access.
 */

#define E1000_MDIC_DATA_MASK   GENMASK(15, 0)
#define E1000_MDIC_REG_SHIFT   16
#define E1000_MDIC_PHY_SHIFT   21
#define E1000_MDIC_OP_SHIFT    26

#define E1000_MDIC_OP_WRITE    1
#define E1000_MDIC_OP_READ     2

#define E1000_MDIC_READY       BIT(28)
#define E1000_MDIC_ERROR       BIT(30)


/*
 * ============================================================
 * PHY registers
 * ============================================================
 */

#define PHY_CTRL               0
#define PHY_STATUS             1
#define PHY_ID1                2
#define PHY_ID2                3
#define PHY_AUTONEG_ADV        4
#define PHY_1000T_CTRL         9


/*
 * PHY Control bits.
 */

#define PHY_CTRL_RESET         BIT(15)
#define PHY_CTRL_LOOPBACK      BIT(14)
#define PHY_CTRL_SPEED_LSB     BIT(13)
#define PHY_CTRL_AUTONEG       BIT(12)
#define PHY_CTRL_POWER_DOWN    BIT(11)
#define PHY_CTRL_RESTART_AN    BIT(9)
#define PHY_CTRL_DUPLEX        BIT(8)
#define PHY_CTRL_SPEED_MSB     BIT(6)


/*
 * PHY Status bits.
 */

#define PHY_STATUS_LINK        BIT(2)
#define PHY_STATUS_AUTONEG     BIT(5)


/*
 * ============================================================
 * Device structure
 * ============================================================
 */

struct e82576_device {
    struct pci_dev *pdev;

    void __iomem *hw_addr;

    resource_size_t bar0_start;
    resource_size_t bar0_length;

    /*
     * Information discovered during initialization.
     */

    u16 phy_id1;
    u16 phy_id2;

    u8 phy_address;

    u8 mac_address[ETH_ALEN];

    bool link_up;
    u32 link_speed;
    bool full_duplex;
};


/*
 * ============================================================
 * MMIO helpers
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
    writel(value,
           dev->hw_addr + reg);
}


static inline void e82576_flush(
    struct e82576_device *dev)
{
    e82576_read_reg(dev,
                    E1000_STATUS);
}


/*
 * ============================================================
 * PCI configuration
 * ============================================================
 */

static void e82576_dump_pci_config(
    struct pci_dev *pdev)
{
    u16 vendor;
    u16 device;
    u16 command;
    u16 status;
    u8 revision;

    pci_read_config_word(pdev,
                         PCI_VENDOR_ID,
                         &vendor);

    pci_read_config_word(pdev,
                         PCI_DEVICE_ID,
                         &device);

    pci_read_config_word(pdev,
                         PCI_COMMAND,
                         &command);

    pci_read_config_word(pdev,
                         PCI_STATUS,
                         &status);

    pci_read_config_byte(pdev,
                         PCI_REVISION_ID,
                         &revision);

    dev_info(&pdev->dev,
             "PCI configuration:\n");

    dev_info(&pdev->dev,
             "  Vendor ID : 0x%04x\n",
             vendor);

    dev_info(&pdev->dev,
             "  Device ID : 0x%04x\n",
             device);

    dev_info(&pdev->dev,
             "  Revision  : 0x%02x\n",
             revision);

    dev_info(&pdev->dev,
             "  Command   : 0x%04x\n",
             command);

    dev_info(&pdev->dev,
             "  Status    : 0x%04x\n",
             status);
}


/*
 * ============================================================
 * PCI BAR information
 * ============================================================
 */

static void e82576_dump_pci_resources(
    struct pci_dev *pdev)
{
    int i;

    dev_info(&pdev->dev,
             "PCI resources:\n");

    for (i = 0; i < PCI_STD_NUM_BARS; i++) {

        resource_size_t start;
        resource_size_t length;
        unsigned long flags;

        start = pci_resource_start(pdev, i);
        length = pci_resource_len(pdev, i);
        flags = pci_resource_flags(pdev, i);

        if (!length)
            continue;

        dev_info(&pdev->dev,
                 "  BAR%d:\n"
                 "    start  = 0x%llx\n"
                 "    length = 0x%llx\n"
                 "    flags  = 0x%lx\n",
                 i,
                 (unsigned long long)start,
                 (unsigned long long)length,
                 flags);
    }
}


/*
 * ============================================================
 * MSI
 * ============================================================
 */

static void e82576_dump_msi_info(
    struct pci_dev *pdev)
{
    int cap;
    u16 control;

    cap = pci_find_capability(pdev,
                              PCI_CAP_ID_MSI);

    if (!cap) {

        dev_info(&pdev->dev,
                 "MSI: not supported");

        return;
    }

    pci_read_config_word(pdev,
                         cap + PCI_MSI_FLAGS,
                         &control);

    dev_info(&pdev->dev,
             "MSI capability found at 0x%x\n",
             cap);

    dev_info(&pdev->dev,
             "  Control = 0x%04x\n",
             control);

    if (control & PCI_MSI_FLAGS_ENABLE)
        dev_info(&pdev->dev,
                 "  MSI status = ENABLED\n");
    else
        dev_info(&pdev->dev,
                 "  MSI status = DISABLED\n");

    if (control & PCI_MSI_FLAGS_64BIT)
        dev_info(&pdev->dev,
                 "  MSI address width = 64-bit\n");
    else
        dev_info(&pdev->dev,
                 "  MSI address width = 32-bit\n");
}


/*
 * ============================================================
 * MSI-X
 * ============================================================
 */

static void e82576_dump_msix_info(
    struct pci_dev *pdev)
{
    int cap;
    u16 control;
    u16 vectors;
    u32 table;
    u32 pba;

    cap = pci_find_capability(pdev,
                              PCI_CAP_ID_MSIX);

    if (!cap) {

        dev_info(&pdev->dev,
                 "MSI-X: not supported");

        return;
    }

    pci_read_config_word(pdev,
                         cap + PCI_MSIX_FLAGS,
                         &control);

    vectors =
        (control & PCI_MSIX_FLAGS_QSIZE) + 1;

    pci_read_config_dword(pdev,
                          cap + PCI_MSIX_TABLE,
                          &table);

    pci_read_config_dword(pdev,
                          cap + PCI_MSIX_PBA,
                          &pba);

    dev_info(&pdev->dev,
             "MSI-X capability found at 0x%x\n",
             cap);

    dev_info(&pdev->dev,
             "  Control      = 0x%04x\n",
             control);

    dev_info(&pdev->dev,
             "  Vector count = %u\n",
             vectors);

    dev_info(&pdev->dev,
             "  Table BAR    = %u\n",
             table & PCI_MSIX_TABLE_BIR);

    dev_info(&pdev->dev,
             "  Table offset = 0x%x\n",
             table & PCI_MSIX_TABLE_OFFSET);

    dev_info(&pdev->dev,
             "  PBA BAR      = %u\n",
             pba & PCI_MSIX_PBA_BIR);

    dev_info(&pdev->dev,
             "  PBA offset   = 0x%x\n",
             pba & PCI_MSIX_PBA_OFFSET);
}


static void e82576_dump_interrupt_capabilities(
    struct pci_dev *pdev)
{
    e82576_dump_msi_info(pdev);
    e82576_dump_msix_info(pdev);
}


/*
 * ============================================================
 * Register dump
 * ============================================================
 */

static void e82576_dump_registers(
    struct e82576_device *dev)
{
    dev_info(&dev->pdev->dev,
             "82576 registers:\n"
             "  CTRL   = 0x%08x\n"
             "  STATUS = 0x%08x\n"
             "  RCTL   = 0x%08x\n"
             "  TCTL   = 0x%08x\n"
             "  MDIC   = 0x%08x\n",
             e82576_read_reg(dev, E1000_CTRL),
             e82576_read_reg(dev, E1000_STATUS),
             e82576_read_reg(dev, E1000_RCTL),
             e82576_read_reg(dev, E1000_TCTL),
             e82576_read_reg(dev, E1000_MDIC));
}


/*
 * ============================================================
 * Hardware reset
 * ============================================================
 */

static int e82576_reset_hw(
    struct e82576_device *dev)
{
    u32 ctrl;
    int timeout;

    dev_info(&dev->pdev->dev,
             "Starting 82576 hardware reset\n");

    /*
     * Mask interrupts.
     */

    e82576_write_reg(dev,
                     E1000_IMC,
                     0xffffffff);

    e82576_flush(dev);


    /*
     * Disable RX.
     */

    e82576_write_reg(dev,
                     E1000_RCTL,
                     0);

    e82576_flush(dev);


    /*
     * Disable TX.
     */

    e82576_write_reg(dev,
                     E1000_TCTL,
                     E1000_TCTL_PSP);

    e82576_flush(dev);

    msleep(10);


    /*
     * Assert reset.
     */

    ctrl = e82576_read_reg(dev,
                           E1000_CTRL);

    dev_info(&dev->pdev->dev,
             "CTRL before reset: 0x%08x\n",
             ctrl);

    e82576_write_reg(dev,
                     E1000_CTRL,
                     ctrl | E1000_CTRL_RST);

    e82576_flush(dev);


    /*
     * Wait for reset to clear.
     */

    for (timeout = 100;
         timeout > 0;
         timeout--) {

        ctrl = e82576_read_reg(dev,
                               E1000_CTRL);

        if (!(ctrl & E1000_CTRL_RST))
            break;

        udelay(100);
    }

    if (!timeout) {

        dev_err(&dev->pdev->dev,
                "Hardware reset timeout\n");

        return -ETIMEDOUT;
    }

    dev_info(&dev->pdev->dev,
             "Hardware reset completed\n");

    /*
     * Clear pending interrupt causes.
     */

    e82576_read_reg(dev,
                    E1000_EICR);

    return 0;
}


/*
 * ============================================================
 * NVM / EEPROM access
 * ============================================================
 */

static int e82576_read_nvm(
    struct e82576_device *dev,
    u16 offset,
    u16 *value)
{
    u32 reg;
    int timeout;

    /*
     * Issue EEPROM read.
     */

    reg = E1000_EERD_START |
          ((u32)offset << E1000_EERD_ADDR_SHIFT);

    e82576_write_reg(dev,
                     E1000_EERD,
                     reg);

    e82576_flush(dev);


    /*
     * Poll for completion.
     */

    for (timeout = 1000;
         timeout > 0;
         timeout--) {

        reg = e82576_read_reg(dev,
                              E1000_EERD);

        if (reg & E1000_EERD_DONE)
            break;

        udelay(10);
    }


    if (!timeout) {

        dev_err(&dev->pdev->dev,
                "NVM read timeout at offset 0x%04x\n",
                offset);

        return -ETIMEDOUT;
    }


    /*
     * Extract data.
     */

    *value = FIELD_GET(E1000_EERD_DATA_MASK,
                       reg);

    return 0;
}


/*
 * ============================================================
 * Read MAC address from NVM
 * ============================================================
 *
 * Intel Ethernet EEPROM stores the MAC address in the first
 * three 16-bit words:
 *
 * word 0 = MAC[0:1]
 * word 1 = MAC[2:3]
 * word 2 = MAC[4:5]
 */

static int e82576_read_nvm_mac(
    struct e82576_device *dev)
{
    u16 word;
    int ret;
    int i;

    for (i = 0; i < 3; i++) {

        ret = e82576_read_nvm(dev,
                              i,
                              &word);

        if (ret)
            return ret;

        dev->mac_address[i * 2] =
            word & 0xff;

        dev->mac_address[i * 2 + 1] =
            (word >> 8) & 0xff;
    }

    return 0;
}


static void e82576_print_mac(
    struct e82576_device *dev)
{
    dev_info(&dev->pdev->dev,
             "NVM MAC address: "
             "%02x:%02x:%02x:%02x:%02x:%02x\n",
             dev->mac_address[0],
             dev->mac_address[1],
             dev->mac_address[2],
             dev->mac_address[3],
             dev->mac_address[4],
             dev->mac_address[5]);
}


/*
 * ============================================================
 * PHY MDIC access
 * ============================================================
 */

static int e82576_phy_read(
    struct e82576_device *dev,
    u8 phy,
    u16 reg,
    u16 *value)
{
    u32 mdic;
    int timeout;

    mdic =
        ((u32)reg << E1000_MDIC_REG_SHIFT) |
        ((u32)phy << E1000_MDIC_PHY_SHIFT) |
        ((u32)E1000_MDIC_OP_READ << E1000_MDIC_OP_SHIFT);

    e82576_write_reg(dev,
                     E1000_MDIC,
                     mdic);

    e82576_flush(dev);


    for (timeout = 1000;
         timeout > 0;
         timeout--) {

        mdic = e82576_read_reg(dev,
                               E1000_MDIC);

        if (mdic & E1000_MDIC_READY)
            break;

        udelay(10);
    }


    if (!timeout) {

        dev_err(&dev->pdev->dev,
                "PHY read timeout: "
                "phy=%u reg=%u\n",
                phy,
                reg);

        return -ETIMEDOUT;
    }


    if (mdic & E1000_MDIC_ERROR) {

        dev_err(&dev->pdev->dev,
                "PHY read error: "
                "phy=%u reg=%u MDIC=0x%08x\n",
                phy,
                reg,
                mdic);

        return -EIO;
    }


    *value =
        FIELD_GET(E1000_MDIC_DATA_MASK,
                  mdic);

    return 0;
}


static int e82576_phy_write(
    struct e82576_device *dev,
    u8 phy,
    u16 reg,
    u16 value)
{
    u32 mdic;
    int timeout;

    mdic =
        value |
        ((u32)reg << E1000_MDIC_REG_SHIFT) |
        ((u32)phy << E1000_MDIC_PHY_SHIFT) |
        ((u32)E1000_MDIC_OP_WRITE << E1000_MDIC_OP_SHIFT);

    e82576_write_reg(dev,
                     E1000_MDIC,
                     mdic);

    e82576_flush(dev);


    for (timeout = 1000;
         timeout > 0;
         timeout--) {

        mdic = e82576_read_reg(dev,
                               E1000_MDIC);

        if (mdic & E1000_MDIC_READY)
            break;

        udelay(10);
    }


    if (!timeout) {

        dev_err(&dev->pdev->dev,
                "PHY write timeout: "
                "phy=%u reg=%u\n",
                phy,
                reg);

        return -ETIMEDOUT;
    }


    if (mdic & E1000_MDIC_ERROR) {

        dev_err(&dev->pdev->dev,
                "PHY write error: "
                "phy=%u reg=%u\n",
                phy,
                reg);

        return -EIO;
    }

    return 0;
}


/*
 * ============================================================
 * Find PHY
 * ============================================================
 */

static int e82576_detect_phy(
    struct e82576_device *dev)
{
    u16 id1;
    u16 id2;
    int ret;
    int phy;


    /*
     * Scan the standard MDIO PHY addresses.
     */

    for (phy = 0;
         phy < 32;
         phy++) {

        ret = e82576_phy_read(dev,
                              phy,
                              PHY_ID1,
                              &id1);

        if (ret)
            continue;

        ret = e82576_phy_read(dev,
                              phy,
                              PHY_ID2,
                              &id2);

        if (ret)
            continue;


        /*
         * Invalid PHY IDs are commonly all-zero or all-one.
         */

        if (id1 == 0x0000 ||
            id1 == 0xffff)
            continue;

        if (id2 == 0x0000 ||
            id2 == 0xffff)
            continue;


        dev->phy_address = phy;
        dev->phy_id1 = id1;
        dev->phy_id2 = id2;

        dev_info(&dev->pdev->dev,
                 "PHY found at address %d\n",
                 phy);

        dev_info(&dev->pdev->dev,
                 "  PHY ID1 = 0x%04x\n",
                 id1);

        dev_info(&dev->pdev->dev,
                 "  PHY ID2 = 0x%04x\n",
                 id2);

        return 0;
    }


    dev_err(&dev->pdev->dev,
            "No PHY detected\n");

    return -ENODEV;
}


/*
 * ============================================================
 * PHY reset
 * ============================================================
 */

static int e82576_reset_phy(
    struct e82576_device *dev)
{
    u16 ctrl;
    int ret;
    int timeout;


    ret = e82576_phy_read(dev,
                          dev->phy_address,
                          PHY_CTRL,
                          &ctrl);

    if (ret)
        return ret;


    ctrl |= PHY_CTRL_RESET;

    ret = e82576_phy_write(dev,
                           dev->phy_address,
                           PHY_CTRL,
                           ctrl);

    if (ret)
        return ret;


    dev_info(&dev->pdev->dev,
             "PHY reset issued\n");


    /*
     * Wait for PHY reset bit to clear.
     */

    for (timeout = 1000;
         timeout > 0;
         timeout--) {

        ret = e82576_phy_read(
            dev,
            dev->phy_address,
            PHY_CTRL,
            &ctrl);

        if (ret)
            return ret;

        if (!(ctrl & PHY_CTRL_RESET))
            break;

        udelay(10);
    }


    if (!timeout) {

        dev_err(&dev->pdev->dev,
                "PHY reset timeout\n");

        return -ETIMEDOUT;
    }


    dev_info(&dev->pdev->dev,
             "PHY reset completed\n");

    return 0;
}


/*
 * ============================================================
 * PHY status
 * ============================================================
 */

static int e82576_get_link_status(
    struct e82576_device *dev)
{
    u16 status;
    u16 ctrl;
    int ret;


    /*
     * Read PHY status twice.
     *
     * The first read may latch/update link state on
     * some PHY implementations.
     */

    ret = e82576_phy_read(dev,
                          dev->phy_address,
                          PHY_STATUS,
                          &status);

    if (ret)
        return ret;


    ret = e82576_phy_read(dev,
                          dev->phy_address,
                          PHY_STATUS,
                          &status);

    if (ret)
        return ret;


    dev->link_up =
        !!(status & PHY_STATUS_LINK);


    if (!dev->link_up) {

        dev->link_speed = 0;
        dev->full_duplex = false;

        dev_info(&dev->pdev->dev,
                 "Link: DOWN\n");

        return 0;
    }


    /*
     * Read PHY control to determine configured speed/duplex.
     */

    ret = e82576_phy_read(dev,
                          dev->phy_address,
                          PHY_CTRL,
                          &ctrl);

    if (ret)
        return ret;


    /*
     * Speed encoding:
     *
     * 00 = 10 Mbps
     * 01 = 100 Mbps
     * 10 = 1000 Mbps
     *
     * SPEED bits are non-contiguous.
     */

    switch (ctrl &
            (PHY_CTRL_SPEED_MSB |
             PHY_CTRL_SPEED_LSB)) {

    case 0:

        dev->link_speed = 10;
        break;

    case PHY_CTRL_SPEED_LSB:

        dev->link_speed = 100;
        break;

    case PHY_CTRL_SPEED_MSB:

        dev->link_speed = 1000;
        break;

    default:

        dev->link_speed = 0;
        break;
    }


    dev->full_duplex =
        !!(ctrl & PHY_CTRL_DUPLEX);


    dev_info(&dev->pdev->dev,
             "Link: UP\n");

    dev_info(&dev->pdev->dev,
             "  Speed: %u Mbps\n",
             dev->link_speed);

    dev_info(&dev->pdev->dev,
             "  Duplex: %s\n",
             dev->full_duplex ?
             "Full" :
             "Half");

    return 0;
}


/*
 * ============================================================
 * PHY initialization
 * ============================================================
 */

static int e82576_init_phy(
    struct e82576_device *dev)
{
    int ret;


    dev_info(&dev->pdev->dev,
             "Initializing PHY\n");


    /*
     * Find PHY.
     */

    ret = e82576_detect_phy(dev);

    if (ret)
        return ret;


    /*
     * Reset PHY.
     */

    // ret = e82576_reset_phy(dev);

    // if (ret)
    //     return ret;


    /*
     * Check link.
     */

    ret = e82576_get_link_status(dev);

    if (ret)
        return ret;


    dev_info(&dev->pdev->dev,
             "PHY initialization complete\n");

    return 0;
}


/*
 * ============================================================
 * PCI probe
 * ============================================================
 */

static int e82576_probe(
    struct pci_dev *pdev,
    const struct pci_device_id *id)
{
    struct e82576_device *dev;
    int ret;


    dev_info(&pdev->dev,
             "e82576: probing Intel 82576\n");


    /*
     * Allocate driver state.
     */

    dev = devm_kzalloc(&pdev->dev,
                       sizeof(*dev),
                       GFP_KERNEL);

    if (!dev)
        return -ENOMEM;


    dev->pdev = pdev;

    pci_set_drvdata(pdev,
                    dev);


    /*
     * Enable PCI device.
     */

    ret = pci_enable_device(pdev);

    if (ret)
        return ret;


    /*
     * Enable PCI bus mastering.
     */

    pci_set_master(pdev);


    /*
     * PCI information.
     */

    e82576_dump_pci_config(pdev);

    e82576_dump_pci_resources(pdev);

    e82576_dump_interrupt_capabilities(pdev);


    /*
     * BAR0 must be MMIO.
     */

    if (!(pci_resource_flags(pdev, 0) &
          IORESOURCE_MEM)) {

        dev_err(&pdev->dev,
                "BAR0 is not a memory resource\n");

        ret = -ENODEV;
        goto err_disable;
    }


    dev->bar0_start =
        pci_resource_start(pdev, 0);

    dev->bar0_length =
        pci_resource_len(pdev, 0);


    /*
     * Claim BAR0.
     */

    ret = pci_request_region(pdev,
                             0,
                             DRIVER_NAME);

    if (ret)
        goto err_disable;


    /*
     * Map BAR0.
     */

    dev->hw_addr =
        pci_iomap(pdev,
                  0,
                  0);

    if (!dev->hw_addr) {

        ret = -ENOMEM;
        goto err_release;
    }


    dev_info(&pdev->dev,
             "BAR0 mapped at %p\n",
             dev->hw_addr);


    /*
     * Hardware reset.
     */

    ret = e82576_reset_hw(dev);

    if (ret)
        goto err_unmap;


    /*
     * --------------------------------------------------------
     * NVM initialization
     * --------------------------------------------------------
     */

    dev_info(&pdev->dev,
             "Reading MAC address from NVM\n");

    ret = e82576_read_nvm_mac(dev);

    if (ret) {

        dev_err(&pdev->dev,
                "Failed to read MAC from NVM: %d\n",
                ret);

        goto err_unmap;
    }


    e82576_print_mac(dev);


    /*
     * --------------------------------------------------------
     * PHY initialization
     * --------------------------------------------------------
     */

    ret = e82576_init_phy(dev);

    if (ret) {

        dev_err(&pdev->dev,
                "PHY initialization failed: %d\n",
                ret);

        goto err_unmap;
    }


    /*
     * --------------------------------------------------------
     * Initialization successful.
     * --------------------------------------------------------
     */

    dev_info(&pdev->dev,
             "====================================\n");

    dev_info(&pdev->dev,
             "82576 initialization successful\n");

    dev_info(&pdev->dev,
             "MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
             dev->mac_address[0],
             dev->mac_address[1],
             dev->mac_address[2],
             dev->mac_address[3],
             dev->mac_address[4],
             dev->mac_address[5]);

    dev_info(&pdev->dev,
             "PHY address: %u\n",
             dev->phy_address);

    dev_info(&pdev->dev,
             "Link: %s\n",
             dev->link_up ?
             "UP" :
             "DOWN");

    if (dev->link_up) {

        dev_info(&pdev->dev,
                 "Speed: %u Mbps\n",
                 dev->link_speed);

        dev_info(&pdev->dev,
                 "Duplex: %s\n",
                 dev->full_duplex ?
                 "Full" :
                 "Half");
    }

    dev_info(&pdev->dev,
             "====================================\n");


    return 0;


err_unmap:

    pci_iounmap(pdev,
                dev->hw_addr);

    dev->hw_addr = NULL;


err_release:

    pci_release_region(pdev,
                       0);


err_disable:

    pci_clear_master(pdev);

    pci_disable_device(pdev);

    return ret;
}


/*
 * ============================================================
 * PCI remove
 * ============================================================
 */

static void e82576_remove(
    struct pci_dev *pdev)
{
    struct e82576_device *dev;

    dev = pci_get_drvdata(pdev);


    dev_info(&pdev->dev,
             "e82576: removing driver\n");


    if (dev && dev->hw_addr) {

        pci_iounmap(pdev,
                    dev->hw_addr);

        dev->hw_addr = NULL;
    }


    pci_release_region(pdev,
                       0);

    pci_clear_master(pdev);

    pci_disable_device(pdev);


    dev_info(&pdev->dev,
             "e82576: removed successfully\n");
}


/*
 * ============================================================
 * PCI IDs
 * ============================================================
 */

static const struct pci_device_id e82576_pci_ids[] = {
    {
        PCI_DEVICE(INTEL_VENDOR_ID,
                   INTEL_82576_DEVICE_ID)
    },
    { 0, }
};

MODULE_DEVICE_TABLE(pci,
                    e82576_pci_ids);


/*
 * ============================================================
 * PCI driver
 * ============================================================
 */

static struct pci_driver e82576_driver = {
    .name     = DRIVER_NAME,
    .id_table = e82576_pci_ids,
    .probe    = e82576_probe,
    .remove   = e82576_remove,
};


module_pci_driver(e82576_driver);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION(
    "Minimal Intel 82576 PCIe driver");
MODULE_VERSION("0.5");