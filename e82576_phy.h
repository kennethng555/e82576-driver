#ifndef E82576_PHY_H
#define E82576_PHY_H

#include "e82576.h"

int e82576_init_phy(struct e82576_device *dev);
int e82576_get_link_status(struct e82576_device *dev);
int e82576_read_phy(struct e82576_device *dev, u8 phy, u8 reg, u16 *data);
void e82576_dump_phy_status(struct e82576_device *dev);

#endif /* E82576_PHY_H */