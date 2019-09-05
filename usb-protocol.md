# USB Protocol

Like much of modern built-in hardware, the E-Ink display actually sits
on a USB bus (sorry for
[RAS syndrome](https://en.wikipedia.org/wiki/RAS_syndrome).)
Since USB has a simple packet-based protocol, you can record and analyse
the packet exchange using [Wireshark](https://www.wireshark.org/) with
[USBPcap](https://desowin.org/usbpcap/) on Windows.


## The three-packet exchange

The most important part to understand is the 3-packet exchange.
I'm not sure to what extent this fits within a standard USB or USB-SCSI
protocol but every transaction seems to work like this:

 * Host sends a control packet which asks the device to perform some
   function. This packet specifies whether it will send another packet
   (which I call DIR\_OUT) or is expecting the device to send a response
   packet (which I call DIR\_IN).
 * Host either sends another packet, or the device sends a packet, based
   on the previous step.
 * Device then sends an "OK" packet.

The Wireshark plugin labels these three packets with arrows and a tick
(check mark) to make it easier to follow.

I painstakingly cut up and labelled every field you see in the Wireshark
plugin. Initially it was a confusing mess of 8, 16 and 32-bit fields but
over time it all started fitting together into something much simpler
and I feel more confident I've labelled things correctly.
Nonetheless, remember it is all guesswork so anything could be wrong.

There is a sample capture file called enable\_kb.pcap. The keyboard is
enabled after 8 seconds into the trace, where Windows does its scrolling
animation thing then displays a little menu in the top right corner.

If you'd like to contribute, contact me and I can explain further.
