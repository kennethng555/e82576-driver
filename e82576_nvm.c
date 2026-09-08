#include "e82576.h"

/*
 * ============================================================
 * HARDWARE NVM SEMAPHORE
 * ============================================================
 *
 * This is the important fix for:
 *
 *     NVM hardware semaphore busy
 *
 * We first acquire SMBI.
 *
 * Then we acquire SWESMBI.
 *
 * This follows the synchronization model used by Intel's
 * upstream igb implementation for this hardware family.
 */


/*
 * Release hardware semaphore.
 */

void e82576_put_hw_semaphore(
    struct e82576_device *dev)
{
    u32 swsm;
    swsm = e82576_read_reg(dev, E1000_SWSM);

    /*
     * Clear software semaphore first.
     */
    swsm &= ~E1000_SWSM_SWESMBI;

    e82576_write_reg(dev, E1000_SWSM, swsm);
    e82576_flush(dev);

    /*
     * Clear SMBI.
     */
    swsm = e82576_read_reg(dev, E1000_SWSM);
    swsm &= ~E1000_SWSM_SMBI;

    e82576_write_reg(dev, E1000_SWSM, swsm);
    e82576_flush(dev);
}


/*
 * Acquire hardware semaphore.
 */

int e82576_get_hw_semaphore(
    struct e82576_device *dev)
{
    u32 swsm;
    int i;

    /*
     * First wait for SMBI to become clear.
     *
     * Do not immediately force-clear it.
     *
     * Another function or firmware may legitimately own it.
     */
    for (i = 0; i < 200; i++) {
        swsm = e82576_read_reg(dev, E1000_SWSM);
        if (!(swsm & E1000_SWSM_SMBI))
            break;

        usleep_range(500, 600);
    }

    if (i == 200) {
        swsm = e82576_read_reg(dev, E1000_SWSM);

        dev_err(&dev->pdev->dev, "NVM hardware semaphore busy: SWSM=0x%08x\n", swsm);

        return -EBUSY;
    }

    /*
     * Request SMBI.
     */
    swsm = e82576_read_reg(dev, E1000_SWSM);
    swsm |= E1000_SWSM_SMBI;
    e82576_write_reg(dev, E1000_SWSM, swsm);
    e82576_flush(dev);

    /*
     * Verify that SMBI latched.
     */
    swsm = e82576_read_reg(dev, E1000_SWSM);
    if (!(swsm & E1000_SWSM_SMBI)) {
        dev_err(&dev->pdev->dev, "Failed to acquire NVM hardware semaphore: SWSM=0x%08x\n", swsm);
        return -EBUSY;
    }

    /*
     * Acquire SWESMBI.
     */
    for (i = 0; i < 200; i++) {
        swsm = e82576_read_reg(dev, E1000_SWSM);
        swsm |= E1000_SWSM_SWESMBI;
        e82576_write_reg(dev, E1000_SWSM, swsm);
        e82576_flush(dev);

        swsm = e82576_read_reg(dev, E1000_SWSM);
        if (swsm & E1000_SWSM_SWESMBI)
            return 0;

        usleep_range(500, 600);
    }

    dev_err(&dev->pdev->dev, "Failed to acquire NVM software semaphore\n");
    e82576_put_hw_semaphore(dev);

    return -EBUSY;
}


/*
 * ============================================================
 * SOFTWARE / FIRMWARE NVM LOCK
 * ============================================================
 */

int e82576_acquire_nvm(
    struct e82576_device *dev)
{
    u32 swfw;
    u32 swmask;
    u32 fwmask;

    int i;

    swmask = E1000_SWFW_EEP_SM;
    fwmask = E1000_SWFW_EEP_SM << 16;

