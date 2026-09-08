#include "e82576.h"

int e82576_setup_rings(
    struct e82576_device *dev)
{
    int ret;

    ret = e82576_setup_tx_ring(dev);
    if (ret)
        return ret;

    ret = e82576_setup_rx_ring(dev);
    if (ret) {
        e82576_free_tx_ring(dev);
        return ret;
    }

    e82576_enable_dma(dev);

    return 0;
}
