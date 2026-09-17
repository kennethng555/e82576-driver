from scapy.all import Ether, Raw, sendp

pkt = Ether(
    src="00:e0:4c:29:16:24",
    dst="00:1b:22:57:1d:64"
) / Raw(b"hello 82576")

sendp(pkt, iface="eth0", count=1, verbose=True)
