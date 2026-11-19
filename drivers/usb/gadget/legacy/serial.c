#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/a52_ack_secure_flight_recorder.h>
// SPDX-License-Identifier: GPL-2.0+
/*
 * serial.c -- USB gadget serial driver
 *
 * Copyright (C) 2003 Al Borchers (alborchers@steinerpoint.com)
 * Copyright (C) 2008 by David Brownell
 * Copyright (C) 2008 by Nokia Corporation
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/a52_ack_secure_flight_recorder.h>

#include "u_serial.h"


/* Defines */

#define GS_VERSION_STR			"v2.4"
#define GS_VERSION_NUM			0x2400

#define GS_LONG_NAME			"Gadget Serial"
#define GS_VERSION_NAME			GS_LONG_NAME " " GS_VERSION_STR

/*-------------------------------------------------------------------------*/
USB_GADGET_COMPOSITE_OPTIONS();

/* Thanks to NetChip Technologies for donating this product ID.
*
* DO NOT REUSE THESE IDs with a protocol-incompatible driver!!  Ever!!
* Instead:  allocate your own, using normal USB-IF procedures.
*/
#define GS_VENDOR_ID			0x0525	/* NetChip */
#define GS_PRODUCT_ID			0xa4a6	/* Linux-USB Serial Gadget */
#define GS_CDC_PRODUCT_ID		0xa4a7	/* ... as CDC-ACM */
#define GS_CDC_OBEX_PRODUCT_ID		0xa4a9	/* ... as CDC-OBEX */

/* string IDs are assigned dynamically */

#define STRING_DESCRIPTION_IDX		USB_GADGET_FIRST_AVAIL_IDX

static struct usb_string strings_dev[] = {
	[USB_GADGET_MANUFACTURER_IDX].s = "",
	[USB_GADGET_PRODUCT_IDX].s = GS_VERSION_NAME,
	[USB_GADGET_SERIAL_IDX].s = "",
	[STRING_DESCRIPTION_IDX].s = NULL /* updated; f(use_acm) */,
	{  } /* end of list */
};

static struct usb_gadget_strings stringtab_dev = {
	.language	= 0x0409,	/* en-us */
	.strings	= strings_dev,
};

static struct usb_gadget_strings *dev_strings[] = {
	&stringtab_dev,
	NULL,
};

static struct usb_device_descriptor device_desc = {
	.bLength =		USB_DT_DEVICE_SIZE,
	.bDescriptorType =	USB_DT_DEVICE,
	/* .bcdUSB = DYNAMIC */
	/* .bDeviceClass = f(use_acm) */
	.bDeviceSubClass =	0,
	.bDeviceProtocol =	0,
	/* .bMaxPacketSize0 = f(hardware) */
	.idVendor =		cpu_to_le16(GS_VENDOR_ID),
	/* .idProduct =	f(use_acm) */
	.bcdDevice = cpu_to_le16(GS_VERSION_NUM),
	/* .iManufacturer = DYNAMIC */
	/* .iProduct = DYNAMIC */
	.bNumConfigurations =	1,
};

static const struct usb_descriptor_header *otg_desc[2];

/*-------------------------------------------------------------------------*/

/* Module */
MODULE_DESCRIPTION(GS_VERSION_NAME);
MODULE_AUTHOR("Al Borchers");
MODULE_AUTHOR("David Brownell");
MODULE_LICENSE("GPL");

static bool use_acm = true;
module_param(use_acm, bool, 0);
MODULE_PARM_DESC(use_acm, "Use CDC ACM, default=yes");

static bool use_obex = false;
module_param(use_obex, bool, 0);
MODULE_PARM_DESC(use_obex, "Use CDC OBEX, default=no");

static unsigned n_ports = 1;
module_param(n_ports, uint, 0);
MODULE_PARM_DESC(n_ports, "number of ports to create, default=1");

static bool enable = true;

static int switch_gserial_enable(bool do_enable);

static int enable_set(const char *s, const struct kernel_param *kp)
{
	bool do_enable;
	int ret;

	if (!s)	/* called for no-arg enable == default */
		return 0;

	ret = strtobool(s, &do_enable);
	if (ret || enable == do_enable)
		return ret;

	ret = switch_gserial_enable(do_enable);
	if (!ret)
		enable = do_enable;

	return ret;
}

static const struct kernel_param_ops enable_ops = {
	.set = enable_set,
	.get = param_get_bool,
};

module_param_cb(enable, &enable_ops, &enable, 0644);

/*-------------------------------------------------------------------------*/

static struct usb_configuration serial_config_driver = {
	/* .label = f(use_acm) */
	/* .bConfigurationValue = f(use_acm) */
	/* .iConfiguration = DYNAMIC */
	.bmAttributes	= USB_CONFIG_ATT_SELFPOWER,
};

static struct usb_function_instance *fi_serial[MAX_U_SERIAL_PORTS];
static struct usb_function *f_serial[MAX_U_SERIAL_PORTS];

static int serial_register_ports(struct usb_composite_dev *cdev,
		struct usb_configuration *c, const char *f_name)
{
	int i;
	int ret;

