/*
 * arch/arm/mach-omap2/serial.c
 *
 * OMAP2 serial support.
 *
 * Copyright (C) 2005-2008 Nokia Corporation
 * Author: Paul Mundt <paul.mundt@nokia.com>
 *
 * Major rework for PM support by Kevin Hilman
 *
 * Based off of arch/arm/mach-omap/omap1/serial.c
 *
 * Copyright (C) 2009 Texas Instruments
 * Added OMAP4 support - Santosh Shilimkar <santosh.shilimkar@ti.com
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License. See the file "COPYING" in the main directory of this archive
 * for more details.
 */
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/serial_reg.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/serial_8250.h>
#include <linux/console.h>
#include <linux/gpio.h>

#ifdef CONFIG_SERIAL_OMAP
#include <plat/omap-serial.h>
#include <plat/serial.h>
#endif

#include <plat/common.h>
#include <plat/board.h>
#include <plat/clock.h>
#include <plat/control.h>
#include <plat/dma.h>
#include <plat/omap_hwmod.h>
#include <plat/omap_device.h>

#include "mux.h"
#include "prm.h"
#include "pm.h"
#include "cm.h"
#include "prm-regbits-34xx.h"

#define UART_OMAP_NO_EMPTY_FIFO_READ_IP_REV	0x52

/* #define SERIAL_DEBUG */
#ifdef SERIAL_DEBUG
#define serial_dbg_printk(format, ...)	 printk(format, ## __VA_ARGS__)
#else
#define serial_dbg_printk(format, ...)
#endif

/*
 * NOTE: By default the serial timeout is disabled as it causes lost characters
 * over the serial ports. This means that the UART clocks will stay on until
 * disabled via sysfs. This also causes that any deeper omap sleep states are
 * blocked.
 */

#define MAX_UART_HWMOD_NAME_LEN		16
#define MAX_UART_WORK_Q_NAME_LEN	32

struct omap_uart_state {
	int num;
	struct timer_list timer;
	u32 timeout;

	void __iomem *wk_st;
	void __iomem *wk_en;
	u16 padconf_wake_ev;
	u32 wk_mask;
	u16 padconf;
	u32 dma_enabled;

	u16 rts_padconf;
	int rts_override;
	u32 rts_padvalue;

	struct clk *ick;
	struct clk *fck;
	int clocked;

	int irq;
	int regshift;
	int irqflags;
	void __iomem *membase;
	resource_size_t mapbase;

	struct list_head node;
	struct omap_hwmod *oh;
	struct platform_device *pdev;
	struct notifier_block nb;
	struct work_struct work;
	struct workqueue_struct *omap4_serial_timer_wq;

#if defined(CONFIG_PM)
	int context_valid;

	/* Registers to be saved/restored for OFF-mode */
	u16 dll;
	u16 dlh;
	u16 ier;
	u16 sysc;
	u16 scr;
	u16 wer;
	u16 mcr;
	u16 mdr3;
	u16 dma_thresh;
	u16 lcr;
	u16 dpll_ier_state;
	u16 dpll_lcr_state;
#endif
	unsigned int port_timer_active;
};

static LIST_HEAD(uart_list);
static u8 num_uarts;
static spinlock_t serial_work_lock;
static void omap4_serial_timer_work(struct work_struct *work);

static bool plat_check_bt_active(struct omap_uart_state *uart);
static void omap_uart_disable_wakeup(struct omap_uart_state *uart);
static void omap_uart_smart_idle_enable(struct omap_uart_state *uart,
							int enable);
void __init omap2_set_globals_uart(struct omap_globals *omap2_globals)
{
}

/*
 * Since these idle/enable hooks are used in the idle path itself
 * which has interrupts disabled, use the non-locking versions of
 * the hwmod enable/disable functions.
 */
static int uart_idle_hwmod(struct omap_device *od)
{
	if (!irqs_disabled())
		omap_hwmod_idle(od->hwmods[0]);
	else
		_omap_hwmod_idle(od->hwmods[0]);

	return 0;
}

static int uart_enable_hwmod(struct omap_device *od)
{
	if (!irqs_disabled())
		omap_hwmod_enable(od->hwmods[0]);
	else
		_omap_hwmod_enable(od->hwmods[0]);

	return 0;
}

static struct omap_device_pm_latency omap_uart_latency[] = {
	{
		.deactivate_func = uart_idle_hwmod,
		.activate_func	 = uart_enable_hwmod,
		.flags = OMAP_DEVICE_LATENCY_AUTO_ADJUST,
	},
};

static inline unsigned int __serial_read_reg(struct uart_port *up,
					     int offset)
{
	offset <<= up->regshift;
	return (unsigned int)__raw_readb(up->membase + offset);
}

static inline unsigned int serial_read_reg(struct omap_uart_state *uart,
					   int offset)
{
	unsigned int val;
	struct uart_omap_port *up = platform_get_drvdata(uart->pdev);

	if (up)
		up->port_reg_access_active = 1;
	omap_uart_enable_clock_from_ext(uart->num);

	offset <<= uart->regshift;
	val = (unsigned int)__raw_readb(uart->membase + offset);
	if (up)
		up->port_reg_access_active = 0;
	return val;
}

static inline void __serial_write_reg(struct uart_port *up, int offset,
		int value)
{
	offset <<= up->regshift;
	__raw_writeb(value, up->membase + offset);
}

static inline void serial_write_reg(struct omap_uart_state *uart, int offset,
				    int value)
{
	struct uart_omap_port *up = platform_get_drvdata(uart->pdev);

	if (up)
		up->port_reg_access_active = 1;
	omap_uart_enable_clock_from_ext(uart->num);

	offset <<= uart->regshift;
	__raw_writeb(value, uart->membase + offset);

	if (up)
		up->port_reg_access_active = 0;
}

