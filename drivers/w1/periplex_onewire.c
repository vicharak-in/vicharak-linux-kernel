/*
** Significance of the onewire.c file:
** 1. The periplex_onewire_search() function discovers all connected OneWire
**    slave devices and lists them under /sys/bus/w1/devices with their
**    unique addressable IDs.
** 2. Parameters of detected slave devices can be accessed, and the underlying
**    operations for reading or writing data are handled by the read_block(),
**    write_block(), and write_byte() functions.
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/moduleparam.h>
#include <linux/w1.h>
#include <linux/types.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

#define DRIVER_NAME "periplex-onewire"

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define OW_DEBUG(fmt, ...)               \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info("periplex_onewire: "fmt, ##__VA_ARGS__); \
    } while (0)

/* One-wire parameters */
u8 OW_RESET_PULSE = 1;
u8 OW_WRITE_PULSE = 2;
u8 OW_READ_PULSE = 3;
u8 OW_SEARCH_PULSE = 4;

/* 
** Operation states
*/
enum onewire_operation_state
{
    OW_IDLE = 0,
    OW_SEARCHING,
    OW_READING
};

/*
** Structure to hold per-onewire adapter data
*/
struct periplex_onewire_data
{
    char dev_id[64];
    int periplex_id;
    struct w1_bus_master bus_master;
    struct periplex_device *pdev;
    struct device *device;

    struct mutex onewire_mutex;
    struct mutex operation_mutex; // for Serialize operations

    // Operation state management
    enum onewire_operation_state current_operation;
    bool operation_in_progress;

    // wait queues and their flags
    wait_queue_head_t wait_queue_ow_search;
    wait_queue_head_t wait_queue_ow_read;
    int wait_queue_flag_search;
    int wait_queue_flag_read;

    //buffer for search data
    char *search_data_onewire;
    int search_length_onewire;

    // FIFO for managing data transfer
    int total_length;
    int expected_len;

    struct kfifo fifo;
};

/*
** function use in read data for onewire
*/
int read_data_for_onewire(struct periplex_device *peri_dev, char *message,
                          const int length)
{
    int actual_stored = 0;
    struct periplex_onewire_data *ow_data = periplex_get_drvdata(peri_dev);

    if (!ow_data)
    {
        pr_err("periplex_onewire: No OneWire data found for device\n");
        return -ENODEV;
    }

    // Check if we're in a valid state to receive data
    mutex_lock(&ow_data->operation_mutex);

    if (!ow_data->operation_in_progress)
    {
        pr_warn("periplex_onewire: Received data but no operation in progress\n");
        mutex_unlock(&ow_data->operation_mutex);
        return -EINVAL;
    }

    if (message[0] != 0)
    {
        // Handle search data
        if (ow_data->current_operation != OW_SEARCHING)
        {
            pr_err("periplex_onewire: Received search data but not in search operation\n");
            mutex_unlock(&ow_data->operation_mutex);
            return -EINVAL;
        }

        OW_DEBUG("Search data received, message[1] = %02x\n", message[1]);

        // Clean up any existing search data first
        if (ow_data->search_data_onewire)
        {
            devm_kfree(ow_data->device, ow_data->search_data_onewire);
            ow_data->search_data_onewire = NULL;
        }

        ow_data->search_length_onewire = length;
        ow_data->search_data_onewire = devm_kmalloc(ow_data->device, length, GFP_KERNEL);

        if (!ow_data->search_data_onewire)
        {
            pr_err("periplex_onewire: Failed to allocate memory for search_data_onewire\n");
            mutex_unlock(&ow_data->operation_mutex);
            return -ENOMEM;
        }

        memcpy(ow_data->search_data_onewire, message, length);

        ow_data->wait_queue_flag_search = 1;
        mutex_unlock(&ow_data->operation_mutex);
        wake_up_interruptible(&ow_data->wait_queue_ow_search);

        msleep(5); // Sleep for 5ms to allow processing
    }
    else
    {
        // Handle read data
        if (ow_data->current_operation != OW_READING)
        {
            pr_err("periplex_onewire: Received read data but not in read operation\n");
            mutex_unlock(&ow_data->operation_mutex);
            return -EINVAL;
        }

        mutex_unlock(&ow_data->operation_mutex);

        // Use existing read data handling with proper locking
        mutex_lock(&ow_data->onewire_mutex);

        actual_stored = kfifo_in(&ow_data->fifo, message + 1, length - 1);
        if (actual_stored == 0)
        {
            dev_err(ow_data->device, "FIFO full, cannot store %d bytes\n", length - 1);
            mutex_unlock(&ow_data->onewire_mutex);
            return -ENOSPC;
        }

        OW_DEBUG("FIFO status: stored=%d, expected=%d, fifo_len=%u\n",
                 actual_stored, ow_data->expected_len, kfifo_len(&ow_data->fifo));

        if (ow_data->expected_len > 0 &&
            kfifo_len(&ow_data->fifo) >= ow_data->expected_len)
        {
            ow_data->wait_queue_flag_read = 1;
            mutex_unlock(&ow_data->onewire_mutex);
            wake_up_interruptible(&ow_data->wait_queue_ow_read);
        }
        else
        {
            mutex_unlock(&ow_data->onewire_mutex);
        }
    }

    return 0;
}

