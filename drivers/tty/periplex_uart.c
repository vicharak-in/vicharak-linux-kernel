/*
** Significant of uart.c file :
** 1.Make multiple uart devices with the use of dtso and create the
** ttyPERI* series into the /dev
** 2.Allow Write/read data in any specific uart(ttyPERI*) device
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/moduleparam.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

#define DRIVER_NAME "periplex-uart"

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define UART_DEBUG(fmt, ...)             \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info("periplex_uart: "fmt, ##__VA_ARGS__); \
    } while (0)

/* UART Configuration Structure */
struct uart_config
{
    int baud_rate;
    int data_bits;
    int parity;    /* 0=none, 1=odd, 2=even */
    int stop_bits; /* 1 or 2 */
};

/*
** Helper function to get data width from termios
*/
static int get_data_width(struct ktermios *termios)
{
    switch (termios->c_cflag & CSIZE)
    {
    case CS5:
        return 5;
    case CS6:
        return 6;
    case CS7:
        return 7;
    case CS8:
        return 8;
    default:
        return 8; /* Default to 8 bits */
    }
}

/*
** Helper function to get parity from termios
*/
static int get_parity_type(struct ktermios *termios)
{
    if (!(termios->c_cflag & PARENB))
    {
        return 0; /* No parity */
    }
    else if (termios->c_cflag & PARODD)
    {
        return 2; /* Odd parity */
    }
    else
    {
        return 1; /* Even parity */
    }
}

/*
** Helper function to get stop bits from termios
*/
static int get_stop_bits(struct ktermios *termios)
{
    return (termios->c_cflag & CSTOPB) ? 2 : 1;
}

/*
** Helper function to get baud rate from termios
*/
static int get_baud_rate(struct ktermios *termios)
{
    return (int)tty_termios_baud_rate(termios);
}

/*
** this functions is used in read for uart
*/
int read_data_for_uart(struct periplex_device *pdev, char *message, const int len)
{
    struct tty_driver *uart_driver = periplex_get_drvdata(pdev);
    int i;

    if (!uart_driver || !uart_driver->ports[0])
    {
        UART_DEBUG("UART driver or port is NULL\n");
        return -EINVAL;
    }

    if (!message || len <= 0)
    {
        UART_DEBUG("Invalid message or length\n");
        return -EINVAL;
    }

    UART_DEBUG("uart read calling\n");
    UART_DEBUG("length is %d\n", len);
    
    for (i = 0; i < len; i++)
    {
        tty_insert_flip_char(uart_driver->ports[0], message[i], TTY_NORMAL);
    }
    tty_flip_buffer_push(uart_driver->ports[0]);

    return 0;
}

/*
** uart open function
*/
static int tty_periplex_open(struct tty_struct *tty, struct file *file)
{
    return 0;
}

/*
** uart close function
*/
static void tty_periplex_close(struct tty_struct *tty, struct file *file)
{
    return;
}

/*
** uart write function
*/
static int tty_periplex_write(struct tty_struct *tty, const unsigned char *buffer,
                          int count)
{
    int periplex_id = tty->driver->name_base;
    set_periplex_data(periplex_id, count, (char *)buffer);
    return count;
}

/*
** uart write-room function
*/
static int tty_periplex_write_room(struct tty_struct *tty)
{
    return 1;
}

/*
** uart set-termios function used for set the configurations
*/
static void tty_periplex_set_termios(struct tty_struct *tty, struct ktermios *old)
{
    struct uart_config config;
    int periplex_id;
    int baud_divisor;
    u32 width_parity_stop;

    if (!tty || !tty->driver) {
        pr_err("periplex_uart_set_termios: Invalid tty or driver\n");
        return;
    }

    periplex_id = tty->driver->name_base;

    /* Extract UART parameters */
    config.baud_rate = get_baud_rate(&tty->termios);
    config.data_bits = get_data_width(&tty->termios);
    config.parity    = get_parity_type(&tty->termios);
    config.stop_bits = get_stop_bits(&tty->termios);

    /* Validate parameters */
    if (config.baud_rate <= 0) {
        pr_warn("periplex_uart: Invalid baud rate: %d, using default 9600\n", config.baud_rate);
        config.baud_rate = 9600;
    }

    if (config.data_bits < 5 || config.data_bits > 8) {
        pr_warn("periplex_uart: Invalid data bits: %d, using default 8\n", config.data_bits);
        config.data_bits = 8;
    }

    if (config.parity < 0 || config.parity > 2) {
        pr_warn("periplex_uart: Invalid parity: %d, using default 0 (none)\n", config.parity);
        config.parity = 0;
    }

    if (config.stop_bits < 1 || config.stop_bits > 2) {
        pr_warn("periplex_uart: Invalid stop bits: %d, using default 1\n", config.stop_bits);
        config.stop_bits = 1;
    }

    UART_DEBUG("UART Config [ID: %d] => Baud: %d, DataBits: %d, Parity: %d, StopBits: %d\n",
            periplex_id, config.baud_rate, config.data_bits,
            config.parity, config.stop_bits);

    /* Calculate baud divisor */
    baud_divisor = 50000000 / config.baud_rate;
    set_periplex_configuration(periplex_id, 0, baud_divisor);

    /* Pack data_bits, parity, stop_bits into 32-bit integer */
    width_parity_stop = ((config.data_bits & 0xFF) << 24) |
                        ((config.parity    & 0xFF) << 16) |
                        ((config.stop_bits & 0xFF) << 8);

    set_periplex_configuration(periplex_id, 1, width_parity_stop);
}

