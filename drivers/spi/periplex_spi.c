/*
** Significant of spi.c file :
** 1.Make multiple spi chip with the use of dtso and create the
** spi* series into the /sys/class/spi_master.
** 2.Set SPI max-frequency and chip-select number to attach flash(for store data).
*/

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/kfifo.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

#define DRIVER_NAME "periplex-spi"
#define MAX_FPGA_FREQ 100000000
#define MAX_CHUNK_SIZE 32767
#define FIFO_SIZE 32767

/*
** Debug flag (can be set via module parameter)
*/
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define SPI_DEBUG(fmt, ...)              \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info("periplex_spi: "fmt, ##__VA_ARGS__); \
    } while (0)

/*
** struct periplex_spi_data - Runtime info holder for SPI driver
*/
struct periplex_spi_data
{
    struct spi_controller *controller;
    int periplex_id;
    struct device *device;

    struct mutex spi_mutex; /* Mutex for SPI operations */

    /* waitqueue */
    wait_queue_head_t wait_queue_spi;
    int wait_flag;

    int total_length; /* Current length of accumulated data */
    int expected_len; /* Expected length for read data */

    struct kfifo fifo; /* FIFO for managing data transfer */
};

/*
** print buffer in debug mode
*/
static void debug_print_buffer(const char *prefix, const void *buf, int len)
{
    const unsigned char *data = (const unsigned char *)buf;
    int i = 0;

    if (!debug || !buf || len <= 0)
        return;

    pr_info("%s (%d bytes): ", prefix, len);
    for (i = 0; i < len && i < 16; i++)
    {
        pr_cont("%02X ", data[i]);
    }
    if (len > 16)
    {
        pr_cont("...");
    }
    pr_cont("\n");
}

/*
** read data for spi from periplex
*/
int read_data_for_spi(struct periplex_device *pdev, char *message, const int len)
{
    struct periplex_spi_data *peri_spi = periplex_get_drvdata(pdev);
    int actual_stored = 0;

    if (!message || len <= 0)
    {
        pr_err("periplex_spi: Invalid message or length\n");
        return -EINVAL;
    }

    if (!peri_spi)
    {
        pr_err("periplex_spi: SPI data not available\n");
        return -ENODEV;
    }

    debug_print_buffer("RX_DATA", message, len);

    mutex_lock(&peri_spi->spi_mutex);

    /* Store data in FIFO */
    actual_stored = kfifo_in(&peri_spi->fifo, message, len);
    if (actual_stored == 0)
    {
        dev_err(peri_spi->device, "FIFO full, cannot store %d bytes (current fifo_len=%u)\n",
                len, kfifo_len(&peri_spi->fifo));
        mutex_unlock(&peri_spi->spi_mutex);
        return -ENOSPC;
    }

    if (actual_stored != len)
    {
        dev_warn(peri_spi->device, "FIFO partially full: stored %d out of %d bytes\n",
                 actual_stored, len);
    }

    SPI_DEBUG("FIFO status: stored=%d, expected=%d, fifo_len=%u\n",
              actual_stored, peri_spi->expected_len, kfifo_len(&peri_spi->fifo));

    /* Check if we have enough data to wake up waiting thread */
    if (peri_spi->expected_len > 0 &&
        kfifo_len(&peri_spi->fifo) >= peri_spi->expected_len)
    {
        SPI_DEBUG("Waking up SPI thread: fifo_len=%u >= expected_len=%d\n",
                  kfifo_len(&peri_spi->fifo), peri_spi->expected_len);

        peri_spi->wait_flag = 1;
        mutex_unlock(&peri_spi->spi_mutex);
        wake_up_interruptible(&peri_spi->wait_queue_spi);
    }
    else
    {
        mutex_unlock(&peri_spi->spi_mutex);

        if (peri_spi->expected_len > 0)
        {
            SPI_DEBUG("Still waiting for more data: fifo_len=%u < expected_len=%d\n",
                      kfifo_len(&peri_spi->fifo), peri_spi->expected_len);
        }
    }

    return 0;
}

/*
** get_clk_per_half_bit - Calculate clock cycles per half bit
*/
static uint8_t get_clk_per_half_bit(u32 sclk_freq)
{
    return MAX_FPGA_FREQ / (2 * sclk_freq);
}

