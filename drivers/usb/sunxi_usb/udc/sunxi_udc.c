/*
 * drivers/usb/sunxi_usb/udc/sunxi_udc.c
 * (C) Copyright 2010-2015
 * Allwinner Technology Co., Ltd. <www.allwinnertech.com>
 * javen, 2010-3-3, create this file
 *
 * usb device contoller driver
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 */

#undef DEBUG
#include <common.h>
#include <asm/errno.h>
#include <linux/list.h>
#include <malloc.h>

#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>

#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <asm/io.h>

#include <asm/mach-types.h>
#include <asm/arch/gpio.h>

#include <usb/lin_gadget_compat.h>

#include  "sunxi_udc_config.h"
#include  "sunxi_udc_board.h"
#include  "sunxi_udc_dma.h"

/***********************************************************/

#define DRIVER_DESC "SoftWinner USB Device Controller"
#define DRIVER_VERSION "20080411"
#define DRIVER_AUTHOR	"SoftWinner USB Developer"

struct sunxi_udc *the_controller = NULL;

static const char gadget_name[] = "sunxi_usb_udc";
static const char driver_desc[] = DRIVER_DESC;

#define	DMA_ADDR_INVALID	(~(dma_addr_t)0)

static u8 crq_bRequest = 0;
static u8 crq_wIndex = 0;
static const unsigned char TestPkt[54] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAA,
	0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xEE, 0xEE, 0xEE,
	0xEE, 0xEE, 0xEE, 0xEE, 0xEE, 0xFE, 0xFF, 0xFF, 0xFF, 0xFF,
	0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0xBF, 0xDF,
	0xEF, 0xF7, 0xFB, 0xFD, 0xFC, 0x7E, 0xBF, 0xDF, 0xEF, 0xF7,
	0xFB, 0xFD, 0x7E, 0x00
};

static u32 usbd_port_no = 0;
static sunxi_udc_io_t g_sunxi_udc_io;
static u32 is_controller_alive = 0;
static __u32 dma_working = 0;
static u32 usb_connect = 0;

static int g_write_debug = 0;
static int g_read_debug = 0;
static int g_queue_debug = 0;
static int g_dma_debug = 0;
static int g_irq_debug = 0;

/*
  Local declarations.
*/
static int sunxi_udc_ep_enable(struct usb_ep *ep,
			 const struct usb_endpoint_descriptor *);
static int sunxi_udc_ep_disable(struct usb_ep *ep);
static struct usb_request *sunxi_udc_alloc_request(struct usb_ep *ep,
					     gfp_t gfp_flags);
static void sunxi_udc_free_request(struct usb_ep *ep, struct usb_request *);
static int sunxi_udc_queue(struct usb_ep *ep, struct usb_request *, gfp_t gfp_flags);
static int sunxi_udc_dequeue(struct usb_ep *ep, struct usb_request *);
static int sunxi_udc_set_halt(struct usb_ep *_ep, int value);
static int sunxi_udc_set_halt_ex(struct usb_ep *_ep, int value);

static void clear_all_irq(void);
static void cfg_udc_command(enum sunxi_udc_cmd_e cmd);

static __u32 is_peripheral_active(void)
{
	return is_controller_alive;
}

/* DMA transfer conditions:
 * 1. the driver support dma transfer
 * 2. not EP0
 * 3. more than one packet
 */
#define  big_req(req, ep)			((req->req.length != req->req.actual) \
							? ((req->req.length - req->req.actual) > ep->ep.maxpacket) \
							: (req->req.length > ep->ep.maxpacket))
#define  is_sunxi_udc_dma_capable(req, ep)	(is_udc_support_dma() \
							&& big_req(req, ep) \
							&& req->req.dma_flag \
							&& ep->num)
#define is_buffer_mapped(req, ep) (is_sunxi_udc_dma_capable(req, ep) && (req->map_state != UN_MAPPED))

static struct usb_ep_ops sunxi_udc_ep_ops = {
	.enable = sunxi_udc_ep_enable,
	.disable = sunxi_udc_ep_disable,

	.alloc_request = sunxi_udc_alloc_request,
	.free_request = sunxi_udc_free_request,

	.queue = sunxi_udc_queue,
	.dequeue = sunxi_udc_dequeue,

	.set_halt = sunxi_udc_set_halt,
	//.fifo_status = s3c_fifo_status,
	//.fifo_flush = s3c_fifo_flush,
};

/*
 *	udc_disable - disable USB device controller
 */
static void sunxi_udc_disable(struct sunxi_udc *dev)
{
	DMSG_DBG_UDC("sunxi_udc_disable\n");

	/* Disable all interrupts */
	USBC_INT_DisableUsbMiscAll(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_INT_DisableEpAll(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
	USBC_INT_DisableEpAll(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);

	/* Clear the interrupt registers */
	clear_all_irq();
	cfg_udc_command(SW_UDC_P_DISABLE);

	/* Set speed to unknown */
	dev->gadget.speed = USB_SPEED_UNKNOWN;
	return;
}

/*
 *	udc_reinit - initialize software state
 */
static void sunxi_udc_reinit(struct sunxi_udc *dev)
{
	u32 i = 0;

	/* device/ep0 records init */
	INIT_LIST_HEAD (&dev->gadget.ep_list);
	INIT_LIST_HEAD (&dev->gadget.ep0->ep_list);
	dev->ep0state = EP0_IDLE;

	for (i = 0; i < SW_UDC_ENDPOINTS; i++) {
		struct sunxi_udc_ep *ep = &dev->ep[i];

		if (i != 0) {
			list_add_tail (&ep->ep.ep_list, &dev->gadget.ep_list);
		}

		ep->dev	 = dev;
		ep->desc = NULL;
		ep->halted  = 0;
		INIT_LIST_HEAD (&ep->queue);
	}
	return;
}

/* until it's enabled, this UDC should be completely invisible
 * to any USB host.
 */
static void sunxi_udc_enable(struct sunxi_udc *dev)
{
	DMSG_DBG_UDC("sunxi_udc_enable called\n");

	/* dev->gadget.speed = USB_SPEED_UNKNOWN; */
	dev->gadget.speed = USB_SPEED_UNKNOWN;

#ifdef	CONFIG_USB_GADGET_DUALSPEED
	DMSG_INFO_UDC("CONFIG_USB_GADGET_DUALSPEED: USBC_TS_MODE_HS\n");

	USBC_Dev_ConfigTransferMode(g_sunxi_udc_io.usb_bsp_hdle, USBC_TS_TYPE_BULK, USBC_TS_MODE_HS);
#else
	DMSG_INFO_UDC("CONFIG_USB_GADGET_DUALSPEED: USBC_TS_MODE_FS\n");

	USBC_Dev_ConfigTransferMode(g_sunxi_udc_io.usb_bsp_hdle, USBC_TS_TYPE_BULK, USBC_TS_MODE_FS);
#endif

	/* Enable reset and suspend interrupt interrupts */
	USBC_INT_EnableUsbMiscUint(g_sunxi_udc_io.usb_bsp_hdle, USBC_BP_INTUSB_SUSPEND);
	USBC_INT_EnableUsbMiscUint(g_sunxi_udc_io.usb_bsp_hdle, USBC_BP_INTUSB_RESUME);
	USBC_INT_EnableUsbMiscUint(g_sunxi_udc_io.usb_bsp_hdle, USBC_BP_INTUSB_RESET);
	USBC_INT_EnableUsbMiscUint(g_sunxi_udc_io.usb_bsp_hdle, USBC_BP_INTUSB_DISCONNECT);

	/* Enable ep0 interrupt */
	USBC_INT_EnableEp(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX, 0);

	cfg_udc_command(SW_UDC_P_ENABLE);

	return ;
}

/*
  Register entry point for the peripheral controller driver.
*/
int usb_gadget_register_driver(struct usb_gadget_driver *driver)
{
	struct sunxi_udc *udc = the_controller;
	int retval = 0;

	/* Sanity checks */
	if(!udc){
	    DMSG_PANIC("ERR: usb_gadget_register_driver, udc is null\n");
		return -ENODEV;
	}

	if (udc->driver){
	    DMSG_PANIC("ERR: usb_gadget_register_driver, udc->driver is not null\n");
		return -EBUSY;
	}

	if (!driver->bind || !driver->setup
			|| driver->speed < USB_SPEED_FULL) {
		DMSG_PANIC("ERR: Invalid driver: bind %p setup %p speed %d\n",
			driver->bind, driver->setup, driver->speed);
		return -EINVAL;
	}

	/* Hook the driver */
	udc->driver = driver;

	if ((retval = driver->bind (&udc->gadget)) != 0) {
	    DMSG_PANIC("ERR: Error in bind() : %d\n",retval);
		goto register_error;
	}

	/* Enable udc */
	sunxi_udc_enable(udc);

	return 0;

register_error:
	udc->driver = NULL;

	return retval;
}

/*
 * Unregister entry point for the peripheral controller driver.
 */
int usb_gadget_unregister_driver(struct usb_gadget_driver *driver)
{
	struct sunxi_udc *udc = the_controller;

	if(!udc){
		DMSG_PANIC("ERR: usb_gadget_register_driver, udc is null\n");
		return -ENODEV;
	}

	if(!driver || driver != udc->driver || !driver->unbind){
		DMSG_PANIC("ERR: usb_gadget_unregister_driver, driver is null\n");
		return -EINVAL;
	}

	DMSG_INFO_UDC("[%s]: usb_gadget_register_driver() '%s'\n", gadget_name, driver->driver.name);

	if(driver->disconnect){
		driver->disconnect(&udc->gadget);
	}

	/* unbind gadget driver */
	driver->unbind(&udc->gadget);
	udc->driver = NULL;

	/* Disable udc */
	sunxi_udc_disable(udc);

	return 0;
}

/* Maps the buffer to dma  */
static inline void sunxi_udc_map_dma_buffer(struct sunxi_udc_request *req, struct sunxi_udc *udc, struct sunxi_udc_ep *ep)
{
	if (!is_sunxi_udc_dma_capable(req, ep)) {
		DMSG_PANIC("err: need not to dma map\n");
		return;
	}

	req->map_state = UN_MAPPED;

	if (req->req.dma == DMA_ADDR_INVALID) {
		req->req.dma = dma_map_single(req->req.buf,
						req->req.length,
						(is_tx_ep(ep) ? DMA_TO_DEVICE : DMA_FROM_DEVICE));
		req->map_state = SW_UDC_USB_MAPPED;
	} else {
		dma_sync_single_for_device(req->req.dma,
					    req->req.length,
					    (is_tx_ep(ep) ? DMA_TO_DEVICE : DMA_FROM_DEVICE));
					    req->map_state = PRE_MAPPED;
	}

	return;
}

/* Unmap the buffer from dma and maps it back to cpu */
static inline void sunxi_udc_unmap_dma_buffer(struct sunxi_udc_request *req, struct sunxi_udc *udc, struct sunxi_udc_ep *ep)
{
	if (!is_buffer_mapped(req, ep)) {
		//DMSG_PANIC("err: need not to dma ummap\n");
		return;
	}

	if (req->req.dma == DMA_ADDR_INVALID) {
		DMSG_PANIC("not unmapping a never mapped buffer\n");
		return;
	}

	if (req->map_state == SW_UDC_USB_MAPPED) {
		dma_unmap_single((void *)req->req.dma,
				  req->req.length,
				  (is_tx_ep(ep) ? DMA_TO_DEVICE : DMA_FROM_DEVICE));

		req->req.dma = DMA_ADDR_INVALID;
	} else { /* PRE_MAPPED */
		dma_sync_single_for_cpu(req->req.dma,
					 req->req.length,
					 (is_tx_ep(ep) ? DMA_TO_DEVICE : DMA_FROM_DEVICE));
	}

	req->map_state = UN_MAPPED;
	return;
}

/* disable usb module irq */
static void disable_irq_udc(struct sunxi_udc *dev)
{
	//disable_irq(dev->irq_no);
}

/* enable usb module irq */
static void enable_irq_udc(struct sunxi_udc *dev)
{
	//enable_irq(dev->irq_no);
}

static void sunxi_udc_done(struct sunxi_udc_ep *ep, struct sunxi_udc_request *req, int status)
{
	unsigned halted = ep->halted;

	if(g_queue_debug){
		//DMSG_INFO("d: (0x%p, %d, %d)\n\n\n", &(req->req), req->req.length, req->req.actual);
		DMSG_INFO_UDC("d: (%s, %p, %d, %d)\n\n\n", ep->ep.name, &(req->req), req->req.length, req->req.actual);
	}

	list_del_init(&req->queue);

	if (likely (req->req.status == -EINPROGRESS))
		req->req.status = status;
	else
		status = req->req.status;

	ep->halted = 1;
	spin_unlock(&ep->dev->lock);
	if (is_sunxi_udc_dma_capable(req, ep)) {
		sunxi_udc_unmap_dma_buffer(req, ep->dev, ep);
	}
	req->req.complete(&ep->ep, &req->req);
	spin_lock(&ep->dev->lock);
	ep->halted = halted;

	if(g_queue_debug){
		printk("%s:%d: %s, %p\n", __func__, __LINE__, ep->ep.name, &(req->req));
	}
}

/*
 *	nuke - dequeue ALL requests
 */
static void sunxi_udc_nuke(struct sunxi_udc *udc, struct sunxi_udc_ep *ep, int status)
{
	/* Sanity check */
	if (&ep->queue == NULL)
		return;

	while (!list_empty (&ep->queue)) {
		struct sunxi_udc_request *req;
		req = list_entry (ep->queue.next, struct sunxi_udc_request,queue);
		DMSG_INFO_UDC("nuke: ep num is %d\n", ep->num);
		sunxi_udc_done(ep, req, status);
	}
}

static inline int sunxi_udc_fifo_count_out(__hdle usb_bsp_hdle, __u8 ep_index)
{
	if (ep_index) {
		return USBC_ReadLenFromFifo(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
	} else {
		return USBC_ReadLenFromFifo(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
	}
}

static inline int sunxi_udc_write_packet(int fifo,
						struct sunxi_udc_request *req,
						unsigned max)
{
	unsigned len = min(req->req.length - req->req.actual, max);
	u8 *buf = req->req.buf + req->req.actual;

	prefetch(buf);

	DMSG_DBG_UDC("W: req.actual(%d), req.length(%d), len(%d), total(%d)\n",
		req->req.actual, req->req.length, len, req->req.actual + len);

	req->req.actual += len;

	udelay(5);
	USBC_WritePacket(g_sunxi_udc_io.usb_bsp_hdle, fifo, len, buf);

	return len;
}

static int pio_write_fifo(struct sunxi_udc_ep *ep, struct sunxi_udc_request *req)
{
	unsigned	count		= 0;
	int		is_last		= 0;
	u32		idx		= 0;
	int		fifo_reg	= 0;
	__s32		ret		= 0;
	u8		old_ep_index	= 0;

	idx = ep->bEndpointAddress & 0x7F;

	/* write data */

	/* select ep */
	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, idx);

	/* select fifo */
	fifo_reg = USBC_SelectFIFO(g_sunxi_udc_io.usb_bsp_hdle, idx);

	count = sunxi_udc_write_packet(fifo_reg, req, ep->ep.maxpacket);

	/* check if the last packet */

	/* last packet is often short (sometimes a zlp) */
	if (count != ep->ep.maxpacket)
		is_last = 1;
	else if (req->req.length != req->req.actual || req->req.zero)
		is_last = 0;
	else
		is_last = 2;

	if (g_write_debug) {
		DMSG_INFO_UDC("pw: (0x%p, %d, %d)\n", &(req->req), req->req.length, req->req.actual);
	}

	if (idx) { /* ep1~4 */
		ret = USBC_Dev_WriteDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX, is_last);
		if (ret != 0) {
			DMSG_PANIC("ERR: USBC_Dev_WriteDataStatus, failed\n");
			req->req.status = -EOVERFLOW;
		}
	} else {  /* ep0 */
		ret = USBC_Dev_WriteDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, is_last);
		if (ret != 0) {
			DMSG_PANIC("ERR: USBC_Dev_WriteDataStatus, failed\n");
			req->req.status = -EOVERFLOW;
		}
	}

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);

	if (is_last) {
		if (!idx) {  /* ep0 */
			ep->dev->ep0state=EP0_IDLE;
		}

		sunxi_udc_done(ep,req, 0);
		is_last = 1;
	}

	return is_last;
}

