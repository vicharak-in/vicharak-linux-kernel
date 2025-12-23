/*
** Significant of gpio.c file :
** 1. Make multiple gpio chip with the use of dtso and create the
** gpiochip-* series into the /dev.
** 2. Allow get/set functionality for any specific (gpiochip*-).
** 3. interrupt functionality implement for any pins of gpiochip.
*/

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/gpio/driver.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

#define DRIVER_NAME "periplex-gpio"

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define GPIO_DEBUG(fmt, ...)             \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info("periplex_gpio: "fmt, ##__VA_ARGS__); \
    } while (0)

/*
** waitqueue used for internal gpio operations
*/
wait_queue_head_t wait_queue_gpio_ioctl;
int wait_queue_flag_com_gpio;

static void periplex_gpio_set(struct gpio_chip *chip, unsigned offset, int value);
static int periplex_gpio_get(struct gpio_chip *chip, unsigned offset);
static int periplex_gpio_request(struct gpio_chip *chip, unsigned offset);
static void periplex_gpio_free(struct gpio_chip *chip, unsigned offset);

struct periplex_gpio_irq_line
{
    unsigned int type;
};

struct periplex_gpio
{
    u8 out;
    int peri_id;
    int in_out_pins;
    int interrupt_pins;
    int rising_falling_pins;
    struct mutex lock;
    struct irq_chip irq;
    struct gpio_chip chip;
    struct irq_domain *irq_domain;
    struct periplex_gpio_irq_line *irq_lines;
};

static int periplex_gpio_write(struct gpio_chip *gpio, u8 value)
{
    char buffer[2] = {0};
    struct periplex_gpio *chip = gpiochip_get_data(gpio);
    int periplex_id = chip->peri_id;
    pr_info("periplex_gpio: write value is %u\n", value);
    buffer[0] = value;
    buffer[1] = '\0';
    set_periplex_data(periplex_id, 1, buffer);
    return 0;
}

char read_data_gpio;

int read_data_for_gpio(struct periplex_device *pdev, char *message, const int len)
{
    int pins = 0;
    int i = 0;
    unsigned int bitmask = 0x01;
    unsigned long flags = 0;
    struct periplex_gpio *gpio = NULL;

    if (!pdev || !message || len <= 0)
    {
        pr_err("periplex_gpio: Invalid parameters\n");
        return -EINVAL;
    }

    // Get driver data
    gpio = periplex_get_drvdata(pdev);
    if (!gpio)
    {
        pr_err("periplex_gpio: Failed to get GPIO driver data\n");
        return -ENODEV;
    }

    // Read GPIO data
    read_data_gpio = message[0];

    // Check for interrupt pins
    pins = (message[0] & gpio->interrupt_pins);
    if (pins > 0)
    {
        // Loop through all 8 bits
        for (i = 0; i < 8; i++)
        {
            if (pins & bitmask)
            {
                // Disable interrupts and handle IRQ
                local_irq_save(flags);
                generic_handle_irq(irq_find_mapping(gpio->chip.irq.domain, i));
                local_irq_restore(flags);
                pr_info("periplex_gpio: the offset is %d\n", i);
                break;
            }
            bitmask <<= 1;
        }
    }

    wait_queue_flag_com_gpio = 1;
    wake_up_interruptible(&wait_queue_gpio_ioctl);

    return 0;
}
static int periplex_gpio_read(struct gpio_chip *gpio, u8 *value)
{

    wait_queue_flag_com_gpio = 0;
    wait_event_interruptible(wait_queue_gpio_ioctl, wait_queue_flag_com_gpio != 0);

    *value = read_data_gpio;
    pr_info("periplex_gpio: read value is %d\n", *value);
    return 0;
}

static int periplex_gpio_direction_input(struct gpio_chip *chip, unsigned offset)
{
    struct periplex_gpio *gpio = gpiochip_get_data(chip);
    int periplex_id = gpio->peri_id;
    pr_info("periplex_gpio: IN offset : %d\n", offset);

    if ((gpio->in_out_pins & (1 << offset)) != 0)
    {
        gpio->in_out_pins = gpio->in_out_pins ^ (1 << offset);
    }

    set_periplex_configuration(periplex_id, 0, gpio->in_out_pins);
    periplex_gpio_get(chip, offset);

    return 0;
}