	ret = usb_add_config_only(cdev, c);
	if (ret)
		goto out;

	for (i = 0; i < n_ports; i++) {

		fi_serial[i] = usb_get_function_instance(f_name);
		if (IS_ERR(fi_serial[i])) {
			ret = PTR_ERR(fi_serial[i]);
			goto fail;
		}

		f_serial[i] = usb_get_function(fi_serial[i]);
		if (IS_ERR(f_serial[i])) {
			ret = PTR_ERR(f_serial[i]);
			goto err_get_func;
		}

		ret = usb_add_function(c, f_serial[i]);
		if (ret)
			goto err_add_func;
	}

	return 0;

err_add_func:
	usb_put_function(f_serial[i]);
err_get_func:
	usb_put_function_instance(fi_serial[i]);

fail:
	i--;
	while (i >= 0) {
		usb_remove_function(c, f_serial[i]);
		usb_put_function(f_serial[i]);
		usb_put_function_instance(fi_serial[i]);
		i--;
	}
out:
	return ret;
}

static int gs_bind(struct usb_composite_dev *cdev)
{
	int			status;

	/* Allocate string descriptor numbers ... note that string
	 * contents can be overridden by the composite_dev glue.
	 */

	status = usb_string_ids_tab(cdev, strings_dev);
	if (status < 0)
		goto fail;
	device_desc.iManufacturer = strings_dev[USB_GADGET_MANUFACTURER_IDX].id;
	device_desc.iProduct = strings_dev[USB_GADGET_PRODUCT_IDX].id;
	status = strings_dev[STRING_DESCRIPTION_IDX].id;
	serial_config_driver.iConfiguration = status;

	if (gadget_is_otg(cdev->gadget)) {
		if (!otg_desc[0]) {
			struct usb_descriptor_header *usb_desc;

			usb_desc = usb_otg_descriptor_alloc(cdev->gadget);
			if (!usb_desc) {
				status = -ENOMEM;
				goto fail;
			}
			usb_otg_descriptor_init(cdev->gadget, usb_desc);
			otg_desc[0] = usb_desc;
			otg_desc[1] = NULL;
		}
		serial_config_driver.descriptors = otg_desc;
		serial_config_driver.bmAttributes |= USB_CONFIG_ATT_WAKEUP;
	}

	/* register our configuration */
	if (use_acm) {
		status  = serial_register_ports(cdev, &serial_config_driver,
				"acm");
		usb_ep_autoconfig_reset(cdev->gadget);
	} else if (use_obex)
		status = serial_register_ports(cdev, &serial_config_driver,
				"obex");
	else {
		status = serial_register_ports(cdev, &serial_config_driver,
				"gser");
	}
	if (status < 0)
		goto fail1;

	usb_composite_overwrite_options(cdev, &coverwrite);
	INFO(cdev, "%s\n", GS_VERSION_NAME);

	return 0;
fail1:
	kfree(otg_desc[0]);
	otg_desc[0] = NULL;
fail:
	return status;
}

static int gs_unbind(struct usb_composite_dev *cdev)
{
	int i;

	a52_ackfr_record("U446 UNBIND gs_unbind enter");

	for (i = 0; i < n_ports; i++) {
		a52_ackfr_record("U446 UNBIND func_put enter i=%d",i);
		usb_put_function(f_serial[i]);
		a52_ackfr_record("U446 UNBIND func_put exit i=%d",i);
		a52_ackfr_record("U446 UNBIND inst_put enter i=%d",i);
		usb_put_function_instance(fi_serial[i]);
		a52_ackfr_record("U446 UNBIND inst_put exit i=%d",i);
	}

	kfree(otg_desc[0]);
	otg_desc[0] = NULL;

	a52_ackfr_record("U446 UNBIND gs_unbind exit");
	return 0;
}

static struct usb_composite_driver gserial_driver = {
	.name		= "g_serial",
	.dev		= &device_desc,
	.strings	= dev_strings,
	.max_speed	= USB_SPEED_SUPER,
	.bind		= gs_bind,
	.unbind		= gs_unbind,
};

static int switch_gserial_enable(bool do_enable)
{
	if (!serial_config_driver.label)
		/* init() was not called, yet */
		return 0;

	if (do_enable)
		return usb_composite_probe(&gserial_driver);

	usb_composite_unregister(&gserial_driver);
	return 0;
}

/* A52_PHASE381_GSERIAL_BIND_TRACE_V1 */

/* A52_PHASE446U_USB_SAFE_COORDINATOR_V2
 * No composite teardown from ConfigFS's gi->lock critical section.
 * Timeout returns -EAGAIN: a future UDC write may recheck the state.
 */
static bool a52u_usb_handover = true;
static int __init a52u_usb_handover_arg(char *value)
{
	if (!value) return -EINVAL;
	if (value[0]=='0' && !value[1]) a52u_usb_handover=false;
	else if (value[0]=='1' && !value[1]) a52u_usb_handover=true;
	else return -EINVAL;
	return 0;
}
early_param("a52.usb_handover",a52u_usb_handover_arg);

