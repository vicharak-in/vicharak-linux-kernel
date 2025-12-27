/*
 * Purpose of Single Wire Interface (SWI):
 * 1. Make multiple SWI devices using the DTBO and create devices in /dev/dht-X
 *    accordingly. 
 *
 * 2. Capablities of this drivers is to get temperature and humidity from the
 *    DHT11/DHT21 and DHT22 sensors.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/delay.h>

#include <linux/peripheral.h>

#define SET_DHT_SENSOR_TYPE _IOWR('a', 'b', enum dht_type *) 
#define MEASURE_DHT_VALUE _IOWR('a', 'c', void *) 
#define GET_TEMP_HUMD _IOWR('a', 'd', char *) 

#define DRIVER_NAME "periplex-dht"

/*
 *       | max pulses | pull down time (ms)
 * ----------------------------------------
 * DHT11 |     81     |         18 
 * DHT21 |    129     |          1
 * DHT22 |     81     |          1
 *
 * Periplex needs the pull down time in ticks on FPGA side,
 * Peripheral clock = 50 MHz
 * Pull down time will be = pull down time (ms) / 20 ns
 */

#define DHT11_PULL_DOWN_TIME 900000
#define DHT21_PULL_DOWN_TIME 50000
#define DHT22_PULL_DOWN_TIME 50000

#define DHT11_MAX_PULSES 81
#define DHT21_MAX_PULSES 129
#define DHT22_MAX_PULSES 81

#define PULL_DOWN_TIME_CONF_ID 0
#define MAX_PULSES_CONF_ID 1

#define MAX_DEVICE_NAME 64
#define MAX_BYTES_DATA 8

/* Mutex for the DHT sensors */
struct mutex dht_mutex;

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

enum dht_type {
	DHT11 = 0,
	DHT21 = 1,
	DHT22 = 2
};

struct buf {
	int humidity;
	int temperature;
};

struct periplex_dht {
	/* Periplex required fields */
	dev_t dev;
	char name[MAX_DEVICE_NAME];
	int periplex_id;
	struct class *dev_class;
	struct cdev dht_cdev;
	struct device *device;

	/* Wait queue for DHT */
	wait_queue_head_t wait_queue_dht_ioctl;
	int wait_queue_flag_com_dht;

	/* Mutex required by DHT */
	struct mutex receive_lock;

	/* Configuration fields */
	int pull_down_time;
	int max_pulses;

	/* Data fields */
	enum dht_type val;
	char data[MAX_BYTES_DATA];
	int data_len;
	struct buf b;
};

/*
 * This function returns the data received from DHT Sensor.
 */
int read_data_for_dht(struct periplex_device *pdev, char *message,
		const int len)
{
	struct periplex_dht *dht = periplex_get_drvdata(pdev);

	mutex_lock(&dht->receive_lock);

	dht->data_len = len;
	memcpy(dht->data, message, len);

    dht->wait_queue_flag_com_dht= 1;
    wake_up_interruptible(&dht->wait_queue_dht_ioctl);

	mutex_unlock(&dht->receive_lock);

	msleep(1);

	return 0;
}

/* Device file open */
static int dht_open(struct inode *inode, struct file *file)
{
	struct periplex_dht *dht = container_of(inode->i_cdev, struct periplex_dht,
			dht_cdev);
	file->private_data = dht;
	return 0;
}

