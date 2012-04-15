#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/ioport.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/interrupt.h>
#include <linux/proc_fs.h>
#include <linux/mm.h>
#include <linux/moduleparam.h>
#include <linux/device.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/otg.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/fsl_devices.h>
#include <linux/dmapool.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>
#include <linux/gpio.h>

#include <asm/byteorder.h>
#include <asm/io.h>
#include <asm/system.h>
#include <asm/unaligned.h>
#include <asm/dma.h>

#include <mach/iomap.h>

#include "gpio-names.h"
#include "board-tf201.h"

// fsl_udc_core.c function
int fsl_get_ac_connected(void);

/* Enable or disable the callback for the other driver. */
#define BATTERY_CALLBACK_ENABLED 1
#define TOUCH_CALLBACK_ENABLED 1
#define DOCK_EC_ENABLED 1

static int gpio_limit_set1_irq;

struct cable_info {
	/*
	* The cable status:
	* 0000: no cable
	* 0001: USB cable
	* 0011: AC apdater
	*/
	unsigned int cable_status;
	int is_active;
	int udc_vbus_active;
	int ac_connected;
	int ac_15v_connected;
	struct delayed_work cable_detection_work;
	struct mutex cable_info_mutex;
};

static struct cable_info s_cable_info;

#if BATTERY_CALLBACK_ENABLED
extern void battery_callback(unsigned cable_status);
#endif
#if TOUCH_CALLBACK_ENABLED
extern void touch_callback(unsigned cable_status);
#endif
#if DOCK_EC_ENABLED
extern int asusdec_is_ac_over_10v_callback(void);
#endif

/* Export the function "unsigned int get_usb_cable_status(void)" for others to query the USB cable status. */
unsigned int get_usb_cable_status(void)
{
	printk(KERN_INFO "The USB cable status = %x\n", s_cable_info.cable_status);
	return s_cable_info.cable_status;
}
EXPORT_SYMBOL( get_usb_cable_status);


/*
 * add for GPIO LIMIT_SET0 set
 * USB Cable -> LIMIT_SET0 = 0
 * AC adaptor -> LIMIT_SET0 = 1
 */
static void gpio_limit_set0_set(int enable)
{
	int ret = 0;

	ret = gpio_direction_output(TEGRA_GPIO_PR1, enable);
	if (ret < 0)
		printk(KERN_ERR "Failed to set the GPIO%d to the status(%d): %d\n", TEGRA_GPIO_PR1, enable, ret);

}