/*
 * Internal UARTs need to be initialized for the 8250 autoconfig to work
 * properly. Note that the TX watermark initialization may not be needed
 * once the 8250.c watermark handling code is merged.
 */

static inline void __init omap_uart_reset(struct omap_uart_state *p)
{
	serial_write_reg(p, UART_OMAP_MDR1, 0x07);
	serial_write_reg(p, UART_OMAP_SCR, 0x08);
	serial_write_reg(p, UART_OMAP_MDR1, 0x00);
}

static inline void omap_uart_disable_rtspullup(struct omap_uart_state *uart)
{
	if (!uart->rts_padconf || !uart->rts_override)
		return;

	if (cpu_is_omap44xx()) {
		/* FIXME: should this be done atomically? */
		u16 offset = uart->rts_padconf & ~0x3; /* 32-bit align */
		u32 value = omap4_ctrl_pad_readl(offset);
		value = ((uart->rts_padconf & 0x2 ? 0x0000FFFF : 0xFFFF0000)
			& value) | uart->rts_padvalue;
		omap4_ctrl_pad_writel(value, offset);
		uart->rts_override = 0;
	}
}

static inline void omap_uart_enable_rtspullup(struct omap_uart_state *uart)
{
	if (!uart->rts_padconf || uart->rts_override)
		return;

	if (cpu_is_omap44xx()) {
		/* FIXME: should this be done atomically? */
		u16 offset = uart->rts_padconf & ~0x3; /* 32-bit align */
		u32 value = omap4_ctrl_pad_readl(offset);
		if (uart->rts_padconf & 0x2) {
			uart->rts_padvalue = value & 0xFFFF0000;
			value &= 0x011FFFFF;
			value |= 0x011F0000; /* Set the PU Enable */
		} else {
			uart->rts_padvalue = value & 0x0000FFFF;
			value &= 0xFFFF011F;
			value |= 0x0000011F; /* Set the PU Enable */
		}
		omap4_ctrl_pad_writel(value, offset);
		uart->rts_override = 1;
	}
}

#if defined(CONFIG_PM)

static void omap_uart_save_context(struct omap_uart_state *uart)
{
	u16 lcr = 0;

	if (!enable_off_mode)
		return;

	lcr = serial_read_reg(uart, UART_LCR);
	uart->lcr = lcr;
	serial_write_reg(uart, UART_LCR, OMAP_UART_LCR_CONF_MDB);
	uart->dll = serial_read_reg(uart, UART_DLL);
	uart->dlh = serial_read_reg(uart, UART_DLM);
	serial_write_reg(uart, UART_LCR, lcr);
	uart->ier = serial_read_reg(uart, UART_IER);
	uart->sysc = serial_read_reg(uart, UART_OMAP_SYSC);
	uart->scr = serial_read_reg(uart, UART_OMAP_SCR);
	uart->wer = serial_read_reg(uart, UART_OMAP_WER);
	lcr = serial_read_reg(uart, UART_LCR);

	serial_write_reg(uart, UART_LCR, OMAP_UART_LCR_CONF_MDA);
	uart->mcr = serial_read_reg(uart, UART_MCR);
	serial_write_reg(uart, UART_LCR, lcr);

	/* HACK to reset UART module if DMA is enabled
	 * For some reason if DMA is enabled the module is
	 * stuck in transition state.
	 */
	if (uart->dma_enabled && cpu_is_omap44xx()) {
		uart->mdr3 = serial_read_reg(uart, UART_MDR3);
		uart->dma_thresh = serial_read_reg(uart, UART_TX_DMA_THRESHOLD);
		serial_write_reg(uart, UART_OMAP_SYSC, 0x2);
	}
	uart->context_valid = 1;
}

static void omap_uart_restore_context(struct omap_uart_state *uart)
{
	u16 efr = 0;

	if (!enable_off_mode)
		return;

	if (!uart->context_valid)
		return;

	uart->context_valid = 0;

	serial_write_reg(uart, UART_OMAP_MDR1, 0x7);
	/* Config B mode */
	serial_write_reg(uart, UART_LCR, OMAP_UART_LCR_CONF_MDB);
	efr = serial_read_reg(uart, UART_EFR);
	serial_write_reg(uart, UART_EFR, UART_EFR_ECB);
	/* Operational mode */
	serial_write_reg(uart, UART_LCR, OMAP_UART_LCR_CONF_MOPER);
	serial_write_reg(uart, UART_IER, 0x0);
	/* Config B mode */
	serial_write_reg(uart, UART_LCR, OMAP_UART_LCR_CONF_MDB);
	serial_write_reg(uart, UART_DLL, uart->dll);
	serial_write_reg(uart, UART_DLM, uart->dlh);
	/* Operational mode */
	serial_write_reg(uart, UART_LCR, OMAP_UART_LCR_CONF_MOPER);
	serial_write_reg(uart, UART_IER, uart->ier);
	/* Enable FiFo and Trig Threshold */
	if (uart->dma_enabled)
		serial_write_reg(uart, UART_FCR, 0x59);
	else
		serial_write_reg(uart, UART_FCR, 0x51);

	serial_write_reg(uart, UART_LCR, OMAP_UART_LCR_CONF_MDA);
	serial_write_reg(uart, UART_MCR, uart->mcr);
	/* Config B mode */
	serial_write_reg(uart, UART_LCR, OMAP_UART_LCR_CONF_MDB);
	serial_write_reg(uart, UART_EFR, efr);
	serial_write_reg(uart, UART_LCR, uart->lcr);
	serial_write_reg(uart, UART_OMAP_SCR, uart->scr);
	serial_write_reg(uart, UART_OMAP_WER, uart->wer);
	serial_write_reg(uart, UART_OMAP_SYSC, uart->sysc);

	if (uart->dma_enabled && cpu_is_omap44xx()) {
		serial_write_reg(uart, UART_TX_DMA_THRESHOLD, uart->dma_thresh);
		serial_write_reg(uart, UART_MDR3, uart->mdr3);
	}

	serial_write_reg(uart, UART_OMAP_MDR1, 0x00); /* UART 16x mode */
}
#else
static inline void omap_uart_save_context(struct omap_uart_state *uart) {}
static inline void omap_uart_restore_context(struct omap_uart_state *uart) {}
#endif /* CONFIG_PM && CONFIG_ARCH_OMAP3 */

