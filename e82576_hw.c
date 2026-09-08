#include "e82576.h"

/*
 * ============================================================
 * HARDWARE RESET
 * ============================================================
 */

int e82576_reset_hw(
    struct e82576_device *dev)
{
    u32 ctrl;
    int timeout;

    /*
     * Disable interrupts before reset.
     */

    e82576_write_reg(dev, E1000_EIMC, 0xffffffff);
    e82576_flush(dev);

    /*
     * MAC reset.
     */
    ctrl = e82576_read_reg(dev, E1000_CTRL);
    ctrl |= E1000_CTRL_RST;

    e82576_write_reg(dev, E1000_CTRL, ctrl);
    e82576_flush(dev);

    for (timeout = 0; timeout < 1000; timeout++) {
        ctrl = e82576_read_reg(dev, E1000_CTRL);
        if (!(ctrl & E1000_CTRL_RST))
            break;

        udelay(10);
    }

    if (ctrl & E1000_CTRL_RST) {
        dev_err(&dev->pdev->dev, "MAC reset timeout\n");
        return -ETIMEDOUT;
    }

    msleep(10);

    return 0;
}