/* Device file close */
static int dht_release(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t dht_read(struct file *filp, char __user *buf, size_t len,
		loff_t *off)
{
	return 0;
}

static ssize_t dht_write(struct file *filp, const char __user *buf, size_t len,
		loff_t *off)
{
	return len;
}

static int calculate_checksum(struct periplex_dht *dht, char *data, int len)
{
	int checksum = 0;
	int i = 0;

	for (i = 0; i < 4; i++)
		checksum += data[i];

	if ((checksum & 0xFF) != data[4])
	{
		dev_err(dht->device, "Checksum did not validate, try again.\n");
		return -EAGAIN;
	}

	return 0;
}

static long dht_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct periplex_dht *dht = file->private_data;
	unsigned char p_cmd[1] = {0};
	unsigned char data[8] = {0};
	int i = 0;
	int needed_bytes = 0;
	int ret = 0;

	mutex_lock(&dht_mutex);

	switch (cmd)
	{
		case SET_DHT_SENSOR_TYPE:
			if (copy_from_user(&dht->val, (enum dht_type *) arg,
					sizeof(enum dht_type)))
			{
				dev_err(dht->device, "Failed to copy data from user\n");
				mutex_unlock(&dht_mutex);
				return -EFAULT;
			}

			switch (dht->val)
			{
				case DHT11:
					dht->pull_down_time = DHT11_PULL_DOWN_TIME;
					dht->max_pulses = DHT11_MAX_PULSES;
					break;
				case DHT21:
					dht->pull_down_time = DHT21_PULL_DOWN_TIME;
					dht->max_pulses = DHT21_MAX_PULSES;
					break;
				case DHT22:
				default:
					dht->pull_down_time = DHT22_PULL_DOWN_TIME;
					dht->max_pulses = DHT22_MAX_PULSES;
					break;
			}

			set_periplex_configuration(dht->periplex_id, PULL_DOWN_TIME_CONF_ID, 
					dht->pull_down_time);
			set_periplex_configuration(dht->periplex_id, MAX_PULSES_CONF_ID, 
					dht->max_pulses);
			break;

		case MEASURE_DHT_VALUE:
			needed_bytes = (dht->max_pulses - 1) / 16;

			/* Send cmd 0x00 */
			set_periplex_data(dht->periplex_id, 1, p_cmd);

			/* Get bytes from the periplex */
			while (i < needed_bytes)
			{

				dht->wait_queue_flag_com_dht = 0;
				wait_event_interruptible(dht->wait_queue_dht_ioctl,
						dht->wait_queue_flag_com_dht != 0);

				mutex_lock(&dht->receive_lock);

				memcpy(data + i, dht->data, dht->data_len);
				i += dht->data_len;

				mutex_unlock(&dht->receive_lock);
			}

			for (i = 0; i < needed_bytes; i++) {
				dev_info(dht->device, "%02x\n", data[i]);
			}

			/* Calculate checksum of received data */
			ret = calculate_checksum(dht, data, needed_bytes);
			if (ret < 0) {
				mutex_unlock(&dht_mutex);
				return ret;
			}

			switch (dht->val)
			{
				case DHT11:
					/* Humidity is 1 byte */
					dht->b.humidity = 10 * data[0];

					/* Temperature is 2 bytes */
					/* 1 for integral and 1 byte for 1st decimal place */
					dht->b.temperature = (10 * data[2]) + (data[3] & 0x0F);

					break;
				case DHT21:
				case DHT22:
				default:
					/* Humidity is 2 bytes */
					dht->b.humidity = (data[0] << 8) | data[1];

					/* Temperature is 2 bytes */
					/* MSB is sign, bits 0-14 are magnitude */
					dht->b.temperature = ((data[2] & 0x7F) << 8) + data[3];

					/* Set sign */
					if (data[2] & 0x80)
						dht->b.temperature = -dht->b.temperature;

					break;
			}

			if ((dht->b.humidity < 0) || (dht->b.humidity > 1000))
			{
				dev_err(dht->device, "received unplausible data, try again.\n");
				mutex_unlock(&dht_mutex);
				return -EAGAIN;
			}

			break;

		case GET_TEMP_HUMD:
			if (copy_to_user((struct buf *) arg, &dht->b, sizeof(struct buf)))
			{
				dev_err(dht->device, "Failed to copy data to user\n");
				mutex_unlock(&dht_mutex);
				return -EFAULT;
			}

			break;
	}

	mutex_unlock(&dht_mutex);
	return 0;
}

/* DHT device file operations */
struct file_operations dht_ioctl_ops = {
	.owner = THIS_MODULE,
	.open = dht_open,
	.write = dht_write,
	.read = dht_read,
	.unlocked_ioctl = dht_ioctl,
	.release = dht_release
};