/*
** Function to set spi chip select through periplex configuration
*/
static void periplex_spi_set_cs(struct spi_device *spi, bool enable)
{
    struct periplex_spi_data *peri_spi = spi_controller_get_devdata(spi->controller);
    int config = 0;
    config = (spi->chip_select << 8) + enable;
    set_periplex_configuration(peri_spi->periplex_id, 1, config);
}

/*
**periplex_spi_setup - callback to setup the controller
*/
static int periplex_spi_setup(struct spi_device *spi)
{
    /*
        SPI CONFIG FRAME
        ┌─────────┬─────────────────┬────────┬───────┬────┬────┐
        │ config  │       res       │clk per │  res  │cpol│cpha│
        │         │                 │half bit│       │    │    │
        └─────────┴─────────────────┴────────┴───────┴────┴────┘
        <--------> <---------------> <------> <-----> <--> <-->
          8 bits       16 bits        8 bits   6 bits   1    1
    */

    struct periplex_spi_data *peri_spi = spi_controller_get_devdata(spi->controller);
    int config = 0;
    uint8_t clk_per_half_bit = 0;

    SPI_DEBUG("max-speed %d\n", spi->max_speed_hz);
    // TODO: check if the device freq is greater than master freq or not
    clk_per_half_bit = get_clk_per_half_bit(spi->max_speed_hz);
    config = (clk_per_half_bit << 8) + (spi->mode);
    set_periplex_configuration(peri_spi->periplex_id, 0, config);
    return 0;
}

static void transfer_buf(struct spi_controller *ctlr, const void *buf, int len, int offset, bool read_write)
{
    /*
        SPI DATA FRAME
        ┌──────┬──────────┬──────────┬────────────┐
        │ R/W  │ LengthHi │ LengthLo │Data(0..N-1)│
        └──────┴──────────┴──────────┴────────────┘
         <----> <-------------------> <----------->
           1bit        15 bits           N * 8 bits
    */

    struct periplex_spi_data *peri_spi = spi_controller_get_devdata(ctlr);
    char *data = NULL;

    /* Allocate memory: 2 bytes header + payload */
    data = kmalloc(len + 2, GFP_KERNEL);
    if (!data) {
        pr_err("periplex_spi: Failed to allocate memory for chunk\n");
        return;
    }

    /* Pack R/W and length */
    data[0] = (read_write << 7) | ((len >> 8) & 0x7F);  // MSB: R/W, next 7 bits: len[14:8]
    data[1] = len & 0xFF;                               // lower 8 bits

    debug_print_buffer("Transfer_buf", buf + offset, len);

    /* Copy data for this chunk */
    if (len > 0 && memcpy(data + 2, buf + offset, len) == NULL) {
        pr_err("periplex_spi: Not able to copy chunk data\n");
        kfree(data);
        return;
    }

    if (len > 0) {
	    SPI_DEBUG("len is %d\n",len);
        set_periplex_data(peri_spi->periplex_id, len + 2, data);
    }

    kfree(data);
}