static inline void omap_uart_enable_clocks(struct omap_uart_state *uart)
{
	struct platform_device *pdev = uart->pdev;
	struct omap_device *od = NULL;

	od = container_of(pdev, struct omap_device, pdev);

	if (uart->clocked)
		return;

#ifdef CONFIG_PM_RUNTIME
	if (od && (od->_state == OMAP_DEVICE_STATE_ENABLED))
		return;

	omap_device_enable(uart->pdev);
#endif
	omap_uart_restore_context(uart);
	omap_uart_smart_idle_enable(uart, 0);
	omap_uart_disable_wakeup(uart);
	omap_uart_disable_rtspullup(uart);
	uart->clocked = 1;
	omap_uart_start_inactivity_timer(uart->num);
	serial_dbg_printk(KERN_INFO "%s(): UART%u enabled.\n",
		__func__, uart->num);
}

#ifdef CONFIG_PM
static void omap_uart_enable_wakeup(struct omap_uart_state *uart);
static void omap_uart_disable_wakeup(struct omap_uart_state *uart);

void omap_uart_stop_inactivity_timer(unsigned int uart_num)
{
	struct omap_uart_state *uart;

	list_for_each_entry(uart, &uart_list, node) {
		if (uart_num == uart->num) {
			if (uart->port_timer_active == 1) {
				del_timer_sync(&uart->timer);
				uart->port_timer_active = 0;
			}
			break;
		}
	}
	return;
}

static inline void omap_uart_disable_clocks(struct omap_uart_state *uart)
{
	struct platform_device *pdev = uart->pdev;
	struct omap_device *od = NULL;

	od = container_of(pdev, struct omap_device, pdev);

	if (!uart->clocked)
		return;

	serial_dbg_printk(KERN_INFO "%s(): disabling UART%u...\n",
		__func__, uart->num);
	omap_uart_stop_inactivity_timer(uart->num);
	if (device_may_wakeup(&uart->pdev->dev))
		omap_uart_enable_wakeup(uart);
	else
		omap_uart_disable_wakeup(uart);
	omap_uart_smart_idle_enable(uart, 1);
	omap_uart_save_context(uart);
	uart->clocked = 0;
#ifdef CONFIG_PM_RUNTIME
	if (od && (od->_state != OMAP_DEVICE_STATE_ENABLED))
		return;

	omap_device_idle(uart->pdev);
#endif
}

static irqreturn_t omap_uart_gpio_irq(int irq, void *args)
{
	struct omap_uart_state *uart = (struct omap_uart_state *)args;

	omap_uart_enable_clocks(uart);

	return IRQ_HANDLED;
}

static void omap_uart_enable_wakeup(struct omap_uart_state *uart)
{
	struct omap_uart_port_info *up = uart->pdev->dev.platform_data;
	u16 offset = 0;
	u32 mask = 0;
	u32 v_32b;
	u16 v_16b;

	/* Set wake-enable bit */
	if (uart->wk_en && uart->wk_mask) {
		v_32b = __raw_readl(uart->wk_en);
		v_32b |= uart->wk_mask;
		__raw_writel(v_32b, uart->wk_en);
	}

	/* Ensure IOPAD wake-enables are set */
	if (cpu_is_omap34xx() && uart->padconf) {
		v_16b = omap_ctrl_readw(uart->padconf);
		v_16b |= OMAP3_PADCONF_WAKEUPENABLE0;
		omap_ctrl_writew(v_16b, uart->padconf);
	}

	if (cpu_is_omap44xx() && uart->padconf) {
		offset = uart->padconf & ~0x3; /* 32-bit align */
		mask = 0;
		mask = uart->padconf & 0x2 ?
			OMAP44XX_PADCONF_WAKEUPENABLE1 :
			OMAP44XX_PADCONF_WAKEUPENABLE0;

		/* If set to -1 follow the normal wake-up setting,
		 * else chnage the mode to GPIO wake-up if it has
		 * a valid GPIO number.
		 */
		mask = (up && (up->omap_uart_gpio_mux_mode == -1)) ?
			mask : (mask | OMAP_MUX_MODE3);

		v_32b = omap4_ctrl_pad_readl(offset);
		v_32b |= mask;
		omap4_ctrl_pad_writel(v_32b, offset);
	}
}

static void omap_uart_disable_wakeup(struct omap_uart_state *uart)
{
	struct omap_uart_port_info *up = uart->pdev->dev.platform_data;
	u16 offset;
	u32 mask = 0;
	u32 v_32b;
	u16 v_16b;

	/* Clear wake-enable bit */
	if (uart->wk_en && uart->wk_mask) {
		v_32b = __raw_readl(uart->wk_en);
		v_32b &= ~uart->wk_mask;
		__raw_writel(v_32b, uart->wk_en);
	}

	/* Ensure IOPAD wake-enables are cleared */
	if (cpu_is_omap34xx() && uart->padconf) {
		v_16b = omap_ctrl_readw(uart->padconf);
		v_16b &= ~OMAP3_PADCONF_WAKEUPENABLE0;
		omap_ctrl_writew(v_16b, uart->padconf);
	}

	/* Ensure IOPAD wake-enables are cleared */
	if (cpu_is_omap44xx() && uart->padconf) {
		offset = uart->padconf & ~0x3; /* 32-bit align */
		mask = 0;
		mask = uart->padconf & 0x2 ?
			OMAP44XX_PADCONF_WAKEUPENABLE1 :
			OMAP44XX_PADCONF_WAKEUPENABLE0;

		/* If set to -1 follow the normal wake-up setting,
		 * else change the mode to GPIO wake-up if it has
		 * a valid GPIO number.
		 */
		mask = (up && (up->omap_uart_gpio_mux_mode == -1)) ?
			mask : (mask | OMAP_MUX_MODE3);

		v_32b = omap4_ctrl_pad_readl(offset);
		v_32b &= ~mask;
		omap4_ctrl_pad_writel(v_32b, offset);
	}
}