static int dma_write_fifo(struct sunxi_udc_ep *ep, struct sunxi_udc_request *req)
{
	u32   	left_len	= 0;
	u32	idx		= 0;
	int	fifo_reg	= 0;
	u8	old_ep_index	= 0;

	idx = ep->bEndpointAddress & 0x7F;

	/* select ep */
	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, idx);

	/* select fifo */
	fifo_reg = USBC_SelectFIFO(g_sunxi_udc_io.usb_bsp_hdle, idx);

	/* auto_set, tx_mode, dma_tx_en, mode1 */
	USBC_Dev_ConfigEpDma(ep->dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_TX);

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);


	/* cut fragment part(??) */
	left_len = req->req.length - req->req.actual;
	left_len = left_len - (left_len % ep->ep.maxpacket);

	ep->dma_working	= 1;
	dma_working = 1;
	ep->dma_transfer_len = left_len;

	if (g_dma_debug) {
		DMSG_INFO_UDC("dw: (0x%p, %d, %d)\n", &(req->req), req->req.length, req->req.actual);
	}

	spin_unlock(&ep->dev->lock);

	sunxi_udc_dma_set_config(ep, req, (__u32)req->req.dma, left_len);
	sunxi_udc_dma_start(ep, fifo_reg, (__u32)req->req.dma, left_len);
	spin_lock(&ep->dev->lock);

	return 0;
}

/* return: 0 = still running, 1 = completed, negative = errno */
static int sunxi_udc_write_fifo(struct sunxi_udc_ep *ep, struct sunxi_udc_request *req)
{
	if (ep->dma_working) {
		if (g_dma_debug) {
			struct sunxi_udc_request *req_next = NULL;

			DMSG_PANIC("ERR: dma is busy, write fifo. ep(0x%p, %d), req(0x%p, 0x%p, 0x%x, %d, %d)\n\n",
				ep, ep->num,
				req, &(req->req), (u32)req->req.buf, req->req.length, req->req.actual);

			if (likely (!list_empty(&ep->queue))) {
				req_next = list_entry(ep->queue.next, struct sunxi_udc_request, queue);
			} else {
				req_next = NULL;
			}

			if (req_next) {
				DMSG_PANIC("ERR: dma is busy, write fifo. req(0x%p, 0x%p, 0x%x, %d, %d)\n\n",
					req_next, &(req_next->req), (u32)req_next->req.buf, req_next->req.length, req_next->req.actual);
			}
		}

		return 0;
	}

	if (is_sunxi_udc_dma_capable(req, ep))
		return dma_write_fifo(ep, req);
	else
		return pio_write_fifo(ep, req);
}

static inline int sunxi_udc_read_packet(int fifo, u8 *buf,
						struct sunxi_udc_request *req, unsigned avail)
{
	unsigned len = 0;

	len = min(req->req.length - req->req.actual, avail);
	req->req.actual += len;

	DMSG_DBG_UDC("R: req.actual(%d), req.length(%d), len(%d), total(%d)\n",
		req->req.actual, req->req.length, len, req->req.actual + len);

	USBC_ReadPacket(g_sunxi_udc_io.usb_bsp_hdle, fifo, len, buf);

	return len;
}

/* return: 0 = still running, 1 = completed, negative = errno */
static int pio_read_fifo(struct sunxi_udc_ep *ep, struct sunxi_udc_request *req)
{
	u8		*buf 		= NULL;
	unsigned	bufferspace	= 0;
	int		is_last		= 1;
	unsigned	avail 		= 0;
	int		fifo_count 	= 0;
	u32		idx 		= 0;
	int		fifo_reg 	= 0;
	__s32 		ret 		= 0;
	u8		old_ep_index 	= 0;

	idx = ep->bEndpointAddress & 0x7F;

	/* select fifo */
	fifo_reg = USBC_SelectFIFO(g_sunxi_udc_io.usb_bsp_hdle, idx);

	if (!req->req.length) {
		DMSG_PANIC("ERR: req->req.length == 0\n");
		return 1;
	}

	buf = req->req.buf + req->req.actual;
	bufferspace = req->req.length - req->req.actual;
	if (!bufferspace) {
		DMSG_PANIC("ERR: buffer full!\n");
		return -1;
	}

	/* select ep */
	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, idx);

	fifo_count = sunxi_udc_fifo_count_out(g_sunxi_udc_io.usb_bsp_hdle, idx);
	if (fifo_count > ep->ep.maxpacket) {
		avail = ep->ep.maxpacket;
	} else {
		avail = fifo_count;
	}

	fifo_count = sunxi_udc_read_packet(fifo_reg, buf, req, avail);

	/* checking this with ep0 is not accurate as we already
	 * read a control request */
	if (idx != 0 && fifo_count < ep->ep.maxpacket) {
		is_last = 1;
		/* overflowed this request?  flush extra data */
		if (fifo_count != avail)
			req->req.status = -EOVERFLOW;
	} else {
		is_last = (req->req.length <= req->req.actual) ? 1 : 0;
	}

	if (g_read_debug) {
		DMSG_INFO_UDC("pr: (0x%p, %d, %d)\n", &(req->req), req->req.length, req->req.actual);
	}

	if (idx) {
		ret = USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX, is_last);
		if (ret != 0) {
			DMSG_PANIC("ERR: pio_read_fifo: USBC_Dev_WriteDataStatus, failed\n");
			req->req.status = -EOVERFLOW;
		}
	} else {
		ret = USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, is_last);
		if (ret != 0) {
			DMSG_PANIC("ERR: pio_read_fifo: USBC_Dev_WriteDataStatus, failed\n");
			req->req.status = -EOVERFLOW;
		}
	}

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);

	if (is_last) {
		if (!idx) {
			ep->dev->ep0state = EP0_IDLE;
		}

		sunxi_udc_done(ep, req, 0);
		is_last = 1;
	}

	return is_last;
}

