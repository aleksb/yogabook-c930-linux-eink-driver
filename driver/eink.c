#include <linux/blkdev.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/module.h>

#include <linux/usb.h>
#include <linux/usb/storage.h>

#include "menu.h"

#define USB_EINK_VENDOR_ID  0x048d
#define USB_EINK_PRODUCT_ID 0x8951

/* Get a minor range for your devices from the usb maintainer */
#define USB_EINK_MINOR_BASE	199

static struct usb_device_id eink_usb_ids[] = {
	{ USB_DEVICE(USB_EINK_VENDOR_ID, USB_EINK_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, eink_usb_ids);

static struct usb_driver eink_driver;

struct usb_eink {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char		*bulk_in_buffer;	/* the buffer to receive data */
	size_t			bulk_in_size;		/* the size of the receive buffer */
	size_t			bulk_in_filled;		/* number of bytes in the buffer */
	size_t			bulk_in_copied;		/* already copied to user space */
	__u8			bulk_in_endpointAddr;	/* the address of the bulk in endpoint */
	__u8			bulk_out_endpointAddr;	/* the address of the bulk out endpoint */
	int			errors;			/* the last request tanked */
	bool			ongoing_read;		/* a read is going on */
	spinlock_t		err_lock;		/* lock for errors */
	struct kref		kref;
	struct mutex		io_mutex;		/* synchronize I/O with disconnect */
	wait_queue_head_t	prot_wait;		/* to wait for protocol events */


	enum {
		NOT_WAITING,
		WAITING_FOR_WRITE_COMPLETE,
		WAITING_FOR_RESPONSE,
		WAITING_FOR_OK,
		WAITING_FOR_FRESHSTART,
	} prot_stage;

	/*
	enum {
		EINK_IDLE,
		//EINK_DOORKNOCK,
		EINK_INITIALISING,
		EINK_ENABLING_KB,
		EINK_ENABLING_DRAW,
		EINK_WRITING,
		EINK_BLITTING,
	} state;


	union {
		int initialising;  // count of doorknocks remaining
		int enabling_kb;   // integer sequence
		int enabling_draw; // integer sequence
	} st;

	*/
	uint32_t x, y, w, h, off; // commonly used parameters

	unsigned char user_buf[65536];
	size_t user_buf_len;

	struct semaphore sem_executing_usercmd;

};
#define to_eink_dev(d) container_of(d, struct usb_eink, kref)

#define err(s) dev_err(&dev->interface->dev, \
		"%s - %s\n", \
		__func__, s)



#define packet(s) (struct seq) { .data = (s), .len = (sizeof(s) - 1), }

#define CMD_PAYLOAD_SIZE 0x10

#define DIR_IN  0
#define DIR_OUT 1

#define LE_32(v) ((v) & 0xff), (((v)>>8) & 0xff), (((v)>>16) & 0xff), (((v)>>24) & 0xff)
#define BE_32(v) (((v)>>24) & 0xff), (((v)>>16) & 0xff), (((v)>>8) & 0xff), ((v) & 0xff)

#define LE_16(v) ((v) & 0xff), (((v)>>8) & 0xff)
#define BE_16(v) (((v)>>8) & 0xff), ((v) & 0xff)

#define BOILERPLATE 0x55, 0x53, 0x42, 0x43, 0x61, 0x89, 0x51, 0x89

#define ARR_COMMA_SIZE(arr) (arr), sizeof(arr)



#define BLIT_CMD(x, y, w, h) \
	((const char[]) {\
	BE_32(0x00382f30 ), BE_32(0x000000ff),\
	BE_32(x), BE_32(y), BE_32(w), BE_32(h), BE_32(0x0)} \
	)


#define NUM_DOORKNOCKS 4
#define SEND_DOORKNOCK() send_ctrl(dev, 112, DIR_IN, 0x80, 0x38393531, 1, 2, 0, 0)



void read_cb(struct usb_eink *dev);
void write_cb(struct usb_eink *dev);



static void eink_read_bulk_callback(struct urb *urb)
{
	struct usb_eink *dev;

	dev = urb->context;
	printk("eink: read_bulk_callback\n");

	spin_lock(&dev->err_lock);
	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
	urb->status == -ECONNRESET ||
	urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		dev->errors = urb->status;
	} else {
		dev->bulk_in_filled = urb->actual_length;
	}
	dev->ongoing_read = 0;
	spin_unlock(&dev->err_lock);