static void omap_uart_smart_idle_enable(struct omap_uart_state *uart,
					       int enable)
{
	u8 idlemode;

	/**
	 * Errata 2.15: [UART]:Cannot Acknowledge Idle Requests
	 * in Smartidle Mode When Configured for DMA Operations.
	 */
	if (uart->dma_enabled)
		idlemode = HWMOD_IDLEMODE_FORCE;
	else
		idlemode = HWMOD_IDLEMODE_SMART_WKUP;

	omap_hwmod_set_slave_idlemode(uart->oh, idlemode);
}

static void omap_uart_idle_timer(unsigned long data)
{
	struct omap_uart_state *uart = (struct omap_uart_state *)data;

	if (uart->omap4_serial_timer_wq != NULL) {
		INIT_WORK(&uart->work, omap4_serial_timer_work);
		queue_work(uart->omap4_serial_timer_wq, &uart->work);
	}

	return;
}

static void omap4_serial_timer_work(struct work_struct *work)
{
	struct omap_uart_state *uart =
			container_of(work, struct omap_uart_state, work);
	struct uart_omap_port *up = platform_get_drvdata(uart->pdev);
	/* As the Callback is done, now timer can be modified */
	uart->port_timer_active = 0;

	spin_lock_irq(&serial_work_lock);

	if (up == NULL) {
		printk(KERN_ERR "%s(): up == NULL?!\n", __func__);
		goto omap_spin_lock_unlock_work;
	}

	if (unlikely(in_atomic_preempt_off())) {
		omap_uart_start_inactivity_timer(uart->num);
		goto omap_spin_lock_unlock_work;
	}

	/* Check if BT Use case is active */
	if (plat_check_bt_active(uart)) {
		omap_uart_start_inactivity_timer(uart->num);
		goto omap_spin_lock_unlock_work;
	}


#ifdef CONFIG_SERIAL_OMAP
	/* check if the uart port is active
	 * if port is active then dont allow
	 * sleep.
	 */
	if (omap_uart_active(uart->num, uart->timeout)) {
		omap_uart_start_inactivity_timer(uart->num);
		goto omap_spin_lock_unlock_work;
	}
#endif

	if ((up->port_tx_active == 0) && (up->port_rx_active == 0) &&
			(up->port_reg_access_active == 0)) {
		serial_dbg_printk(KERN_INFO "%s(): timer expired, "
			"disabling UART%u.\n", __func__, uart->num);
		omap_uart_disable_clocks(uart);
		goto omap_spin_lock_unlock_work;
	} else {
		omap_uart_start_inactivity_timer(uart->num);
	}

omap_spin_lock_unlock_work:
	spin_unlock_irq(&serial_work_lock);
}

static bool omap_uart_is_wakeup_src(struct omap_uart_state *uart)
{
	if (cpu_is_omap34xx() && uart->padconf) {
		u16 p = omap_ctrl_readw(uart->padconf);

		if (p & OMAP3_PADCONF_WAKEUPEVENT0)
			return true;
	}

	if (cpu_is_omap44xx() && uart->padconf) {
		u16 offset = uart->padconf & ~0x3; /* 32-bit align */
		u32 event_mask = uart->padconf & 0x2
			? OMAP44XX_PADCONF_WAKEUPEVENT1
			: OMAP44XX_PADCONF_WAKEUPEVENT0;
		u32 p = omap4_ctrl_pad_readl(offset);
		if ((p & event_mask) && (uart->padconf_wake_ev != 0))
			if ((omap4_ctrl_pad_readl(uart->padconf_wake_ev)
						& (uart->wk_mask)))
				return true;
	}

	return false;
}

void omap_uart_resume_idle(void)
{
	struct omap_uart_state *uart;
	struct omap_uart_port_info *pdata;

	list_for_each_entry(uart, &uart_list, node) {
		pdata = uart->pdev->dev.platform_data;

		/* Check for IO pad wakeup */
		if (omap_uart_is_wakeup_src(uart)) {
			omap_uart_enable_clocks(uart);
			if (pdata && pdata->plat_hold_wakelock)
				pdata->uart_wakeup_event = 1;
			if (cpu_is_omap44xx())
				omap_uart_update_jiffies(uart->num);
			serial_dbg_printk(KERN_INFO "%s(): UART%u "
				"IO pad wakeup.\n", __func__, uart->num);
		}

		/* Check for normal UART wakeup */
		if (uart->wk_st && uart->wk_mask)
			if (__raw_readl(uart->wk_st) & uart->wk_mask)
				omap_uart_enable_clocks(uart);
	}
}

int omap_uart_is_enabled(int uart_num)
{
	struct omap_uart_state *uart;

	list_for_each_entry(uart, &uart_list, node) {
		if (uart_num == uart->num)
			return uart->clocked;
	}

	return -EINVAL;
}
EXPORT_SYMBOL(omap_uart_is_enabled);