static int dma_read_fifo(struct sunxi_udc_ep *ep, struct sunxi_udc_request *req)
{
	u32   	left_len 	= 0;
	u32	idx		= 0;
	int	fifo_reg	= 0;
	u8	old_ep_index 	= 0;

	idx = ep->bEndpointAddress & 0x7F;

	/* select ep */
	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, idx);

	/* select fifo */
	fifo_reg = USBC_SelectFIFO(g_sunxi_udc_io.usb_bsp_hdle, idx);

	/* auto_set, tx_mode, dma_tx_en, mode1 */
	USBC_Dev_ConfigEpDma(ep->dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_RX);

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);

	/* cut fragment packet part */
	left_len = req->req.length - req->req.actual;
	left_len = left_len - (left_len % ep->ep.maxpacket);

	if (g_dma_debug) {
		DMSG_INFO_UDC("dr: (0x%p, %d, %d)\n", &(req->req), req->req.length, req->req.actual);
	}

	ep->dma_working	= 1;
	dma_working = 1;
	ep->dma_transfer_len = left_len;

	spin_unlock(&ep->dev->lock);

	sunxi_udc_dma_set_config(ep, req, (__u32)req->req.dma, left_len);
	sunxi_udc_dma_start(ep, fifo_reg, (__u32)req->req.dma, left_len);
	spin_lock(&ep->dev->lock);

	return 0;
}

/* return: 0 = still running, 1 = completed, negative = errno */
static int sunxi_udc_read_fifo(struct sunxi_udc_ep *ep, struct sunxi_udc_request *req)
{
	if (ep->dma_working) {
		if (g_dma_debug) {
			struct sunxi_udc_request *req_next = NULL;

			DMSG_PANIC("ERR: dma is busy, read fifo. ep(0x%p, %d), req(0x%p, 0x%p, 0x%x, %d, %d)\n\n",
				ep, ep->num,
				req, &(req->req), (u32)req->req.buf, req->req.length, req->req.actual);

			if (likely (!list_empty(&ep->queue))) {
				req_next = list_entry(ep->queue.next, struct sunxi_udc_request, queue);
			} else {
				req_next = NULL;
			}

			if (req_next) {
				DMSG_PANIC("ERR: dma is busy, read fifo. req(0x%p, 0x%p, 0x%x, %d, %d)\n\n",
					req_next, &(req_next->req), (u32)req_next->req.buf, req_next->req.length, req_next->req.actual);
			}
		}

		return 0;
	}

	if (is_sunxi_udc_dma_capable(req, ep)) {
		return dma_read_fifo(ep, req);
	} else {
		return pio_read_fifo(ep, req);
	}
}

static int sunxi_udc_read_fifo_crq(struct usb_ctrlrequest *crq)
{
	u32 fifo_count  = 0;
	u32 i		= 0;
	u8  *pOut	= (u8 *) crq;
	u32 fifo	= 0;

	fifo = USBC_SelectFIFO(g_sunxi_udc_io.usb_bsp_hdle, 0);
	fifo_count = USBC_ReadLenFromFifo(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);

	if (fifo_count != 8) {
		i = 0;

		while(i < 16 && (fifo_count != 8) ) {
			fifo_count = USBC_ReadLenFromFifo(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
			i++;
		}

		if (i >= 16) {
			DMSG_PANIC("ERR: get ep0 fifo len failed\n");
		}
	}

	return USBC_ReadPacket(g_sunxi_udc_io.usb_bsp_hdle, fifo, fifo_count, pOut);
}

static int sunxi_udc_get_status(struct sunxi_udc *dev, struct usb_ctrlrequest *crq)
{
	u16 status  = 0;
	u8 	buf[8];
	u8  ep_num  = crq->wIndex & 0x7F;
	u8  is_in   = crq->wIndex & USB_DIR_IN;
	u32 fifo = 0;
	u8 old_ep_index = 0;

	switch (crq->bRequestType & USB_RECIP_MASK) {
	case USB_RECIP_INTERFACE:
		buf[0] = 0x00;
		buf[1] = 0x00;
		break;

	case USB_RECIP_DEVICE:
		status = dev->devstatus;
		buf[0] = 0x01;
		buf[1] = 0x00;
		break;

	case USB_RECIP_ENDPOINT:
		if (ep_num > 4 || crq->wLength > 2) {
			return 1;
		}

		old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
		USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, ep_num);
		if (ep_num == 0) {
			status = USBC_Dev_IsEpStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
		} else {
			if (is_in) {
				status = USBC_Dev_IsEpStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
			} else {
				status = USBC_Dev_IsEpStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
			}
		}
		status = status ? 1 : 0;
		if (status) {
			buf[0] = 0x01;
			buf[1] = 0x00;
		} else {
			buf[0] = 0x00;
			buf[1] = 0x00;
		}
		USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);
		break;

	default:
		return 1;
	}

	/* Seems to be needed to get it working. ouch :( */
	udelay(5);
	USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 0);

	fifo = USBC_SelectFIFO(g_sunxi_udc_io.usb_bsp_hdle, 0);
	USBC_WritePacket(g_sunxi_udc_io.usb_bsp_hdle, fifo, crq->wLength, buf);
	USBC_Dev_WriteDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);

	return 0;
}

static void sunxi_udc_handle_ep0_idle(struct sunxi_udc *dev,
						struct sunxi_udc_ep *ep,
						struct usb_ctrlrequest *crq,
						u32 ep0csr)
{
	int len = 0, ret = 0, tmp = 0;

	/* start control request? */
	if (!USBC_Dev_IsReadDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0)) {
		DMSG_WRN("ERR: data is ready, can not read data.\n");
		return;
	}

	sunxi_udc_nuke(dev, ep, -EPROTO);

	len = sunxi_udc_read_fifo_crq(crq);
	if (len != sizeof(*crq)) {
		DMSG_PANIC("setup begin: fifo READ ERROR"
			" wanted %d bytes got %d. Stalling out...\n",
			sizeof(*crq), len);

		USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 0);
		USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);

		return;
	}

	DMSG_DBG_UDC("ep0: bRequest = %d bRequestType %d wLength = %d\n",
		crq->bRequest, crq->bRequestType, crq->wLength);

	/* cope with automagic for some standard requests. */
	dev->req_std = ((crq->bRequestType & USB_TYPE_MASK) == USB_TYPE_STANDARD);
	dev->req_config = 0;
	dev->req_pending = 1;

	if(dev->req_std){   //standard request
		switch (crq->bRequest) {
		case USB_REQ_SET_CONFIGURATION:
			DMSG_DBG_UDC("USB_REQ_SET_CONFIGURATION ... \n");

			if (crq->bRequestType == USB_RECIP_DEVICE) {
				dev->req_config = 1;
			}
			break;
		case USB_REQ_SET_INTERFACE:
			DMSG_DBG_UDC("USB_REQ_SET_INTERFACE ... \n");

			if (crq->bRequestType == USB_RECIP_INTERFACE) {
				dev->req_config = 1;
			}
			break;
		case USB_REQ_SET_ADDRESS:
			DMSG_DBG_UDC("USB_REQ_SET_ADDRESS ... \n");

			if (crq->bRequestType == USB_RECIP_DEVICE) {
				tmp = crq->wValue & 0x7F;
				dev->address = tmp;

				//rx receive over, dataend, tx_pakect ready
				USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);

				dev->ep0state = EP0_END_XFER;

				crq_bRequest = USB_REQ_SET_ADDRESS;

				return;
			}
			break;
		case USB_REQ_GET_STATUS:
			DMSG_DBG_UDC("USB_REQ_GET_STATUS ... \n");

			if (!sunxi_udc_get_status(dev, crq)) {
				return;
			}
			break;
		case USB_REQ_CLEAR_FEATURE:
			//--<1>--data direction must be host to device
			if(x_test_bit(crq->bRequestType, 7)){
				DMSG_PANIC("USB_REQ_CLEAR_FEATURE: data is not host to device\n");
				break;
			}

			USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);

			//--<3>--data stage
			if(crq->bRequestType == USB_RECIP_DEVICE){
				/* wValue 0-1 */
				if(crq->wValue){
					dev->devstatus &= ~(1 << USB_DEVICE_REMOTE_WAKEUP);
				}else{
					int k = 0;
					for(k = 0;k < SW_UDC_ENDPOINTS;k++){
						sunxi_udc_set_halt_ex(&dev->ep[k].ep, 0);
					}
				}

			}else if(crq->bRequestType == USB_RECIP_INTERFACE){
				//--<2>--token stage over

				//do nothing

			}else if(crq->bRequestType == USB_RECIP_ENDPOINT){
				//--<3>--release the forbidden of ep
				//sunxi_udc_set_halt(&dev->ep[crq->wIndex & 0x7f].ep, 0);
				//dev->devstatus &= ~(1 << USB_DEVICE_REMOTE_WAKEUP);
				/* wValue 0-1 */
				if(crq->wValue){
					dev->devstatus &= ~(1 << USB_DEVICE_REMOTE_WAKEUP);
				}else{
					sunxi_udc_set_halt_ex(&dev->ep[crq->wIndex & 0x7f].ep, 0);
				}

			}else{
				DMSG_PANIC("PANIC : nonsupport set feature request. (%d)\n", crq->bRequestType);
				USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
			}

			dev->ep0state = EP0_IDLE;

			return;
			//break;

		case USB_REQ_SET_FEATURE:
			//--<1>--data direction must be host to device
			if(x_test_bit(crq->bRequestType, 7)){
				DMSG_PANIC("USB_REQ_SET_FEATURE: data is not host to device\n");
				break;
			}

			//--<3>--data stage
			if(crq->bRequestType == USB_RECIP_DEVICE){
				if((crq->wValue == USB_DEVICE_TEST_MODE) ){
					//setup packet receive over
					switch (crq->wIndex){
					case SUNXI_UDC_TEST_J:
						USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
						dev->ep0state = EP0_END_XFER;
						crq_wIndex = TEST_J;
						crq_bRequest = USB_REQ_SET_FEATURE;
						return;
					case SUNXI_UDC_TEST_K:
						USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
						dev->ep0state = EP0_END_XFER;
						crq_wIndex = TEST_K;
						crq_bRequest = USB_REQ_SET_FEATURE;
						return;
					case SUNXI_UDC_TEST_SE0_NAK:
						USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
						dev->ep0state = EP0_END_XFER;
						crq_wIndex = TEST_SE0_NAK;
						crq_bRequest = USB_REQ_SET_FEATURE;
						return;
					case SUNXI_UDC_TEST_PACKET:
						USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
						dev->ep0state = EP0_END_XFER;
						crq_wIndex = TEST_PACKET;
						crq_bRequest = USB_REQ_SET_FEATURE;
						return;
					default:
						break;
					}
				}

				USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
				dev->devstatus |= (1 << USB_DEVICE_REMOTE_WAKEUP);
			}else if(crq->bRequestType == USB_RECIP_INTERFACE){
				//--<2>--token stage over
				USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
				//do nothing

			}else if(crq->bRequestType == USB_RECIP_ENDPOINT){
				//--<3>--forbidden ep
				USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
						sunxi_udc_set_halt_ex(&dev->ep[crq->wIndex & 0x7f].ep, 1);
			}else{
				DMSG_PANIC("PANIC : nonsupport set feature request. (%d)\n", crq->bRequestType);

				USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
				USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
			}

			dev->ep0state = EP0_IDLE;

			return;
			//break;

		default:
			/* only receive setup_data packet, cannot set DataEnd */
			USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 0);
			break;
		}
	}else{
		USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 0);