static void cable_detection_work_handler(struct work_struct *w)
{
	int	dock_in = 0;
	int	adapter_in = 0;
	int	dock_ac = 0;
	static int	ask_ec_num = 0;

	mutex_lock(&s_cable_info.cable_info_mutex);
	s_cable_info.cable_status &= (0<<3|0<<2|0<<1|0<<0); //0000
	printk(KERN_INFO "%s, vbus_active=%d, is_active=%d\n", __func__, s_cable_info.udc_vbus_active, s_cable_info.is_active);

	if (s_cable_info.udc_vbus_active && !s_cable_info.is_active) {
		if(!s_cable_info.ac_connected)
			printk(KERN_INFO "The USB cable is disconnected.\n");
		else
			printk(KERN_INFO "The AC adapter is disconnected.\n");

		s_cable_info.ac_connected = 0;
		s_cable_info.ac_15v_connected = 0;
		gpio_limit_set0_set(0);
#if BATTERY_CALLBACK_ENABLED
		battery_callback(s_cable_info.cable_status);
#endif
#if TOUCH_CALLBACK_ENABLED
		touch_callback(s_cable_info.cable_status);
#endif

	} else if (!s_cable_info.udc_vbus_active && s_cable_info.is_active) {
		s_cable_info.ac_connected = fsl_get_ac_connected();

		dock_in = gpio_get_value(TEGRA_GPIO_PU4);
		adapter_in = gpio_get_value(TEGRA_GPIO_PH5);

		if(!s_cable_info.ac_connected) {
			if(adapter_in == 0) {
				printk(KERN_INFO "The USB cable is connected (0.5A)\n");
				s_cable_info.cable_status |= 1<<0; //0001
				s_cable_info.ac_15v_connected = 0;
				gpio_limit_set0_set(0);
			}
			else if(adapter_in == 1) {
				printk(KERN_INFO "USB cable + AC adapter 15V connect (1A)\n");
				s_cable_info.cable_status |= 1<<1|1<<0; //0011
				s_cable_info.ac_15v_connected = 1;
				gpio_limit_set0_set(1);
			}
			else{
				printk(KERN_INFO "unknown status\n");
				s_cable_info.cable_status |= 1<<0; //0001
				s_cable_info.ac_15v_connected = 0;
				gpio_limit_set0_set(0);
			}
		}
		else{
			if(dock_in == 1) {//no dock in
				if(adapter_in == 1) {
					printk(KERN_INFO "AC adapter 15V connect (1A)\n");
					s_cable_info.cable_status |= 1<<1|1<<0; //0011
					s_cable_info.ac_15v_connected = 1;
				}else if(adapter_in == 0) {
					printk(KERN_INFO "AC adapter 5V connect (1A)\n");
					s_cable_info.cable_status |= 1<<0; //0001
					s_cable_info.ac_15v_connected = 0;
				}else{
					printk(KERN_ERR "No define adapter status\n");
					s_cable_info.cable_status |= 1<<0; //0001
					s_cable_info.ac_15v_connected = 0;
				}
			}else if(dock_in == 0) {// dock in
				#if DOCK_EC_ENABLED
				dock_ac = asusdec_is_ac_over_10v_callback();
				#endif

				if(dock_ac == 0x20) {
					printk(KERN_INFO "AC adapter + Docking 15V connect (1A)\n");
					s_cable_info.cable_status |= 1<<1|1<<0; //0011
					s_cable_info.ac_15v_connected = 1;
					ask_ec_num = 0;
				}else if(dock_ac == 0) {
					printk(KERN_INFO "AC adapter + Docking 5V connect (1A)\n");
					s_cable_info.cable_status |= 1<<0; //0001
					s_cable_info.ac_15v_connected = 0;
					ask_ec_num = 0;
				}else{
					if(ask_ec_num < 3) {
						ask_ec_num ++;
						printk(KERN_INFO "%s dock_ac = %d ask_ec_num = %d\n", __func__, dock_ac, ask_ec_num);
						s_cable_info.cable_status |= 1<<0; //0001
						s_cable_info.ac_15v_connected = 0;
						schedule_delayed_work(&s_cable_info.cable_detection_work, 0.5*HZ);
					}
					else{
						printk(KERN_INFO "unknown status\n");
						if(adapter_in == 1) {
							printk(KERN_INFO "LIMIT SET1: 15V connect (1A)\n");
							s_cable_info.cable_status |= 1<<1|1<<0; //0011
							s_cable_info.ac_15v_connected = 1;
						}else if(adapter_in == 0) {
							printk(KERN_INFO "LIMIT SET1: 5V connect (1A)\n");
							s_cable_info.cable_status |= 1<<0; //0001
							s_cable_info.ac_15v_connected = 0;
						}else{
							printk(KERN_ERR "LIMIT SET1 error status\n");
							s_cable_info.cable_status |= 1<<0; //0001
							s_cable_info.ac_15v_connected = 0;
						}
						ask_ec_num = 0;
					}
				}
			}
			else{
				printk(KERN_ERR "No define the USB status\n");
			}

			gpio_limit_set0_set(1);
		}
#if BATTERY_CALLBACK_ENABLED
		battery_callback(s_cable_info.cable_status);
#endif
#if TOUCH_CALLBACK_ENABLED
		touch_callback(s_cable_info.cable_status);
#endif

	}
	mutex_unlock(&s_cable_info.cable_info_mutex);
}

