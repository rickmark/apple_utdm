
// Kernel Environment
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kref.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/usb/storage.h>
#include <linux/usb/uas.h>
#include <linux/mutex.h>
#include <linux/init.h>

#include <usb.h>

// Apple USB Target Disk Mode Immutables
#define USB_APPLE_UTDM_VENDOR_ID	0x05ac
#define USB_APPLE_UTDM_PRODUCT_ID	0x1800
#define USB_APPLE_UTDM_CLASS_ID		0xdc  // Coresponds to USB Diagnostic class
#define USB_APPLE_UTDM_SUBCLASS_ID	0x02  // USB Target Disk Mode
#define USB_APPLE_UTDM_PROTOCOL_ID	0x01  // Likly matches USB_PR_CB Control/Bulk w/o interrupt

#define WRITES_IN_FLIGHT 8

/* Table of devices that work with this driver */
static const struct usb_device_id apple_utdm_table[] = {
	{ USB_DEVICE_AND_INTERFACE_INFO(USB_APPLE_UTDM_VENDOR_ID, USB_APPLE_UTDM_PRODUCT_ID, \
	  USB_APPLE_UTDM_CLASS_ID, USB_APPLE_UTDM_SUBCLASS_ID, USB_APPLE_UTDM_PROTOCOL_ID) },
	{ } /* Terminating entry */
};
MODULE_DEVICE_TABLE(usb, apple_utdm_table);

struct us_unusual_dev {
	const char* vendorName;
	const char* productName;
	__u8  useProtocol;
	__u8  useTransport;
	int (*initFunction)(struct us_data *);
};

/* Structure to hold all of our device specific stuff */
struct usb_apple_utdm {
	struct usb_device	*udev;			/* the usb device for this device */
	struct usb_interface	*interface;		/* the interface for this device */
	struct semaphore	limit_sem;		/* limiting the number of writes in progress */
	struct usb_anchor	submitted;		/* in case we need to retract our submissions */
	struct urb		*bulk_in_urb;		/* the urb to read data with */
	unsigned char           *bulk_in_buffer;	/* the buffer to receive data */
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
	unsigned long		disconnected:1;
	wait_queue_head_t	bulk_in_wait;		/* to wait for an ongoing read */
	// SCSI Control LUN
	// SCSI Apple Key Store LUN
	// SCSI Apple Effacable Storage LUN
	// SCSI Block Storage LUN
	struct us_data		*usdev;
	// Key state
};
#define to_apple_utdm_dev(d) container_of(d, struct usb_apple_utdm, kref)

// Module level state

static void apple_utdm_us_init(struct us_data *data) {
	return 0;
}

static const struct us_unusual_dev us_apple_us_unusual_dev = {
	"Apple",
	"Macintosh",
	USB_SC_SCSI,
	USB_PR_BULK,
	&apple_utdm_us_init
};

static struct usb_driver apple_utdm_driver;
static void apple_utdm_draw_down(struct usb_apple_utdm *dev);

static void apple_utdm_delete(struct kref *kref)
{
	struct usb_apple_utdm *dev = to_apple_utdm_dev(kref);

	usb_free_urb(dev->bulk_in_urb);
	usb_put_intf(dev->interface);
	usb_put_dev(dev->udev);
	kfree(dev->bulk_in_buffer);
	kfree(dev);
}

static int apple_utdm_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_apple_utdm *dev;
	struct usb_endpoint_descriptor *bulk_in, *bulk_out;
	int retval;

	/* allocate memory for our device state and initialize it */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	kref_init(&dev->kref);
	sema_init(&dev->limit_sem, WRITES_IN_FLIGHT);
	mutex_init(&dev->io_mutex);
	spin_lock_init(&dev->err_lock);
	init_usb_anchor(&dev->submitted);
	init_waitqueue_head(&dev->bulk_in_wait);

	dev->udev = usb_get_dev(interface_to_usbdev(interface));
	dev->interface = usb_get_intf(interface);

	/* set up the endpoint information */
	/* use only the first bulk-in and bulk-out endpoints */
	retval = usb_find_common_endpoints(interface->cur_altsetting,
			&bulk_in, &bulk_out, NULL, NULL);
	if (retval) {
		dev_err(&interface->dev,
			"Could not find both bulk-in and bulk-out endpoints\n");
		goto error;
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
	usb_set_intfdata(interface, dev);

	extern int usb_stor_probe1(&dev->usdev, dev->interface,
		NULL, // const struct usb_device_id *id,
		us_apple_us_unusual_dev,
		NULL); //struct scsi_host_template *sht);

	/* let the user know what node this device is now attached to */
	dev_info(&interface->dev,
		 "%s USB Target Disk Mode (Serial %s) attached",
		 dev->udev->manufacturer, dev->udev->serial);
	return 0;

error:
	/* this frees allocated memory */
	kref_put(&dev->kref, apple_utdm_delete);

	return retval;
}

static void apple_utdm_disconnect(struct usb_interface *interface)
{
	struct usb_apple_utdm *dev;

	dev = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);

	/* prevent more I/O from starting */
	mutex_lock(&dev->io_mutex);
	dev->disconnected = 1;
	mutex_unlock(&dev->io_mutex);

	usb_kill_anchored_urbs(&dev->submitted);

	/* decrement our usage count */
	kref_put(&dev->kref, apple_utdm_delete);

	dev_info(&interface->dev, "USB Target Disk Mode disconnected");
}

static int apple_utdm_suspend(struct usb_interface *intf, pm_message_t message)
{
	struct usb_apple_utdm *dev = usb_get_intfdata(intf);

	if (!dev)
		return 0;
	apple_utdm_draw_down(dev);
	return 0;
}

static int apple_utdm_resume(struct usb_interface *intf)
{
	return 0;
}

static void apple_utdm_draw_down(struct usb_apple_utdm *dev)
{
	int time;

	time = usb_wait_anchor_empty_timeout(&dev->submitted, 1000);
	if (!time)
		usb_kill_anchored_urbs(&dev->submitted);
	usb_kill_urb(dev->bulk_in_urb);
}

static int apple_utdm_pre_reset(struct usb_interface *intf)
{
	struct usb_apple_utdm *dev = usb_get_intfdata(intf);

	mutex_lock(&dev->io_mutex);
	apple_utdm_draw_down(dev);

	return 0;
}

static int apple_utdm_post_reset(struct usb_interface *intf)
{
	struct usb_apple_utdm *dev = usb_get_intfdata(intf);

	/* we are sure no URBs are active - no locking needed */
	dev->errors = -EPIPE;
	mutex_unlock(&dev->io_mutex);

	return 0;
}

static struct usb_driver apple_utdm_driver = {
	.name =		"apple_utdm",
	.probe =	apple_utdm_probe,
	.disconnect =	apple_utdm_disconnect,
	.suspend =	apple_utdm_suspend,
	.resume =	apple_utdm_resume,
	.pre_reset =	apple_utdm_pre_reset,
	.post_reset =	apple_utdm_post_reset,
	.id_table =	apple_utdm_table,
	.supports_autosuspend = 0,
};

static int __init apple_utdm_init(void)
{
	int result;

	result = usb_register(&apple_utdm_driver);
	if (result < 0) {
		err("usb_register failed for the "__FILE__" driver. ENO = %d", result);
		return -1;
	}

	return 0;
}

static void __exit apple_utdm_exit(void)
{
	usb_deregister(&apple_utdm_driver);

	return 0;
}

module_init(apple_utdm_init);
module_exit(apple_utdm_exit);

MODULE_LICENSE("GPL");