#if defined (CONFIG_ARCH_SUN8IW8)
		/* getinfo request about exposure time asolute, iris absolute, brightness of webcam. */
		if (crq->bRequest == 0x86 && crq->bRequestType == 0xa1 && crq->wLength == 0x1
			&& ((crq->wValue == 0x400 && crq->wIndex == 0x100)
				|| (crq->wValue == 0x900 && crq->wIndex == 0x100)
				|| (crq->wValue == 0x200 && crq->wIndex == 0x200))) {
			USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
			return;
		}
#endif
	}

	if(crq->bRequestType & USB_DIR_IN){
		dev->ep0state = EP0_IN_DATA_PHASE;
	}else{
		dev->ep0state = EP0_OUT_DATA_PHASE;
	}

	if(!dev->driver)
		return;

	spin_unlock(&dev->lock);
	ret = dev->driver->setup(&dev->gadget, crq);
	spin_lock(&dev->lock);
	if (ret < 0) {
		if (dev->req_config) {
			DMSG_PANIC("ERR: config change %02x fail %d?\n", crq->bRequest, ret);
			return;
		}

		if (ret == -EOPNOTSUPP) {
			DMSG_PANIC("ERR: Operation not supported\n");
		} else {
			DMSG_PANIC("ERR: dev->driver->setup failed. (%d)\n", ret);
		}

		udelay(5);

		USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
		USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);

		dev->ep0state = EP0_IDLE;
		/* deferred i/o == no response yet */
	} else if (dev->req_pending) {
		//DMSG_PANIC("ERR: dev->req_pending... what now?\n");
		dev->req_pending=0;
	}

	if (crq->bRequest == USB_REQ_SET_CONFIGURATION || crq->bRequest == USB_REQ_SET_INTERFACE) {
		//rx_packet receive over
		USBC_Dev_ReadDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 1);
	}

	return;
}

static void sunxi_udc_handle_ep0(struct sunxi_udc *dev)
{
	u32 ep0csr = 0;
	struct sunxi_udc_ep *ep = &dev->ep[0];
	struct sunxi_udc_request *req = NULL;
	struct usb_ctrlrequest crq;

	DMSG_DBG_UDC("sunxi_udc_handle_ep0--1--\n");

	if (list_empty(&ep->queue)) {
		req = NULL;
	} else {
		req = list_entry(ep->queue.next, struct sunxi_udc_request, queue);
	}

	DMSG_DBG_UDC("sunxi_udc_handle_ep0--2--\n");

	/* We make the assumption that sunxi_udc_UDC_IN_CSR1_REG equal to
	 * sunxi_udc_UDC_EP0_CSR_REG when index is zero */
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, 0);

	/* clear stall status */
	if (USBC_Dev_IsEpStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0)) {
		DMSG_PANIC("ERR: ep0 stall\n");

		sunxi_udc_nuke(dev, ep, -EPIPE);
		USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
		dev->ep0state = EP0_IDLE;
		return;
	}

	/* clear setup end */
	if (USBC_Dev_Ctrl_IsSetupEnd(g_sunxi_udc_io.usb_bsp_hdle)) {
		DMSG_PANIC("handle_ep0: ep0 setup end\n");

		sunxi_udc_nuke(dev, ep, 0);
		USBC_Dev_Ctrl_ClearSetupEnd(g_sunxi_udc_io.usb_bsp_hdle);
		dev->ep0state = EP0_IDLE;
	}

	DMSG_DBG_UDC("sunxi_udc_handle_ep0--3--%d\n", dev->ep0state);

	ep0csr = USBC_Readw(USBC_REG_CSR0(g_sunxi_udc_io.usb_pbase));

	switch (dev->ep0state) {
	case EP0_IDLE:
		sunxi_udc_handle_ep0_idle(dev, ep, &crq, ep0csr);
		break;
	case EP0_IN_DATA_PHASE:			/* GET_DESCRIPTOR etc */
		DMSG_DBG_UDC("EP0_IN_DATA_PHASE ... what now?\n");

		if (!USBC_Dev_IsWriteDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0) && req) {
			sunxi_udc_write_fifo(ep, req);
		}
		break;
	case EP0_OUT_DATA_PHASE:		/* SET_DESCRIPTOR etc */
		DMSG_DBG_UDC("EP0_OUT_DATA_PHASE ... what now?\n");

		if (USBC_Dev_IsReadDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0) && req ) {
			sunxi_udc_read_fifo(ep,req);
		}
		break;
	case EP0_END_XFER:
		DMSG_DBG_UDC("EP0_END_XFER ... what now?\n");
		DMSG_DBG_UDC("crq_bRequest = 0x%x\n", crq_bRequest);

		switch (crq_bRequest) {
		case USB_REQ_SET_ADDRESS:
			USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, 0);

			USBC_Dev_Ctrl_ClearSetupEnd(g_sunxi_udc_io.usb_bsp_hdle);
			USBC_Dev_SetAddress(g_sunxi_udc_io.usb_bsp_hdle, dev->address);

			DMSG_INFO_UDC("Set address %d\n", dev->address);
			break;
		case USB_REQ_SET_FEATURE:
			switch (crq_wIndex){
			case TEST_J:
				USBC_EnterMode_Test_J(g_sunxi_udc_io.usb_bsp_hdle);
				break;

			case TEST_K:
				USBC_EnterMode_Test_K(g_sunxi_udc_io.usb_bsp_hdle);
				break;

			case TEST_SE0_NAK:
				USBC_EnterMode_Test_SE0_NAK(g_sunxi_udc_io.usb_bsp_hdle);
				break;

			case TEST_PACKET:
			{
				u32 fifo = 0;
				fifo = USBC_SelectFIFO(g_sunxi_udc_io.usb_bsp_hdle, 0);
				USBC_WritePacket(g_sunxi_udc_io.usb_bsp_hdle, fifo, 54, (u32 *)TestPkt);
				USBC_EnterMode_TestPacket(g_sunxi_udc_io.usb_bsp_hdle);
				USBC_Dev_WriteDataStatus(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0, 0);
			}
				break;
			default:
				break;
			}

			crq_wIndex = 0;
			break;
		default:
			break;
		}

		crq_bRequest = 0;

		dev->ep0state = EP0_IDLE;
		break;

	case EP0_STALL:
		DMSG_DBG_UDC("EP0_STALL ... what now?\n");
		dev->ep0state = EP0_IDLE;
		break;
	}

	DMSG_DBG_UDC("sunxi_udc_handle_ep0--4--%d\n", dev->ep0state);
	return;
}

static void sunxi_udc_handle_ep(struct sunxi_udc_ep *ep)
{
	struct sunxi_udc_request *req = NULL;
	int is_in = ep->bEndpointAddress & USB_DIR_IN;
	u32 idx = 0;
	u8 old_ep_index = 0;

	/* see sunxi_udc_queue. */
	if (likely (!list_empty(&ep->queue))) {
		req = list_entry(ep->queue.next, struct sunxi_udc_request, queue);
	} else {
		req = NULL;
	}

	if(g_irq_debug){
		DMSG_INFO_UDC("e: (%s), tx_csr=0x%x\n", ep->ep.name, USBC_Readw(USBC_REG_TXCSR(g_sunxi_udc_io.usb_vbase)));
		if(req){
			DMSG_INFO_UDC("req: (0x%p, %d, %d)\n", &(req->req), req->req.length, req->req.actual);
		}
	}

	idx = ep->bEndpointAddress & 0x7F;

	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, idx);

	if (is_in) {
		if (USBC_Dev_IsEpStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX)) {
			DMSG_PANIC("ERR: tx ep(%d) is stall\n", idx);
			USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
			goto end;
		}
	} else {
		if (USBC_Dev_IsEpStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX)) {
			DMSG_PANIC("ERR: rx ep(%d) is stall\n", idx);
			USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
			goto end;
		}
	}

	if (req) {
		if (is_in) {
			if (!USBC_Dev_IsWriteDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX)) {
				sunxi_udc_write_fifo(ep, req);
			}
		} else {
			if (USBC_Dev_IsReadDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX)) {
				sunxi_udc_read_fifo(ep,req);
			}
		}
	}

end:
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);
	return;
}