void omap_uart_prepare_suspend(void)
{
	struct omap_uart_state *uart;

	serial_dbg_printk(KERN_INFO "%s(): suspending UARTs...\n", __func__);
	list_for_each_entry(uart, &uart_list, node) {
		omap_uart_disable_clocks(uart);
	}
}

void omap_uart_resume(int uart_num)
{
	struct omap_uart_state *uart;

	list_for_each_entry(uart, &uart_list, node) {
		if (uart_num == uart->num) {
			omap_uart_enable_clocks(uart);
			break;
		}
	}
	serial_dbg_printk(KERN_INFO "%s(): UARTs resumed.\n", __func__);
}
EXPORT_SYMBOL(omap_uart_resume);

static bool plat_check_bt_active(struct omap_uart_state *uart)
{
	struct omap_uart_port_info *pdata;
	pdata = uart->pdev->dev.platform_data;

	if (pdata && pdata->plat_omap_bt_active)
		return pdata->plat_omap_bt_active();

	return false;
}

int omap_uart_can_sleep(void)
{
	struct omap_uart_state *uart;

	list_for_each_entry(uart, &uart_list, node) {
		if (uart->clocked)
			return 0;
	}

	return 1;
}

void omap_uart_start_inactivity_timer(unsigned int uart_num)
{
	struct omap_uart_state *uart;

	list_for_each_entry(uart, &uart_list, node) {
		if (uart_num == uart->num) {
			if ((uart->timeout) && (uart->port_timer_active == 0)) {
				mod_timer(&uart->timer,
					jiffies + uart->timeout);
				uart->port_timer_active = 1;
			}
			break;
		}
	}
	return;
}

/**
 * omap_uart_interrupt()
 *
 * This handler is used only to detect that *any* UART interrupt has
 * occurred.  It does _nothing_ to handle the interrupt.  Rather,
 * any UART interrupt will trigger the inactivity timer so the
 * UART will not idle or sleep for its timeout period.
 *
 **/
/* static int first_interrupt; */
static irqreturn_t omap_uart_interrupt(int irq, void *dev_id)
{
	struct omap_uart_state *uart = dev_id;

	omap_uart_enable_clocks(uart);

	return IRQ_NONE;
}

/*
 * This function enabled clock. This is exported function
 * hence call be called by other module to enable the UART
 * clocks.
 */
void omap_uart_enable_clock_from_ext(int uart_num)
{
	struct omap_uart_state *uart;

	list_for_each_entry(uart, &uart_list, node) {
		if (uart_num == uart->num) {
			omap_uart_enable_clocks(uart);
			break;
		}
	}
	return;
}
EXPORT_SYMBOL(omap_uart_enable_clock_from_ext);

/*
 * This function disable clock. This is exported function
 * hence call be called by other module to enable the UART
 * clocks.
 */
void omap_uart_disable_clock_from_ext(int uart_num)
{
	struct omap_uart_state *uart;

	list_for_each_entry(uart, &uart_list, node) {
		if (uart_num == uart->num) {
			omap_uart_disable_clocks(uart);
			break;
		}
	}
	return;
}
EXPORT_SYMBOL(omap_uart_disable_clock_from_ext);

static int omap_uart_recalibrate_baud_cb(struct notifier_block *nb,
			unsigned long status, void *data)
{
	struct uart_omap_port *up = NULL;
	struct omap_uart_state *uart = NULL;
	struct clk_notifier_data *cnd = (struct clk_notifier_data *)data;
	unsigned int baud_quot;
	unsigned int divisor;
	u16 lcr = 0;

	uart = container_of(nb, struct omap_uart_state, nb);

	if (status == CLK_ABORT_RATE_CHANGE)
		return 0;

	if (status == CLK_PRE_RATE_CHANGE) {
		list_for_each_entry(uart, &uart_list, node) {
			up = platform_get_drvdata(uart->pdev);
			/* If Console, Stop from here, till the Frequencies
			 * Change.
			 */
			if (omap_is_console_port(&up->port)) {
				up->port.cons->flags &= ~CON_ENABLED;
				up->try_locked = spin_trylock(&up->port.lock);
			}

			/* If the device uses the RTS based controlling,
			 * Pull Up the signal to stop transaction. As the
			 * Clocks are not disabled. It even if the data
			 * comes in it should be able to sample.
			 */
			omap_uart_enable_rtspullup(uart);

			/* This delay is based on the observation with
			 * WL1283 and OMAP 4 Blaze, after Pulling the RTS
			 * high, it takes almost 8us for the transactions
			 * to stop plus another 2 us buffer. This would
			 * allow the data to come in before the clocks are
			 * changed.
			 */
			udelay(10);

			/*
			 * All the Software requirements taken cares, Now
			 * Block the UART IP.
			 * Disable the Interrupts ( Since the RTS disabled,
			 * there shouldnt be no RX ).
			 * Disable the UART Mode  ( TX case is forced stop )
			 */
			uart->dpll_lcr_state = serial_read_reg(uart, UART_LCR);
			uart->dpll_ier_state = serial_read_reg(uart, UART_IER);
			/* Hence Forth no interrupts till DPLL is changed */
			serial_write_reg(uart, UART_IER, 0);

			/* Disable the UART Module, Enabled after DPLL Exit */
			serial_write_reg(uart, UART_OMAP_MDR1, 0x7);
			/* Do the Required thing for the Devices in The
			 * Pre State
			 */
		}
		return 0;
	}

	/* These would take care of the POst Rate Change requirements */
	list_for_each_entry(uart, &uart_list, node) {
		up = platform_get_drvdata(uart->pdev);

		if (up == NULL)
			continue;

		omap_uart_enable_clock_from_ext(uart->num);

		up->port_reg_access_active = 1;
		lcr = serial_read_reg(uart, UART_LCR);
		/* these are hard coded here since the clock
		 * framework is not return the correct value.
		 */
		if (cnd->new_rate == 3072000) /* Chnaged */
			up->port.uartclk = 24576000;
		else if (cnd->new_rate == 24000000) /* Original */
			up->port.uartclk = 49152000;

		/* if zero mean the driver has not been opened. */
		if (up->baud_rate != 0) {
			if (up->baud_rate > OMAP_MODE13X_SPEED
					&& up->baud_rate != 3000000)
				divisor = 13;
			else
				divisor = 16;

			baud_quot = up->port.uartclk/(up->baud_rate * divisor);

			serial_write_reg(uart, UART_LCR,
					OMAP_UART_LCR_CONF_MDB);
			serial_write_reg(uart, UART_DLL, baud_quot & 0xff);
			serial_write_reg(uart, UART_DLM, baud_quot >> 8);
			serial_write_reg(uart, UART_LCR, uart->dpll_lcr_state);
			serial_write_reg(uart, UART_IER, uart->dpll_ier_state);
			/* Based on the Baud change the UART 16x or 13x mode
			 * Restore the UART mode which is disabled in Pre
			 * DPLL state.
			 */
			if (up->baud_rate > OMAP_MODE13X_SPEED &&
				up->baud_rate != 3000000)
				serial_write_reg(uart, UART_OMAP_MDR1,
						OMAP_MDR1_MODE13X);
			else
				serial_write_reg(uart, UART_OMAP_MDR1,
						OMAP_MDR1_MODE16X);
#if 0
			dev_dbg(uart->pdev->dev, "Per Functional Clock Changed"
					" %u Hz Change baud DLL %d DLM %d\n",
					clk_get_rate(func_48m_fclk),
					baud_quot & 0xff, baud_quot >> 8);
#endif
		}

		if (status == CLK_POST_RATE_CHANGE) {
			/* Do the Required thing for the Devices in Post
			 * State
			 */
			if (omap_is_console_port(&up->port)) {
				up->port.cons->flags |= CON_ENABLED;
				if (up->try_locked)
					spin_unlock(&up->port.lock);
			}

			omap_uart_disable_rtspullup(uart);
		}
		up->port_reg_access_active = 0;
		omap_uart_start_inactivity_timer(uart->num);
	}
	return 0;
}