/* Set up DHT devices */
static int setup_device(struct periplex_device *pdev, struct periplex_dht *dht)
{
	int ret;

	/* Allocate character device region */
	ret = alloc_chrdev_region(&dht->dev, 0, 1, dht->name);
	if (ret < 0)
		return ret;

	/* Initialize character device */
	cdev_init(&dht->dht_cdev, &dht_ioctl_ops);
	dht->dht_cdev.owner = THIS_MODULE;

	/* Add character device to system */
	ret = cdev_add(&dht->dht_cdev, dht->dev, 1);
	if (ret < 0)
	{
		dev_err(&pdev->dev, "Cannot add the device to the system\n");
		goto unregister_chrdev;
	}

	/* Create device class */
	dht->dev_class = class_create(THIS_MODULE, dht->name);
	if (IS_ERR(dht->dev_class))
	{
		ret = PTR_ERR(dht->dev_class);
		dev_err(&pdev->dev, "Cannot create the struct class\n");
		goto del_cdev;
	}

	/* Create device node */
	dht->device = device_create(dht->dev_class, NULL, dht->dev, NULL, dht->name);
	if (IS_ERR(dht->device))
	{
		ret = PTR_ERR(dht->device);
		dev_err(&pdev->dev, "Cannot create the device\n");
		goto destroy_class;
	}

	return 0;

destroy_class:
	class_destroy(dht->dev_class);
del_cdev:
	cdev_del(&dht->dht_cdev);
unregister_chrdev:
	unregister_chrdev_region(dht->dev, 1);
	return ret;
}

/* Clean up device resources */
static void cleanup_device(struct periplex_dht *dht)
{
	if (!dht)
		return;

	if (dht->device)
	{
		device_destroy(dht->dev_class, dht->dev);
		dht->device = NULL;
	}

	if (dht->dev_class)
	{
		class_destroy(dht->dev_class);
		dht->dev_class = NULL;
	}

	cdev_del(&dht->dht_cdev);
	unregister_chrdev_region(dht->dev, 1);
}

static int periplex_dht_probe(struct periplex_device *pdev)
{
	int ret;
	struct periplex_dht *dht;

	/* Intialize the mutex for the DHT sensors */
	mutex_init(&dht_mutex);

	/* Allocate DHT structure */
	dht = kzalloc(sizeof(struct periplex_dht), GFP_KERNEL);
	if (!dht)
		return -ENOMEM;

	/* Read the device properties */
	ret = device_property_read_u32(&pdev->dev, "periplex-id",
			&dht->periplex_id);
	if (ret)
	{
		dev_err(&pdev->dev, "Failed to read periplex-id\n");
		goto free_dht;
	}

	/* Create device for DHT Sensors */
	snprintf(dht->name, sizeof(dht->name), "dht-%d", dht->periplex_id);

	/* Setup character device */
	ret = setup_device(pdev, dht);
	if (ret < 0)
		goto free_dht;

	/* Setup periplex device */
	pdev->periplex_id = dht->periplex_id;
	pdev->get_periplex_data = read_data_for_dht;

	mutex_init(&dht->receive_lock);

	/* 	initialize i2c internal wait queue */
	init_waitqueue_head(&dht->wait_queue_dht_ioctl);

	periplex_link_device(pdev);
	periplex_set_drvdata(pdev, dht);

	return 0;

free_dht:
	kfree(dht);
	return ret;
}

static int periplex_dht_remove(struct periplex_device *pdev)
{
	struct periplex_dht *dht = periplex_get_drvdata(pdev);

	periplex_unlink_device(pdev);
	cleanup_device(dht);
	kfree(dht);

	return 0;
}

struct of_device_id periplex_dht_dt_match[] = {
    {
		.compatible = "vicharak,periplex-dht"
	},
    {},
};

MODULE_DEVICE_TABLE(of, periplex_dht_dt_match);

struct periplex_driver periplex_dht_driver = {
    .probe = periplex_dht_probe,
    .remove = periplex_dht_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_dht_dt_match,
    },
};

module_periplex_driver(periplex_dht_driver);

MODULE_ALIAS("periplex:dht");
MODULE_AUTHOR("djkabutar <d.kabutarwala@yahoo.com>");
MODULE_DESCRIPTION("DHT Sensors driver for the periplex");
MODULE_LICENSE("GPL");