    /*
     * The SW/FW synchronization register is protected by
     * the hardware semaphore.
     */
    for (i = 0; i < 200; i++) {
        int ret;

        ret = e82576_get_hw_semaphore(dev);
        if (ret)
            return ret;

        swfw = e82576_read_reg(dev, E1000_SW_FW_SYNC);

        /*
         * Either firmware or another software owner has
         * the EEPROM resource.
         */
        if (!(swfw & (swmask | fwmask)))
            break;

        dev_dbg(&dev->pdev->dev, "NVM resource busy: SW_FW_SYNC=0x%08x\n", swfw);
        e82576_put_hw_semaphore(dev);

        msleep(5);
    }

    if (i == 200) {
        dev_err(&dev->pdev->dev, "NVM SW/FW synchronization timeout\n");
        return -EBUSY;
    }

    /*
     * Claim the software side of the EEPROM semaphore.
     */
    swfw |= swmask;
    e82576_write_reg(dev, E1000_SW_FW_SYNC, swfw);
    e82576_flush(dev);

    /*
     * We no longer need the hardware semaphore while
     * performing the EERD operation.
     */
    e82576_put_hw_semaphore(dev);

    return 0;
}


/*
 * Release NVM software/firmware ownership.
 */
void e82576_release_nvm(
    struct e82576_device *dev)
{
    u32 swfw;
    int ret;

    /*
     * We need the hardware semaphore again before
     * modifying SW_FW_SYNC.
     */
    ret = e82576_get_hw_semaphore(dev);
    if (ret) {
        dev_err(&dev->pdev->dev, "Failed to reacquire NVM semaphore during release\n");
        return;
    }

    swfw = e82576_read_reg(dev, E1000_SW_FW_SYNC);
    swfw &= ~E1000_SWFW_EEP_SM;

    e82576_write_reg(dev, E1000_SW_FW_SYNC, swfw);
    e82576_flush(dev);

    e82576_put_hw_semaphore(dev);
}


/*
 * ============================================================
 * NVM READ
 * ============================================================
 */

int e82576_read_nvm_word(
    struct e82576_device *dev,
    u16 address,
    u16 *data)
{
    u32 value;
    int timeout;

    /*
     * Start EERD operation.
     */

    value = E1000_EERD_START | ((u32)address << E1000_EERD_ADDR_SHIFT);

    e82576_write_reg(dev, E1000_EERD, value);
    e82576_flush(dev);

    /*
     * Wait for DONE.
     */

    for (timeout = 0; timeout < 10000; timeout++) {
        value = e82576_read_reg(dev, E1000_EERD);
        if (value & E1000_EERD_DONE) {
            *data = (u16)((value >> E1000_EERD_DATA_SHIFT) & 0xffff);
            return 0;
        }

        udelay(10);
    }

    dev_err(&dev->pdev->dev, "NVM read timeout: address=0x%04x EERD=0x%08x\n", address, value);

    return -ETIMEDOUT;
}



/*
 * ============================================================
 * MAC ADDRESS
 * ============================================================
 */

int e82576_read_mac_address(
    struct e82576_device *dev)
{
    u16 word;
    int i;
    int ret;

    ret = e82576_acquire_nvm(dev);

    if (ret) {
        dev_err(&dev->pdev->dev, "Failed to acquire NVM: %d\n", ret);
        return ret;
    }

    for (i = 0; i < 3; i++) {
        ret = e82576_read_nvm_word(dev, i, &word);
        if (ret) {
            e82576_release_nvm(dev);
            return ret;
        }

        dev_info(&dev->pdev->dev, "NVM word %d = 0x%04x\n", i, word);

        dev->mac_address[i * 2] = word & 0xff;
        dev->mac_address[i * 2 + 1] = word >> 8;
    }


    e82576_release_nvm(dev);


    if (!is_valid_ether_addr(
            dev->mac_address)) {

        dev_err(
            &dev->pdev->dev,
            "Invalid MAC address %pM\n",
            dev->mac_address);


        return -EINVAL;
    }


    dev_info(
        &dev->pdev->dev,
        "MAC address: %pM\n",
        dev->mac_address);


    return 0;
}