static void omap_uart_idle_init(struct omap_uart_state *uart)
{
	int ret;

	if (cpu_is_omap34xx()) {
		u32 mod = (uart->num == 2) ? OMAP3430_PER_MOD : CORE_MOD;
		u32 wk_mask = 0;
		u32 padconf = 0;

		uart->wk_en = OMAP34XX_PRM_REGADDR(mod, PM_WKEN1);
		uart->wk_st = OMAP34XX_PRM_REGADDR(mod, PM_WKST1);
		switch (uart->num) {
		case 0:
			wk_mask = OMAP3430_ST_UART1_MASK;
			padconf = 0x182;
			break;
		case 1:
			wk_mask = OMAP3430_ST_UART2_MASK;
			padconf = 0x17a;
			break;
		case 2:
			wk_mask = OMAP3430_ST_UART3_MASK;
			padconf = 0x19e;
			break;
		}
		uart->wk_mask = wk_mask;
		uart->padconf = padconf;
	} else if (cpu_is_omap24xx()) {
		u32 wk_mask = 0;

		if (cpu_is_omap2430()) {
			uart->wk_en = OMAP2430_PRM_REGADDR(CORE_MOD, PM_WKEN1);
			uart->wk_st = OMAP2430_PRM_REGADDR(CORE_MOD, PM_WKST1);
		} else if (cpu_is_omap2420()) {
			uart->wk_en = OMAP2420_PRM_REGADDR(CORE_MOD, PM_WKEN1);
			uart->wk_st = OMAP2420_PRM_REGADDR(CORE_MOD, PM_WKST1);
		}
		switch (uart->num) {
		case 0:
			wk_mask = OMAP24XX_ST_UART1_MASK;
			break;
		case 1:
			wk_mask = OMAP24XX_ST_UART2_MASK;
			break;
		case 2:
			wk_mask = OMAP24XX_ST_UART3_MASK;
			break;
		}
		uart->wk_mask = wk_mask;

	} else {
		uart->wk_en = 0;
		uart->wk_st = 0;
	}

	uart->irqflags |= IRQF_SHARED;
	ret = request_threaded_irq(uart->irq, NULL, omap_uart_interrupt,
				   IRQF_SHARED, "serial idle", (void *)uart);
	WARN_ON(ret);
}

void omap_uart_enable_irqs(int enable)
{
	int ret;
	struct omap_uart_state *uart;

	list_for_each_entry(uart, &uart_list, node) {
		if (enable)
			ret = request_threaded_irq(uart->irq, NULL,
						   omap_uart_interrupt,
						   IRQF_SHARED,
						   "serial idle",
						   (void *)uart);
		else
			free_irq(uart->irq, (void *)uart);
	}
}

static ssize_t sleep_timeout_show(struct device *dev,
				  struct device_attribute *attr,
				  char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_device *odev = to_omap_device(pdev);
	struct omap_uart_state *uart = odev->hwmods[0]->dev_attr;

	return sprintf(buf, "%u\n", jiffies_to_msecs(uart->timeout));
}

static ssize_t sleep_timeout_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t n)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct omap_device *odev = to_omap_device(pdev);
	struct omap_uart_state *uart = odev->hwmods[0]->dev_attr;
	unsigned int value;

	if (sscanf(buf, "%u", &value) != 1) {
		dev_err(dev, "sleep_timeout_store: Invalid value\n");
		return -EINVAL;
	}

	uart->timeout = msecs_to_jiffies(value);
	/* A zero value means disable timeout feature */
	omap_uart_stop_inactivity_timer(uart->num);
	/*
	 * Enable UART and restart timer with new timeout
	 * (timer not restarted if timeout == 0)
	 */
	omap_uart_enable_clocks(uart);

	return n;
}

