# How to use the image drawing commands

The way you issue commands to the driver (from userspace, by writing to
`/dev/einkX`) is a little odd because of how the (currently-known) data
transfer and display operations of the device work. I'll be explaining
that on this page. There are also a couple of Python examples you can
check out.

Remember that the driver is quick and dirty and the interface will
probably be changed and improved as more is discovered about the
protocol.


## tl;dr

If you want to display a single 20 × 30 image to the device and display
at (10, 50) and (100, 80):


    xfer 0 20 30
    <600 bytes of pixel data>
    blit 0 10 50 20 30
    blit 0 10 50 100 80

If you want to load multiple images into device RAM at the same time,
you need to vary the `o` parameter, like:

    xfer 0 20 30
    <600 bytes of pixel data>
    xfer 20 40 40
    <800 bytes of pixel data>

    blit 0 100 100 20 30
    blit 20 300 300 20 30

The tricky part is in picking `o` values which won't clash. Read on for
more.


## Rectangle views

It's intuitive that the framebuffer is laid out as an array of bytes,
1920 × 1080 bytes in length. The first byte is the top-left of the
display, the second byte is the pixel to the right of that, and so on.

You can also store images in the device's RAM and copy from them to the
framebuffer (which is much faster than copying them from the host
machine to the device).

The confusing part is that these images are not stored as a simple array
of bytes but rather as rectangles on a 'virtual screen' of size
1920 × 1080. The key thing is that you are not using `memcpy()`-like
functions but something that works with rectangles.


## The hardware protocol commands

There is a command (XFER) that takes an _array_ of bytes from the host
and copies it to a _rect_ of pixels in the device RAM. In host memory,
a 20x20 image would be an array of 400 bytes. However, in device RAM,
the image would be laid out as 20 bytes followed by 1900 bytes of
unrelated pixels and then the next 20 bytes of the image and another
1900 unrelated bytes and so on.

XFER does not trash these "in-between bytes". So you can store lots of
small images in device RAM, but it needs to be carefully managed to make
sure they don't overlap.

Then there is BLIT which copies a rectangle from device RAM to the
device framebuffer (i.e. it becomes visible).

Both BLIT and XFER take a pointer which defines the start of the
device-RAM buffer. (In the case of XFER, this is the destination buffer;
in the case of BLIT it is the source buffer.) They then take rectangular
co-ordinates within this buffer.

From the little I have seen so far, the Windows driver uses the same
pointer for both, effectively "lining up" and locking the co-ordinates
of the RAM buffer with the framebuffer. However, I have found that by
tweaking pointers you can load data to anywhere in the internal buffer
and paste it anywhere on the screen (potentially multiple times—like a
sprite!)

I suspect there is more than 1920 × 1080 bytes worth of usable RAM
space, however I have only used the region that Windows seems to use so
far.


## Transfer limits

The Windows driver seems to max out its XFER payloads at 61440 bytes,
which is exactly 1920 × 32 or 0xf000. So if it is painting a full-screen
image, it does it 32 rows at a time. Is this because of a 64KB packet
limit? Maybe. The driver doesn't support larger packets at this time.

However, you can XFER a large image into device RAM chunk by chunk and
then BLIT the whole thing in one go, for a fast visual change.


## Actually using the commands

The commands accepted by the driver are much simpler, but have a quirk
based on the rectangle thing.

The `o` parameter to `xfer` and `blit` is an offset _relative to the
pointer Windows uses_. To be safe, don't use anything less than 0 or
that might overflow the region. (If someone does more investigation then
hopefully we can figure out how much space is available.)

If you match `o`, `w` and `h` between an `xfer` and later `blit`
commands, you can vary `x` and `y` to put the image anywhere on the
screen that you like. (Out-of-bounds copies fail, I think? Fixable.)

So how do you select `o` parameters for your images?

If you only want to draw one image at a time, simply use 0. Otherwise,
you need to know that `o` is simply an index into a rectangle that is
1920 pixels wide. You can arrange your images however you like within
this rectangle as long as they don't overlap.

For example, if you have four images that are 900 pixels wide and 10
pixels tall, you might put two side by side, and the next two
underneath. Your four `o` parameters would be 0, 900, 19200, 20100.
This sets them up and blits the 2nd one to (50, 50).


    xfer 0 900 10
    <data>
    xfer 900 900 10
    <data>
    xfer 19200 900 10
    <data>
    xfer 20100 900 10
    <data>
    blit 900 50 50 900 10


## E-Ink artifacts

I have tried moving little sprites around the screen but so far it seems
that the E-Ink artifacts mean this looks bad. I wonder whether the device
offers "cleaning" facilities or if a few manual repaints will do the trick.