/* mask the useless irq, save disconect, reset, resume, suspend */
static u32 filtrate_irq_misc(u32 irq_misc)
{
	u32 irq = irq_misc;

	irq &= ~(USBC_INTUSB_VBUS_ERROR | USBC_INTUSB_SESSION_REQ | USBC_INTUSB_CONNECT | USBC_INTUSB_SOF);
	USBC_INT_ClearMiscPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_INTUSB_VBUS_ERROR);
	USBC_INT_ClearMiscPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_INTUSB_SESSION_REQ);
	USBC_INT_ClearMiscPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_INTUSB_CONNECT);
	USBC_INT_ClearMiscPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_INTUSB_SOF);

	return irq;
}

static void clear_all_irq(void)
{
	USBC_INT_ClearEpPendingAll(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
	USBC_INT_ClearEpPendingAll(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
	USBC_INT_ClearMiscPendingAll(g_sunxi_udc_io.usb_bsp_hdle);
}

static void throw_away_all_urb(struct sunxi_udc *dev)
{
	int k = 0;

	DMSG_INFO_UDC("irq: reset happen, throw away all urb\n");
	for(k = 0; k < SW_UDC_ENDPOINTS; k++){
		sunxi_udc_nuke(dev, (struct sunxi_udc_ep * )&(dev->ep[k]), -ECONNRESET);
	}
}

/* clear all dma status of the EP, called when dma exception */
static void sunxi_udc_clean_dma_status(struct sunxi_udc_ep *ep)
{
	u8 ep_index = 0;
	u8 old_ep_index = 0;
	struct sunxi_udc_request *req = NULL;

	ep_index = ep->bEndpointAddress & 0x7F;

	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, ep_index);

	if ((ep->bEndpointAddress) & USB_DIR_IN) {  // dma_mode1
		/* clear ep dma status */
		USBC_Dev_ClearEpDma(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);

		/* select bus to pio */
		sunxi_udc_switch_bus_to_pio(ep, 1);
	} else {  // dma_mode0
		/* clear ep dma status */
		USBC_Dev_ClearEpDma(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);

		/* select bus to pio */
		sunxi_udc_switch_bus_to_pio(ep, 0);
	}

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);

	/* done req */
	while(likely (!list_empty(&ep->queue))) {
		req = list_entry(ep->queue.next, struct sunxi_udc_request, queue);
		if (req) {
			req->req.status = -ECONNRESET;
			req->req.actual = 0;
			sunxi_udc_done(ep, req, -ECONNRESET);
		}
	}

	ep->dma_working = 0;
	dma_working = 0;
	return;
}

static void sunxi_udc_stop_dma_work(struct sunxi_udc *dev, u32 unlock)
{
	__u32 i = 0;
	struct sunxi_udc_ep *ep = NULL;

	for(i = 0; i < SW_UDC_ENDPOINTS; i++) {
		ep = &dev->ep[i];

		if (sunxi_udc_dma_is_busy(ep)) {
			DMSG_PANIC("wrn: ep(%d) must stop working\n", i);

			if (unlock){
				spin_unlock(&ep->dev->lock);
				sunxi_udc_dma_stop(ep);
				spin_lock(&ep->dev->lock);
			}else {
				sunxi_udc_dma_stop(ep);
			}

#ifdef SW_UDC_DMA_INNER
			ep->dev->dma_hdle = 0;
#else
			ep->dev->sunxi_udc_dma[ep->num].is_start = 0;
#endif
			ep->dma_transfer_len = 0;

			sunxi_udc_clean_dma_status(ep);
		}
	}

	return;
}

void sunxi_udc_dma_completion(struct sunxi_udc *dev, struct sunxi_udc_ep *ep, struct sunxi_udc_request *req)
{
	unsigned long		flags 			= 0;
	__u8  			old_ep_index 		= 0;
	__u32 			dma_transmit_len	= 0;
	int 			is_complete		= 0;
	struct sunxi_udc_request *req_next		= NULL;

	if (dev == NULL || ep == NULL || req == NULL) {
		DMSG_PANIC("ERR: argment invaild. (0x%p, 0x%p, 0x%p)\n", dev, ep, req);
		return;
	}

	if (!ep->dma_working) {
		DMSG_PANIC("ERR: dma is not work, can not callback\n");
		return;
	}

	sunxi_udc_unmap_dma_buffer(req, dev, ep);

	spin_lock_irqsave(&dev->lock, flags);

	old_ep_index = USBC_GetActiveEp(dev->sunxi_udc_io->usb_bsp_hdle);
	USBC_SelectActiveEp(dev->sunxi_udc_io->usb_bsp_hdle, ep->num);

	if ((ep->bEndpointAddress) & USB_DIR_IN) {  //tx, dma_mode1
		while(USBC_Dev_IsWriteDataReady_FifoEmpty(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_TX));
			USBC_Dev_ClearEpDma(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_TX);
	} else {  //rx, dma_mode0
		USBC_Dev_ClearEpDma(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_RX);
	}

	dma_transmit_len = sunxi_udc_dma_transmit_length(ep, ((ep->bEndpointAddress) & USB_DIR_IN), (__u32)req->req.buf);
	if (dma_transmit_len < req->req.length) {
		if ((ep->bEndpointAddress) & USB_DIR_IN) {
			USBC_Dev_ClearEpDma(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_TX);
		} else {
			USBC_Dev_ClearEpDma(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_RX);
		}
	}

	if (g_dma_debug) {
		DMSG_INFO_UDC("di: (0x%p, %d, %d)\n", &(req->req), req->req.length, req->req.actual);
	}

	ep->dma_working = 0;
	dma_working = 0;
	ep->dma_transfer_len = 0;

	/* if current data transfer not complete, then go on */
	req->req.actual += dma_transmit_len;
	if(req->req.length > req->req.actual){
		if(((ep->bEndpointAddress & USB_DIR_IN) != 0)
			&& !USBC_Dev_IsWriteDataReady_FifoEmpty(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_TX)){
			if(pio_write_fifo(ep, req)){
				req = NULL;
				is_complete = 1;
			}
		}else if(((ep->bEndpointAddress & USB_DIR_IN) == 0)
			&& USBC_Dev_IsReadDataReady(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_RX)){
			if(pio_read_fifo(ep, req)){
				req = NULL;
				is_complete = 1;
			}
		}
	}else{	/* if DMA transfer data over, then done */
		sunxi_udc_done(ep, req, 0);
		is_complete = 1;
	}

	/* start next transfer */
	if(is_complete){
		if(likely (!list_empty(&ep->queue))){

			req_next = list_entry(ep->queue.next, struct sunxi_udc_request, queue);
		}else{
			req_next = NULL;
		}

		if(req_next){
			if((ep->bEndpointAddress & USB_DIR_IN) != 0) {
				while(USBC_Dev_IsWriteDataReady_FifoEmpty(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_TX));
					sunxi_udc_write_fifo(ep, req_next);
			}else if(((ep->bEndpointAddress & USB_DIR_IN) == 0)
				&& USBC_Dev_IsReadDataReady(dev->sunxi_udc_io->usb_bsp_hdle, USBC_EP_TYPE_RX)){
				sunxi_udc_read_fifo(ep, req_next);
			}
		}
	}

	USBC_SelectActiveEp(dev->sunxi_udc_io->usb_bsp_hdle, old_ep_index);

	spin_unlock_irqrestore(&dev->lock, flags);
	return;
}

static int sunxi_udc_irq(int dummy, void *_dev)
{
	struct sunxi_udc *dev = _dev;
	int usb_irq	= 0;
	int tx_irq	= 0;
	int rx_irq	= 0;
	int i		= 0;
	u32 old_ep_idx  = 0;
	unsigned long flags = 0;
#ifdef SW_UDC_DMA_INNER
	int dma_irq	= 0;
#endif

	spin_lock_irqsave(&dev->lock, flags);

	/* Driver connected ? */
	if (!dev->driver || !is_peripheral_active()) {
		DMSG_PANIC("ERR: functoin driver is not exist, or udc is not active.\n");

		/* Clear interrupts */
		clear_all_irq();

		spin_unlock_irqrestore(&dev->lock, flags);

		return IRQ_NONE;
	}

	/* Save index */
	old_ep_idx = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);

	/* Read status registers */
	usb_irq = USBC_INT_MiscPending(g_sunxi_udc_io.usb_bsp_hdle);
	tx_irq  = USBC_INT_EpPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
	rx_irq  = USBC_INT_EpPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
#ifdef SW_UDC_DMA_INNER
	dma_irq = USBC_Readw(USBC_REG_DMA_INTS(dev->sunxi_udc_io->usb_pbase));
#endif

	usb_irq = filtrate_irq_misc(usb_irq);

	if(g_irq_debug){
		DMSG_INFO_UDC("\n\nirq: usb_irq=%02x, tx_irq=%02x, rx_irq=%02x\n", usb_irq, tx_irq, rx_irq);
	}
	/*
	 * Now, handle interrupts. There's two types :
	 * - Reset, Resume, Suspend coming -> usb_int_reg
	 * - EP -> ep_int_reg
	 */

	/* RESET */
	if (usb_irq & USBC_INTUSB_RESET) {
		DMSG_INFO_UDC("IRQ: reset\n");

#if defined (CONFIG_USB_G_ANDROID) || defined (CONFIG_USB_MASS_STORAGE)
		DMSG_INFO_UDC("(1:star,2:end): vfs_read:%d, vfs_write:%d,dma_working:%d,amount:%u,file_offset:%llu\n",
			atomic_read(&vfs_read_flag), atomic_read(&vfs_write_flag), dma_working, vfs_amount, (unsigned long long)vfs_file_offset);
#endif

		USBC_INT_ClearMiscPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_INTUSB_RESET);
		clear_all_irq();

		usb_connect = 1;

		USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, 0);
		USBC_Dev_SetAddress_default(g_sunxi_udc_io.usb_bsp_hdle);

		if (is_udc_support_dma()) {
			sunxi_udc_stop_dma_work(dev, 1);
		}

		throw_away_all_urb(dev);

		dev->address = 0;
		dev->ep0state = EP0_IDLE;
		//dev->gadget.speed = USB_SPEED_FULL;
		g_irq_debug = 0;
		g_queue_debug = 0;
		g_dma_debug = 0;

		spin_unlock_irqrestore(&dev->lock, flags);

