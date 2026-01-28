/*
** Significant of ws2812b.c file :
** 1.Make multiple ws2812b dummy device with the use of dtso and create the
** ws2812b-* series into the /dev.
** 2.Allow Write/read data in any specific ws2812b(WS2812B-*) device
** 3.Accumulate data written to each device and periodically send it in a burst
** to optimize performance and reduce overhead.
*/

#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/ioctl.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/workqueue.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Flush interval in milliseconds (can be set via module parameter) */
static int flush_interval_ms = 17;
module_param(flush_interval_ms, int, 0644);
MODULE_PARM_DESC(flush_interval_ms, "Data flush interval in milliseconds (default: 10ms)");

/* Macro to conditionally print debug info */
#define WS_DEBUG(fmt, ...)               \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info("periplex_ws: "fmt, ##__VA_ARGS__); \
    } while (0)

/*
** ioctl commands for the ws2812b device
*/
#define PUSH_DATA _IOW('a', 'b', struct data *)

/*
**define the driver name and maximum device name length
*/
#define DRIVER_NAME "periplex-ws"
#define MAX_DEVICE_NAME 64
#define MAX_BUFFER_SIZE 1024 * 64 /* Maximum buffer size for accumulated data */

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

/*
** Structure to hold buffered data for each WS2812B device
*/
struct ws_data_buffer
{
    char *buffer;       /* Accumulated data buffer */
    int current_length; /* Current length of accumulated data */
    int capacity;       /* Total capacity of buffer */
};

/*
** structure for the ws2812b device
*/
struct periplex_ws
{
    dev_t dev;
    char name[MAX_DEVICE_NAME];
    int periplex_id;
    struct class *dev_class;
    struct cdev ws_cdev;
    struct device *device;
    struct ws_data_buffer data_buf;
    struct mutex buf_mutex;
    struct list_head list;
};

/*
** Global list to track all WS2812B devices
*/
static LIST_HEAD(ws_device_list);
static DEFINE_MUTEX(ws_list_mutex);

/*
** Timer and workqueue for periodic data transmission
*/
static struct timer_list ws_timer;
static struct workqueue_struct *ws_workqueue;
static struct work_struct ws_work;
static bool timer_active = false;
static atomic_t data_pending = ATOMIC_INIT(0);
static atomic_t ws_device_count = ATOMIC_INIT(0);
static bool workqueue_initialized = false;

/*
** global device count
*/
int nr = 0;

/*
** structure for the data that will be sent to the ws2812b device
*/
struct data
{
    int length;
    char *data;
};

/*
** Initialize data buffer for a WS2812B device
*/
static int init_data_buffer(struct ws_data_buffer *buf)
{
    buf->buffer = vmalloc(MAX_BUFFER_SIZE);
    if (!buf->buffer)
    {
        pr_err("Failed to allocate data buffer\n");
        return -ENOMEM;
    }
    memset(buf->buffer, 0, MAX_BUFFER_SIZE);
    buf->current_length = 0;
    buf->capacity = MAX_BUFFER_SIZE;
    return 0;
}

static void free_data_buffer(struct ws_data_buffer *buf)
{
    if (buf->buffer)
    {
        vfree(buf->buffer);
        buf->buffer = NULL;
    }
    buf->current_length = 0;
    buf->capacity = 0;
}

/*
** Append data to the buffer
*/
static int append_to_buffer(struct periplex_ws *ws, const char *data, int length)
{
    struct ws_data_buffer *buf = &ws->data_buf;

    if (buf->current_length + length > buf->capacity)
    {
        pr_err("Buffer overflow: current=%d, adding=%d, capacity=%d\n",
               buf->current_length, length, buf->capacity);
        return -ENOMEM;
    }

    memcpy(buf->buffer + buf->current_length, data, length);
    buf->current_length += length;

    WS_DEBUG("Appended %d bytes to device %d buffer. Total: %d bytes\n",
             length, ws->periplex_id, buf->current_length);

    return 0;
}

/*
** Timer callback: Schedule work to send data
*/
static void ws_timer_callback(struct timer_list *timer)
{
    queue_work(ws_workqueue, &ws_work);

    if (timer_active)
    {
        mod_timer(&ws_timer, jiffies + msecs_to_jiffies(flush_interval_ms));
    }
}