DEVICE_ATTR(sleep_timeout, 0644, sleep_timeout_show, sleep_timeout_store);
#define DEV_CREATE_FILE(dev, attr) WARN_ON(device_create_file(dev, attr))
#else
static inline void omap_uart_idle_init(struct omap_uart_state *uart) {}
static inline void omap_uart_disable_clocks(struct omap_uart_state *uart) {}
#define DEV_CREATE_FILE(dev, attr)
#endif /* CONFIG_PM */

#ifndef CONFIG_SERIAL_OMAP
/*
 * Override the default 8250 read handler: mem_serial_in()
 * Empty RX fifo read causes an abort on omap3630 and omap4
 * This function makes sure that an empty rx fifo is not read on these silicons
 * (OMAP1/2/3430 are not affected)
 */
static unsigned int serial_in_override(struct uart_port *up, int offset)
{
	if (UART_RX == offset) {
		unsigned int lsr;
		lsr = __serial_read_reg(up, UART_LSR);
		if (!(lsr & UART_LSR_DR))
			return -EPERM;
	}

	return __serial_read_reg(up, offset);
}

static void serial_out_override(struct uart_port *up, int offset, int value)
{
	unsigned int status, tmout = 10000;

	status = __serial_read_reg(up, UART_LSR);
	while (!(status & UART_LSR_THRE)) {
		/* Wait up to 10ms for the character(s) to be sent. */
		if (--tmout == 0)
			break;
		udelay(1);
		status = __serial_read_reg(up, UART_LSR);
	}
	__serial_write_reg(up, offset, value);
}
#endif

void __init omap_serial_early_init(void)
{
	do {
		char oh_name[MAX_UART_HWMOD_NAME_LEN];
		struct omap_hwmod *oh;
		struct omap_uart_state *uart;

		snprintf(oh_name, MAX_UART_HWMOD_NAME_LEN,
			 "uart%d", num_uarts + 1);
		oh = omap_hwmod_lookup(oh_name);
		if (!oh)
			break;

		uart = kzalloc(sizeof(struct omap_uart_state), GFP_KERNEL);
		if (WARN_ON(!uart))
			return;

		uart->oh = oh;
		uart->num = num_uarts++;
		list_add_tail(&uart->node, &uart_list);

		/*
		 * During UART early init, device need to be probed
		 * to determine SoC specific init before omap_device
		 * is ready.  Therefore, don't allow idle here
		 */
		uart->oh->flags |= HWMOD_INIT_NO_IDLE;
	} while (1);
}

/**
 * omap_serial_init_port() - initialize single serial port
 * @port: serial port number (0-3)
 *
 * This function initialies serial driver for given @port only.
 * Platforms can call this function instead of omap_serial_init()
 * if they don't plan to use all available UARTs as serial ports.
 *
 * Don't mix calls to omap_serial_init_port() and omap_serial_init(),
 * use only one of the two.
 */
void __init omap_serial_init_port(int port,
			struct omap_uart_port_info *platform_data)
{
	struct omap_uart_state *uart;
	struct omap_hwmod *oh = NULL;
	struct omap_device *od = NULL;
	void *pdata = NULL;
	u32 pdata_size = 0;
	char *name;
	struct clk *func_48m_fclk;
	static unsigned int dll_cb_init;

#ifndef CONFIG_SERIAL_OMAP
	struct plat_serial8250_port ports[2] = {
		{},
		{.flags = 0},
	};
	struct plat_serial8250_port *p = &ports[0];
#else
	char gpio_name[MAX_UART_HWMOD_NAME_LEN];
	char work_queue_name[MAX_UART_WORK_Q_NAME_LEN];
	unsigned int gpio_irq = 0;
	struct omap_uart_port_info omap_up;
#endif

	if (WARN_ON(port < 0))
		return;
	if (WARN_ON(port >= num_uarts))
		return;

	list_for_each_entry(uart, &uart_list, node)
		if (port == uart->num)
			break;

	oh = uart->oh;
	if (WARN_ON(!oh))
		return;

	uart->dma_enabled = 0;
	uart->irq = oh->mpu_irqs[0].irq;
	uart->regshift = 2;
	uart->mapbase = oh->slaves[0]->addr->pa_start;
	uart->membase = ioremap(uart->mapbase, SZ_8K);

	oh->dev_attr = uart;
#ifndef CONFIG_SERIAL_OMAP
	name = "serial8250";

	/*
	 * !! 8250 driver does not use standard IORESOURCE* It
	 * has it's own custom pdata that can be taken from
	 * the hwmod resource data.  But, this needs to be
	 * done after the build.
	 *
	 * ?? does it have to be done before the register ??
	 * YES, because platform_device_data_add() copies
	 * pdata, it does not use a pointer.
	 */
	p->flags = UPF_BOOT_AUTOCONF;
	p->iotype = UPIO_MEM;
	p->regshift = uart->regshift;
	p->uartclk = OMAP24XX_BASE_BAUD * 16;
	p->irq = uart->irq;
	p->mapbase = uart->mapbase;
	p->membase = uart->membase;
	p->irqflags = IRQF_SHARED;
	p->private_data = uart;

	/*
	 * omap44xx: Never read empty UART fifo
	 * omap3xxx: Never read empty UART fifo on UARTs
	 * with IP rev >=0x52
	 */
	if (cpu_is_omap44xx()) {
		p->serial_in = serial_in_override;
		p->serial_out = serial_out_override;
	} else if ((serial_read_reg(uart, UART_OMAP_MVER) & 0xFF)
			>= UART_OMAP_NO_EMPTY_FIFO_READ_IP_REV) {
		p->serial_in = serial_in_override;
		p->serial_out = serial_out_override;
	}