/*
** convert u8-array to u64
*/
static inline u64 convert_u8_array_to_u64_le(u8 buf[8])
{
    u64 result = 0;

    /* Combine bytes with bitwise operations (little-endian format) */
    result = ((u64)buf[7] << 56) |
             ((u64)buf[6] << 48) |
             ((u64)buf[5] << 40) |
             ((u64)buf[4] << 32) |
             ((u64)buf[3] << 24) |
             ((u64)buf[2] << 16) |
             ((u64)buf[1] << 8) |
             ((u64)buf[0]);

    return result;
}

/*
** Touch_bit function
*/
static u8 periplex_onewire_touch_bit(void *data, u8 bit)
{
    OW_DEBUG("touch-bit one-wire\n");
    return bit;
}

/*
** Reset the one-wire bus
*/
static u8 periplex_onewire_reset_bus(void *data)
{
    struct periplex_onewire_data *ow_data = data;
    u8 message[1] = {0};
    message[0] = OW_RESET_PULSE;
    set_periplex_data(ow_data->periplex_id, 1, (char *)message);
    OW_DEBUG("Resetting one-wire bus\n");
    return 0;
}

/*
** periplex_onewire write block
*/
static void periplex_onewire_write_block(void *data, const u8 *buf, int len)
{
    int i;
    u8 len_u8 = 0;
    int chunk_size = 0;
    u8 *message = NULL;
    int remaining_len = 0;
    int current_offset = 0;
    struct periplex_onewire_data *ow_data = data;

    OW_DEBUG("write one-wire length %d\n", len);

    remaining_len = len;

    while (remaining_len > 0)
    {
        chunk_size = min(remaining_len, MAX_CHUNK_SIZE);

        message = kmalloc(chunk_size + 1, GFP_KERNEL);
        if (!message)
        {
            pr_info("periplex_onewire: Failed to allocate memory\n");
            return;
        }

        // Initialize the buffer to zero
        memset(message, 0, chunk_size + 1);
        len_u8 = (u8)(chunk_size - 1);
        message[0] = ((len_u8 << 3) & 0xF8) | OW_WRITE_PULSE;

        // Print buffer contents
        if (buf && chunk_size > 0)
        {
            OW_DEBUG("buffer contents: ");
            for (i = 0; i < len; i++)
            {
                message[i + 1] = buf[i];
                OW_DEBUG("0x%02x ", buf[i]);
            }
        }

        set_periplex_data(ow_data->periplex_id, chunk_size + 1, (char *)message);
        kfree(message);

        current_offset += chunk_size;
        remaining_len -= chunk_size;
    }

    return;
}