extern unsigned char a52w_early_console_line(void);
extern int a52_446u_preflight_port(unsigned char port_num);
extern void a52_446u_unblock_opens(unsigned char port_num);
extern int a52_446u_detach_console(unsigned char port_num);

static DEFINE_MUTEX(a52u_handover_lock);
static DECLARE_COMPLETION(a52u_handover_done);
static int a52u_state; /* 0=idle, 1=inflight, 2=success, 3=defer */
static int a52u_result;

static void a52u_handover_worker(struct work_struct *work);
static DECLARE_WORK(a52u_handover_work,a52u_handover_worker);

static void a52u_handover_worker(struct work_struct *work)
{
	int rc;
	a52_ackfr_record("U446 coordinator worker-enter");
	pr_info_ratelimited("P446W U446 COORDINATOR START console=ttyGS%u\n",a52w_early_console_line());
	rc=a52_446u_preflight_port(a52w_early_console_line());
	if (rc) goto done;
	rc=a52_446u_detach_console(a52w_early_console_line());
	if (rc) goto done; /* never unregister with a live console */
	a52_ackfr_record("U446 UNBIND composite-unregister-enter");
	rc=switch_gserial_enable(false);
	a52_ackfr_record("U446 UNBIND composite-unregister-exit rc=%d",rc);
	if (!rc) {
		enable=false;
		a52_ackfr_record("U446 UDC legacy-unregistered");
	}
done:
	/* If a worker or request is still active, no memory is freed here. */
	if (rc)
		a52_446u_unblock_opens(a52w_early_console_line());
	mutex_lock(&a52u_handover_lock);
	a52u_result=rc;
	a52u_state=rc ? 3 : 2;
	complete_all(&a52u_handover_done);
	mutex_unlock(&a52u_handover_lock);
	a52_ackfr_record("U446 coordinator done rc=%d",rc);
	pr_info_ratelimited("P446W U446 COORDINATOR DONE console=ttyGS%u rc=%d state=%d\n",a52w_early_console_line(),rc,a52u_state);
}
int a52_446u_release_early_serial(void)
{
	unsigned long ack;
	int rc;
	if (!a52u_usb_handover) {
		a52_ackfr_record("U446 HANDOVER_DISABLED COM3_PRESERVED");
		return 0; /* The still-owned UDC will reject Android's bind. */
	}
	if (!enable) return 0;
	mutex_lock(&a52u_handover_lock);
	if (a52u_state != 1 && a52u_state != 2) {
		a52u_state=1;
		reinit_completion(&a52u_handover_done);
		schedule_work(&a52u_handover_work);
	}
	mutex_unlock(&a52u_handover_lock);
	ack=wait_for_completion_timeout(&a52u_handover_done,
					msecs_to_jiffies(2500));
	if (!ack) {
		a52_ackfr_record("U446 coordinator bounded-timeout ret=EAGAIN");
		return -EAGAIN;
	}
	mutex_lock(&a52u_handover_lock);
	rc=a52u_result;
	mutex_unlock(&a52u_handover_lock);
	return rc ? -EAGAIN : 0;
}
EXPORT_SYMBOL_GPL(a52_446u_release_early_serial);

static int __init init(void)
{
	int a52_ret;

	a52_ackfr_record("V381 GS init enable=%u acm=%u obex=%u ports=%u",
			  enable, use_acm, use_obex, n_ports);
	/* We *could* export two configs; that'd be much cleaner...
	 * but neither of these product IDs was defined that way.
	 */
	if (use_acm) {
		serial_config_driver.label = "CDC ACM config";
		serial_config_driver.bConfigurationValue = 2;
		device_desc.bDeviceClass = USB_CLASS_COMM;
		device_desc.idProduct =
				cpu_to_le16(GS_CDC_PRODUCT_ID);
	} else if (use_obex) {
		serial_config_driver.label = "CDC OBEX config";
		serial_config_driver.bConfigurationValue = 3;
		device_desc.bDeviceClass = USB_CLASS_COMM;
		device_desc.idProduct =
			cpu_to_le16(GS_CDC_OBEX_PRODUCT_ID);
	} else {
		serial_config_driver.label = "Generic Serial config";
		serial_config_driver.bConfigurationValue = 1;
		device_desc.bDeviceClass = USB_CLASS_VENDOR_SPEC;
		device_desc.idProduct =
				cpu_to_le16(GS_PRODUCT_ID);
	}
	strings_dev[STRING_DESCRIPTION_IDX].s = serial_config_driver.label;

	if (!enable) {
		a52_ackfr_record("V381 GS disabled");
		return 0;
	}

	a52_ret = usb_composite_probe(&gserial_driver);
	a52_ackfr_record("V381 GS composite_probe ret=%d", a52_ret);
	a52_ackfr_usbdiag_set(A52_ACKFR_USB_GS_PROBE_RC, a52_ret);
	return a52_ret;
}
module_init(init);

static void __exit cleanup(void)
{
	if (enable)
		usb_composite_unregister(&gserial_driver);
}
module_exit(cleanup);