/*
** Start the periodic timer
*/
static void start_periodic_timer(void)
{
    if (!timer_active)
    {
        timer_active = true;
        mod_timer(&ws_timer, jiffies + msecs_to_jiffies(flush_interval_ms));
        WS_DEBUG("Periodic timer started (interval: %d ms)\n", flush_interval_ms);
    }
}

/*
** Stop the periodic timer
*/
static void stop_periodic_timer(void)
{
    if (timer_active)
    {
        timer_active = false;
        del_timer_sync(&ws_timer);
        WS_DEBUG("Periodic timer stopped\n");
    }
}

/*
** Check if any device has pending data and start/stop timer accordingly
*/
static void check_and_manage_timer(void)
{
    struct periplex_ws *ws;
    bool has_data = false;

    mutex_lock(&ws_list_mutex);

    list_for_each_entry(ws, &ws_device_list, list)
    {
        mutex_lock(&ws->buf_mutex);
        if (ws->data_buf.current_length > 0)
        {
            has_data = true;
            mutex_unlock(&ws->buf_mutex);
            break;
        }
        mutex_unlock(&ws->buf_mutex);
    }

    mutex_unlock(&ws_list_mutex);

    if (has_data)
    {
        if (atomic_read(&data_pending) == 0)
        {
            atomic_set(&data_pending, 1);
            start_periodic_timer();
            WS_DEBUG("Data detected - timer started\n");
        }
    }
    else
    {
        if (atomic_read(&data_pending) == 1)
        {
            atomic_set(&data_pending, 0);
            if (timer_active)
            {
                timer_active = false;
                del_timer_sync(&ws_timer);
                WS_DEBUG("No more data - timer stopped\n");
            }
        }
    }
}

/*
** Work function: Build structure array and send burst data
*/
static void ws_flush_work(struct work_struct *work)
{
    struct periplex_ws *ws;
    struct peripheral_burst_data *burst_array;
    int total_device_count = 0;
    int device_with_data_count = 0;
    int index = 0;
    int ret = 0;

    WS_DEBUG("Work function triggered - processing pending data\n");

    mutex_lock(&ws_list_mutex);

    /* Count total devices and devices with pending data */
    list_for_each_entry(ws, &ws_device_list, list)
    {
        total_device_count++;
        mutex_lock(&ws->buf_mutex);
        if (ws->data_buf.current_length > 0)
        {
            device_with_data_count++;
        }
        mutex_unlock(&ws->buf_mutex);
    }

    /* If no devices exist at all, just return */
    if (total_device_count == 0)
    {
        mutex_unlock(&ws_list_mutex);
        WS_DEBUG("No devices registered\n");
        /* Stop timer since no devices exist */
        if (atomic_read(&data_pending) == 1)
        {
            atomic_set(&data_pending, 0);
            stop_periodic_timer();
        }
        return;
    }

    /* If no device has pending data, stop timer and skip this cycle */
    if (device_with_data_count == 0)
    {
        mutex_unlock(&ws_list_mutex);
        WS_DEBUG("No devices have pending data - stopping timer\n");
        /* Stop timer automatically when no data */
        if (atomic_read(&data_pending) == 1)
        {
            atomic_set(&data_pending, 0);
            stop_periodic_timer();
        }
        return;
    }

    /* Allocate structure array ONLY for devices with data */
    burst_array = kzalloc(device_with_data_count * sizeof(struct peripheral_burst_data), GFP_KERNEL);
    if (!burst_array)
    {
        mutex_unlock(&ws_list_mutex);
        pr_err("Failed to allocate burst array\n");
        return;
    }

    /* Fill the structure array with ONLY devices that have data */
    list_for_each_entry(ws, &ws_device_list, list)
    {
        mutex_lock(&ws->buf_mutex);
        if (ws->data_buf.current_length > 0)
        {
            burst_array[index].periplex_id = ws->periplex_id;
            burst_array[index].total_length = ws->data_buf.current_length;
            burst_array[index].whole_data = ws->data_buf.buffer;

            WS_DEBUG("Device %d: %d bytes ready for transmission\n",
                     ws->periplex_id, ws->data_buf.current_length);

            index++;
        }
        else
        {
            WS_DEBUG("Device %d: No data - skipping\n", ws->periplex_id);
        }
        mutex_unlock(&ws->buf_mutex);
    }

    WS_DEBUG("Sending burst data for %d out of %d devices\n",
             device_with_data_count, total_device_count);

    /* Call the burst transmission function with structure array */
    ret = set_periplex_data_burst(burst_array, device_with_data_count);

    if (ret < 0)
    {
        pr_err("Failed to send burst data\n");
    }
    else
    {
        /* Reset buffers ONLY for devices that had data and were transmitted */
        list_for_each_entry(ws, &ws_device_list, list)
        {
            mutex_lock(&ws->buf_mutex);
            if (ws->data_buf.current_length > 0)
            {
                ws->data_buf.current_length = 0;
                memset(ws->data_buf.buffer, 0, ws->data_buf.capacity);
                WS_DEBUG("Reset buffer for device %d\n", ws->periplex_id);
            }
            mutex_unlock(&ws->buf_mutex);
        }

        WS_DEBUG("Burst transmission completed successfully\n");
    }

    kfree(burst_array);
    mutex_unlock(&ws_list_mutex);
}

