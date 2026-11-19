#include <linux/ratelimit.h>
#include <linux/atomic.h>
unsigned char a52w_early_console_line(void);
#include <linux/completion.h>
#include <linux/fdtable.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/tty.h>
#include <linux/a52_ack_secure_flight_recorder.h>
// SPDX-License-Identifier: GPL-2.0+
/*
 * u_serial.c - utilities for USB gadget "serial port"/TTY support
 *
 * Copyright (C) 2003 Al Borchers (alborchers@steinerpoint.com)
 * Copyright (C) 2008 David Brownell
 * Copyright (C) 2008 by Nokia Corporation
 *
 * This code also borrows from usbserial.c, which is
 * Copyright (C) 1999 - 2002 Greg Kroah-Hartman (greg@kroah.com)
 * Copyright (C) 2000 Peter Berger (pberger@brimson.com)
 * Copyright (C) 2000 Al Borchers (alborchers@steinerpoint.com)
 */

/* #define VERBOSE_DEBUG */

#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/console.h>
#include <linux/kthread.h>
#include <linux/workqueue.h>
#include <linux/kfifo.h>

#include "u_serial.h"


/*
 * This component encapsulates the TTY layer glue needed to provide basic
 * "serial port" functionality through the USB gadget stack.  Each such
 * port is exposed through a /dev/ttyGS* node.
 *
 * After this module has been loaded, the individual TTY port can be requested
 * (gserial_alloc_line()) and it will stay available until they are removed
 * (gserial_free_line()). Each one may be connected to a USB function
 * (gserial_connect), or disconnected (with gserial_disconnect) when the USB
 * host issues a config change event. Data can only flow when the port is
 * connected to the host.
 *
 * A given TTY port can be made available in multiple configurations.
 * For example, each one might expose a ttyGS0 node which provides a
 * login application.  In one case that might use CDC ACM interface 0,
 * while another configuration might use interface 3 for that.  The
 * work to handle that (including descriptor management) is not part
 * of this component.
 *
 * Configurations may expose more than one TTY port.  For example, if
 * ttyGS0 provides login service, then ttyGS1 might provide dialer access
 * for a telephone or fax link.  And ttyGS2 might be something that just
 * needs a simple byte stream interface for some messaging protocol that
 * is managed in userspace ... OBEX, PTP, and MTP have been mentioned.
 *
 *
 * gserial is the lifecycle interface, used by USB functions
 * gs_port is the I/O nexus, used by the tty driver
 * tty_struct links to the tty/filesystem framework
 *
 * gserial <---> gs_port ... links will be null when the USB link is
 * inactive; managed by gserial_{connect,disconnect}().  each gserial
 * instance can wrap its own USB control protocol.
 *	gserial->ioport == usb_ep->driver_data ... gs_port
 *	gs_port->port_usb ... gserial
 *
 * gs_port <---> tty_struct ... links will be null when the TTY file
 * isn't opened; managed by gs_open()/gs_close()
 *	gserial->port_tty ... tty_struct
 *	tty_struct->driver_data ... gserial
 */

/* RX and TX queues can buffer QUEUE_SIZE packets before they hit the
 * next layer of buffering.  For TX that's a circular buffer; for RX
 * consider it a NOP.  A third layer is provided by the TTY code.
 */
#define QUEUE_SIZE		16
#define WRITE_BUF_SIZE		8192		/* TX only */
#define GS_CONSOLE_BUF_SIZE	(256 * 1024)

/* Prevents race conditions while accessing gser->ioport */
static DEFINE_SPINLOCK(serial_port_lock);

/* console info */
/* A52_PHASE380_USB_KTHREAD_CONSOLE_V1 */
struct gs_console {
	struct console		console;
	struct task_struct	*task;
	struct completion	a52u_worker_ack;
	struct completion	a52u_request_ack;
	wait_queue_head_t	a52u_req_wait;
	atomic_t	a52u_cb_active;
	bool			a52u_stopping;
	bool			a52u_detached;
	bool			a52u_req_pending;
	wait_queue_head_t	wait;
	atomic_t		kick;
	spinlock_t		lock;
	struct usb_request	*req;
	struct kfifo		buf;
	size_t			missed;
};

/*
 * The port structure holds info for each port, one for each minor number
 * (and thus for each /dev/ node).
 */
struct gs_port {
	struct tty_port		port;
	bool			a52u_block_opens;
	spinlock_t		port_lock;	/* guard port_* access */

	struct gserial		*port_usb;
#ifdef CONFIG_U_SERIAL_CONSOLE
	struct gs_console	*console;
#endif

	u8			port_num;

	struct list_head	read_pool;
	int read_started;
	int read_allocated;
	struct list_head	read_queue;
	unsigned		n_read;
	struct delayed_work	push;

	struct list_head	write_pool;
	int write_started;
	int write_allocated;
	struct kfifo		port_write_buf;
	wait_queue_head_t	drain_wait;	/* wait while writes drain */
	bool                    write_busy;
	wait_queue_head_t	close_wait;
	bool			suspended;	/* port suspended */
	bool			start_delayed;	/* delay start when suspended */

	/* REVISIT this state ... */
	struct usb_cdc_line_coding port_line_coding;	/* 8-N-1 etc */
};

static struct portmaster {
	struct mutex	lock;			/* protect open/close */
	struct gs_port	*port;
} ports[MAX_U_SERIAL_PORTS];

#define GS_CLOSE_TIMEOUT		15		/* seconds */