//#if defined CONFIG_AW_AXP && !defined SUNXI_USB_FPGA
//		axp_usbvol(CHARGE_USB_20);
//		axp_usbcur(CHARGE_USB_20);
//#endif
		return IRQ_HANDLED;
	}

	/* RESUME */
	if (usb_irq & USBC_INTUSB_RESUME) {
		DMSG_INFO_UDC("IRQ: resume\n");

		/* clear interrupt */
		USBC_INT_ClearMiscPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_INTUSB_RESUME);

		if (dev->gadget.speed != USB_SPEED_UNKNOWN
			&& dev->driver
			&& dev->driver->resume) {
			spin_unlock(&dev->lock);
			dev->driver->resume(&dev->gadget);
			spin_lock(&dev->lock);
		}
	}

	/* SUSPEND */
	if (usb_irq & USBC_INTUSB_SUSPEND) {
		DMSG_INFO_UDC("IRQ: suspend\n");

		/* clear interrupt */
		USBC_INT_ClearMiscPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_INTUSB_SUSPEND);

		if (dev->gadget.speed != USB_SPEED_UNKNOWN) {

			/* disable usb controller */
			if (dev->driver && dev->driver->disconnect) {
				spin_unlock(&dev->lock);
				dev->driver->disconnect(&dev->gadget);
				spin_lock(&dev->lock);
			}

			usb_connect = 0;
		} else {
			DMSG_INFO_UDC("ERR: usb speed is unkown\n");
		}

		if (dev->gadget.speed != USB_SPEED_UNKNOWN
				&& dev->driver
				&& dev->driver->suspend) {
			spin_unlock(&dev->lock);
			dev->driver->suspend(&dev->gadget);
			spin_lock(&dev->lock);
		}

		dev->ep0state = EP0_IDLE;

	}

	/* DISCONNECT */
	if (usb_irq & USBC_INTUSB_DISCONNECT) {
		DMSG_INFO_UDC("IRQ: disconnect\n");

		USBC_INT_ClearMiscPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_INTUSB_DISCONNECT);

		dev->ep0state = EP0_IDLE;

		usb_connect = 0;
	}

	/* EP */
	/* control traffic */
	/* check on ep0csr != 0 is not a good idea as clearing in_pkt_ready
	 * generate an interrupt
	 */
	if (tx_irq & USBC_INTTx_FLAG_EP0) {
		DMSG_DBG_UDC("USB ep0 irq\n");

		/* Clear the interrupt bit by setting it to 1 */
		USBC_INT_ClearEpPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX, 0);

		if (dev->gadget.speed == USB_SPEED_UNKNOWN) {
			if (USBC_Dev_QueryTransferMode(g_sunxi_udc_io.usb_bsp_hdle) == USBC_TS_MODE_HS) {
				dev->gadget.speed = USB_SPEED_HIGH;

				DMSG_INFO_UDC("\n+++++++++++++++++++++++++++++++++++++\n");
				DMSG_INFO_UDC(" usb enter high speed.\n");
				DMSG_INFO_UDC("\n+++++++++++++++++++++++++++++++++++++\n");
			} else {
				dev->gadget.speed= USB_SPEED_FULL;

				DMSG_INFO_UDC("\n+++++++++++++++++++++++++++++++++++++\n");
				DMSG_INFO_UDC(" usb enter full speed.\n");
				DMSG_INFO_UDC("\n+++++++++++++++++++++++++++++++++++++\n");
			}
		}

		sunxi_udc_handle_ep0(dev);
	}

	/* firstly to get data */

	/* rx endpoint data transfers */
	for (i = 1; i <= SW_UDC_EPNUMS; i++) {
	//for (i = 1; i < SW_UDC_ENDPOINTS; i++) {
		u32 tmp = 1 << i;

		if (rx_irq & tmp) {
			DMSG_DBG_UDC("USB rx ep%d irq\n", i);

			/* Clear the interrupt bit by setting it to 1 */
			USBC_INT_ClearEpPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX, i);

			sunxi_udc_handle_ep(&dev->ep[ep_fifo_out[i]]);
		}
	}

	/* tx endpoint data transfers */
	for (i = 1; i <= SW_UDC_EPNUMS; i++) {
	//for (i = 1; i < SW_UDC_ENDPOINTS; i++) {
		u32 tmp = 1 << i;

		if (tx_irq & tmp) {
			DMSG_DBG_UDC("USB tx ep%d irq\n", i);

			/* Clear the interrupt bit by setting it to 1 */
			USBC_INT_ClearEpPending(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX, i);

			sunxi_udc_handle_ep(&dev->ep[ep_fifo_in[i]]);
		}
	}

#ifdef SW_UDC_DMA_INNER
	if(is_udc_support_dma()){
		struct sunxi_udc_request *req = NULL;
		struct sunxi_udc_ep *ep = NULL;
		int i = 0;

		/* tx endpoint data transfers */
		for (i = 0; i < DMA_CHAN_TOTAL; i++) {
			u32 tmp = 1 << i;

			if (dma_irq & tmp) {
				DMSG_DBG_UDC("USB dma chanle%d irq\n", i);

				/* set 1 to clear pending */
				writel(BIT(i), USBC_REG_DMA_INTS(dev->sunxi_udc_io->usb_pbase));
				//USBC_REG_clear_bit_w(tmp, USBC_REG_DMA_INTS(dev->sunxi_udc_io->usb_vbase));

				ep = &dev->ep[dma_chnl[i].ep_num];
				sunxi_udc_dma_release((dm_hdl_t)ep->dev->dma_hdle);
				ep->dev->dma_hdle = 0;

				if (ep) {
					/* find req */
					if (likely (!list_empty(&ep->queue))) {
						req = list_entry(ep->queue.next, struct sunxi_udc_request, queue);
					} else {
						req = NULL;
					}

					/* call back */
					if (req) {
						spin_unlock_irqrestore(&dev->lock, flags);
						sunxi_udc_dma_completion(dev, ep, req);
						spin_lock_irqsave(&dev->lock, flags);
					}
				} else {
					DMSG_PANIC("ERR: sunxi_udc_dma_callback: dma is remove, but dma irq is happened\n");
				}
			}
		}
	}
#endif

	/* Restore old index */
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_idx);

	spin_unlock_irqrestore(&dev->lock, flags);

	return IRQ_HANDLED;
}

static inline struct sunxi_udc_ep *to_sunxi_udc_ep(struct usb_ep *ep)
{
	return container_of(ep, struct sunxi_udc_ep, ep);
}

static inline struct sunxi_udc *to_sunxi_udc(struct usb_gadget *gadget)
{
	return container_of(gadget, struct sunxi_udc, gadget);
}

static inline struct sunxi_udc_request *to_sunxi_udc_req(struct usb_request *req)
{
	return container_of(req, struct sunxi_udc_request, req);
}

static int sunxi_udc_ep_enable(struct usb_ep *_ep,
				const struct usb_endpoint_descriptor *desc)
{
	struct sunxi_udc	*dev		= NULL;
	struct sunxi_udc_ep	*ep		= NULL;
	u32			 max	 	= 0;
	u32	 		old_ep_index	= 0;
	__u32 			fifo_addr 	= 0;
	unsigned long		flags		= 0;
	u32 ep_type   = 0;
	u32 ts_type   = 0;
	u32 fifo_size = 0;
	u8  double_fifo = 0;
	int i = 0;

	if(_ep == NULL || desc == NULL){
		DMSG_PANIC("ERR: invalid argment\n");
		return -EINVAL;
	}

	if (_ep->name == ep0name || desc->bDescriptorType != USB_DT_ENDPOINT) {
		DMSG_PANIC("PANIC : _ep->name(%s) == ep0name || desc->bDescriptorType(%d) != USB_DT_ENDPOINT\n",
			_ep->name , desc->bDescriptorType);
		return -EINVAL;
	}

	ep = to_sunxi_udc_ep(_ep);
	if (ep == NULL) {
		DMSG_PANIC("ERR: usbd_ep_enable, ep = NULL\n");
		return -EINVAL;
	}

	if (ep->desc) {
		DMSG_PANIC("ERR: usbd_ep_enable, ep->desc is not NULL, ep%d(%s)\n", ep->num, _ep->name);
		return -EINVAL;
	}

	DMSG_INFO_UDC("ep enable: ep%d(0x%p, %s, %d, %d)\n",
		ep->num, _ep, _ep->name,
		(desc->bEndpointAddress & USB_DIR_IN), _ep->maxpacket);

	dev = ep->dev;
	if (!dev->driver || dev->gadget.speed == USB_SPEED_UNKNOWN) {
		DMSG_PANIC("PANIC : dev->driver = 0x%p ?= NULL  dev->gadget->speed =%d ?= USB_SPEED_UNKNOWN\n",
			dev->driver ,dev->gadget.speed);
		return -ESHUTDOWN;
	}

	max = le16_to_cpu(desc->wMaxPacketSize) & 0x1fff;

	spin_lock_irqsave(&ep->dev->lock, flags);

	_ep->maxpacket = max & 0x7ff;
	ep->desc = desc;
	ep->halted = 0;
	ep->bEndpointAddress = desc->bEndpointAddress;

	//ep_type
	if ((ep->bEndpointAddress) & USB_DIR_IN) { /* tx */
		ep_type = USBC_EP_TYPE_TX;
	} else {	 /* rx */
		ep_type = USBC_EP_TYPE_RX;
	}

	//ts_type
	switch (desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK) {
	case USB_ENDPOINT_XFER_CONTROL:
		ts_type   = USBC_TS_TYPE_CTRL;
		break;
	case USB_ENDPOINT_XFER_BULK:
		ts_type   = USBC_TS_TYPE_BULK;
		break;
	case USB_ENDPOINT_XFER_ISOC:
		ts_type   = USBC_TS_TYPE_ISO;
		break;
	case USB_ENDPOINT_XFER_INT:
		ts_type = USBC_TS_TYPE_INT;
		break;
	default:
		DMSG_PANIC("err: usbd_ep_enable, unkown ep type(%d)\n", (desc->bmAttributes & USB_ENDPOINT_XFERTYPE_MASK));
		goto end;
	}

	//fifo_addr && fifo_size && double fifo
	for (i = 0; i < SW_UDC_ENDPOINTS; i++) {
		if (!strcmp(_ep->name, ep_fifo[i].name)) {
			fifo_addr = ep_fifo[i].fifo_addr;
			fifo_size = ep_fifo[i].fifo_size;
			double_fifo = ep_fifo[i].double_fifo;
			break;
		}
	}

	DMSG_INFO_UDC("ep enable: ep%d(0x%p, %s, %d, %d), fifo(%d, %d, %d)\n",
		ep->num, _ep, _ep->name, (desc->bEndpointAddress & USB_DIR_IN), _ep->maxpacket,
		fifo_addr, fifo_size, double_fifo);

	if (i >= SW_UDC_ENDPOINTS) {
		DMSG_PANIC("err: usbd_ep_enable, config fifo failed\n");
		goto end;
	}

	/* check fifo size */
	if ((_ep->maxpacket & 0x7ff) > fifo_size) {
		DMSG_PANIC("err: usbd_ep_enable, fifo size is too small\n");
		goto end;
	}

	/* check double fifo */
	if (double_fifo) {
		if (((_ep->maxpacket & 0x7ff) * 2) > fifo_size) {
			DMSG_PANIC("err: usbd_ep_enable, it is double fifo, but fifo size is too small\n");
			goto end;
		}

		/* ????FIFO, ????????????? */
		fifo_size = _ep->maxpacket & 0x7ff;
	}

	if (!is_peripheral_active()) {
		DMSG_INFO_UDC("usbd_ep_enable, usb device is not active\n");
		goto end;
	}

	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, ep->num);

	USBC_Dev_ConfigEp_Default(g_sunxi_udc_io.usb_bsp_hdle, ep_type);
	USBC_Dev_FlushFifo(g_sunxi_udc_io.usb_bsp_hdle, ep_type);

	//set max packet ,type, direction, address; reset fifo counters, enable irq
	USBC_Dev_ConfigEp(g_sunxi_udc_io.usb_bsp_hdle, ts_type, ep_type, double_fifo, (_ep->maxpacket & 0x7ff));
	USBC_ConfigFifo(g_sunxi_udc_io.usb_bsp_hdle, ep_type, double_fifo, fifo_size, fifo_addr);
	if(ts_type == USBC_TS_TYPE_ISO){
		USBC_Dev_IsoUpdateEnable(g_sunxi_udc_io.usb_bsp_hdle);
	}
	USBC_INT_EnableEp(g_sunxi_udc_io.usb_bsp_hdle, ep_type, ep->num);

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);