/*
** Initialize workqueue and timer (called on first device probe)
*/
static int init_workqueue_and_timer(void)
{
    if (workqueue_initialized)
        return 0;

    ws_workqueue = create_singlethread_workqueue("ws2812b_wq");
    if (!ws_workqueue)
    {
        pr_err("Failed to create workqueue\n");
        return -ENOMEM;
    }

    INIT_WORK(&ws_work, ws_flush_work);

    timer_setup(&ws_timer, ws_timer_callback, 0);

    workqueue_initialized = true;
    WS_DEBUG("WS2812B workqueue and timer initialized (flush interval: %d ms)\n", flush_interval_ms);

    return 0;
}

/*
** Cleanup workqueue and timer (called on last device remove)
*/
static void cleanup_workqueue_and_timer(void)
{
    if (!workqueue_initialized)
        return;

    WS_DEBUG("Starting workqueue and timer cleanup\n");

    if (timer_active)
    {
        timer_active = false;
        del_timer_sync(&ws_timer);
        WS_DEBUG("Timer stopped and synced\n");
    }

    if (ws_workqueue)
    {
        WS_DEBUG("Canceling pending work\n");
        cancel_work_sync(&ws_work);
        WS_DEBUG("Work canceled successfully\n");

        WS_DEBUG("Destroying workqueue\n");
        destroy_workqueue(ws_workqueue);
        ws_workqueue = NULL;
        WS_DEBUG("Workqueue destroyed\n");
    }

    workqueue_initialized = false;
    WS_DEBUG("WS2812B workqueue and timer cleaned up\n");
}

/*
** this function is use in read for ws
*/
int read_data_for_ws(struct periplex_device *pdev, char *message, const int len)
{
    return 0;
}

/*
** call when the device file open
*/
static int ws_open(struct inode *inode, struct file *file)
{
    struct periplex_ws *ws = container_of(inode->i_cdev, struct periplex_ws, ws_cdev);
    file->private_data = ws;
    WS_DEBUG("Device File Opened...!!!\n");
    return 0;
}

/*
** call when the device file close
*/
static int ws_release(struct inode *inode, struct file *file)
{
    WS_DEBUG("Device File Closed...!!!\n");
    return 0;
}

/* 
** call when the device file is read
*/
static ssize_t ws_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
    struct periplex_ws *ws = filp->private_data;
    WS_DEBUG("Read Function for device %d\n", ws->periplex_id);
    return 0;
}

/* 
** call when the device file is written
*/
static ssize_t ws_write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
    struct periplex_ws *ws = filp->private_data;
    WS_DEBUG("Write function for device %d\n", ws->periplex_id);
    return len;
}