/*
** periplex_onewire read block
*/
static u8 periplex_onewire_read_block(void *data, u8 *buf, int len)
{
    struct periplex_onewire_data *ow_data = data;
    int ret = 0;
    int chunk_size = 0;
    int remaining_len = 0;
    int current_offset = 0;
    u8 len_u8 = 0;
    u8 message[1] = {0};
    long timeout = msecs_to_jiffies(1000);

    if (!ow_data || !buf || len <= 0)
    {
        pr_err("periplex_onewire: Invalid parameters for read_block\n");
        return -EINVAL;
    }

    // Acquire operation lock
    if (!mutex_trylock(&ow_data->operation_mutex))
    {
        pr_warn("periplex_onewire: Read operation blocked - another operation in progress\n");
        return -EBUSY;
    }

    if (ow_data->operation_in_progress)
    {
        pr_warn("periplex_onewire: Read blocked - operation already in progress\n");
        mutex_unlock(&ow_data->operation_mutex);
        return -EBUSY;
    }

    // Set operation state
    ow_data->current_operation = OW_READING;
    ow_data->operation_in_progress = true;
    mutex_unlock(&ow_data->operation_mutex);

    OW_DEBUG("Starting OneWire read for %d bytes\n", len);

    // Reset FIFO and read state
    mutex_lock(&ow_data->onewire_mutex);
    kfifo_reset(&ow_data->fifo);
    ow_data->wait_queue_flag_read = 0;
    mutex_unlock(&ow_data->onewire_mutex);

    remaining_len = len;
    current_offset = 0;

    while (remaining_len > 0)
    {
        chunk_size = min(remaining_len, MAX_CHUNK_SIZE);

        // Set expected length for this chunk
        mutex_lock(&ow_data->onewire_mutex);
        ow_data->expected_len = chunk_size;
        ow_data->wait_queue_flag_read = 0;
        mutex_unlock(&ow_data->onewire_mutex);

        // Send read command
        len_u8 = (u8)(chunk_size - 1);
        message[0] = (((len_u8 << 3) & 0xF8) | OW_READ_PULSE);
        set_periplex_data(ow_data->periplex_id, 1, (char *)message);

        // Wait for data
        ret = wait_event_interruptible_timeout(ow_data->wait_queue_ow_read,
                                               ow_data->wait_queue_flag_read != 0,
                                               timeout);
        if (ret <= 0)
        {
            pr_err("periplex_onewire: Read timeout or interrupted: %d\n", ret);
            ret = (ret == 0) ? -ETIMEDOUT : ret;
            goto exit_with_error;
        }

        // Extract data from FIFO
        mutex_lock(&ow_data->onewire_mutex);
        if (kfifo_len(&ow_data->fifo) < chunk_size)
        {
            pr_err("periplex_onewire: Insufficient data in FIFO: %u < %d\n",
                   kfifo_len(&ow_data->fifo), chunk_size);
            mutex_unlock(&ow_data->onewire_mutex);
            ret = -EIO;
            goto exit_with_error;
        }

        ret = kfifo_out(&ow_data->fifo, buf + current_offset, chunk_size);
        ow_data->expected_len = 0;
        mutex_unlock(&ow_data->onewire_mutex);

        if (ret != chunk_size)
        {
            pr_err("periplex_onewire: FIFO extraction failed: %d != %d\n", ret, chunk_size);
            ret = -EIO;
            goto exit_with_error;
        }

        current_offset += chunk_size;
        remaining_len -= chunk_size;
    }

    // Clear operation state
    mutex_lock(&ow_data->operation_mutex);
    ow_data->current_operation = OW_IDLE;
    ow_data->operation_in_progress = false;
    mutex_unlock(&ow_data->operation_mutex);

    OW_DEBUG("OneWire read completed successfully\n");
    return len;

exit_with_error:
    // Clean up on error
    mutex_lock(&ow_data->onewire_mutex);
    kfifo_reset(&ow_data->fifo);
    ow_data->expected_len = 0;
    ow_data->wait_queue_flag_read = 0;
    mutex_unlock(&ow_data->onewire_mutex);

    // Clear operation state
    mutex_lock(&ow_data->operation_mutex);
    ow_data->current_operation = OW_IDLE;
    ow_data->operation_in_progress = false;
    mutex_unlock(&ow_data->operation_mutex);

    return ret;
}

/*
** periplex_onewire write byte
*/
static void periplex_onewire_write_byte(void *data, u8 byte)
{
    struct periplex_onewire_data *ow_data = data;
    int write_length = 0;
    u8 message[2] = {0};
    message[0] = ((write_length << 4) | OW_WRITE_PULSE) & 0xFF;
    message[1] = byte & 0xFF;
    set_periplex_data(ow_data->periplex_id, 2, (char *)message);
    OW_DEBUG("periplex_onewire_write_byte : 0x%02x\n", byte);
    return;
}

/*
** periplex_onewire read byte
*/
static u8 periplex_onewire_read_byte(void *data)
{
    OW_DEBUG("periplex_onewire_read_byte\n");
    return 0;
}