	//wake_up_interruptible(&dev->bulk_in_wait);

	if (!dev->bulk_in_filled) {
		printk("eink: Got 0-length packet?\n");
	} else {
		read_cb(dev);
	}

}

static int eink_do_read_io(struct usb_eink *dev, size_t count)
{
	int rv;
	printk("eink: initiating read\n");

	/* prepare a read */
	usb_fill_bulk_urb(dev->bulk_in_urb,
			dev->udev,
			usb_rcvbulkpipe(dev->udev,
				dev->bulk_in_endpointAddr),
			dev->bulk_in_buffer,
			min(dev->bulk_in_size, count),
			eink_read_bulk_callback,
			dev);
	/* tell everybody to leave the URB alone */
	spin_lock_irq(&dev->err_lock);
	dev->ongoing_read = 1;
	spin_unlock_irq(&dev->err_lock);

	/* submit bulk in urb, which means no data to deliver */
	dev->bulk_in_filled = 0;
	dev->bulk_in_copied = 0;

	/* do it */
	rv = usb_submit_urb(dev->bulk_in_urb, GFP_KERNEL);
	if (rv < 0) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting read urb, error %d\n",
			__func__, rv);
		rv = (rv == -ENOMEM) ? rv : -EIO;
		spin_lock_irq(&dev->err_lock);
		dev->ongoing_read = 0;
		spin_unlock_irq(&dev->err_lock);
	}
	printk("eink: submitted read\n");

	return rv;
}


static void eink_write_bulk_callback(struct urb *urb)
{
	struct usb_eink *dev;
	printk("eink: write callback\n");

	dev = urb->context;

	/* sync/async unlink faults aren't errors */
	if (urb->status) {
		if (!(urb->status == -ENOENT ||
	urb->status == -ECONNRESET ||
	urb->status == -ESHUTDOWN))
			dev_err(&dev->interface->dev,
				"%s - nonzero write bulk status received: %d\n",
				__func__, urb->status);

		spin_lock(&dev->err_lock);
		dev->errors = urb->status;
		spin_unlock(&dev->err_lock);
	}

	/* free up our allocated buffer */
	usb_free_coherent(urb->dev, urb->transfer_buffer_length,
	urb->transfer_buffer, urb->transfer_dma);

	write_cb(dev);
}

static void eink_delete(struct kref *kref)
{
	struct usb_eink *dev = to_eink_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}


static ssize_t eink_usb_write(struct usb_eink *dev, const char *data, size_t count)
{
	int retval = 0;
	struct urb *urb = NULL;
	char *buf = NULL;
	if (count > 65000)
		err("COUNT TOO BIG");
	printk("eink: starting write @%p (%ld bytes)\n", data, count);


	spin_lock_irq(&dev->err_lock);
	retval = dev->errors;
	if (retval < 0) {
		/* any error is reported once */
		dev->errors = 0;
		/* to preserve notifications about reset */
		retval = (retval == -EPIPE) ? retval : -EIO;
	}
	spin_unlock_irq(&dev->err_lock);
	if (retval < 0)
		goto error;

	/* create a urb, and a buffer for it, and copy the data to the urb */
	urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!urb) {
		retval = -ENOMEM;
		goto error;
	}

	buf = usb_alloc_coherent(dev->udev, count, GFP_KERNEL,
				 &urb->transfer_dma);
	if (!buf) {
		retval = -ENOMEM;
		goto error;
	}
	memcpy(buf, data, count);

	/* this lock makes sure we don't submit URBs to gone devices */
	mutex_lock(&dev->io_mutex);
	if (!dev->interface) {		/* disconnect() was called */
		mutex_unlock(&dev->io_mutex);
		retval = -ENODEV;
		goto error;
	}

	/* initialize the urb properly */
	usb_fill_bulk_urb(urb, dev->udev,
	usb_sndbulkpipe(dev->udev, dev->bulk_out_endpointAddr),
	buf, count, eink_write_bulk_callback, dev);
	urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;
	usb_anchor_urb(urb, &dev->submitted);

	/* send the data out the bulk port */
	retval = usb_submit_urb(urb, GFP_KERNEL);
	mutex_unlock(&dev->io_mutex);
	if (retval) {
		dev_err(&dev->interface->dev,
			"%s - failed submitting write urb, error %d\n",
			__func__, retval);
		goto error_unanchor;
	}

	/*
	 * release our reference to this urb, the USB core will eventually free
	 * it entirely
	 */
	usb_free_urb(urb);


	return count;