	pdata = &ports[0];
	pdata_size = 2 * sizeof(struct plat_serial8250_port);
#else
	name = DRIVER_NAME;
	uart->dma_enabled = platform_data->use_dma;
	omap_up.use_dma = platform_data->use_dma;
	omap_up.dma_rx_buf_size = platform_data->dma_rx_buf_size;
	omap_up.dma_rx_poll_rate = platform_data->dma_rx_poll_rate;
	omap_up.dma_rx_timeout = platform_data->dma_rx_timeout;

	if (omap_up.use_dma) {
		if (cpu_is_omap44xx() && (omap_rev() > OMAP4430_REV_ES1_0))
			omap_up.omap4_tx_threshold = true;
	}

	omap_up.uartclk = OMAP24XX_BASE_BAUD * 16;
	omap_up.mapbase = uart->mapbase;
	omap_up.membase = uart->membase;
	omap_up.irqflags = IRQF_SHARED;
	omap_up.flags = UPF_BOOT_AUTOCONF | UPF_SHARE_IRQ;
	omap_up.idle_timeout = platform_data->idle_timeout;
	omap_up.plat_hold_wakelock = platform_data->plat_hold_wakelock;
	omap_up.plat_omap_bt_active = platform_data->plat_omap_bt_active;
	omap_up.omap_uart_gpio_mux_mode =
			platform_data->omap_uart_gpio_mux_mode;

	uart->rts_padconf = platform_data->rts_padconf;
	uart->rts_override = platform_data->rts_override;

	uart->padconf = platform_data->padconf;
	uart->padconf_wake_ev = platform_data->padconf_wake_ev;
	uart->wk_mask = platform_data->wk_mask;

	omap_up.cts_padconf = platform_data->cts_padconf;
	omap_up.cts_padvalue = 0;

	pdata = &omap_up;
	pdata_size = sizeof(struct omap_uart_port_info);
#endif

	od = omap_device_build(name, uart->num, oh, pdata, pdata_size,
			       omap_uart_latency,
			       ARRAY_SIZE(omap_uart_latency), false);
	WARN(IS_ERR(od), "Could not build omap_device for %s: %s.\n",
	     name, oh->name);

	uart->pdev = &od->pdev;

	if (dll_cb_init == 0) {
		uart->nb.notifier_call = omap_uart_recalibrate_baud_cb;
		uart->nb.next = NULL;
		func_48m_fclk = clk_get(NULL, "func_48m_fclk");
		clk_notifier_register(func_48m_fclk, &uart->nb);
		dll_cb_init = 1;
	}

	/* Initialize the Work Queue for the Serial Inactivity Timer Work */
	snprintf(work_queue_name, MAX_UART_WORK_Q_NAME_LEN,
			"omap_uart_wq_%d", uart->num);
	uart->omap4_serial_timer_wq =
			create_singlethread_workqueue(work_queue_name);

#ifdef CONFIG_PM_RUNTIME
	/*
	 * Because of early UART probing, UART did not get idled
	 * on init.  Now that omap_device is ready, ensure full idle
	 * before doing omap_device_enable().
	 */
	omap_hwmod_idle(uart->oh);
#endif

	omap_uart_enable_clocks(uart);
	setup_timer(&uart->timer, omap_uart_idle_timer,
		(unsigned long) uart);
	omap_uart_idle_init(uart);
	omap_uart_reset(uart);
	omap_uart_disable_clocks(uart);

	/*
	 * Need to block sleep long enough for interrupt driven
	 * driver to start.  Console driver is in polling mode
	 * so device needs to be kept enabled while polling driver
	 * is in use.
	 */
	uart->timeout = msecs_to_jiffies(platform_data->idle_timeout);
	uart->port_timer_active = 0;
	omap_uart_disable_clocks(uart);

	/*set GPIO INTERRUPT*/
	if (omap_up.omap_uart_gpio_mux_mode != -1) {
		snprintf(gpio_name, MAX_UART_HWMOD_NAME_LEN,
			 "gpio_%d", omap_up.omap_uart_gpio_mux_mode);
		gpio_request(omap_up.omap_uart_gpio_mux_mode, gpio_name);
		gpio_direction_input(omap_up.omap_uart_gpio_mux_mode);
		gpio_irq = OMAP_GPIO_IRQ(omap_up.omap_uart_gpio_mux_mode);
		/* By default the Line would be high, whenever it
		 * falls and keeps toggling for data.
		 * Hence set to Falling Edge if the Interrupt.
		 */
		request_threaded_irq(gpio_irq, NULL, omap_uart_gpio_irq,
				IRQF_TRIGGER_FALLING | IRQF_SHARED,
				gpio_name, (void *)uart);
	}

	if (((cpu_is_omap34xx() || cpu_is_omap44xx())
		 && uart->padconf) ||
	    (uart->wk_en && uart->wk_mask)) {
		device_init_wakeup(&od->pdev.dev, true);
		DEV_CREATE_FILE(&od->pdev.dev, &dev_attr_sleep_timeout);
	}
}

/**
 * omap_serial_init() - intialize all supported serial ports
 *
 * Initializes all available UARTs as serial ports. Platforms
 * can call this function when they want to have default behaviour
 * for serial ports (e.g initialize them all as serial ports).
 */
void __init omap_serial_init(struct omap_uart_port_info *platform_data)
{
	struct omap_uart_state *uart;
	unsigned int count = 0;

	spin_lock_init(&serial_work_lock);

	/* The Platform Specific Initialisations come from the baord file
	 * which would initialise it to the platfrom requirement.
	 */
	list_for_each_entry(uart, &uart_list, node) {
		omap_serial_init_port(uart->num, &platform_data[count]);
		count++;
	}
}