/*
** periplex_spi_transfer_one_message - callback to transfer spi message
*/
static int periplex_spi_transfer_one_message(struct spi_controller *ctlr, struct spi_message *spi_msg)
{
    struct periplex_spi_data *peri_spi = spi_controller_get_devdata(ctlr);
    struct spi_transfer *transfer = NULL;
    int ret = 0;
    int remaining_len = 0;
    int current_offset = 0;
    int chunk_size = 0;

    if (!ctlr || !spi_msg)
    {
        pr_err("periplex_spi: Invalid controller or message\n");
        return -EINVAL;
    }

    periplex_spi_set_cs(spi_msg->spi, 0);

    list_for_each_entry(transfer, &spi_msg->transfers, transfer_list)
    {

        /* Initialize variables for chunked transfer */
        remaining_len = transfer->len;
        current_offset = 0;

        if (transfer->tx_buf && transfer->len != 0)
        {
            SPI_DEBUG("WRITE:Transfer Length: %d\n", transfer->len);

            /* Process data in chunks */
            while (remaining_len > 0)
            {
                chunk_size = min(remaining_len, MAX_CHUNK_SIZE);
                /* Transfer current chunk */
                transfer_buf(ctlr, transfer->tx_buf, chunk_size, current_offset, 0);

                current_offset += chunk_size;
                remaining_len -= chunk_size;
            }
        }

        if (transfer->rx_buf && transfer->len != 0)
        {
            /* Reset for read operation */
            remaining_len = transfer->len;
            current_offset = 0;

            SPI_DEBUG("READ:Transfer Length: %d\n", transfer->len);

            /* Clear any residual data in FIFO before starting read operation */
            mutex_lock(&peri_spi->spi_mutex);
            kfifo_reset(&peri_spi->fifo);
            peri_spi->wait_flag = 0;
            mutex_unlock(&peri_spi->spi_mutex);

            while (remaining_len > 0)
            {
                chunk_size = min(remaining_len, MAX_CHUNK_SIZE);

                SPI_DEBUG("Requesting chunk: size=%d, offset=%d, remaining=%d\n",
                          chunk_size, current_offset, remaining_len);

                /* Set expected length and reset wait flag */
                mutex_lock(&peri_spi->spi_mutex);
                peri_spi->expected_len = chunk_size;
                peri_spi->wait_flag = 0;
                mutex_unlock(&peri_spi->spi_mutex);

                /* Send read request for current chunk */
                transfer_buf(ctlr, transfer->rx_buf, chunk_size, current_offset, 1);

                /* Wait for data to arrive */
                ret = wait_event_interruptible_timeout(peri_spi->wait_queue_spi,
                                                       peri_spi->wait_flag != 0,
                                                       msecs_to_jiffies(5000));
                if (ret == 0)
                {
                    pr_err("periplex_spi: Timed out waiting for read data (chunk_size=%d)\n", chunk_size);
                    ret = -ETIMEDOUT;
                    goto exit_with_error;
                }
                else if (ret < 0)
                {
                    pr_err("periplex_spi: RX wait interrupted: %d\n", ret);
                    goto exit_with_error;
                }

                /* Process received data */
                mutex_lock(&peri_spi->spi_mutex);

                /* Check if we have enough data in FIFO */
                if (kfifo_len(&peri_spi->fifo) < chunk_size)
                {
                    SPI_DEBUG("FIFO has %u bytes, expected: %d\n",
                              kfifo_len(&peri_spi->fifo), chunk_size);
                    dev_err(peri_spi->device, "Insufficient SPI_READ data: %u bytes, expected %d\n",
                            kfifo_len(&peri_spi->fifo), chunk_size);
                    mutex_unlock(&peri_spi->spi_mutex);
                    ret = -EIO;
                    goto exit_with_error;
                }

                /* Extract exact chunk_size bytes from FIFO to rx_buf */
                ret = kfifo_out(&peri_spi->fifo, transfer->rx_buf + current_offset, chunk_size);
                if (ret != chunk_size)
                {
                    dev_err(peri_spi->device, "FIFO extraction failed: got %d bytes, expected %d\n",
                            ret, chunk_size);
                    mutex_unlock(&peri_spi->spi_mutex);
                    ret = -EIO;
                    goto exit_with_error;
                }

                debug_print_buffer("RX_CHUNK", transfer->rx_buf + current_offset, chunk_size);

                /* Reset expected length for this chunk */
                peri_spi->expected_len = 0;

                SPI_DEBUG("After consuming chunk: fifo_len=%u\n", kfifo_len(&peri_spi->fifo));

                mutex_unlock(&peri_spi->spi_mutex);

                /* Update loop variables */
                current_offset += chunk_size;
                remaining_len -= chunk_size;

                SPI_DEBUG("Chunk processed: size=%d, remaining=%d, offset=%d\n",
                          chunk_size, remaining_len, current_offset);
            }

            SPI_DEBUG("Reading complete for transfer\n");
        }

        spi_msg->actual_length += transfer->len;
    }

    periplex_spi_set_cs(spi_msg->spi, 1);
    spi_msg->status = 0;
    spi_finalize_current_message(ctlr);
    return 0;

exit_with_error:
    /* Clean up on error */
    mutex_lock(&peri_spi->spi_mutex);
    kfifo_reset(&peri_spi->fifo);
    peri_spi->expected_len = 0;
    peri_spi->wait_flag = 0;
    mutex_unlock(&peri_spi->spi_mutex);

    periplex_spi_set_cs(spi_msg->spi, 1);
    spi_msg->status = ret;
    spi_finalize_current_message(ctlr);
    return ret;
}

