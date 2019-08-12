# Lenovo YogaBook C930 E-Ink for Linux project

#### _tl;dr_ if you want to use the E-Ink keyboard on your YogaBook C930, this project will help you do it.

<br>

![Mandelbrot drawing on E-Ink keyboard](mandelbrot.jpg?raw=true)


<br>

## Background

The Lenovo YogaBook C930 **_(NOT THE LENOVO YOGA C930!!!!!)_** sports
an innovative all-in-one E-Ink display, touchpad, drawing tablet, and
keyboard instead of a traditional keyboard.

Ubuntu installs without a problem and graphics, sound, wifi, _and even
the Wacom touch-screen and stylus_ work out of the box. The one thing
that doesn't work is the E-Ink keyboard (and, of course, the usual Linux
laptop sleep and wake issues.)

Linux doesn't have much support for E-Ink displays in general, and no
support for E-Ink keyboards. They just haven't really existed until very
recently.

The one in the YogaBook is one-of-a-kind and its protocol is proprietary.
This project is my attempt to reverse-engineer the protocol so that,
first and foremost, there is a useable keyboard under Linux.

However, whilst attempting to do this, I found that it is quite easy to
display images on the E-Ink display too, so I have added it to the
driver.


## PLEASE NOTE

The driver is immature and the protocol is not well understood.
Using the driver may hang your system. It may even _brick_ your system.
I don't believe it's at all likely but: YOUR LAPTOP MAY BECOME UNUSABLE.
USE AT YOUR OWN RISK.


## Getting started

Do the steps in "Building the module" below. That should get your
keyboard up and running!

Unfortunately if the laptop sleeps or is closed, the driver may not
start again. This needs further investigation (as do other sleep-related
issues the laptop has.)


## Getting the keyboard back after sleep

Install SSH as soon as possible. This will help you get into your laptop
if you need to without rebooting.

Open `enable-eink-kb.sh`. These are reasonably reliable steps to unload
and reload the driver (since the usbhid driver uselessly "claims" the
eink device if it gets loaded first).

If you want to have a desktop icon to restore the keyboard if it gets
stuck:

 * copy the file `enable-eink-kb.desktop` to your Desktop
 * copy `enable-eink-kb.sh` to your /root/ directory
 * allow anyone to run this script by placing it in your `sudoers` file.
   Use the command `sudo visudo` and (assuming you know how to use `vi`)
   add this to the file:

    ALL   ALL=(root) NOPASSWD: /root/enable-eink-kb.sh



## Building the module

    sudo apt install linux-headers-generic build-essential

    cd yogabook-c930-linux-eink-driver # wherever you cloned the repo

    cd driver
    sudo make run
    # you may want to run `dmesg` now and check for errors

There are various recipes in the Makefile for development purposes.

**Only when you have tested the driver**, you can run `sudo make install`
to install the kernel module. This will load it at boot time.


# Development & contributing

The driver as-is lets you do a couple of things without needing to do any
kernel programming, as explained in the next section. This is a
work-in-progress.

One very cool thing that might be possible is to make the device a
second display. Perhaps a very simple X windowing system could be run on
it since the device has basic support for rectangular blitting.

There are 4 main areas where I could use help:

 * Further reverse-engineering of the Windows protocol/driver (I have
   made a Wireshark plugin that means this is easier than it sounds).
   This includes features such as drawing, handwriting recognition, and
   other functions that Windows supports,
 * Adding features to the driver and making it more reliable,
 * Working out how to get sleep and wake working reliably,
 * Creating user-space infrastructure, e.g. to use the device as a
   second display, maybe as an X backend, drawing and handwriting
   recognition... ...


## Talking to the driver from userspace

The driver exposes a file interface (detected by udev; seems to be a bit
flaky and disappears sometimes). It comes up as `/dev/eink0` or some
other number.

The commands can be found in `try_parse()` in the driver. As of writing
they are:

    * `init` — runs the initialisation sequence
    * `kb` — enables the keyboard after initialisation
    * `draw` — enables draw mode
    * `blit o x y w h` — does a rectangular copy from the device's internal
      buffer to the screen
    * `xfer o w h` — writes an array of pixels to the device's internal
      buffer

`init` and `kb` run automatically when the driver is loaded, so you may
not need them.

All commands are terminated with a newline character. However, _after_
this newline character, the `xfer` command accepts a sequence of bytes
exactly w×h characters long, with no newline at the end. If you stuff up
a command, you will probably have to restart the driver.

Be very careful about `o`, `w` and `h` as who knows what might happen if
you overwrite the wrong part of memory in the device. Probably nothing but
don't say I didn't warn you.


## Pixel transfer quirks

The pixel transfer mechanism is a little odd. Basically, you can transfer
a _screen-shaped_ rectangle of bytes into a _screen-shaped_ buffer inside
the device. (The max size of this buffer, etc., are still to be worked out.
It is at least 1920x1080 bytes in size though.)

Put another way, if you want to draw a 20x30 rectangle on the screen, you
will not have a sequence of 600 bytes in the internal buffer. Rather, you
will have 20 bytes and then don't-care for 1900 bytes and then another 20
bytes of your image and then don't care for 1900 bytes etc.

Put yet another way, the internal blit routine works like this:

    init_offset = y * 1920 + x
    for row = 0 to num_rows:
        for col = 0 to num_cols:
            offset = init_offset + col * 1920 + row
            display[offset] = buffer[offset]

using the same offset for the display and the buffer, rather than simply
incrementing the offset into the buffer.

From what I have seen so far, the Windows driver lines up the internal
buffer with the screen perfectly, however I have found that by tweaking
pointers you can load data to anywhere in the internal buffer and paste it
anywhere on the screen (potentially multiple times—like a sprite!)

The Windows driver seems to max out its packets at 61440 bytes, which is
exactly 1920×32 or 0xf000. So if it is paining a full-screen image, it
does it 32 rows at a time. Is this because of a 64KB packet limit? Maybe.


## E-Ink artifacts

I have tried moving little sprites around the screen but so far it seems
that the E-Ink artifacts mean this looks bad. I wonder whether the device
offers "cleaning" facilities or if a few manual repaints will do the trick.



## Protocol details

Like much of modern built-in hardware, the E-Ink display actually sits
on a USB bus (sorry for
[RAS syndrome](https://en.wikipedia.org/wiki/RAS_syndrome).)
Since USB has a simple packet-based protocol, you can record and analyse
the packet exchange using [Wireshark](https://www.wireshark.org/) with
[USBPcap](https://desowin.org/usbpcap/) on Windows.

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

Beside from that, use the Wireshark plugin and sniff around, and compare
to what is in the Linux driver file.

I painstakingly cut up and labelled every field you see in the Wireshark
plugin. Initially it was a confusing mess of 8, 16 and 32-bit fields but
over time it all started fitting together into something much simpler
and I feel more confident I've labelled things correctly.
Nonetheless, remember it is all guesswork so anything could be wrong.

There is a sample capture file called enable\_kb.pcap. The keyboard is
enabled after 8 seconds into the trace, where Windows does its scrolling
animation thing then displays a little menu in the top right corner.

If you'd like to contribute, contact me and I can explain further.


## Driver code

A good example to look at is `drivers/usb/usb-skeleton.c` in the Linux
tree.