static void charging_gpios_init(void)
{
	int ret = 0;

	tegra_gpio_enable(TEGRA_GPIO_PH5);
	tegra_gpio_enable(TEGRA_GPIO_PR1);
	tegra_gpio_enable(TEGRA_GPIO_PU4);

	ret = gpio_request(TEGRA_GPIO_PR1, "LIMIT_SET0");
	if (ret < 0)
		printk(KERN_ERR "Failed to request the GPIO%d: %d\n", TEGRA_GPIO_PR1, ret);

	ret = gpio_request(TEGRA_GPIO_PH5, "LIMIT_SET1");
	if (ret < 0)
		printk(KERN_ERR "LIMIT_SET1 GPIO%d request fault!%d\n",TEGRA_GPIO_PH5, ret);

	ret = gpio_direction_input(TEGRA_GPIO_PH5);
	if (ret)
		printk(KERN_ERR "gpio_direction_input failed for input %d\n", TEGRA_GPIO_PH5);

	ret = gpio_request(TEGRA_GPIO_PU4, "DOCK_IN");
	if (ret < 0)
		printk(KERN_ERR "DOCK_IN GPIO%d request fault!%d\n",TEGRA_GPIO_PU4, ret);

	ret = gpio_direction_input(TEGRA_GPIO_PU4);
	if (ret)
		printk(KERN_ERR "gpio_direction_input failed for input %d\n", TEGRA_GPIO_PU4);

	gpio_limit_set0_set(0);
}

static void charging_gpios_free(void)
{
	gpio_free(TEGRA_GPIO_PH5);
	gpio_free(TEGRA_GPIO_PR1);
	gpio_free(TEGRA_GPIO_PU4);
}

static void cable_status_init(void)
{
	mutex_init(&s_cable_info.cable_info_mutex);
	s_cable_info.cable_status = 0x0;
	s_cable_info.is_active = 0;
	s_cable_info.udc_vbus_active = 0;
	s_cable_info.ac_connected = 0;
	s_cable_info.ac_15v_connected = 0;
	INIT_DELAYED_WORK(&s_cable_info.cable_detection_work, cable_detection_work_handler);
}

//For the issue of USB AC adaptor inserted half on PAD+Docking
void fsl_dock_ec_callback(void)
{
	int dock_in = 0;

	dock_in = gpio_get_value(TEGRA_GPIO_PU4);
	printk(KERN_INFO "%s cable_status=%d\n", __func__, s_cable_info.cable_status);
	if(dock_in == 0 && (s_cable_info.cable_status != 0)) {//dock in
		schedule_delayed_work(&s_cable_info.cable_detection_work, 0*HZ);
	}
}
EXPORT_SYMBOL(fsl_dock_ec_callback);

//For the issue of USB AC adaptor inserted half on PAD
static irqreturn_t gpio_limit_set1_irq_handler(int irq, void *dev_id)
{
	int adapter_in = 0;
	int dock_in = 0;

	adapter_in = gpio_get_value(TEGRA_GPIO_PH5);
	dock_in = gpio_get_value(TEGRA_GPIO_PU4);

	printk(KERN_INFO "%s gpio_limit_set1=%d, ac_15v_connected=%d\n", __func__, adapter_in, s_cable_info.ac_15v_connected);

	if(dock_in == 1 && (adapter_in != s_cable_info.ac_15v_connected)) {//no dock in
		schedule_delayed_work(&s_cable_info.cable_detection_work, 0.2*HZ);
	}
	return IRQ_HANDLED;
}

static void gpio_limit_set1_irq_init(void)
{
	int ret = 0;

	gpio_limit_set1_irq = gpio_to_irq(TEGRA_GPIO_PH5);
	ret = request_irq(gpio_limit_set1_irq, gpio_limit_set1_irq_handler, IRQF_TRIGGER_RISING|IRQF_TRIGGER_FALLING, "gpio_limit_set1_irq_handler", NULL);
	if (ret < 0) {
		printk(KERN_ERR"%s: Could not request IRQ for the GPIO limit set1, irq = %d, ret = %d\n", __func__, gpio_limit_set1_irq, ret);
	}
	printk(KERN_INFO"%s: request irq = %d, ret = %d\n", __func__, gpio_limit_set1_irq, ret);
}

void __init tf201_cabledetect_init(void)
{
	charging_gpios_init();
	cable_status_init();
	gpio_limit_set1_irq_init();
}