error_unanchor:
	usb_unanchor_urb(urb);
error:
	printk("eink: error in eink_usb_write %d\n", retval);
	if (urb) {
		usb_free_coherent(dev->udev, count, buf, urb->transfer_dma);
		usb_free_urb(urb);
	}

exit:
	return retval;
}



static int inline send_ctrl(struct usb_eink *dev, uint32_t payload_sz,
		bool dir, unsigned char cmd, uint32_t addr,
		uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {

	const char PACKET_ARRAY[] = { BOILERPLATE, LE_32(payload_sz),
		(dir == DIR_IN? 0x80 : 0), 0, CMD_PAYLOAD_SIZE, 0xfe, 0x00,
		BE_32(addr), cmd,
		BE_16(arg1), BE_16(arg2),
		BE_16(arg3), BE_16(arg4),
		0,
	};

	print_hex_dump(KERN_INFO, "Sending: ", DUMP_PREFIX_OFFSET, 16, 1,
		(PACKET_ARRAY-11), sizeof(PACKET_ARRAY)+11, true);

	if (dir == DIR_IN) {
		dev->prot_stage = WAITING_FOR_RESPONSE;
		eink_do_read_io(dev, 4000);
	} else {
		dev->prot_stage = WAITING_FOR_WRITE_COMPLETE;
	}

	// This will asynchronously set dev->prot_stage to NOT_WAITING
	// FIXME handle errors
	eink_usb_write(dev, PACKET_ARRAY, sizeof(PACKET_ARRAY));

	return wait_event_interruptible(dev->prot_wait,
					dev->prot_stage == NOT_WAITING);
}

void send_payload(struct usb_eink *dev, const unsigned char *data, size_t len)
{

	dev->prot_stage = WAITING_FOR_OK;
	eink_usb_write(dev, data, len);
}

void await_OK(struct usb_eink *dev)
{
	dev->prot_stage = WAITING_FOR_OK;
	eink_do_read_io(dev, 4000);

	wait_event_interruptible(dev->prot_wait,
				dev->prot_stage == NOT_WAITING);
}

void usercmd_init(struct usb_eink *dev)
{
	// FIXME handle interruptions
	printk("eink: sending init sequence\n");
	SEND_DOORKNOCK();
	WARN_ON(dev->bulk_in_filled != 112);
	await_OK(dev);
	SEND_DOORKNOCK();
	WARN_ON(dev->bulk_in_filled != 112);
	await_OK(dev);
	SEND_DOORKNOCK();
	WARN_ON(dev->bulk_in_filled != 112);
	await_OK(dev);
	SEND_DOORKNOCK();
	WARN_ON(dev->bulk_in_filled != 112);
	await_OK(dev);
}

void usercmd_enable_kb(struct usb_eink *dev)
{
	// FIXME handle interruptions

	uint32_t spin_limit = 20;
	uint32_t value = 0;
	printk("eink: enabling keyboard\n");

	send_ctrl(dev, 1, DIR_IN, 0xa9, 0, 0x200, 0, 0, 0);
	await_OK(dev);

	send_ctrl(dev, 4, DIR_IN, 0xa6, 0x00040000, 0, 0, 0, 0);
	await_OK(dev);

	// some kind of polling
	// FIXME sometimes this hangs unless there is a timeout, but
	// if we proceed anyway things seem to work. Fruther reversing
	// necessary
	while (spin_limit && value != 0x80000000) {
		// FIXME this is a crude way of dealing with signals
		if (send_ctrl(dev, 4, DIR_IN, 0x83, 0x18001224, 4, 0, 0, 0))
			break;

		WARN_ON(dev->bulk_in_filled != 4);
		value = be32_to_cpu(*(uint32_t *)dev->bulk_in_buffer);

		if (value != 0x80000000) {
			printk("eink: polling val @ %x", value);
		}

		await_OK(dev);

		// handle signals
		msleep_interruptible(100); // give it some time to work
		spin_limit--;
	}

	send_ctrl(dev, 4, DIR_IN, 0xa6, 0x00040000, 0, 0, 0, 0);
	await_OK(dev);
	send_ctrl(dev, 4, DIR_IN, 0xa6, 0x01010000, 0, 0, 0, 0);
	await_OK(dev);

	return;
}

void usercmd_enable_draw(struct usb_eink *dev)
{
	send_ctrl(dev, 4, DIR_IN, 0xa6, 0x01010000, 0, 0, 0, 0);
	await_OK(dev);
	send_ctrl(dev, 4, DIR_IN, 0xa6, 0x00000000, 0, 0, 0, 0);
	await_OK(dev);
	send_ctrl(dev, 4, DIR_IN, 0x83, 0x18001224, 4, 0, 0, 0);
	await_OK(dev);
	send_ctrl(dev, 4, DIR_IN, 0x83, 0x18001138, 4, 0, 0, 0);
	await_OK(dev);
	send_ctrl(dev, 4, DIR_OUT, 0x84, 0x18001138, 4, 0, 0, 0);

	send_payload(dev, ARR_COMMA_SIZE(((const char[]) {
		0, 0x2e, 0, 0
	})));

	await_OK(dev);
}

void usercmd_blit(struct usb_eink *dev)
{
	const uint32_t addr = 0x382f30 + dev->off - dev->x - 1920 * dev->y;
	const char BLIT_CMD[] = {
		BE_32(addr), BE_32(0xff), 
		BE_32(dev->x), BE_32(dev->y), 
		BE_32(dev->w), BE_32(dev->h), 
	};
	send_ctrl(dev, sizeof(BLIT_CMD), DIR_OUT, 0x94,
		0x00000000, 0, 0, 0, 0);

	send_payload(dev, ARR_COMMA_SIZE(BLIT_CMD));

	await_OK(dev);
}

void usercmd_xfer(struct usb_eink *dev)
{

	const size_t packet_size = dev->w * dev->h;
	printk("eink xfer: packet size: %ld %d %d\n",
					packet_size, dev->w, dev->h);

	send_ctrl(dev, packet_size, DIR_OUT, 0xa8, 0x00382f30 + dev->off,
		0, 0, dev->w, dev->h);

	send_payload(dev, dev->user_buf, packet_size);

	await_OK(dev);
}

void try_parse(struct usb_eink *dev)
{

	int matched, offset;
	unsigned char *scanner = dev->user_buf;

	size_t remainder;

	if (true)
		print_hex_dump(KERN_ERR, "Command: ", DUMP_PREFIX_OFFSET,
		16, 1,
		dev->user_buf, dev->user_buf_len, true);


	down_interruptible(&dev->sem_executing_usercmd);
#define TEST(str) (dev->user_buf_len >= strlen(str) && \
	memcmp(dev->user_buf, str, strlen(str)) == 0)

#define MEMMOVE_FROM_SCANNER() do {\
	remainder = (dev->user_buf + dev->user_buf_len) - scanner; \
	memmove(dev->user_buf, scanner, remainder); \
	dev->user_buf_len = remainder; \
	scanner = dev->user_buf; \
	} while (0)
	if (TEST("init\n")) {

		printk("eink: beginning init cmd\n");

		scanner += 5;
		MEMMOVE_FROM_SCANNER();


		usercmd_init(dev);

	} else if (TEST("kb\n")) {

		printk("eink: beginning kb cmd\n");

		scanner += 3;
		MEMMOVE_FROM_SCANNER();

		usercmd_enable_kb(dev);

	} else if (TEST("draw\n")) {

		printk("eink: beginning draw cmd\n");
		scanner += 5;
		MEMMOVE_FROM_SCANNER();

		usercmd_enable_draw(dev);

	} else if (TEST("blit")) {

		printk("eink: beginning blit cmd\n");

		scanner += 4;
		matched = sscanf(scanner, "%d%d%d%d%d\n%n",
					&dev->off,
					&dev->x, &dev->y,
					&dev->w, &dev->h,
					&offset);
		if (matched < 5 || 4 + offset > dev->user_buf_len) {
			up(&dev->sem_executing_usercmd);
			return; // don't have a complete command
		}

		scanner += offset;

		MEMMOVE_FROM_SCANNER();

		usercmd_blit(dev);


	} else if (TEST("xfer")) {

		printk("eink: beginning write cmd\n");
		scanner += 4;
		matched = sscanf(scanner, "%d%d%d\n%n",
					&dev->off,
					&dev->w, &dev->h,
					&offset);
		if (matched < 3 || 4 + offset > dev->user_buf_len) {
			up(&dev->sem_executing_usercmd);
			return; // don't have a complete command
		}
		scanner += offset;

		if (dev-> user_buf + dev->user_buf_len - scanner < dev->w * dev->h) {
			up(&dev->sem_executing_usercmd);
			return; // don't have a complete payload
		}

		MEMMOVE_FROM_SCANNER();

		usercmd_xfer(dev);

		scanner += dev->w * dev->h;

		MEMMOVE_FROM_SCANNER();
	} else {
		up(&dev->sem_executing_usercmd);
		return;
	}
	up(&dev->sem_executing_usercmd);
	try_parse(dev);