/*
** initialize the opeations for uart
*/
static const struct tty_operations serial_ops = {
    .open = tty_periplex_open,
    .write = tty_periplex_write,
    .write_room = tty_periplex_write_room,
    .set_termios = tty_periplex_set_termios,
    .close = tty_periplex_close,
};

/*
** probe functions for device registration
*/
static int periplex_uart_probe(struct periplex_device *pdev)
{
    int ret = 0;
    int periplex_id = 0;
    struct tty_driver *uart_driver = NULL;
    struct tty_port *uart_port = NULL;

    uart_port = kzalloc(sizeof(struct tty_port), GFP_KERNEL);
    if (!uart_port)
        return -ENOMEM;

    if (device_property_read_u32(&pdev->dev, "periplex-id", &periplex_id))
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tree for uart\n");
    }

    tty_port_init(uart_port);

    /* Allocate the uart driver */
    uart_driver = tty_alloc_driver(1, TTY_DRIVER_REAL_RAW);

    if (IS_ERR(uart_driver))
    {
        return PTR_ERR(uart_driver);
    }

    /* Initialize the uart driver */
    uart_driver->owner = THIS_MODULE;
    uart_driver->driver_name = "tty_periplex";
    uart_driver->name = "ttyPERI";
    uart_driver->name_base = periplex_id;
    uart_driver->major = 0;
    uart_driver->minor_start = 0;
    uart_driver->type = TTY_DRIVER_TYPE_SERIAL;
    uart_driver->subtype = SERIAL_TYPE_NORMAL;
    uart_driver->flags = TTY_DRIVER_REAL_RAW;
    uart_driver->init_termios = tty_std_termios;

    /* Assigning port to each multiple uart devices */
    tty_set_operations(uart_driver, &serial_ops);
    tty_port_link_device(uart_port, uart_driver, 0);

    pdev->periplex_id = periplex_id;
    pdev->get_periplex_data = read_data_for_uart;

    /* This is mandatory part to register device with periplex */
    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, uart_driver);

    /* Register the uart driver */
    ret = tty_register_driver(uart_driver);
    if (ret)
    {
        pr_info(KERN_ERR "periplex_uart: failed to register tiny tty driver\n");
        goto cleanup;
    }
    pr_info("periplex_uart: ttyPERI are successfully inserted...\n");

    return 0;

cleanup:
    tty_unregister_driver(uart_driver);
    return ret;
}

/*
** remove functions for device unregistration
*/
static int periplex_uart_remove(struct periplex_device *pdev)
{
    struct tty_driver *uart_driver = periplex_get_drvdata(pdev);
    tty_unregister_driver(uart_driver);
    tty_driver_kref_put(uart_driver);
    tty_port_destroy(uart_driver->ports[0]);
    periplex_unlink_device(pdev);
    kfree(uart_driver->ports[0]);
    pr_info("periplex_uart: ttyPERI are removed successfully...\n");
    return 0;
}

/*
** compitable property match with DTSO of uart
*/
static struct of_device_id periplex_uart_dt_match[] = {
    {.compatible = "vicharak,periplex-uart"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_uart_dt_match);

static struct periplex_driver periplex_uart_driver = {
    .probe = periplex_uart_probe,
    .remove = periplex_uart_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_uart_dt_match,
    },
};
module_periplex_driver(periplex_uart_driver);

MODULE_ALIAS("periplex:uart");
MODULE_AUTHOR("Vatsal Kevadiya<vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("UART Driver for the periplex");
MODULE_LICENSE("GPL");