/*
** periplex_onewire search
*/
static void periplex_onewire_search(void *data, struct w1_master *master,
                                    u8 search_type, w1_slave_found_callback callback)
{
    struct periplex_onewire_data *ow_data = data;
    int ret = 0;
    int count = 0;
    u8 message[4] = {0};
    u8 buf[64] = {0};
    u64 rom_code = 0;
    int index = 0;
    int write_length = 0;
    int device_count = 0;
    long timeout = msecs_to_jiffies(1000);

    if (!ow_data || !callback)
    {
        pr_err("periplex_onewire: Invalid parameters for search\n");
        return;
    }

    /* Acquire operation lock to prevent concurrent operations */
    if (!mutex_trylock(&ow_data->operation_mutex))
    {
        pr_warn("periplex_onewire: Search operation blocked - another operation in progress\n");
        return;
    }

    /* Check if another operation is already running */
    if (ow_data->operation_in_progress)
    {
        pr_warn("periplex_onewire: Search blocked - operation already in progress\n");
        mutex_unlock(&ow_data->operation_mutex);
        return;
    }

    /* Set operation state */
    ow_data->current_operation = OW_SEARCHING;
    ow_data->operation_in_progress = true;
    ow_data->wait_queue_flag_search = 0;

    /* Clean up any existing search data */
    if (ow_data->search_data_onewire)
    {
        devm_kfree(ow_data->device, ow_data->search_data_onewire);
        ow_data->search_data_onewire = NULL;
        ow_data->search_length_onewire = 0;
    }

    mutex_unlock(&ow_data->operation_mutex);

    OW_DEBUG("Starting OneWire search, search_type=%02x\n", search_type);

    /* Send search command */
    message[0] = OW_RESET_PULSE;
    message[1] = ((write_length << 4) | OW_WRITE_PULSE) & 0xFF;
    message[2] = (search_type) & 0xFF;
    message[3] = (OW_SEARCH_PULSE) & 0xFF;

    set_periplex_data(ow_data->periplex_id, 4, (char *)message);

    while (1)
    {
        ret = wait_event_interruptible_timeout(ow_data->wait_queue_ow_search,
                                               ow_data->wait_queue_flag_search != 0,
                                               timeout);

        if (ret == 0)
        {
            pr_err("periplex_onewire: OneWire search timeout\n");
            break;
        }
        else if (ret < 0)
        {
            pr_err("periplex_onewire: OneWire search interrupted: %d\n", ret);
            break;
        }

        /* Check if we still have valid search data */
        mutex_lock(&ow_data->operation_mutex);
        if (!ow_data->search_data_onewire || ow_data->current_operation != OW_SEARCHING)
        {
            mutex_unlock(&ow_data->operation_mutex);
            break;
        }

        count++;
        index = ow_data->search_data_onewire[0] & 0x0F;
        if (index > 0 && index <= 8)
        {
            buf[index - 1] = ow_data->search_data_onewire[1];
        }

        /* Clean up current search data */
        devm_kfree(ow_data->device, ow_data->search_data_onewire);
        ow_data->search_data_onewire = NULL;
        ow_data->wait_queue_flag_search = 0;

        mutex_unlock(&ow_data->operation_mutex);

        if ((count % 8) == 0)
        {
            rom_code = convert_u8_array_to_u64_le(buf);
            if (rom_code != 0)
            {
                pr_info("periplex_onewire: Found 1-Wire device with ROM ID: 0x%016llx\n", rom_code);
                callback(master, rom_code);
                device_count++;
            }
            pr_info("periplex_onewire: 1-Wire search completed. Found %d devices\n", device_count);
            memset(buf, 0, sizeof(buf));
            // break; /* Exit after finding devices */
        }

        OW_DEBUG("Search progress: count=%d\n", count);
    }

    /* Clear operation state */
    mutex_lock(&ow_data->operation_mutex);
    ow_data->current_operation = OW_IDLE;
    ow_data->operation_in_progress = false;

    /* Final cleanup */
    if (ow_data->search_data_onewire)
    {
        devm_kfree(ow_data->device, ow_data->search_data_onewire);
        ow_data->search_data_onewire = NULL;
        ow_data->search_length_onewire = 0;
    }

    mutex_unlock(&ow_data->operation_mutex);

    OW_DEBUG("OneWire search operation completed\n");
}

static u8 periplex_onewire_triplet(void *data, u8 bit)
{

    OW_DEBUG("periplex_onewire_triplet %d\n", bit);
    return bit;
}

