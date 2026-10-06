# from scapy.all import Ether, Raw, sendp

# pkt = Ether(
#     src="00:e0:4c:29:16:24",
#     dst="00:1b:22:57:1d:64"
# ) / Raw(b"hello 82576")

# sendp(pkt, iface="enp2s0", count=5, inter=0, verbose=True)


# This should show mtu 1500
# ip link show enp2s0
# Increase MTU to send larger packets
# sudo ip link set enp2s0 mtu 4000
from scapy.all import Ether, Raw, sendp

pkt = (
    Ether(
        src="00:e0:4c:29:16:24",
        dst="00:1b:22:57:1d:64",
        type=0x9000
    )
    / Raw(b"A" * 3000)
)

sendp(
    pkt,
    iface="enp2s0",
    count=1,
    verbose=True
)