/*
** This function handles the ioctl commands sent to the ws2812b device
*/
static long ws_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{

    struct periplex_ws *ws = file->private_data;
    int ret = 0;

    WS_DEBUG("periplex_id %d, cmd=0x%x\n", ws->periplex_id, cmd);

    switch (cmd)
    {
    case PUSH_DATA:
    {
        struct data d;
        char *kernel_data;

        if (copy_from_user(&d, (struct data *)arg, sizeof(struct data)))
        {
            pr_err("periplex_ws: Failed to copy data from user\n");
            return -EFAULT;
        }

        if (d.length <= 0 || d.length > MAX_BUFFER_SIZE)
        {
            pr_err("Invalid data length: %d\n", d.length);
            return -EINVAL;
        }

        kernel_data = kmalloc(d.length, GFP_KERNEL);
        if (!kernel_data)
        {
            pr_err("periplex_ws: Failed to allocate memory for data\n");
            return -ENOMEM;
        }

        if (copy_from_user(kernel_data, d.data, d.length))
        {
            pr_err("periplex_ws: Failed to copy actual data from user\n");
            kfree(kernel_data);
            return -EFAULT;
        }

        WS_DEBUG("Received %d bytes for device %d\n", d.length, ws->periplex_id);

        mutex_lock(&ws->buf_mutex);
        if (ws->data_buf.current_length + d.length > ws->data_buf.capacity)
        {
            WS_DEBUG("Buffer nearly full (%d + %d > %d), forcing immediate flush\n",
                     ws->data_buf.current_length, d.length, ws->data_buf.capacity);

            mutex_unlock(&ws->buf_mutex);

            ws_flush_work(&ws_work);

            mutex_lock(&ws->buf_mutex);
        }

        ret = append_to_buffer(ws, kernel_data, d.length);
        mutex_unlock(&ws->buf_mutex);

        kfree(kernel_data);

        if (ret < 0)
        {
            pr_err("periplex_ws: Failed to append data to buffer\n");
            return ret;
        }

        check_and_manage_timer();

        break;
    }

    default:
        WS_DEBUG("Unknown ioctl command: 0x%x\n", cmd);
        ret = -EINVAL;
        break;
    }

    return ret;
}

/*
** file operations structure for the ws2812b device
*/
static struct file_operations fops = {
    .owner = THIS_MODULE,
    .read = ws_read,
    .write = ws_write,
    .open = ws_open,
    .unlocked_ioctl = ws_ioctl,
    .release = ws_release,
};

/* 
** Clean up device resources
*/
static void cleanup_device(struct periplex_ws *ws)
{
    if (!ws)
        return;

    if (ws->device)
    {
        device_destroy(ws->dev_class, ws->dev);
        ws->device = NULL;
    }

    if (ws->dev_class)
    {
        class_destroy(ws->dev_class);
        ws->dev_class = NULL;
    }

    cdev_del(&ws->ws_cdev);
    unregister_chrdev_region(ws->dev, 1);
    free_data_buffer(&ws->data_buf);
}

/* 
** Set up device resources
*/
static int setup_device(struct periplex_ws *ws)
{
    int ret;

    /* Allocate character device region */
    ret = alloc_chrdev_region(&ws->dev, 0, 1, ws->name);
    if (ret < 0)
    {
        pr_err("periplex_ws: Cannot allocate major number for ws2812b\n");
        return ret;
    }

    /* Initialize character device */
    cdev_init(&ws->ws_cdev, &fops);
    ws->ws_cdev.owner = THIS_MODULE;

    /* Add character device to system */
    ret = cdev_add(&ws->ws_cdev, ws->dev, 1);
    if (ret < 0)
    {
        pr_err("periplex_ws: Cannot add the device to the system\n");
        goto del_chrdev;
    }

    /* Create device class */
    ws->dev_class = class_create(THIS_MODULE, ws->name);
    if (IS_ERR(ws->dev_class))
    {
        ret = PTR_ERR(ws->dev_class);
        pr_err("periplex_ws: Cannot create the struct class\n");
        goto del_cdev;
    }

    /* Create device node */
    ws->device = device_create(ws->dev_class, NULL, ws->dev, NULL, ws->name);
    if (IS_ERR(ws->device))
    {
        ret = PTR_ERR(ws->device);
        pr_err("periplex_ws: Cannot create the Device\n");
        goto destroy_class;
    }

    return 0;

destroy_class:
    class_destroy(ws->dev_class);
del_cdev:
    cdev_del(&ws->ws_cdev);
del_chrdev:
    unregister_chrdev_region(ws->dev, 1);
    return ret;
}