/*
** Probe function for device registration
*/
static int periplex_onewire_probe(struct periplex_device *pdev)
{
    struct periplex_onewire_data *ow_data;
    int ret;

    ow_data = devm_kzalloc(&pdev->dev, sizeof(struct periplex_onewire_data), GFP_KERNEL);
    if (!ow_data)
    {
        dev_err(&pdev->dev, "Failed to allocate memory for OneWire data\n");
        return -ENOMEM;
    }

    ret = kfifo_alloc(&ow_data->fifo, FIFO_SIZE, GFP_KERNEL);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to allocate FIFO\n");
        return ret;
    }

    /* Initialize data structure */
    ow_data->total_length = 0;
    ow_data->expected_len = 0;
    ow_data->device = &pdev->dev;
    ow_data->search_data_onewire = NULL;
    ow_data->search_length_onewire = 0;
    ow_data->wait_queue_flag_search = 0;
    ow_data->wait_queue_flag_read = 0;

    /* Initialize operation state */
    ow_data->current_operation = OW_IDLE;
    ow_data->operation_in_progress = false;

    /* Initialize mutexes */
    mutex_init(&ow_data->onewire_mutex);
    mutex_init(&ow_data->operation_mutex); /* NEW */
    init_waitqueue_head(&ow_data->wait_queue_ow_search);
    init_waitqueue_head(&ow_data->wait_queue_ow_read);

    /* Read device tree properties */
    if (device_property_read_u32(&pdev->dev, "periplex-id", &ow_data->periplex_id))
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tree\n");
        ret = -EINVAL;
        goto err_free_fifo;
    }

    /* Initialize bus master functions */
    ow_data->bus_master.data = ow_data;
    ow_data->bus_master.touch_bit = periplex_onewire_touch_bit;
    ow_data->bus_master.reset_bus = periplex_onewire_reset_bus;
    ow_data->bus_master.write_block = periplex_onewire_write_block;
    ow_data->bus_master.read_block = periplex_onewire_read_block;
    ow_data->bus_master.write_byte = periplex_onewire_write_byte;
    ow_data->bus_master.read_byte = periplex_onewire_read_byte;
    ow_data->bus_master.search = periplex_onewire_search;
    ow_data->bus_master.triplet = periplex_onewire_triplet;
    ow_data->pdev = pdev;

    snprintf(ow_data->dev_id, sizeof(ow_data->dev_id), "onewire-%d", ow_data->periplex_id);
    ow_data->bus_master.dev_id = ow_data->dev_id;

    /* Link with periplex subsystem */
    pdev->periplex_id = ow_data->periplex_id;
    pdev->get_periplex_data = read_data_for_onewire;

    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, ow_data);

    /* Register with w1 subsystem */
    ret = w1_add_master_device(&ow_data->bus_master);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register w1 master device\n");
        goto err_unlink_device;
    }

    pr_info("periplex_onewire: OneWire Bus Driver Added: %s (periplex_id: %d)\n",
            ow_data->dev_id, ow_data->periplex_id);

    return 0;

err_unlink_device:
    periplex_unlink_device(pdev);
    periplex_set_drvdata(pdev, NULL);
err_free_fifo:
    kfifo_free(&ow_data->fifo);
    return ret;
}

/*
** Remove function for device unregistration
*/
static int periplex_onewire_remove(struct periplex_device *pdev)
{
    struct periplex_onewire_data *ow_data = periplex_get_drvdata(pdev);

    if (!ow_data)
    {
        dev_err(&pdev->dev, "No OneWire data found for device\n");
        return -ENODEV;
    }

    OW_DEBUG("Removing OneWire Bus Driver: %s (periplex_id: %d)\n",
             ow_data->dev_id, ow_data->periplex_id);

    // Clear driver data first to prevent further access
    periplex_set_drvdata(pdev, NULL);

    // Wake up any waiting threads before cleanup
    mutex_lock(&ow_data->onewire_mutex);
    ow_data->wait_queue_flag_search = 1;
    ow_data->wait_queue_flag_read = 1;
    mutex_unlock(&ow_data->onewire_mutex);

    wake_up_interruptible(&ow_data->wait_queue_ow_search);
    wake_up_interruptible(&ow_data->wait_queue_ow_read);

    // Remove w1 master device first
    w1_remove_master_device(&ow_data->bus_master);

    // Clean up any remaining search data with proper locking
    mutex_lock(&ow_data->onewire_mutex);
    if (ow_data->search_data_onewire)
    {
        devm_kfree(ow_data->device, ow_data->search_data_onewire);
        ow_data->search_data_onewire = NULL;
        ow_data->search_length_onewire = 0;
    }
    mutex_unlock(&ow_data->onewire_mutex);

    // Free FIFO (this was allocated with kfifo_alloc)
    kfifo_free(&ow_data->fifo);

    // Unlink from periplex framework
    periplex_unlink_device(pdev);

    pr_info("periplex_onewire: OneWire Bus Driver removed successfully\n");
    return 0;
}

/*
** Compatible property match with DTSO of one-wire
*/
static struct of_device_id periplex_onewire_dt_match[] = {
    {.compatible = "vicharak,periplex-onewire"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_onewire_dt_match);

static struct periplex_driver periplex_onewire_driver = {
    .probe = periplex_onewire_probe,
    .remove = periplex_onewire_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_onewire_dt_match,
    },
};
module_periplex_driver(periplex_onewire_driver);

MODULE_ALIAS("periplex:onewire");
MODULE_AUTHOR("vatsal Kevadiya<vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("One-Wire Driver for the Periplex");
MODULE_LICENSE("GPL");