static int periplex_gpio_direction_output(struct gpio_chip *chip, unsigned offset, int value)
{
    struct periplex_gpio *gpio = gpiochip_get_data(chip);
    int periplex_id = gpio->peri_id;
    pr_info("periplex_gpio: OUT offset : %d\n", offset);

    gpio->in_out_pins = gpio->in_out_pins | (1 << offset);

    set_periplex_configuration(periplex_id, 0, gpio->in_out_pins);
    periplex_gpio_set(chip, offset, value);

    return 0;
}

static int periplex_gpio_get(struct gpio_chip *chip, unsigned offset)
{
    struct periplex_gpio *gpio = gpiochip_get_data(chip);
    u8 buffer = 0;
    int ret = 0;
    pr_info("periplex_gpio: gpio get\n");

    periplex_gpio_write(&gpio->chip, gpio->out);
    ret = periplex_gpio_read(&gpio->chip, &buffer);
    if (ret)
        return ret;

    return !!(buffer & BIT(offset));
}

static void periplex_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
    struct periplex_gpio *gpio = gpiochip_get_data(chip);
    u8 buffer = 0;
    int ret = 0;
    pr_info("periplex_gpio: gpio set\n");

    mutex_lock(&gpio->lock);

    buffer = gpio->out;
    if (value)
        buffer |= BIT(offset);
    else
        buffer &= ~BIT(offset);

    ret = periplex_gpio_write(chip, buffer);
    if (ret)
        goto out;

    gpio->out = buffer;

out:
    mutex_unlock(&gpio->lock);
}

static int periplex_gpio_request(struct gpio_chip *chip, unsigned offset)
{
    GPIO_DEBUG("request\n");
    return 0;
}

static void periplex_gpio_free(struct gpio_chip *chip, unsigned offset)
{
    struct periplex_gpio *gpio = gpiochip_get_data(chip);
    gpio->interrupt_pins = 0;
    gpio->rising_falling_pins = 0;
    GPIO_DEBUG("free\n");
}

static int periplex_gpio_irq_set_type(struct irq_data *data, unsigned int flow_type)
{

    struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
    struct periplex_gpio *gpio = gpiochip_get_data(gc);
    struct periplex_gpio_irq_line *irq_line = &gpio->irq_lines[data->hwirq];

    if (data->hwirq >= 8)
    {
        pr_err("periplex_gpio: Invalid hwirq: %lu\n", data->hwirq);
        return -EINVAL;
    }

    switch (flow_type)
    {
    case IRQ_TYPE_EDGE_RISING:
        irq_line->type = flow_type;
        break;
    case IRQ_TYPE_EDGE_FALLING:
        irq_line->type = flow_type;
        break;
    case IRQ_TYPE_EDGE_BOTH:
        irq_line->type = flow_type;
        break;
    case IRQ_TYPE_LEVEL_LOW:
        irq_line->type = flow_type;
        break;
    case IRQ_TYPE_LEVEL_HIGH:
        irq_line->type = flow_type;
        break;
    default:
        return -EINVAL;
    }

    pr_info("periplex_gpio: flow type in irq_set_type %u\n", flow_type);

    return 0;
}

static void periplex_gpio_irq_ack(struct irq_data *data)
{
    GPIO_DEBUG("irq_ack\n");
}

static void periplex_gpio_irq_mask(struct irq_data *data)
{
    GPIO_DEBUG("irq_mask\n");
}

static void periplex_gpio_irq_clr_mask(struct irq_data *data)
{
    GPIO_DEBUG("irq_clr_mask\n");
}