end:
	spin_unlock_irqrestore(&ep->dev->lock, flags);

	sunxi_udc_set_halt(_ep, 0);
	return 0;
}

/*
 * Disable EP
 */
static int sunxi_udc_ep_disable(struct usb_ep *_ep)
{
	struct sunxi_udc_ep *ep = NULL;
	u32 old_ep_index = 0;
	unsigned long flags = 0;

	if (!_ep) {
		DMSG_PANIC("ERR: invalid argment\n");
		return -EINVAL;
	}

	ep = to_sunxi_udc_ep(_ep);
	if (ep == NULL) {
		DMSG_PANIC("ERR: usbd_ep_disable: ep = NULL\n");
		return -EINVAL;
	}

	if (!ep->desc) {
		DMSG_PANIC("ERR: %s not enabled\n", _ep ? ep->ep.name : NULL);
		return -EINVAL;
	}

	DMSG_INFO_UDC("ep disable: ep%d(0x%p, %s, %d, %x)\n",
		ep->num, _ep, _ep->name,
		(ep->bEndpointAddress & USB_DIR_IN), _ep->maxpacket);

	spin_lock_irqsave(&ep->dev->lock, flags);

	DMSG_DBG_UDC("ep_disable: %s\n", _ep->name);

	ep->desc = NULL;
	ep->halted = 1;

	sunxi_udc_nuke (ep->dev, ep, -ESHUTDOWN);

	if (!is_peripheral_active()) {
		DMSG_INFO_UDC("%s_%d: usb device is not active\n", __func__, __LINE__);
		goto end;
	}

	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, ep->num);

	if ((ep->bEndpointAddress) & USB_DIR_IN) { /* tx */
		USBC_Dev_ConfigEp_Default(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
		USBC_INT_DisableEp(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX, ep->num);
	} else { /* rx */
		USBC_Dev_ConfigEp_Default(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
		USBC_INT_DisableEp(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX, ep->num);
	}

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);

end:
	spin_unlock_irqrestore(&ep->dev->lock, flags);

	DMSG_DBG_UDC("%s disabled\n", _ep->name);
	return 0;
}

static struct usb_request * sunxi_udc_alloc_request(struct usb_ep *_ep, gfp_t mem_flags)
{
	struct sunxi_udc_request *req = NULL;

	if (!_ep) {
		DMSG_PANIC("ERR: invalid argment\n");
		return NULL;
	}

	req = kzalloc (sizeof(struct sunxi_udc_request), mem_flags);
	if (!req) {
		DMSG_PANIC("ERR: kzalloc failed\n");
		return NULL;
	}

	memset(req, 0, sizeof(struct sunxi_udc_request));

	req->req.dma = DMA_ADDR_INVALID;

	INIT_LIST_HEAD (&req->queue);

	DMSG_INFO_UDC("alloc request: ep(0x%p, %s, %d), req(0x%p)\n",
		_ep, _ep->name, _ep->maxpacket, req);

	return &req->req;
}

static void sunxi_udc_free_request(struct usb_ep *_ep, struct usb_request *_req)
{
	struct sunxi_udc_request	*req = NULL;

	if (_ep == NULL || _req == NULL) {
		DMSG_PANIC("ERR: invalid argment\n");
		return;
	}

	req = to_sunxi_udc_req(_req);
	if (req == NULL) {
		DMSG_PANIC("ERR: invalid argment\n");
		return;
	}

	DMSG_INFO_UDC("free request: ep(0x%p, %s, %d), req(0x%p)\n",
		_ep, _ep->name, _ep->maxpacket, req);

	kfree(req);
	return;
}

static int sunxi_udc_queue(struct usb_ep *_ep, struct usb_request *_req, gfp_t gfp_flags)
{
	struct sunxi_udc_request *req = NULL;
	struct sunxi_udc_ep *ep = NULL;
	struct sunxi_udc *dev = NULL;
	unsigned long flags = 0;
	u8 old_ep_index = 0;

	if (_ep == NULL || _req == NULL ) {
		DMSG_PANIC("ERR: invalid argment\n");
		return -EINVAL;
	}

	ep = to_sunxi_udc_ep(_ep);
	if ((ep == NULL || (!ep->desc && _ep->name != ep0name))) {
		DMSG_PANIC("ERR: sunxi_udc_queue: inval 2\n");
		return -EINVAL;
	}

	dev = ep->dev;
	if (!dev->driver || dev->gadget.speed == USB_SPEED_UNKNOWN) {
		DMSG_PANIC("ERR : dev->driver=0x%p, dev->gadget.speed=%x\n",
			dev->driver, dev->gadget.speed);
		return -ESHUTDOWN;
	}

	if (!_req->complete || !_req->buf) {
		DMSG_PANIC("ERR: usbd_queue: _req is invalid\n");
		return -EINVAL;
	}

	req = to_sunxi_udc_req(_req);
	if (!req) {
		DMSG_PANIC("ERR: req is NULL\n");
		return -EINVAL;
	}

	spin_lock_irqsave(&ep->dev->lock, flags);
	_req->status = -EINPROGRESS;
	_req->actual = 0;
	spin_unlock_irqrestore(&ep->dev->lock, flags);

	if (is_sunxi_udc_dma_capable(req, ep)) {
		sunxi_udc_map_dma_buffer(req, dev, ep);
	}

	spin_lock_irqsave(&ep->dev->lock, flags);

	list_add_tail(&req->queue, &ep->queue);

	if (!is_peripheral_active()) {
		DMSG_PANIC("warn: peripheral is active\n");
		goto end;
	}

	if(g_queue_debug){
		DMSG_INFO_UDC("q: (0x%p, %d, %d)\n", _req,_req->length, _req->actual);
	}

	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	if (ep->bEndpointAddress) {
		USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, ep->bEndpointAddress & 0x7F);
	} else {
		USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, 0);
	}

	/* if there is only one in the queue, then execute it */
	if (!ep->halted && (&req->queue == ep->queue.next)) {
		if (ep->bEndpointAddress == 0 /* ep0 */) {
			switch (dev->ep0state) {
			case EP0_IN_DATA_PHASE:
				if (!USBC_Dev_IsWriteDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX)
					&& sunxi_udc_write_fifo(ep, req)) {
					dev->ep0state = EP0_IDLE;
					req = NULL;
				}
				break;
			case EP0_OUT_DATA_PHASE:
#if defined (CONFIG_ARCH_SUN8IW8)
				if ((!_req->length)
					|| sunxi_udc_read_fifo(ep, req)) {
#else
				if ((!_req->length)
					|| (USBC_Dev_IsReadDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX)
					&& sunxi_udc_read_fifo(ep, req))) {
#endif
					dev->ep0state = EP0_IDLE;
					req = NULL;
				}
				break;
			default:
				spin_unlock_irqrestore(&ep->dev->lock, flags);
				return -EL2HLT;
			}
		} else if ((ep->bEndpointAddress & USB_DIR_IN) != 0
				&& !USBC_Dev_IsWriteDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX)) {
			if (sunxi_udc_write_fifo(ep, req)) {
				req = NULL;
			}
		} else if ((ep->bEndpointAddress & USB_DIR_IN) == 0
				&& USBC_Dev_IsReadDataReady(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX)) {
			if (sunxi_udc_read_fifo(ep, req)) {
				req = NULL;
			}
		}
	}

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);

end:
	spin_unlock_irqrestore(&ep->dev->lock, flags);
	return 0;
}

/* dequeue JUST ONE request */
static int sunxi_udc_dequeue(struct usb_ep *_ep, struct usb_request *_req)
{
	struct sunxi_udc_ep *ep = NULL;
	struct sunxi_udc *udc = NULL;
	int retval = -EINVAL;
	struct sunxi_udc_request *req = NULL;
	unsigned long flags = 0;

	DMSG_DBG_UDC("(%p,%p)\n", _ep, _req);

	if (!the_controller->driver) {
		DMSG_PANIC("ERR: sunxi_udc_dequeue: driver is null\n");
		return -ESHUTDOWN;
	}

	if (!_ep || !_req) {
		DMSG_PANIC("ERR: sunxi_udc_dequeue: invalid argment\n");
		return retval;
	}

	ep = to_sunxi_udc_ep(_ep);
	if (ep == NULL) {
		DMSG_PANIC("ERR: ep == NULL\n");
		return -EINVAL;
	}

	udc = to_sunxi_udc(ep->gadget);
	if (udc == NULL) {
		DMSG_PANIC("ERR: ep == NULL\n");
		return -EINVAL;
	}

	DMSG_INFO_UDC("dequeue: ep(0x%p, %d), _req(0x%p, %d, %d)\n",
		ep, ep->num,
			_req, _req->length, _req->actual);

	spin_lock_irqsave(&ep->dev->lock, flags);

	list_for_each_entry (req, &ep->queue, queue) {
		if (&req->req == _req) {
			list_del_init (&req->queue);
			_req->status = -ECONNRESET;
			retval = 0;
			break;
		}
	}

	if (retval == 0) {
		DMSG_DBG_UDC("dequeued req %p from %s, len %d buf %p\n",
			req, _ep->name, _req->length, _req->buf);

		sunxi_udc_done(ep, req, -ECONNRESET);
	}

	spin_unlock_irqrestore(&ep->dev->lock, flags);
	return retval;
}