#undef MEMMOVE_FROM_SCANNER
#undef TEST
}

#define OK_PACKET ((char []){'U', 'S', 'B', 'S', LE_32(0x89518961), \
	                    0, 0, 0, 0, 0})

void read_cb(struct usb_eink *dev)
{

	const unsigned char *pkt = dev->bulk_in_buffer;
	const size_t len = dev->bulk_in_filled;

	printk("eink: read_cb, stage %d\n", dev->prot_stage);

	if (dev->prot_stage == WAITING_FOR_OK) {

		print_hex_dump(KERN_ERR, "Got packet: ", DUMP_PREFIX_OFFSET,
			16, 1,
			pkt-11, len+11, true);
		printk("eink: (%lu bytes)", len);
		WARN_ON(len != sizeof(OK_PACKET));
		WARN_ON(memcmp(pkt, ARR_COMMA_SIZE(OK_PACKET)));
	} else {
		if (len == sizeof(OK_PACKET) &&
			memcmp(pkt, OK_PACKET, sizeof(OK_PACKET))) {

			WARN_ON(dev->prot_stage != WAITING_FOR_FRESHSTART);
			dev->prot_stage == WAITING_FOR_OK;
			return read_cb(dev);
		}

		print_hex_dump(KERN_ERR, "Got packet: ", DUMP_PREFIX_OFFSET,
			16, 1,
			pkt-11, len+11, true);
		printk("eink: (%lu bytes)", len);

		WARN_ON(dev->prot_stage == WAITING_FOR_WRITE_COMPLETE);
	}

	dev->prot_stage = NOT_WAITING;
	wake_up_interruptible(&dev->prot_wait);
}