static void periplex_gpio_irq_enable(struct irq_data *data)
{
    int configuration = 0;
    int rising_falling = 0;
    int irq_number = 0;
    struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
    struct periplex_gpio *gpio = gpiochip_get_data(gc);
    struct periplex_gpio_irq_line *irq_line = &gpio->irq_lines[data->hwirq];
    int periplex_id = gpio->peri_id;

    if (data->hwirq >= 8)
    {
        pr_err("periplex_gpio: Invalid hwirq: %lu\n", data->hwirq);
        return;
    }
    GPIO_DEBUG("irq_enable\n");

    gpio->interrupt_pins = gpio->interrupt_pins | (1 << data->hwirq);
    irq_number = (gpio->interrupt_pins << 8);

    switch (irq_line->type)
    {
    case IRQ_TYPE_EDGE_RISING:
        gpio->rising_falling_pins = gpio->rising_falling_pins | (1 << data->hwirq);
        rising_falling = (gpio->rising_falling_pins << 16);
        configuration = rising_falling | irq_number | gpio->in_out_pins;
        set_periplex_configuration(periplex_id, 0, configuration);
        break;
    case IRQ_TYPE_EDGE_FALLING:
        if ((gpio->rising_falling_pins & (1 << data->hwirq)) != 0)
        {
            gpio->rising_falling_pins = gpio->rising_falling_pins ^ (1 << data->hwirq);
        }
        rising_falling = (gpio->rising_falling_pins << 16);
        configuration = rising_falling | irq_number | gpio->in_out_pins;
        set_periplex_configuration(periplex_id, 0, configuration);
        break;
    case IRQ_TYPE_EDGE_BOTH:
        break;
    case IRQ_TYPE_LEVEL_LOW:
        break;
    case IRQ_TYPE_LEVEL_HIGH:
        break;
    default:
        return;
    }
}

static void periplex_gpio_irq_disable(struct irq_data *data)
{
    int configuration = 0;
    int rising_falling = 0;
    int irq_number = 0;
    struct gpio_chip *gc = irq_data_get_irq_chip_data(data);
    struct periplex_gpio *gpio = gpiochip_get_data(gc);
    int periplex_id = gpio->peri_id;
    GPIO_DEBUG("irq_disable\n");

    if ((gpio->interrupt_pins & (1 << data->hwirq)) != 0)
    {
        gpio->interrupt_pins = gpio->interrupt_pins ^ (1 << data->hwirq);
        if ((gpio->rising_falling_pins & (1 << data->hwirq)) != 0)
        {
            gpio->rising_falling_pins = gpio->rising_falling_pins ^ (1 << data->hwirq);
        }
        irq_number = (gpio->interrupt_pins << 8);
        rising_falling = (gpio->rising_falling_pins << 16);
        configuration = rising_falling | irq_number | gpio->in_out_pins;
        set_periplex_configuration(periplex_id, 0, configuration);
    }
}

static int periplex_gpio_to_irq(struct gpio_chip *gc, unsigned int offset)
{
    struct periplex_gpio *gpio = gpiochip_get_data(gc);
    int irq = irq_create_mapping(gpio->irq_domain, offset); // Create IRQ mapping

    if (irq < 0)
        return irq;

    irq_set_chip_data(irq, gpio); // Associate GPIO with IRQ
    GPIO_DEBUG("IRQ offset is %d, IRQ number is %d\n", offset, irq);
    return irq;
}