/* 
** Probe function to initialize the ws2812b device
*/
static int periplex_ws_probe(struct periplex_device *pdev)
{
    int ret;
    struct periplex_ws *ws;
    bool is_first_device = false;

    if (atomic_read(&ws_device_count) == 0)
    {
        is_first_device = true;
    }

    ws = kzalloc(sizeof(struct periplex_ws), GFP_KERNEL);
    if (!ws)
        return -ENOMEM;

    mutex_init(&ws->buf_mutex);
    INIT_LIST_HEAD(&ws->list);

    ret = device_property_read_u32(&pdev->dev, "periplex-id", &ws->periplex_id);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tree for ws2812b\n");
        goto free_ws;
    }

    ret = init_data_buffer(&ws->data_buf);
    if (ret < 0)
    {
        dev_err(&pdev->dev, "Failed to initialize data buffer\n");
        goto free_ws;
    }

    snprintf(ws->name, sizeof(ws->name), "ws2812b-%d", nr);

    /* Setup character device */
    ret = setup_device(ws);
    if (ret < 0)
        goto free_buffer;

    if (is_first_device)
    {
        ret = init_workqueue_and_timer();
        if (ret < 0)
        {
            pr_err("Failed to initialize workqueue and timer\n");
            goto cleanup_dev;
        }
    }

    mutex_lock(&ws_list_mutex);
    list_add_tail(&ws->list, &ws_device_list);
    mutex_unlock(&ws_list_mutex);

    atomic_inc(&ws_device_count);

    pdev->periplex_id = ws->periplex_id;
    pdev->get_periplex_data = read_data_for_ws;

    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, ws);

    pr_info("ws2812b-%d Insert...Done!!!\n", nr);
    nr++;

    return 0;

cleanup_dev:
    cleanup_device(ws);
    return ret;
free_buffer:
    free_data_buffer(&ws->data_buf);
free_ws:
    kfree(ws);
    return ret;
}

/* 
** remove function to clean up the ws2812b device
*/
static int periplex_ws_remove(struct periplex_device *pdev)
{
    struct periplex_ws *ws = periplex_get_drvdata(pdev);
    bool is_last_device = false;

    WS_DEBUG("Removing device, current device count: %d\n", atomic_read(&ws_device_count));

    mutex_lock(&ws_list_mutex);
    list_del(&ws->list);

    atomic_dec(&ws_device_count);
    WS_DEBUG("After decrement, device count: %d\n", atomic_read(&ws_device_count));

    if (atomic_read(&ws_device_count) == 0)
    {
        is_last_device = true;
        WS_DEBUG("This is the last device - will cleanup workqueue\n");
    }
    mutex_unlock(&ws_list_mutex);

    if (is_last_device)
    {
        WS_DEBUG("Calling cleanup_workqueue_and_timer()\n");
        cleanup_workqueue_and_timer();
        WS_DEBUG("Workqueue cleanup completed\n");
    }
    else
    {
        check_and_manage_timer();
    }

    periplex_unlink_device(pdev);

    if (ws->device)
    {
        device_destroy(ws->dev_class, ws->dev);
        ws->device = NULL;
    }

    if (ws->dev_class)
    {
        class_destroy(ws->dev_class);
        ws->dev_class = NULL;
    }

    cdev_del(&ws->ws_cdev);
    unregister_chrdev_region(ws->dev, 1);

    free_data_buffer(&ws->data_buf);

    pr_info("ws2812b-%d Remove...Done!!!\n", nr);
    nr--;
    kfree(ws);

    return 0;
}

/*
** compitable property match with DTSO of ws2812b
*/
static struct of_device_id periplex_ws_dt_match[] = {
    {.compatible = "vicharak,periplex-ws"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_ws_dt_match);

static struct periplex_driver periplex_ws_driver = {
    .probe = periplex_ws_probe,
    .remove = periplex_ws_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_ws_dt_match,
    },
};
module_periplex_driver(periplex_ws_driver);

MODULE_ALIAS("periplex:ws2812b");
MODULE_AUTHOR("Vatsal Kevadiya <vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("ws2812b driver with periodic burst transmission");
MODULE_LICENSE("GPL");