void write_cb(struct usb_eink *dev)
{

	printk("In write_cb, stage %d\n", dev->prot_stage);
	if (dev->prot_stage == WAITING_FOR_WRITE_COMPLETE) {
		dev->prot_stage = NOT_WAITING;
		wake_up_interruptible(&dev->prot_wait);
	} else {
		WARN_ON(dev->prot_stage == NOT_WAITING);
	}

}


static ssize_t eink_write(struct file *file, const char __user *buffer,
					size_t count, loff_t *ppos)
{

	struct usb_eink *dev = file->private_data;
	char *kbuf = dev->user_buf;
	const size_t space = sizeof(dev->user_buf) - dev->user_buf_len;
	printk("eink: in eink_write");

	count = min(count, space);
	if (copy_from_user(kbuf + dev->user_buf_len, buffer, count))
		return -EFAULT;

	dev->user_buf_len += count;

	try_parse(dev);
	

	return (ssize_t)count;
}

static int eink_open(struct inode *inode, struct file *file)
{
	struct usb_eink *dev;
	struct usb_interface *interface;
	int subminor;
	int retval = 0;

	subminor = iminor(inode);

	printk("<1> in eink_open");
	interface = usb_find_interface(&eink_driver, subminor);
	if (!interface) {
		pr_err("%s - error, can't find device for minor %d\n",
			__func__, subminor);
		retval = -ENODEV;
	printk("<1> enodev1");
		goto exit;
	}

	dev = usb_get_intfdata(interface);
	if (!dev) {
		retval = -ENODEV;
		goto exit;
	}

	retval = usb_autopm_get_interface(interface);
	if (retval)
		goto exit;

	/* increment our usage count for the device */
	kref_get(&dev->kref);

	/* save our object in the file's private structure */
	file->private_data = dev;

exit:
	return retval;
}