static int sunxi_udc_set_halt(struct usb_ep *_ep, int value)
{
	struct sunxi_udc_ep	*ep = NULL;
	unsigned long		flags = 0;
	u32			idx = 0;
	__u8			old_ep_index = 0;

	if (_ep == NULL) {
		DMSG_PANIC("ERR: invalid argment\n");
		return -EINVAL;
	}

	ep = to_sunxi_udc_ep(_ep);
	if (ep == NULL) {
		DMSG_PANIC("ERR: invalid argment\n");
		return -EINVAL;
	}

	if (!ep->desc && ep->ep.name != ep0name) {
		DMSG_PANIC("ERR: !ep->desc && ep->ep.name != ep0name\n");
		return -EINVAL;
	}

	if (!is_peripheral_active()) {
		DMSG_INFO_UDC("%s_%d: usb device is not active\n", __func__, __LINE__);
		return 0;
	}

	spin_lock_irqsave(&ep->dev->lock, flags);

	idx = ep->bEndpointAddress & 0x7F;

	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, idx);

	if (idx == 0) {
		USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
	} else {
		if ((ep->bEndpointAddress & USB_DIR_IN) != 0) {
			if (value) {
				USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
			} else {
				USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
			}
		} else {
			if (value) {
				USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
			} else {
				USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
			}
		}
	}

	ep->halted = value ? 1 : 0;

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);

	spin_unlock_irqrestore(&ep->dev->lock, flags);
	return 0;
}

static int sunxi_udc_set_halt_ex(struct usb_ep *_ep, int value)
{
	struct sunxi_udc_ep *ep = NULL;
	u32 idx = 0;
	__u8 old_ep_index = 0;

	if (_ep == NULL) {
		DMSG_PANIC("ERR: invalid argment\n");
		return -EINVAL;
	}

	ep = to_sunxi_udc_ep(_ep);
	if (ep == NULL) {
		DMSG_PANIC("ERR: invalid argment\n");
		return -EINVAL;
	}

	if (!ep->desc && ep->ep.name != ep0name) {
		DMSG_PANIC("ERR: !ep->desc && ep->ep.name != ep0name\n");
		return -EINVAL;
	}

	if (!is_peripheral_active()) {
		DMSG_INFO_UDC("%s_%d: usb device is not active\n", __func__, __LINE__);
		return 0;
	}

	idx = ep->bEndpointAddress & 0x7F;

	old_ep_index = USBC_GetActiveEp(g_sunxi_udc_io.usb_bsp_hdle);
	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, idx);

	if (idx == 0) {
		USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_EP0);
	} else {
		if ((ep->bEndpointAddress & USB_DIR_IN) != 0) {
			if (value) {
				USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
			} else {
				USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_TX);
			}
		} else {
			if (value) {
				USBC_Dev_EpSendStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
			} else {
				USBC_Dev_EpClearStall(g_sunxi_udc_io.usb_bsp_hdle, USBC_EP_TYPE_RX);
			}
		}
	}

	ep->halted = value ? 1 : 0;

	USBC_SelectActiveEp(g_sunxi_udc_io.usb_bsp_hdle, old_ep_index);
	return 0;
}

static s32  usbd_start_work(void)
{
	DMSG_INFO_UDC("usbd_start_work\n");

	if (!is_peripheral_active()) {
		DMSG_INFO_UDC("%s_%d: usb device is not active\n", __func__, __LINE__);
		return 0;
	}

	enable_irq_udc(the_controller);
	USBC_Dev_ConectSwitch(g_sunxi_udc_io.usb_bsp_hdle, USBC_DEVICE_SWITCH_ON);
	return 0;
}

static s32  usbd_stop_work(void)
{
	DMSG_INFO_UDC("usbd_stop_work\n");

	if (!is_peripheral_active()) {
		DMSG_INFO_UDC("%s_%d: usb device is not active\n", __func__, __LINE__);
		return 0;
	}

	disable_irq_udc(the_controller);
	USBC_Dev_ConectSwitch(g_sunxi_udc_io.usb_bsp_hdle, USBC_DEVICE_SWITCH_OFF); // default is pulldown
	return 0;
}

static const struct usb_gadget_ops sunxi_udc_ops = {
	/* current versions must always be self-powered */
};

static struct sunxi_udc sunxi_udc = {
	.gadget = {
		.ops		= &sunxi_udc_ops,
		.ep0		= &sunxi_udc.ep[0].ep,
		.name		= gadget_name,
	},

	.ep[0] = {
		.num			= 0,
		.ep = {
			.name		= ep0name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= EP0_FIFO_SIZE,
		},
		.dev			= &sunxi_udc,
	},

	.ep[1] = {
		.num			= 1,
		.ep = {
			.name		= ep1in_bulk_name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= SW_UDC_EP_FIFO_SIZE,
		},
		.dev		        = &sunxi_udc,
		//.fifo_size	        = SW_UDC_EP_FIFO_SIZE,
		.bEndpointAddress   = (USB_DIR_IN | 1),
		.bmAttributes	    = USB_ENDPOINT_XFER_BULK,
	},

	.ep[2] = {
		.num			= 1,
		.ep = {
			.name		= ep1out_bulk_name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= SW_UDC_EP_FIFO_SIZE,
		},
		.dev		        = &sunxi_udc,
		//.fifo_size	        = SW_UDC_EP_FIFO_SIZE,
		.bEndpointAddress   = (USB_DIR_OUT | 1),
		.bmAttributes	    = USB_ENDPOINT_XFER_BULK,
	},

	.ep[3] = {
		.num			= 2,
		.ep = {
			.name		= ep2in_bulk_name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= SW_UDC_EP_FIFO_SIZE,
		},
		.dev		        = &sunxi_udc,
		//.fifo_size	        = SW_UDC_EP_FIFO_SIZE,
		.bEndpointAddress   = (USB_DIR_IN | 2),
		.bmAttributes	    = USB_ENDPOINT_XFER_BULK,
	},

	.ep[4] = {
		.num			= 2,
		.ep = {
			.name		= ep2out_bulk_name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= SW_UDC_EP_FIFO_SIZE,
		},
		.dev		        = &sunxi_udc,
		//.fifo_size	        = SW_UDC_EP_FIFO_SIZE,
		.bEndpointAddress   = (USB_DIR_OUT | 2),
		.bmAttributes	    = USB_ENDPOINT_XFER_BULK,
	},

	.ep[5] = {
		.num			= 3,
		.ep = {
			.name		= ep3_iso_name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= SW_UDC_EP_ISO_FIFO_SIZE,
		},
		.dev		        = &sunxi_udc,
		//.fifo_size	        = SW_UDC_EP_FIFO_SIZE,
		.bEndpointAddress   = 3,
		.bmAttributes	    = USB_ENDPOINT_XFER_ISOC,
	},

	.ep[6] = {
		.num			= 4,
		.ep = {
			.name		= ep4_int_name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= SW_UDC_EP_FIFO_SIZE,
		},
		.dev		        = &sunxi_udc,
		//.fifo_size	        = SW_UDC_EP_FIFO_SIZE,
		.bEndpointAddress   = 4,
		.bmAttributes	    = USB_ENDPOINT_XFER_INT,
	},

#if defined (CONFIG_ARCH_SUN50IW1P1)
	.ep[7] = {
		.num			= 5,
		.ep = {
			.name		= ep5in_bulk_name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= SW_UDC_EP_FIFO_SIZE,
		},
		.dev		        = &sunxi_udc,
		//.fifo_size	        = SW_UDC_EP_FIFO_SIZE,
		.bEndpointAddress   = (USB_DIR_IN | 5),
		.bmAttributes	    = USB_ENDPOINT_XFER_BULK,
	},

	.ep[8] = {
		.num			= 5,
		.ep = {
			.name		= ep5out_bulk_name,
			.ops		= &sunxi_udc_ep_ops,
			.maxpacket	= SW_UDC_EP_FIFO_SIZE,
		},
		.dev		        = &sunxi_udc,
		//.fifo_size	        = SW_UDC_EP_FIFO_SIZE,
		.bEndpointAddress   = (USB_DIR_OUT | 5),
		.bmAttributes	    = USB_ENDPOINT_XFER_BULK,
	},
#endif
};

static void cfg_udc_command(enum sunxi_udc_cmd_e cmd)
{
	struct sunxi_udc *udc = the_controller;

	switch (cmd)
	{
	case SW_UDC_P_ENABLE:
		{
			if (udc->driver) {
				usbd_start_work();
			} else {
				DMSG_INFO_UDC("udc->driver is null, udc is need not start\n");
			}
		}
		break;
	case SW_UDC_P_DISABLE:
		{
			if (udc->driver) {
				usbd_stop_work();
			} else {
				DMSG_INFO_UDC("udc->driver is null, udc is need not stop\n");
			}
		}
		break;
	case SW_UDC_P_RESET :
		DMSG_PANIC("ERR: reset is not support\n");
		break;
	default:
		DMSG_PANIC("ERR: unkown cmd(%d)\n",cmd);
		break;
	}

	return ;
}

/*
 *	probe - binds to the platform device
 */
int sunxi_udc_probe(void)
{
	struct sunxi_udc *udc	= &sunxi_udc;
	int		retval	= 0;

	memset(&g_sunxi_udc_io, 0, sizeof(sunxi_udc_io_t));

	retval = sunxi_udc_io_init(usbd_port_no, &g_sunxi_udc_io);
	if (retval != 0) {
		DMSG_PANIC("ERR: sunxi_udc_io_init failed\n");
		return -1;
	}

	spin_lock_init (&udc->lock);

	is_controller_alive = 1;
	the_controller = udc;

	sunxi_udc_disable(udc);
	sunxi_udc_reinit(udc);

	udc->sunxi_udc_io = &g_sunxi_udc_io;
	udc->usbc_no = usbd_port_no;
	strcpy((char *)udc->driver_name, gadget_name);

	if (is_udc_support_dma()) {
		retval = sunxi_udc_dma_probe(udc);
		if (retval != 0) {
			DMSG_PANIC("ERR: sunxi_udc_dma_probe failef\n");
			retval = -EBUSY;
			goto err;
		}
	}

	return 0;

err:
	if (is_udc_support_dma()) {
		sunxi_udc_dma_remove(udc);
	}

	sunxi_udc_io_exit(usbd_port_no, &g_sunxi_udc_io);
	return retval;
}

int usb_gadget_handle_interrupts()
{
	return sunxi_udc_irq(1, (void *)the_controller);
}