#ifdef VERBOSE_DEBUG
#ifndef pr_vdebug
#define pr_vdebug(fmt, arg...) \
	pr_debug(fmt, ##arg)
#endif /* pr_vdebug */
#else
#ifndef pr_vdebug
#define pr_vdebug(fmt, arg...) \
	({ if (0) pr_debug(fmt, ##arg); })
#endif /* pr_vdebug */
#endif

/*-------------------------------------------------------------------------*/

/* I/O glue between TTY (upper) and USB function (lower) driver layers */

/*
 * gs_alloc_req
 *
 * Allocate a usb_request and its buffer.  Returns a pointer to the
 * usb_request or NULL if there is an error.
 */
struct usb_request *
gs_alloc_req(struct usb_ep *ep, unsigned len, gfp_t kmalloc_flags)
{
	struct usb_request *req;

	req = usb_ep_alloc_request(ep, kmalloc_flags);

	if (req != NULL) {
		req->length = len;
		req->buf = kmalloc(len, kmalloc_flags);
		if (req->buf == NULL) {
			usb_ep_free_request(ep, req);
			return NULL;
		}
	}

	return req;
}
EXPORT_SYMBOL_GPL(gs_alloc_req);

/*
 * gs_free_req
 *
 * Free a usb_request and its buffer.
 */
void gs_free_req(struct usb_ep *ep, struct usb_request *req)
{
	kfree(req->buf);
	usb_ep_free_request(ep, req);
}
EXPORT_SYMBOL_GPL(gs_free_req);

/*
 * gs_send_packet
 *
 * If there is data to send, a packet is built in the given
 * buffer and the size is returned.  If there is no data to
 * send, 0 is returned.
 *
 * Called with port_lock held.
 */
static unsigned
gs_send_packet(struct gs_port *port, char *packet, unsigned size)
{
	unsigned len;

	len = kfifo_len(&port->port_write_buf);
	if (len < size)
		size = len;
	if (size != 0)
		size = kfifo_out(&port->port_write_buf, packet, size);
	return size;
}

/*
 * gs_start_tx
 *
 * This function finds available write requests, calls
 * gs_send_packet to fill these packets with data, and
 * continues until either there are no more write requests
 * available or no more data to send.  This function is
 * run whenever data arrives or write requests are available.
 *
 * Context: caller owns port_lock; port_usb is non-null.
 */
static int gs_start_tx(struct gs_port *port)
/*
__releases(&port->port_lock)
__acquires(&port->port_lock)
*/
{
	struct list_head	*pool = &port->write_pool;
	struct usb_ep		*in;
	int			status = 0;
	bool			do_tty_wake = false;

	if (!port->port_usb)
		return status;

	in = port->port_usb->in;

	while (!port->write_busy && !list_empty(pool)) {
		struct usb_request	*req;
		int			len;

		if (port->write_started >= QUEUE_SIZE)
			break;

		req = list_entry(pool->next, struct usb_request, list);
		len = gs_send_packet(port, req->buf, in->maxpacket);
		if (len == 0) {
			wake_up_interruptible(&port->drain_wait);
			break;
		}
		do_tty_wake = true;

		req->length = len;
		list_del(&req->list);
		req->zero = kfifo_is_empty(&port->port_write_buf);

		pr_vdebug("ttyGS%d: tx len=%d, %3ph ...\n", port->port_num, len, req->buf);

		/* Drop lock while we call out of driver; completions
		 * could be issued while we do so.  Disconnection may
		 * happen too; maybe immediately before we queue this!
		 *
		 * NOTE that we may keep sending data for a while after
		 * the TTY closed (dev->ioport->port_tty is NULL).
		 */
		port->write_busy = true;
		spin_unlock(&port->port_lock);
		status = usb_ep_queue(in, req, GFP_ATOMIC);
		spin_lock(&port->port_lock);
		port->write_busy = false;

		if (status) {
			pr_debug("%s: %s %s err %d\n",
					__func__, "queue", in->name, status);
			list_add(&req->list, pool);
			break;
		}

		port->write_started++;

		/* abort immediately after disconnect */
		if (!port->port_usb)
			break;
	}

	if (do_tty_wake)
		tty_port_tty_wakeup(&port->port);
	return status;
}

/*
 * Context: caller owns port_lock, and port_usb is set
 */
static unsigned gs_start_rx(struct gs_port *port)
/*
__releases(&port->port_lock)
__acquires(&port->port_lock)
*/
{
	struct list_head	*pool = &port->read_pool;
	struct usb_ep		*out = port->port_usb->out;

	while (!list_empty(pool)) {
		struct usb_request	*req;
		int			status;
		struct tty_struct	*tty;

		/* no more rx if closed */
		tty = port->port.tty;
		if (!tty)
			break;

		if (port->read_started >= QUEUE_SIZE)
			break;

		req = list_entry(pool->next, struct usb_request, list);
		list_del(&req->list);
		req->length = out->maxpacket;

		/* drop lock while we call out; the controller driver
		 * may need to call us back (e.g. for disconnect)
		 */
		spin_unlock(&port->port_lock);
		status = usb_ep_queue(out, req, GFP_ATOMIC);
		spin_lock(&port->port_lock);

		if (status) {
			pr_debug("%s: %s %s err %d\n",
					__func__, "queue", out->name, status);
			list_add(&req->list, pool);
			break;
		}
		port->read_started++;

		/* abort immediately after disconnect */
		if (!port->port_usb)
			break;
	}
	return port->read_started;
}

/*
 * RX work takes data out of the RX queue and hands it up to the TTY
 * layer until it refuses to take any more data (or is throttled back).
 * Then it issues reads for any further data.
 *
 * If the RX queue becomes full enough that no usb_request is queued,
 * the OUT endpoint may begin NAKing as soon as its FIFO fills up.
 * So QUEUE_SIZE packets plus however many the FIFO holds (usually two)
 * can be buffered before the TTY layer's buffers (currently 64 KB).
 */
static void gs_rx_push(struct work_struct *work)
{
	struct delayed_work	*w = to_delayed_work(work);
	struct gs_port		*port = container_of(w, struct gs_port, push);
	struct tty_struct	*tty;
	struct list_head	*queue = &port->read_queue;
	bool			disconnect = false;
	bool			do_push = false;

	/* hand any queued data to the tty */
	spin_lock_irq(&port->port_lock);
	tty = port->port.tty;
	while (!list_empty(queue)) {
		struct usb_request	*req;

		req = list_first_entry(queue, struct usb_request, list);

		/* leave data queued if tty was rx throttled */
		if (tty && tty_throttled(tty))
			break;

		switch (req->status) {
		case -ESHUTDOWN:
			disconnect = true;
			pr_vdebug("ttyGS%d: shutdown\n", port->port_num);
			break;

		default:
			/* presumably a transient fault */
			pr_warn("ttyGS%d: unexpected RX status %d\n",
				port->port_num, req->status);
			fallthrough;
		case 0:
			/* normal completion */
			break;
		}

		/* push data to (open) tty */
		if (req->actual && tty) {
			char		*packet = req->buf;
			unsigned	size = req->actual;
			unsigned	n;
			int		count;

			/* we may have pushed part of this packet already... */
			n = port->n_read;
			if (n) {
				packet += n;
				size -= n;
			}

			count = tty_insert_flip_string(&port->port, packet,
					size);
			if (count)
				do_push = true;
			if (count != size) {
				/* stop pushing; TTY layer can't handle more */
				port->n_read += count;
				pr_vdebug("ttyGS%d: rx block %d/%d\n",
					  port->port_num, count, req->actual);
				break;
			}
			port->n_read = 0;
		}

		list_move(&req->list, &port->read_pool);
		port->read_started--;
	}

	/* Push from tty to ldisc; this is handled by a workqueue,
	 * so we won't get callbacks and can hold port_lock
	 */
	if (do_push)
		tty_flip_buffer_push(&port->port);


	/* We want our data queue to become empty ASAP, keeping data
	 * in the tty and ldisc (not here).  If we couldn't push any
	 * this time around, RX may be starved, so wait until next jiffy.
	 *
	 * We may leave non-empty queue only when there is a tty, and
	 * either it is throttled or there is no more room in flip buffer.
	 */
	if (!list_empty(queue) && !tty_throttled(tty))
		schedule_delayed_work(&port->push, 1);

	/* If we're still connected, refill the USB RX queue. */
	if (!disconnect && port->port_usb)
		gs_start_rx(port);

	spin_unlock_irq(&port->port_lock);
}

static void gs_read_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct gs_port	*port = ep->driver_data;

	/* Queue all received data until the tty layer is ready for it. */
	spin_lock(&port->port_lock);
	list_add_tail(&req->list, &port->read_queue);
	schedule_delayed_work(&port->push, 0);
	spin_unlock(&port->port_lock);
}

static void gs_write_complete(struct usb_ep *ep, struct usb_request *req)
{
	struct gs_port	*port = ep->driver_data;

	spin_lock(&port->port_lock);
	list_add(&req->list, &port->write_pool);
	port->write_started--;

	switch (req->status) {
	default:
		/* presumably a transient fault */
		pr_warn("%s: unexpected %s status %d\n",
			__func__, ep->name, req->status);
		fallthrough;
	case 0:
		/* normal completion */
		gs_start_tx(port);
		break;

	case -ESHUTDOWN:
		/* disconnect */
		pr_vdebug("%s: %s shutdown\n", __func__, ep->name);
		break;
	}

	spin_unlock(&port->port_lock);
}

static void gs_free_requests(struct usb_ep *ep, struct list_head *head,
							 int *allocated)
{
	struct usb_request	*req;

	while (!list_empty(head)) {
		req = list_entry(head->next, struct usb_request, list);
		list_del(&req->list);
		gs_free_req(ep, req);
		if (allocated)
			(*allocated)--;
	}
}

static int gs_alloc_requests(struct usb_ep *ep, struct list_head *head,
		void (*fn)(struct usb_ep *, struct usb_request *),
		int *allocated)
{
	int			i;
	struct usb_request	*req;
	int n = allocated ? QUEUE_SIZE - *allocated : QUEUE_SIZE;

	/* Pre-allocate up to QUEUE_SIZE transfers, but if we can't
	 * do quite that many this time, don't fail ... we just won't
	 * be as speedy as we might otherwise be.
	 */
	for (i = 0; i < n; i++) {
		req = gs_alloc_req(ep, ep->maxpacket, GFP_ATOMIC);
		if (!req)
			return list_empty(head) ? -ENOMEM : 0;
		req->complete = fn;
		list_add_tail(&req->list, head);
		if (allocated)
			(*allocated)++;
	}
	return 0;
}

/**
 * gs_start_io - start USB I/O streams
 * @port: port to use
 * Context: holding port_lock; port_tty and port_usb are non-null
 *
 * We only start I/O when something is connected to both sides of
 * this port.  If nothing is listening on the host side, we may
 * be pointlessly filling up our TX buffers and FIFO.
 */
static int gs_start_io(struct gs_port *port)
{
	struct list_head	*head = &port->read_pool;
	struct usb_ep		*ep = port->port_usb->out;
	int			status;
	unsigned		started;

	/* Allocate RX and TX I/O buffers.  We can't easily do this much
	 * earlier (with GFP_KERNEL) because the requests are coupled to
	 * endpoints, as are the packet sizes we'll be using.  Different
	 * configurations may use different endpoints with a given port;
	 * and high speed vs full speed changes packet sizes too.
	 */
	status = gs_alloc_requests(ep, head, gs_read_complete,
		&port->read_allocated);
	if (status)
		return status;

	status = gs_alloc_requests(port->port_usb->in, &port->write_pool,
			gs_write_complete, &port->write_allocated);
	if (status) {
		gs_free_requests(ep, head, &port->read_allocated);
		return status;
	}

	/* queue read requests */
	port->n_read = 0;
	started = gs_start_rx(port);

	if (started) {
		gs_start_tx(port);
		/* Unblock any pending writes into our circular buffer, in case
		 * we didn't in gs_start_tx() */
		tty_port_tty_wakeup(&port->port);
	} else {
		/* Free reqs only if we are still connected */
		if (port->port_usb) {
			gs_free_requests(ep, head, &port->read_allocated);
			gs_free_requests(port->port_usb->in, &port->write_pool,
				&port->write_allocated);
		}
		status = -EIO;
	}

	return status;
}

/*-------------------------------------------------------------------------*/

/* TTY Driver */

/*
 * gs_open sets up the link between a gs_port and its associated TTY.
 * That link is broken *only* by TTY close(), and all driver methods
 * know that.
 */
static int gs_open(struct tty_struct *tty, struct file *file)
{
	int		port_num = tty->index;
	struct gs_port	*port;
	int		status = 0;

	if (!strncmp(current->comm, "smdexe", 6))
		a52_ackfr_record("U446 SMD_OPEN pid=%d comm=%s port=ttyGS%d",
				 task_pid_nr(current), current->comm, port_num);
	mutex_lock(&ports[port_num].lock);
	port = ports[port_num].port;
	if (!port) {
		status = -ENODEV;
		goto out;
	}

	spin_lock_irq(&port->port_lock);
	if (port_num == a52w_early_console_line() && unlikely(port->a52u_block_opens)) {
		spin_unlock_irq(&port->port_lock);
		a52_ackfr_record("U446 OPEN_BLOCKED pid=%d comm=%s port=%d",
				 task_pid_nr(current), current->comm, port_num);
		status = -EBUSY;
		goto out;
	}


	/* allocate circular buffer on first open */
	if (!kfifo_initialized(&port->port_write_buf)) {

		spin_unlock_irq(&port->port_lock);

		/*
		 * portmaster's mutex still protects from simultaneous open(),
		 * and close() can't happen, yet.
		 */

		status = kfifo_alloc(&port->port_write_buf,
				     WRITE_BUF_SIZE, GFP_KERNEL);
		if (status) {
			pr_debug("gs_open: ttyGS%d (%p,%p) no buffer\n",
				 port_num, tty, file);
			goto out;
		}

		spin_lock_irq(&port->port_lock);
	}

	/* already open?  Great. */
	if (port->port.count++)
		goto exit_unlock_port;

	tty->driver_data = port;
	port->port.tty = tty;

	/* if connected, start the I/O stream */
	if (port->port_usb) {
		/* if port is suspended, wait resume to start I/0 stream */
		if (!port->suspended) {
			struct gserial	*gser = port->port_usb;

			pr_debug("gs_open: start ttyGS%d\n", port->port_num);
			gs_start_io(port);

			if (gser->connect)
				gser->connect(gser);
		} else {
			pr_debug("delay start of ttyGS%d\n", port->port_num);
			port->start_delayed = true;
		}
	}

	pr_debug("gs_open: ttyGS%d (%p,%p)\n", port->port_num, tty, file);

exit_unlock_port:
	spin_unlock_irq(&port->port_lock);
out:
	mutex_unlock(&ports[port_num].lock);
	return status;
}

static int gs_close_flush_done(struct gs_port *p)
{
	int cond;

	/* return true on disconnect or empty buffer or if raced with open() */
	spin_lock_irq(&p->port_lock);
	cond = p->port_usb == NULL || !kfifo_len(&p->port_write_buf) ||
		p->port.count > 1;
	spin_unlock_irq(&p->port_lock);

	return cond;
}

static void gs_close(struct tty_struct *tty, struct file *file)
{
	struct gs_port *port = tty->driver_data;
	struct gserial	*gser;

	/* A52_PHASE446U_TTY_NULLSAFE_V3: failed gs_open needs safe .close */
	if (unlikely(!port))
		return;

	spin_lock_irq(&port->port_lock);

	if (port->port.count != 1) {
raced_with_open:
		if (port->port.count == 0)
			WARN_ON(1);
		else
			--port->port.count;
		goto exit;
	}

	pr_debug("gs_close: ttyGS%d (%p,%p) ...\n", port->port_num, tty, file);

	gser = port->port_usb;
	if (gser && !port->suspended && gser->disconnect)
		gser->disconnect(gser);

	/* wait for circular write buffer to drain, disconnect, or at
	 * most GS_CLOSE_TIMEOUT seconds; then discard the rest
	 */
	if (kfifo_len(&port->port_write_buf) > 0 && gser) {
		spin_unlock_irq(&port->port_lock);
		wait_event_interruptible_timeout(port->drain_wait,
					gs_close_flush_done(port),
					GS_CLOSE_TIMEOUT * HZ);
		spin_lock_irq(&port->port_lock);

		if (port->port.count != 1)
			goto raced_with_open;

		gser = port->port_usb;
	}

	/* Iff we're disconnected, there can be no I/O in flight so it's
	 * ok to free the circular buffer; else just scrub it.  And don't
	 * let the push async work fire again until we're re-opened.
	 */
	if (gser == NULL)
		kfifo_free(&port->port_write_buf);
	else
		kfifo_reset(&port->port_write_buf);

	port->start_delayed = false;
	port->port.count = 0;
	port->port.tty = NULL;

	pr_debug("gs_close: ttyGS%d (%p,%p) done!\n",
			port->port_num, tty, file);

	wake_up(&port->close_wait);
exit:
	spin_unlock_irq(&port->port_lock);
}

static int gs_write(struct tty_struct *tty, const unsigned char *buf, int count)
{
	struct gs_port	*port = tty->driver_data;
	unsigned long	flags;

	/* A52_PHASE446U_TTY_NULLSAFE_V3: failed gs_open needs safe .close */
	if (unlikely(!port))
		return -ENODEV;

	pr_vdebug("gs_write: ttyGS%d (%p) writing %d bytes\n",
			port->port_num, tty, count);

	spin_lock_irqsave(&port->port_lock, flags);
	if (count)
		count = kfifo_in(&port->port_write_buf, buf, count);
	/* treat count == 0 as flush_chars() */
	if (port->port_usb)
		gs_start_tx(port);
	spin_unlock_irqrestore(&port->port_lock, flags);

	return count;
}

static int gs_put_char(struct tty_struct *tty, unsigned char ch)
{
	struct gs_port	*port = tty->driver_data;
	unsigned long	flags;
	int		status;

	/* A52_PHASE446U_TTY_NULLSAFE_V3: failed gs_open needs safe .close */
	if (unlikely(!port))
		return 0;

	pr_vdebug("gs_put_char: (%d,%p) char=0x%x, called from %ps\n",
		port->port_num, tty, ch, __builtin_return_address(0));

	spin_lock_irqsave(&port->port_lock, flags);
	status = kfifo_put(&port->port_write_buf, ch);
	spin_unlock_irqrestore(&port->port_lock, flags);

	return status;
}

static void gs_flush_chars(struct tty_struct *tty)
{
	struct gs_port	*port = tty->driver_data;
	unsigned long	flags;

	/* A52_PHASE446U_TTY_NULLSAFE_V3: failed gs_open needs safe .close */
	if (unlikely(!port))
		return;

	pr_vdebug("gs_flush_chars: (%d,%p)\n", port->port_num, tty);

	spin_lock_irqsave(&port->port_lock, flags);
	if (port->port_usb)
		gs_start_tx(port);
	spin_unlock_irqrestore(&port->port_lock, flags);
}

static int gs_write_room(struct tty_struct *tty)
{
	struct gs_port	*port = tty->driver_data;
	unsigned long	flags;
	int		room = 0;

	/* A52_PHASE446U_TTY_NULLSAFE_V3: failed gs_open needs safe .close */
	if (unlikely(!port))
		return 0;

	spin_lock_irqsave(&port->port_lock, flags);
	if (port->port_usb)
		room = kfifo_avail(&port->port_write_buf);
	spin_unlock_irqrestore(&port->port_lock, flags);

	pr_vdebug("gs_write_room: (%d,%p) room=%d\n",
		port->port_num, tty, room);

	return room;
}

static int gs_chars_in_buffer(struct tty_struct *tty)
{
	struct gs_port	*port = tty->driver_data;
	unsigned long	flags;
	int		chars = 0;

	/* A52_PHASE446U_TTY_NULLSAFE_V3: failed gs_open needs safe .close */
	if (unlikely(!port))
		return 0;

	spin_lock_irqsave(&port->port_lock, flags);
	chars = kfifo_len(&port->port_write_buf);
	spin_unlock_irqrestore(&port->port_lock, flags);

	pr_vdebug("gs_chars_in_buffer: (%d,%p) chars=%d\n",
		port->port_num, tty, chars);

	return chars;
}

/* undo side effects of setting TTY_THROTTLED */
static void gs_unthrottle(struct tty_struct *tty)
{
	struct gs_port		*port = tty->driver_data;
	unsigned long		flags;

	/* A52_PHASE446U_TTY_NULLSAFE_V3: failed gs_open needs safe .close */
	if (unlikely(!port))
		return;

	spin_lock_irqsave(&port->port_lock, flags);
	if (port->port_usb) {
		/* Kickstart read queue processing.  We don't do xon/xoff,
		 * rts/cts, or other handshaking with the host, but if the
		 * read queue backs up enough we'll be NAKing OUT packets.
		 */
		pr_vdebug("ttyGS%d: unthrottle\n", port->port_num);
		schedule_delayed_work(&port->push, 0);
	}
	spin_unlock_irqrestore(&port->port_lock, flags);
}

static int gs_break_ctl(struct tty_struct *tty, int duration)
{
	struct gs_port	*port = tty->driver_data;
	int		status = 0;
	struct gserial	*gser;

	/* A52_PHASE446U_TTY_NULLSAFE_V3: failed gs_open needs safe .close */
	if (unlikely(!port))
		return -ENODEV;

	pr_vdebug("gs_break_ctl: ttyGS%d, send break (%d) \n",
			port->port_num, duration);

	spin_lock_irq(&port->port_lock);
	gser = port->port_usb;
	if (gser && gser->send_break)
		status = gser->send_break(gser, duration);
	spin_unlock_irq(&port->port_lock);

	return status;
}

static const struct tty_operations gs_tty_ops = {
	.open =			gs_open,
	.close =		gs_close,
	.write =		gs_write,
	.put_char =		gs_put_char,
	.flush_chars =		gs_flush_chars,
	.write_room =		gs_write_room,
	.chars_in_buffer =	gs_chars_in_buffer,
	.unthrottle =		gs_unthrottle,
	.break_ctl =		gs_break_ctl,
};

/*-------------------------------------------------------------------------*/

static struct tty_driver *gs_tty_driver;

#ifdef CONFIG_U_SERIAL_CONSOLE

static void a52_gs_console_kick(struct gs_console *cons)
{
	atomic_set(&cons->kick, 1);
	wake_up_interruptible(&cons->wait);
}

static void gs_console_complete_out(struct usb_ep *ep, struct usb_request *req)
{
	struct gs_console *cons = req->context;
	bool stopping;
	int status = req->status;

	/* Mark callback active BEFORE clearing pending. The worker may queue
	 * the next request during the callback; never clear its pending flag.
	 */
	atomic_inc(&cons->a52u_cb_active);
	spin_lock(&cons->lock);
	cons->a52u_req_pending = false;
	req->length = 0;
	stopping = cons->a52u_stopping;
	spin_unlock(&cons->lock);
	if (stopping) {
		a52_ackfr_record("U446 request-complete status=%d",status);
		goto a52u_complete_done;
	}

	switch (req->status) {
	default:
		pr_warn("%s: unexpected %s status %d\n",
			__func__, ep->name, req->status);
		fallthrough;
	case 0:
		/* normal completion */
		spin_lock(&cons->lock);
		req->length = 0;
		a52_gs_console_kick(cons);
		spin_unlock(&cons->lock);
		break;
	case -ECONNRESET:
	case -ESHUTDOWN:
		/* disconnect */
		pr_vdebug("%s: %s shutdown\n", __func__, ep->name);
		break;
	}
a52u_complete_done:
	complete(&cons->a52u_request_ack);
	atomic_dec(&cons->a52u_cb_active);
	/* Last operation touching cons: coordinator only frees after ACK. */
	wake_up_all(&cons->a52u_req_wait);

}

static void __gs_console_push(struct gs_console *cons)
{
	struct usb_request *req = cons->req;
	struct usb_ep *ep;
	size_t size;
	int queue_rc;

	if (!req || READ_ONCE(cons->a52u_stopping))
		return;	/* disconnected or stopping */

	if (req->length)
		return;	/* busy */

	ep = cons->console.data;
	size = kfifo_out(&cons->buf, req->buf, ep->maxpacket);
	if (!size)
		return;

	if (cons->missed && ep->maxpacket >= 64) {
		char buf[64];
		size_t len;

		len = sprintf(buf, "\n[missed %zu bytes]\n", cons->missed);
		kfifo_in(&cons->buf, buf, len);
		cons->missed = 0;
	}

	req->length = size;
	reinit_completion(&cons->a52u_request_ack);
	cons->a52u_req_pending = true;
	spin_unlock_irq(&cons->lock);
	queue_rc = usb_ep_queue(ep, req, GFP_ATOMIC);
	spin_lock_irq(&cons->lock);
	if (queue_rc) {
		req->length = 0;
		cons->a52u_req_pending = false;
		complete(&cons->a52u_request_ack);
	}
}

static int a52_gs_console_thread(void *data)
{
	struct gs_console *cons = data;

	while (!kthread_should_stop() && !READ_ONCE(cons->a52u_stopping)) {
		wait_event_interruptible(cons->wait,
			kthread_should_stop() || READ_ONCE(cons->a52u_stopping) ||
			atomic_xchg(&cons->kick, 0));
		if (kthread_should_stop() || READ_ONCE(cons->a52u_stopping))
			break;

		spin_lock_irq(&cons->lock);
		__gs_console_push(cons);
		spin_unlock_irq(&cons->lock);
	}
	complete(&cons->a52u_worker_ack);
	return 0;
}

static void gs_console_write(struct console *co,
			     const char *buf, unsigned count)
{
	struct gs_console *cons = container_of(co, struct gs_console, console);
	unsigned long flags;
	size_t n;

	spin_lock_irqsave(&cons->lock, flags);
	if (unlikely(cons->a52u_stopping))
		goto a52u_write_out;

	n = kfifo_in(&cons->buf, buf, count);
	if (n < count)
		cons->missed += count - n;

	if (cons->req && !cons->req->length)
		a52_gs_console_kick(cons);
a52u_write_out:
	spin_unlock_irqrestore(&cons->lock, flags);
}

static struct tty_driver *gs_console_device(struct console *co, int *index)
{
	*index = co->index;
	return gs_tty_driver;
}

static int gs_console_connect(struct gs_port *port)
{
	struct gs_console *cons = port->console;
	struct usb_request *req;
	struct usb_ep *ep;

	if (!cons)
		return 0;
	if (READ_ONCE(cons->a52u_stopping))
		return -EBUSY;

	ep = port->port_usb->in;
	req = gs_alloc_req(ep, ep->maxpacket, GFP_ATOMIC);
	if (!req)
		return -ENOMEM;
	req->complete = gs_console_complete_out;
	req->context = cons;
	req->length = 0;

	spin_lock(&cons->lock);
	cons->req = req;
	cons->console.data = ep;
	spin_unlock(&cons->lock);

	pr_debug("ttyGS%d: console connected!\n", port->port_num);

	a52_gs_console_kick(cons);

	return 0;
}

static void gs_console_disconnect(struct gs_port *port)
{
	struct gs_console *cons = port->console;
	struct usb_request *req;
	struct usb_ep *ep;

	if (!cons)
		return;
	if (READ_ONCE(cons->a52u_stopping))
		return; /* request owned by U446 coordinator */

	spin_lock(&cons->lock);

	req = cons->req;
	ep = cons->console.data;
	cons->req = NULL;

	spin_unlock(&cons->lock);

	if (!req || !ep) {
		a52_ackfr_record("U446 CONSOLE skip_dequeue req=%u ep=%u",
				 !!req, !!ep);
		return;
	}

	usb_ep_dequeue(ep, req);
	gs_free_req(ep, req);
}

static int gs_console_init(struct gs_port *port)
{
	struct gs_console *cons;
	int err;

	if (port->console)
		return 0;

	cons = kzalloc(sizeof(*port->console), GFP_KERNEL);
	if (!cons)
		return -ENOMEM;

	strcpy(cons->console.name, "ttyGS");
	cons->console.write = gs_console_write;
	cons->console.device = gs_console_device;
	cons->console.flags = CON_PRINTBUFFER | CON_ANYTIME;
	cons->console.index = port->port_num;

	spin_lock_init(&cons->lock);
	init_waitqueue_head(&cons->wait);
	init_completion(&cons->a52u_worker_ack);
	init_completion(&cons->a52u_request_ack);
	cons->a52u_stopping = false;
	cons->a52u_detached = false;
	cons->a52u_req_pending = false;
	init_waitqueue_head(&cons->a52u_req_wait);
	atomic_set(&cons->a52u_cb_active, 0);
	atomic_set(&cons->kick, 0);

	err = kfifo_alloc(&cons->buf, GS_CONSOLE_BUF_SIZE, GFP_KERNEL);
	if (err) {
		pr_err("ttyGS%d: allocate console buffer failed\n", port->port_num);
		kfree(cons);
		return err;
	}

	cons->task = kthread_run(a52_gs_console_thread, cons,
				 "a52_usb_console");
	if (IS_ERR(cons->task)) {
		err = PTR_ERR(cons->task);
		cons->task = NULL;
		kfifo_free(&cons->buf);
		kfree(cons);
		return err;
	}

	port->console = cons;
	register_console(&cons->console);
	pr_info("USB380 ttyGS%d live console ready fifo=%u transport=kthread\n",
		port->port_num, GS_CONSOLE_BUF_SIZE);

	spin_lock_irq(&port->port_lock);
	if (port->port_usb)
		gs_console_connect(port);
	spin_unlock_irq(&port->port_lock);

	return 0;
}

static void gs_console_exit(struct gs_port *port)
{
	struct gs_console *cons = port->console;

	if (!cons)
		return;

	if (!cons->a52u_detached)
		unregister_console(&cons->console);

	spin_lock_irq(&port->port_lock);
	if (cons->req)
		gs_console_disconnect(port);
	spin_unlock_irq(&port->port_lock);

	if (cons->task)
		kthread_stop(cons->task);
	kfifo_free(&cons->buf);
	kfree(cons);
	port->console = NULL;
}


/* A52_PHASE446U_USB_SAFE_COORDINATOR_V2
 * Scan open fds with tty_file_private while file locks protect fds.
 * /dev/console and /dev/ttyGS0 both resolve to the same tty_struct.
 */
struct a52u_owner_find {
	struct tty_struct *tty;
	int fd;
};
static int a52u_owner_fd(const void *data, struct file *file, unsigned fd)
{
	struct a52u_owner_find *find = (struct a52u_owner_find *)data;
	struct tty_file_private *tp;
	dev_t dev;
	if (!file || !file_inode(file))
		return 0;
	dev = file_inode(file)->i_rdev;
	if (!S_ISCHR(file_inode(file)->i_mode) ||
	    (dev != MKDEV(gs_tty_driver->major, a52w_early_console_line()) && dev != MKDEV(5, 1)))
		return 0;
	tp = file->private_data;
	if (!tp || tp->tty != find->tty)
		return 0;
	find->fd = fd;
	return 1;
}
static void a52u_log_tty_holders(struct tty_struct *tty)
{
	struct task_struct *task;
	unsigned int shown=0;
	if (!tty) {
		a52_ackfr_record("U446 holder tty pointer unavailable");
		return;
	}
	rcu_read_lock();
	for_each_process(task) {
		struct files_struct *files;
		struct a52u_owner_find find = {.tty=tty,.fd=-1};
		char name[TASK_COMM_LEN];
		files = get_files_struct(task);
		if (!files) continue;
		iterate_fd(files,0,a52u_owner_fd,&find);
		put_files_struct(files);
		if (find.fd<0) continue;
		get_task_comm(name,task);
		a52_ackfr_record("U446 holder pid=%d comm=%s fd=%d",
				task_pid_nr(task),name,find.fd);
		if (++shown>=16) break;
	}
	rcu_read_unlock();
	if (!shown)
		a52_ackfr_record("U446 holder count>0 owner_scan=none (kernel/tty refs possible)");
}

/* Separate from ConfigFS gi->lock and never calls composite operations. */
int a52_446u_preflight_port(unsigned char port_num)
{
	struct gs_port *port;
	struct tty_struct *tty=NULL;
	unsigned int count;
	if (port_num>=MAX_U_SERIAL_PORTS) return -EINVAL;
	mutex_lock(&ports[port_num].lock);
	port=ports[port_num].port;
	if (!port) {
		mutex_unlock(&ports[port_num].lock);
		return -ENODEV;
	}
	spin_lock_irq(&port->port_lock);
	count=port->port.count;
	tty=port->port.tty;
	if (!count)
		port->a52u_block_opens=true;
	spin_unlock_irq(&port->port_lock);
	pr_info_ratelimited("P446W U446 PRE port=%u tty_count=%u console=%d usb_connected=%d\n",
		port_num,count,!!port->console,!!port->port_usb);
	a52_ackfr_record("U446 PRE count=%u port_usb=%u console=%u req=%u req_len=%u fifo=%u udc=a600000.dwc3",
			 count,!!port->port_usb,!!port->console,
			 port->console && !!port->console->req,
			 port->console && port->console->req ?
				 port->console->req->length : 0,
			 port->console ? kfifo_len(&port->console->buf) : 0);
	if (count) {
		a52u_log_tty_holders(tty);
		a52_ackfr_record("U446 DEFER tty_open_count=%u",count);
		pr_info_ratelimited("P446W U446 DEFER port=%u reason=tty_open_count count=%u (no unbind)\n",port_num,count);
	}
	mutex_unlock(&ports[port_num].lock);
	return count ? -EBUSY : 0;
}
EXPORT_SYMBOL_GPL(a52_446u_preflight_port);

void a52_446u_unblock_opens(unsigned char port_num)
{
	if (port_num>=MAX_U_SERIAL_PORTS) return;
	mutex_lock(&ports[port_num].lock);
	if (ports[port_num].port)
		WRITE_ONCE(ports[port_num].port->a52u_block_opens,false);
	mutex_unlock(&ports[port_num].lock);
}
EXPORT_SYMBOL_GPL(a52_446u_unblock_opens);

int a52_446u_detach_console(unsigned char port_num)
{
	struct gs_port *port;
	struct gs_console *cons;
	struct usb_request *req;
	struct usb_ep *ep;
	unsigned long flags, deadline;
	unsigned int fifo=0;
	bool pending=false;
	int ret=0, deq_rc=0;
	if (port_num>=MAX_U_SERIAL_PORTS) return -EINVAL;
	mutex_lock(&ports[port_num].lock);
	port=ports[port_num].port;
	if (!port || !port->console) {
		ret=port ? 0 : -ENODEV;
		goto out;
	}
	cons=port->console;
	if (cons->a52u_detached)
		goto out;
	/* Preflight must have blocked new tty opens already. */
	if (!READ_ONCE(port->a52u_block_opens)) {
		ret=-EPERM;
		goto out;
	}
	a52_ackfr_record("U446 CONSOLE worker-stop-enter port=%u",port_num);
	spin_lock_irqsave(&cons->lock,flags);
	cons->a52u_stopping=true;
	spin_unlock_irqrestore(&cons->lock,flags);
	a52_gs_console_kick(cons);
	if (cons->task) {
		deadline=wait_for_completion_timeout(&cons->a52u_worker_ack,
						    msecs_to_jiffies(500));
		if (!deadline) {
			a52_ackfr_record("U446 CONSOLE worker-stop-timeout (ABORT)");
			spin_lock_irqsave(&cons->lock,flags);
			cons->a52u_stopping=false;
			spin_unlock_irqrestore(&cons->lock,flags);
			ret=-ETIMEDOUT;
			goto out;
		}
		a52_ackfr_record("U446 CONSOLE worker-stop-ack");
		/* Ack guarantees kthread_stop cannot wait on USB requests. */
		kthread_stop(cons->task);
		cons->task=NULL;
	}
	spin_lock_irqsave(&cons->lock,flags);
	fifo=kfifo_len(&cons->buf);
	kfifo_reset(&cons->buf);
	spin_unlock_irqrestore(&cons->lock,flags);
	a52_ackfr_record("U446 CONSOLE fifo_drop=%u",fifo);
	a52_ackfr_record("U446 CONSOLE unregister-enter");
	ret=unregister_console(&cons->console);
	a52_ackfr_record("U446 CONSOLE unregister-exit rc=%d",ret);
	if (ret) goto out; /* No composite teardown; retain allocated cons/req. */

	spin_lock_irqsave(&cons->lock,flags);
	req=cons->req;
	ep=cons->console.data;
	pending=cons->a52u_req_pending;
	spin_unlock_irqrestore(&cons->lock,flags);
	if (req && ep && pending) {
		a52_ackfr_record("U446 CONSOLE request-dequeue-enter pending=1");
		deq_rc=usb_ep_dequeue(ep,req);
		a52_ackfr_record("U446 CONSOLE request-dequeue-exit rc=%d",deq_rc);
	}
	/* Both the queued request AND every callback must have exited.
	 * A completion event alone is insufficient if another request was
	 * queued while a previous completion was still running.
	 */
	if (req && ep) {
		deadline=wait_event_timeout(cons->a52u_req_wait,
				!READ_ONCE(cons->a52u_req_pending) &&
				atomic_read(&cons->a52u_cb_active)==0,
				msecs_to_jiffies(500));
		if (!deadline) {
			a52_ackfr_record("U446 CONSOLE callback-drain-timeout (ABORT)");
			ret=-ETIMEDOUT;
			goto out;
		}
		a52_ackfr_record("U446 CONSOLE request-drain-ack");
	}
	if (req && ep) {
		spin_lock_irqsave(&cons->lock,flags);
		cons->req=NULL;
		spin_unlock_irqrestore(&cons->lock,flags);
		gs_free_req(ep,req);
		a52_ackfr_record("U446 CONSOLE request-free");
	}
	cons->a52u_detached=true;
	a52_ackfr_record("U446 CONSOLE detach-complete");
	pr_info_ratelimited("P446W U446 DETACH COMPLETE port=%u\n",port_num);
out:
	mutex_unlock(&ports[port_num].lock);
	return ret;
}
EXPORT_SYMBOL_GPL(a52_446u_detach_console);

ssize_t gserial_set_console(unsigned char port_num, const char *page, size_t count)
{
	struct gs_port *port;
	bool enable;
	int ret;

	ret = strtobool(page, &enable);
	if (ret)
		return ret;

	mutex_lock(&ports[port_num].lock);
	port = ports[port_num].port;

	if (WARN_ON(port == NULL)) {
		ret = -ENXIO;
		goto out;
	}

	if (enable)
		ret = gs_console_init(port);
	else
		gs_console_exit(port);
out:
	mutex_unlock(&ports[port_num].lock);

	return ret < 0 ? ret : count;
}
EXPORT_SYMBOL_GPL(gserial_set_console);

ssize_t gserial_get_console(unsigned char port_num, char *page)
{
	struct gs_port *port;
	ssize_t ret;

	mutex_lock(&ports[port_num].lock);
	port = ports[port_num].port;

	if (WARN_ON(port == NULL))
		ret = -ENXIO;
	else
		ret = sprintf(page, "%u\n", !!port->console);

	mutex_unlock(&ports[port_num].lock);

	return ret;
}
EXPORT_SYMBOL_GPL(gserial_get_console);

#else

static int gs_console_connect(struct gs_port *port)
{
	return 0;
}

static void gs_console_disconnect(struct gs_port *port)
{
}

static int gs_console_init(struct gs_port *port)
{
	return -ENOSYS;
}

static void gs_console_exit(struct gs_port *port)
{
}

#endif

static int
gs_port_alloc(unsigned port_num, struct usb_cdc_line_coding *coding)
{
	struct gs_port	*port;
	int		ret = 0;

	mutex_lock(&ports[port_num].lock);
	if (ports[port_num].port) {
		ret = -EBUSY;
		goto out;
	}

	port = kzalloc(sizeof(struct gs_port), GFP_KERNEL);
	if (port == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	tty_port_init(&port->port);
	spin_lock_init(&port->port_lock);
	init_waitqueue_head(&port->drain_wait);
	init_waitqueue_head(&port->close_wait);

	INIT_DELAYED_WORK(&port->push, gs_rx_push);

	INIT_LIST_HEAD(&port->read_pool);
	INIT_LIST_HEAD(&port->read_queue);
	INIT_LIST_HEAD(&port->write_pool);

	port->port_num = port_num;
	port->port_line_coding = *coding;

	ports[port_num].port = port;
out:
	mutex_unlock(&ports[port_num].lock);
	return ret;
}

static int gs_closed(struct gs_port *port)
{
	int cond;

	spin_lock_irq(&port->port_lock);
	cond = port->port.count == 0;
	spin_unlock_irq(&port->port_lock);

	return cond;
}

static void gserial_free_port(struct gs_port *port)
{
	cancel_delayed_work_sync(&port->push);
	/* The U446 coordinator must have ruled out live tty openers. */
	if (READ_ONCE(port->a52u_block_opens))
		a52_ackfr_record("U446 UNBIND free_port before_close_wait count=%u",
				 port->port.count);
	/* Original 446T wait retained; preflight+open gate make it immediate. */
	wait_event(port->close_wait, gs_closed(port));
	WARN_ON(port->port_usb != NULL);
	tty_port_destroy(&port->port);
	kfree(port);
}

void gserial_free_line(unsigned char port_num)
{
	struct gs_port	*port;

	mutex_lock(&ports[port_num].lock);
	if (WARN_ON(!ports[port_num].port)) {
		mutex_unlock(&ports[port_num].lock);
		return;
	}
	port = ports[port_num].port;
	if (READ_ONCE(port->a52u_block_opens))
		a52_ackfr_record("U446 UNBIND gserial_free_line count=%u",port->port.count);
	gs_console_exit(port);
	ports[port_num].port = NULL;
	mutex_unlock(&ports[port_num].lock);

	gserial_free_port(port);
	tty_unregister_device(gs_tty_driver, port_num);
}
EXPORT_SYMBOL_GPL(gserial_free_line);

/* A52_PHASE446W_TTYGS3_EARLY_PORT_V1 */
static unsigned char a52w_console_port=3;
static atomic_t a52w_first_alloc=ATOMIC_INIT(0);
static int __init a52w_console_arg(char *s)
{
	if (!s)return -EINVAL;
	if (!strcmp(s,"0")) a52w_console_port=0;
	else if (!strcmp(s,"3")) a52w_console_port=3;
	else return -EINVAL;
	return 0;
}
early_param("a52.446w_tty",a52w_console_arg);
unsigned char a52w_early_console_line(void) { return a52w_console_port; }
EXPORT_SYMBOL_GPL(a52w_early_console_line);

int gserial_alloc_line_no_console(unsigned char *line_num)
{
	struct usb_cdc_line_coding	coding;
	struct gs_port			*port;
	struct device			*tty_dev;
	int				ret;
	int				port_num;

	coding.dwDTERate = cpu_to_le32(9600);
	coding.bCharFormat = 8;
	coding.bParityType = USB_CDC_NO_PARITY;
	coding.bDataBits = USB_CDC_1_STOP_BITS;

	/*
	 * g_serial is the first USB serial function on A52 boot.
	 * Allocate ONLY line3 for it; don't occupy lines0-2. Once early
	 * g_serial is removed, Samsung configfs ACM/DUN uses line0.
	 */
	if (a52w_console_port==3 &&
	    atomic_cmpxchg(&a52w_first_alloc,0,1)==0) {
		port_num=3;
		ret=gs_port_alloc(port_num,&coding);
		if (ret) {
			pr_err("P446W USB early ttyGS3 allocation FAILED rc=%d\n",ret);
			return ret; /* fail closed, never hijack ttyGS0 */
		}
		pr_info("P446W USB early legacy g_serial allocated ttyGS3; ttyGS0 reserved for future Android configfs\n");
	} else {
		for (port_num=0;port_num<MAX_U_SERIAL_PORTS;port_num++) {
			ret=gs_port_alloc(port_num,&coding);
			if (ret==-EBUSY)continue;
			if (ret)return ret;
			break;
		}
		if (ret)return ret;
	}

	/* ... and sysfs class devices, so mdev/udev make /dev/ttyGS* */

	port = ports[port_num].port;
	tty_dev = tty_port_register_device(&port->port,
			gs_tty_driver, port_num, NULL);
	if (IS_ERR(tty_dev)) {
		pr_err("%s: failed to register tty for port %d, err %ld\n",
				__func__, port_num, PTR_ERR(tty_dev));

		ret = PTR_ERR(tty_dev);
		mutex_lock(&ports[port_num].lock);
		ports[port_num].port = NULL;
		mutex_unlock(&ports[port_num].lock);
		gserial_free_port(port);
		goto err;
	}
	*line_num = port_num;
err:
	return ret;
}
EXPORT_SYMBOL_GPL(gserial_alloc_line_no_console);

int gserial_alloc_line(unsigned char *line_num)
{
	int ret = gserial_alloc_line_no_console(line_num);

	if (!ret && *line_num == a52w_early_console_line())
		gs_console_init(ports[*line_num].port);

	return ret;
}
EXPORT_SYMBOL_GPL(gserial_alloc_line);

/**
 * gserial_connect - notify TTY I/O glue that USB link is active
 * @gser: the function, set up with endpoints and descriptors
 * @port_num: which port is active
 * Context: any (usually from irq)
 *
 * This is called activate endpoints and let the TTY layer know that
 * the connection is active ... not unlike "carrier detect".  It won't
 * necessarily start I/O queues; unless the TTY is held open by any
 * task, there would be no point.  However, the endpoints will be
 * activated so the USB host can perform I/O, subject to basic USB
 * hardware flow control.
 *
 * Caller needs to have set up the endpoints and USB function in @dev
 * before calling this, as well as the appropriate (speed-specific)
 * endpoint descriptors, and also have allocate @port_num by calling
 * @gserial_alloc_line().
 *
 * Returns negative errno or zero.
 * On success, ep->driver_data will be overwritten.
 */
int gserial_connect(struct gserial *gser, u8 port_num)
{
	struct gs_port	*port;
	unsigned long	flags;
	int		status;

	if (port_num >= MAX_U_SERIAL_PORTS)
		return -ENXIO;

	port = ports[port_num].port;
	if (!port) {
		pr_err("serial line %d not allocated.\n", port_num);
		return -EINVAL;
	}
	if (port->port_usb) {
		pr_err("serial line %d is in use.\n", port_num);
		return -EBUSY;
	}

	/* activate the endpoints */
	status = usb_ep_enable(gser->in);
	if (status < 0)
		return status;
	gser->in->driver_data = port;

	status = usb_ep_enable(gser->out);
	if (status < 0)
		goto fail_out;
	gser->out->driver_data = port;

	/* then tell the tty glue that I/O can work */
	spin_lock_irqsave(&port->port_lock, flags);
	gser->ioport = port;
	port->port_usb = gser;

	/* REVISIT unclear how best to handle this state...
	 * we don't really couple it with the Linux TTY.
	 */
	gser->port_line_coding = port->port_line_coding;

	/* REVISIT if waiting on "carrier detect", signal. */

	/* if it's already open, start I/O ... and notify the serial
	 * protocol about open/close status (connect/disconnect).
	 */
	if (port->port.count) {
		pr_debug("gserial_connect: start ttyGS%d\n", port->port_num);
		gs_start_io(port);
		if (gser->connect)
			gser->connect(gser);
	} else {
		if (gser->disconnect)
			gser->disconnect(gser);
	}

	status = gs_console_connect(port);
	spin_unlock_irqrestore(&port->port_lock, flags);

	return status;

fail_out:
	usb_ep_disable(gser->in);
	return status;
}
EXPORT_SYMBOL_GPL(gserial_connect);
/**
 * gserial_disconnect - notify TTY I/O glue that USB link is inactive
 * @gser: the function, on which gserial_connect() was called
 * Context: any (usually from irq)
 *
 * This is called to deactivate endpoints and let the TTY layer know
 * that the connection went inactive ... not unlike "hangup".
 *
 * On return, the state is as if gserial_connect() had never been called;
 * there is no active USB I/O on these endpoints.
 */
void gserial_disconnect(struct gserial *gser)
{
	struct gs_port	*port = gser->ioport;
	unsigned long	flags;

	if (!port)
		return;

	spin_lock_irqsave(&serial_port_lock, flags);

	/* tell the TTY glue not to do I/O here any more */
	spin_lock(&port->port_lock);

	gs_console_disconnect(port);

	/* REVISIT as above: how best to track this? */
	port->port_line_coding = gser->port_line_coding;

	port->port_usb = NULL;
	gser->ioport = NULL;
	if (port->port.count > 0) {
		wake_up_interruptible(&port->drain_wait);
		if (port->port.tty)
			tty_hangup(port->port.tty);
	}
	port->suspended = false;
	spin_unlock(&port->port_lock);
	spin_unlock_irqrestore(&serial_port_lock, flags);

	/* disable endpoints, aborting down any active I/O */
	usb_ep_disable(gser->out);
	usb_ep_disable(gser->in);

	/* finally, free any unused/unusable I/O buffers */
	spin_lock_irqsave(&port->port_lock, flags);
	if (port->port.count == 0)
		kfifo_free(&port->port_write_buf);
	gs_free_requests(gser->out, &port->read_pool, NULL);
	gs_free_requests(gser->out, &port->read_queue, NULL);
	gs_free_requests(gser->in, &port->write_pool, NULL);

	port->read_allocated = port->read_started =
		port->write_allocated = port->write_started = 0;

	spin_unlock_irqrestore(&port->port_lock, flags);
}
EXPORT_SYMBOL_GPL(gserial_disconnect);

void gserial_suspend(struct gserial *gser)
{
	struct gs_port	*port;
	unsigned long	flags;

	spin_lock_irqsave(&serial_port_lock, flags);
	port = gser->ioport;

	if (!port) {
		spin_unlock_irqrestore(&serial_port_lock, flags);
		return;
	}

	spin_lock(&port->port_lock);
	spin_unlock(&serial_port_lock);
	port->suspended = true;
	port->start_delayed = true;
	spin_unlock_irqrestore(&port->port_lock, flags);
}
EXPORT_SYMBOL_GPL(gserial_suspend);

void gserial_resume(struct gserial *gser)
{
	struct gs_port *port;
	unsigned long	flags;

	spin_lock_irqsave(&serial_port_lock, flags);
	port = gser->ioport;

	if (!port) {
		spin_unlock_irqrestore(&serial_port_lock, flags);
		return;
	}

	spin_lock(&port->port_lock);
	spin_unlock(&serial_port_lock);
	port->suspended = false;
	if (!port->start_delayed) {
		spin_unlock_irqrestore(&port->port_lock, flags);
		return;
	}

	pr_debug("delayed start ttyGS%d\n", port->port_num);
	gs_start_io(port);
	if (gser->connect)
		gser->connect(gser);
	port->start_delayed = false;
	spin_unlock_irqrestore(&port->port_lock, flags);
}
EXPORT_SYMBOL_GPL(gserial_resume);

static int userial_init(void)
{
	unsigned			i;
	int				status;

	gs_tty_driver = alloc_tty_driver(MAX_U_SERIAL_PORTS);
	if (!gs_tty_driver)
		return -ENOMEM;

	gs_tty_driver->driver_name = "g_serial";
	gs_tty_driver->name = "ttyGS";
	/* uses dynamically assigned dev_t values */

	gs_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	gs_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	gs_tty_driver->flags = TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV;
	gs_tty_driver->init_termios = tty_std_termios;

	/* 9600-8-N-1 ... matches defaults expected by "usbser.sys" on
	 * MS-Windows.  Otherwise, most of these flags shouldn't affect
	 * anything unless we were to actually hook up to a serial line.
	 */
	gs_tty_driver->init_termios.c_cflag =
			B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	gs_tty_driver->init_termios.c_ispeed = 9600;
	gs_tty_driver->init_termios.c_ospeed = 9600;

	tty_set_operations(gs_tty_driver, &gs_tty_ops);
	for (i = 0; i < MAX_U_SERIAL_PORTS; i++)
		mutex_init(&ports[i].lock);

	/* export the driver ... */
	status = tty_register_driver(gs_tty_driver);
	if (status) {
		pr_err("%s: cannot register, err %d\n",
				__func__, status);
		goto fail;
	}

	pr_debug("%s: registered %d ttyGS* device%s\n", __func__,
			MAX_U_SERIAL_PORTS,
			(MAX_U_SERIAL_PORTS == 1) ? "" : "s");

	return status;
fail:
	put_tty_driver(gs_tty_driver);
	gs_tty_driver = NULL;
	return status;
}
module_init(userial_init);

static void userial_cleanup(void)
{
	tty_unregister_driver(gs_tty_driver);
	put_tty_driver(gs_tty_driver);
	gs_tty_driver = NULL;
}
module_exit(userial_cleanup);

MODULE_LICENSE("GPL");