/*
** Called when a matched spi controller device is found
*/
static int probe_periplex_spi_controller(struct periplex_device *pdev)
{
    int ret = 0;
    u32 num_cs = 0;
    u32 max_freq = 0;
    struct spi_controller *ctlr = NULL;
    struct periplex_spi_data *peri_spi = NULL;

    peri_spi = devm_kzalloc(&pdev->dev, sizeof(struct periplex_spi_data), GFP_KERNEL);
    if (!peri_spi)
    {
        dev_err(&pdev->dev, "Failed to allocate memory for SPI data\n");
        return -ENOMEM;
    }

    ret = kfifo_alloc(&peri_spi->fifo, FIFO_SIZE, GFP_KERNEL);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to allocate FIFO\n");
        kfree(peri_spi);
        return ret;
    }

    ctlr = devm_spi_alloc_master(&pdev->dev, sizeof(struct periplex_spi_data));
    if (!ctlr)
    {
        dev_err(&pdev->dev, "Failed to allocate SPI master\n");
        return -ENOMEM;
    }

    ret = device_property_read_u32(&pdev->dev, "periplex-id", &peri_spi->periplex_id);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tree for peri_spi\n");
        return ret;
    }

    if (device_property_read_u32(&pdev->dev, "vicharak,peri-spi-cs-num", &num_cs))
    {
        num_cs = 1;
    }

    if (device_property_read_u32(&pdev->dev, "vicharak,peri-spi-max-freq", &max_freq))
    {
        max_freq = 25000000;
    }

    // Initialize controller
    ctlr->mode_bits = SPI_CPOL | SPI_CPHA;
    ctlr->bits_per_word_mask = SPI_BPW_MASK(8);
    ctlr->bus_num = -1;
    ctlr->num_chipselect = num_cs;
    ctlr->max_speed_hz = max_freq;

    ctlr->transfer_one_message = periplex_spi_transfer_one_message;
    ctlr->set_cs = periplex_spi_set_cs;
    ctlr->setup = periplex_spi_setup;
    ctlr->dev.parent = &pdev->dev;
    ctlr->dev.of_node = pdev->dev.of_node;

    peri_spi->controller = ctlr;
    peri_spi->device = &pdev->dev;
    peri_spi->expected_len = 0;
    peri_spi->wait_flag = 0;
    init_waitqueue_head(&peri_spi->wait_queue_spi);
    mutex_init(&peri_spi->spi_mutex);

    pdev->periplex_id = peri_spi->periplex_id;
    pdev->get_periplex_data = read_data_for_spi;

    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, peri_spi);
    spi_controller_set_devdata(ctlr, peri_spi);

    ret = spi_register_controller(peri_spi->controller);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register SPI master\n");
        return ret;
    }
    pr_info("periplex_spi: Periplex SPI controller registered %d\n", peri_spi->periplex_id);
    return 0;
}

/*
** called when device is removed from the system
*/
static int remove_periplex_spi_controller(struct periplex_device *pdev)
{
    struct periplex_spi_data *peri_spi = periplex_get_drvdata(pdev);
    if (!peri_spi)
    {
        dev_err(&pdev->dev, "No SPI data found for device\n");
        return -ENODEV;
    }
    if (peri_spi->controller)
    {
        spi_unregister_controller(peri_spi->controller);
        pr_info("periplex_spi: Removing SPI controller with periplex_id: %d\n", peri_spi->periplex_id);
    }
    
    // Clean up
    kfifo_free(&peri_spi->fifo);

    periplex_unlink_device(pdev);
    return 0;
}

struct of_device_id periplex_spi_dt_match[] = {
    {.compatible = "vicharak,periplex-spi"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_spi_dt_match);

struct periplex_driver periplex_spi_driver = {
    .probe = probe_periplex_spi_controller,
    .remove = remove_periplex_spi_controller,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_spi_dt_match}};

module_periplex_driver(periplex_spi_driver);

MODULE_ALIAS("periplex:spi");
MODULE_AUTHOR("shailparmar03");
MODULE_DESCRIPTION("SPI Driver for the Periplex");
MODULE_LICENSE("GPL");