static void eink_freshstart(struct usb_eink *dev)
{

	// Do some reads to clear out any outstanding messages from
	// the device, otherwise everything gets out of sync

	// FIXME
	// Instead of trying to drain device reads with timeouts like this
	// maybe we should make it part of the init/doorknock process
	// and do it both now _and_ when userspace requests an init
	printk("eink-freshstart: doing a read");
	eink_do_read_io(dev, 4000);
	dev->prot_stage = WAITING_FOR_FRESHSTART;
	if (wait_event_timeout(dev->prot_wait,
				dev->prot_stage == NOT_WAITING,
				HZ / 40)) {
		return eink_freshstart(dev);
	}
	printk("eink-freshstart: timed out");
	usb_kill_urb(dev->bulk_in_urb);

	dev->errors = 0;
	dev->prot_stage = NOT_WAITING;

	usercmd_init(dev);
	usercmd_enable_kb(dev);

	// allow other processes to use it now
	up(&dev->sem_executing_usercmd);
}

static int eink_release(struct inode *inode, struct file *file)
{
	struct usb_eink *dev;

	dev = file->private_data;
	if (dev == NULL)
		return -ENODEV;

	/* allow the device to be autosuspended */
	mutex_lock(&dev->io_mutex);
	if (dev->interface)
		usb_autopm_put_interface(dev->interface);
	mutex_unlock(&dev->io_mutex);

	/* decrement the count on our device */
	kref_put(&dev->kref, eink_delete);
	return 0;
}



static const struct file_operations eink_fops = {
	.owner =	THIS_MODULE,
	//.read =		eink_read,
	.open =		eink_open,
	.write =	eink_write,
	.release =      eink_release,
	/*.flush =	eink_flush,
	.llseek =	noop_llseek,*/
};



/*
 * usb class driver info in order to get a minor number from the usb core,
 * and to have the device registered with the driver core
 */
static struct usb_class_driver eink_class = {
	.name =		"eink%d",
	.fops =		&eink_fops,
	.minor_base =	USB_EINK_MINOR_BASE,
};

static void eink_disconnect(struct usb_interface *interface)
{
	struct usb_eink *dev;
	int minor = interface->minor;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* give back our minor */
	usb_deregister_dev(interface, &eink_class);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->interface = NULL;
	mutex_unlock(&dev->io_mutex);

	usb_kill_anchored_urbs(&dev->submitted);

	/* decrement our usage count */
	kref_put(&dev->kref, eink_delete);

	dev_info(&interface->dev, "Eink device #%d now disconnected", minor);
}