static int periplex_gpio_probe(struct periplex_device *pdev)
{
    int ret = 0;
    int periplex_id = 0;
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    struct periplex_gpio *gpio = NULL;
    struct gpio_irq_chip *girq = NULL;

    // Allocate memory for the periplex GPIO structure
    gpio = kzalloc(sizeof(struct periplex_gpio), GFP_KERNEL);
    if (!gpio)
    {
        ret = -ENOMEM;
        goto err_free_gpio;
    }

    // Initialize mutex
    mutex_init(&gpio->lock);

    /* 	initialize gpio internal wait queue */
    init_waitqueue_head(&wait_queue_gpio_ioctl);

    gpio->irq_lines = kzalloc(sizeof(struct periplex_gpio_irq_line) * 8, GFP_KERNEL);
    if (!gpio->irq_lines)
    {
        dev_err(&pdev->dev, "Failed to allocate irq_lines\n");
        ret = -ENOMEM;
        goto err_free_gpio;
    }

    if (!np)
    {
        dev_err(dev, "No device tree node found\n");
        ret = -ENODEV;
    }
    // Read periplex-id from device tree
    ret = of_property_read_u32(np, "periplex-id", &periplex_id);
    if (ret)
    {
        dev_err(dev, "Failed to read periplex-id from device tree\n");
        return ret;
    }

    // set the periplex_gpio variable
    gpio->in_out_pins = 0;
    gpio->interrupt_pins = 0;
    gpio->rising_falling_pins = 0;

    // Set up IRQ domain
    gpio->irq_domain = irq_domain_add_linear(np, 8, &irq_domain_simple_ops, gpio);
    if (!gpio->irq_domain)
    {
        pr_err("periplex_gpio: Failed to allocate IRQ domain\n");
        ret = -ENOMEM;
        goto err_free_gpio;
    }

    if (gpio->irq_domain)
    {
        pr_info("periplex_gpio: Allocate IRQ domain for %d\n", periplex_id);
    }

    // GPIO chip setup
    gpio->chip.label = "vicharak";
    gpio->chip.owner = THIS_MODULE;
    gpio->chip.request = periplex_gpio_request;
    gpio->chip.free = periplex_gpio_free;
    gpio->chip.set = periplex_gpio_set;
    gpio->chip.get = periplex_gpio_get;
    gpio->chip.direction_input = periplex_gpio_direction_input;
    gpio->chip.direction_output = periplex_gpio_direction_output;
    gpio->chip.base = -1;
    gpio->chip.ngpio = 8;
    gpio->chip.can_sleep = false;
    gpio->peri_id = periplex_id;
    gpio->chip.to_irq = periplex_gpio_to_irq;
    gpio->chip.irq.domain = gpio->irq_domain;

    gpio->irq.name = "periplex-gpio";
    gpio->irq.irq_ack = periplex_gpio_irq_ack;
    gpio->irq.irq_mask = periplex_gpio_irq_mask;
    gpio->irq.irq_unmask = periplex_gpio_irq_clr_mask;
    gpio->irq.irq_set_type = periplex_gpio_irq_set_type;
    gpio->irq.irq_enable = periplex_gpio_irq_enable;
    gpio->irq.irq_disable = periplex_gpio_irq_disable;

    girq = &gpio->chip.irq;
    girq->chip = &gpio->irq;
    girq->handler = handle_level_irq;
    girq->default_type = IRQ_TYPE_NONE;

    pdev->periplex_id = periplex_id;
    pdev->get_periplex_data = read_data_for_gpio;

    /* This is mandatory part to register device with periplex */
    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, gpio);

    // Register the GPIO chip
    ret = gpiochip_add_data(&gpio->chip, gpio);
    if (ret)
    {
        pr_err("Failed to add gpio_chip\n");
        goto err_remove_irq_domain;
    }

    pr_info("periplex_gpio: GPIO driver is inserted successfully...%d\n", gpio->chip.base);
    return 0;

err_remove_irq_domain:
    irq_domain_remove(gpio->irq_domain);
err_free_gpio:
    kfree(gpio);

    return ret;
}

static int periplex_gpio_remove(struct periplex_device *pdev)
{
    struct periplex_gpio *gpio = periplex_get_drvdata(pdev);
    if (!gpio)
    {
        dev_err(&pdev->dev, "No GPIO data found\n");
        return -ENODEV;
    }

    // Remove the gpio_chip first
    gpiochip_remove(&gpio->chip);

    // remove the irq_domain
    irq_domain_remove(gpio->irq_domain);

    // Unregister the device
    periplex_unlink_device(pdev);

    // Free dynamically allocated memory
    kfree(gpio->irq_lines);
    kfree(gpio);

    pr_info("periplex_gpio: GPIO driver is removed successfully\n");
    return 0;
}

struct of_device_id periplex_gpio_dt_match[] = {
    {.compatible = "vicharak,periplex-gpio"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_gpio_dt_match);

struct periplex_driver periplex_gpio_driver = {
    .probe = periplex_gpio_probe,
    .remove = periplex_gpio_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_gpio_dt_match,
    },
};
module_periplex_driver(periplex_gpio_driver);

MODULE_ALIAS("periplex:gpio");
MODULE_AUTHOR("vatsal kevadiya <vhkeavdiya15@gamil.com>");
MODULE_DESCRIPTION("GPIO Driver for the Periplex");
MODULE_LICENSE("GPL");