static int eink_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	int retval;
	struct usb_device *udev = interface_to_usbdev(intf);
	struct usb_eink *dev;



	if (intf->cur_altsetting->desc.bInterfaceClass == 0xff) {
		struct usb_endpoint_descriptor *bulk_in, *bulk_out;
		printk("eink: found device with correct class\n");
		dev = kzalloc(sizeof(*dev), GFP_KERNEL);
		if (!dev)
			return -ENOMEM;

		kref_init(&dev->kref);
		mutex_init(&dev->io_mutex);
		spin_lock_init(&dev->err_lock);
		init_usb_anchor(&dev->submitted);
		init_waitqueue_head(&dev->prot_wait);

		dev->udev = usb_get_dev(udev);
		dev->interface = intf;

		/* set up the endpoint information */
		/* use only the first bulk-in and bulk-out endpoints */
		retval = usb_find_common_endpoints(intf->cur_altsetting,
				&bulk_in, &bulk_out, NULL, NULL);
		if (retval) {
			dev_err(&intf->dev,
					"Could not find both bulk-in and bulk-out endpoints\n");
			//goto error;
			err("Z\n");
		}

		dev->bulk_in_size = usb_endpoint_maxp(bulk_in);
		dev->bulk_in_endpointAddr = bulk_in->bEndpointAddress;
		dev->bulk_in_buffer = kmalloc(dev->bulk_in_size, GFP_KERNEL);
		if (!dev->bulk_in_buffer) {
			retval = -ENOMEM;
			goto error;
		}
		dev->bulk_in_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!dev->bulk_in_urb) {
			retval = -ENOMEM;
			goto error;
		}

		dev->bulk_out_endpointAddr = bulk_out->bEndpointAddress;

		/* save our data pointer in this interface device */
		usb_set_intfdata(intf, dev);

		/* we can register the device now, as it is ready */
		retval = usb_register_dev(intf, &eink_class);
		if (retval) {
			/* something prevented us from registering this driver */
			dev_err(&intf->dev,
				"Not able to get a minor for this device.\n");
			usb_set_intfdata(intf, NULL);
			goto error;
		}

		sema_init(&dev->sem_executing_usercmd, 0);

		/* let the user know what node this device is now attached to */
		dev_info(&intf->dev,
			 "USB EInk device now attached to USBeink-%d",
			 intf->minor);

		eink_freshstart(dev);


		return 0;

	}
	return -1;
error:
	/* this frees allocated memory */
	kref_put(&dev->kref, eink_delete);

	return retval;
}



static void eink_draw_down(struct usb_eink *dev)
{
	int time;

	time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&dev->submitted);
	usb_kill_urb(dev->bulk_in_urb);
}

static int eink_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_eink *dev = usb_get_intfdata(intf);

	printk("eink: eink_suspend\n");

	if (!dev)
		return 0;
	eink_draw_down(dev);
	return 0;
}

static int eink_resume(struct usb_interface *intf)
{
	struct usb_eink *dev = usb_get_intfdata(intf);

	printk("eink: eink_resume\n");
	return 0;
}

static int eink_pre_reset(struct usb_interface *intf)
{
	struct usb_eink *dev = usb_get_intfdata(intf);
	printk("eink: eink_pre_reset\n");

	mutex_lock(&dev->io_mutex);
	eink_draw_down(dev);

	return 0;
}

static int eink_post_reset(struct usb_interface *intf)
{
	struct usb_eink *dev = usb_get_intfdata(intf);
	printk("eink: eink_post_reset\n");

	/* we are sure no URBs are active - no locking needed */
	dev->errors = -EPIPE;
	mutex_unlock(&dev->io_mutex);

	return 0;
}


static struct usb_driver eink_driver = {
	.name = "eink",
	.probe = eink_probe,
	.disconnect = eink_disconnect,
	.pre_reset =  eink_pre_reset,
	.post_reset = eink_post_reset,
	.suspend =    eink_suspend,
	.resume =     eink_resume,
	.pre_reset =  eink_pre_reset,
	.post_reset = eink_post_reset,
	.supports_autosuspend = 1, // <--- need to check this
	.id_table = eink_usb_ids,
};


module_usb_driver(eink_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(
	"FIXME pls");

