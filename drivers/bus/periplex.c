/*
** Purpose of periplex.c file :
** 1.handle Read/write data coming from specific dummy devices like uart,i2c,spi,gpio
** and so on.
** 2.this is parent file of all other peripheral's(uart,i2c,spi,etc.)device file.
** 3.create separate bus for `periplex`, and all the peripheral's (uart,i2c,spi,
** gpio,pwm,etc..) runing on top the bus.
** 4.Also create a one character device for periplex, which is used for read/write of
** peripheral's through ioctl calls.
*/

#include <linux/fs.h>
#include <linux/err.h>
#include <linux/cdev.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>
// #include "include/peripheral.h"

/*
** ioctl call for transfer configuration structure from kernel space to user-space
** (used only in configuration write case)
*/
#define WAIT_FOR_CONFIGURATION _IOW('a', 'a', struct kernel_config *)

/*
** this is ioctl call for acknowledge the interrupt call form user space to kernel
** space (used in configuration transfer)
*/
#define DONE_CONFIGURATION _IOR('a', 'b', int *)

/*
** ioctl call for transfer structure from kernel space to user-space
** (used only in write case)
*/
#define WAIT_FOR_STRUCTURE _IOW('a', 'c', struct kernel_data *)

/*
** ioctl call for reading the address of message from user-space and then transfer
** actual message from kernel-space to user-space(used only in write case)
*/
#define SND_ADDRESS _IOW('a', 'd', unsigned long *)

/*
** this is ioctl call for acknowledge the interrupt call form user side to kernel
** side(used in data transfer)
*/
#define DONE_DATA _IOR('a', 'e', int *)

/*
** ioctl call for transfer data from user-space to kernel-space
** (used only in read case)
*/
#define SND_READ_STRUCTURE _IOR('a', 'f', unsigned long *)

/* 
** ioctl call to send an interrupt signal to the kernel 
*/
#define SEND_INTERRUPT_SIGNAL _IOR('a', 'g', int *)

/*
** ioctl call for transfer burst structure from kernel space to user-space
*/
#define WAIT_FOR_BURST_STRUCTURE _IOW('a', 'h', struct kernel_burst_data *)

/*
** ioctl call for sending burst metadata to user-space
*/
#define SND_BURST_METADATA _IOW('a', 'i', unsigned long *)

/*
** ioctl call for getting burst device data from kernel-space to user-space
*/
#define GET_BURST_DEVICE_DATA _IOWR('a', 'j', struct burst_device_request *)

/*
** ioctl call for acknowledging completion of burst data transfer
*/
#define DONE_BURST_DATA _IOR('a', 'k', int *)

static int register_count = 0;

/* these variable are used in write case */
char *write_message = NULL;
struct kernel_data w_data = {0};
struct kernel_config w_config = {0};
unsigned long write_message_address;

/* Wait queue and flag for data  */
wait_queue_head_t wait_data_queue_ioctl;
int wait_data_queue_flag;

/* Wait queue and flag for ack data  */
wait_queue_head_t wait_done_data_queue_ioctl;
int wait_done_data_queue_flag;

/* Wait queue and flag for config  */
wait_queue_head_t wait_configuration_queue_ioctl;
int wait_configuration_queue_flag;

/* Wait queue and flag for ack config  */
wait_queue_head_t wait_done_configuration_queue_ioctl;
int wait_done_configuration_queue_flag;

/* Wait queue and flag for burst data */
wait_queue_head_t wait_burst_data_queue_ioctl;
int wait_burst_data_queue_flag;

/* Wait queue and flag for ack burst data */
wait_queue_head_t wait_done_burst_data_queue_ioctl;
int wait_done_burst_data_queue_flag;

struct mutex periplex_mutex;
struct mutex periplex_read_mutex;

static unsigned long periplex_dev[MAXIMUM_DEVICE] = {0};

/* device number */
dev_t periplex_num = 0;

/* class */
static struct class *periplex_class;

/* cdev variable */
struct cdev periplex_cdev;

struct kernel_burst_data w_burst_data = {0};
struct peripheral_burst_data *burst_data_array = NULL;

/* Periplex bus methods */
static int periplex_bus_match(struct device *dev, struct device_driver *drv);
static int periplex_driver_probe(struct device *dev);
static int periplex_driver_remove(struct device *dev);

/* Structure representing periplex bus type */
struct bus_type periplex_bus_type = {
	.name = "periplex",
	.match = periplex_bus_match,
	.probe = periplex_driver_probe,
	.remove = periplex_driver_remove,
};

/* Function to register periplex device */
int periplex_register_device(struct periplex_device *pdev)
{
	int ret = 0;

	device_initialize(&pdev->dev);
	pdev->dev.release = device_release_driver;

	ret = dev_set_name(&pdev->dev, "%s", kbasename(pdev->dev.of_node->full_name));
	if (ret)
	{
		return ret;
	}

	ret = device_add(&pdev->dev);
	if (ret)
	{
		dev_err(&pdev->dev, "Failed to add device '%s'\n", dev_name(&pdev->dev));
		return ret;
	}

	return 0;
}

/* Function to unregister periplex device */
void periplex_unregister_device(struct periplex_device *pdev)
{
	device_unregister(&pdev->dev);
	kfree(pdev);
}

/* Periplex bus device match function */
static int periplex_bus_match(struct device *dev, struct device_driver *drv)
{
	return of_driver_match_device(dev, drv);
}

/* Probe function for the periplex bus */
static int periplex_driver_probe(struct device *dev)
{
	struct periplex_driver *pdrv = to_periplex_driver(dev->driver);
	struct periplex_device *pdev = to_periplex_device(dev);

	if (pdrv->probe)
	{
		return pdrv->probe(pdev);
	}

	return -ENODEV;
}

/* Remove function for the periplex bus */
static int periplex_driver_remove(struct device *dev)
{
	struct periplex_driver *pdrv = to_periplex_driver(dev->driver);
	struct periplex_device *pdev = to_periplex_device(dev);

	if (pdrv->remove)
	{
		return pdrv->remove(pdev);
	}

	return 0;
}

/* Periplex bus init function */
static int periplex_bus_init(void)
{
	struct device_node *periplex_node;
	struct device_node *np;
	struct periplex_device *pdev;
	struct platform_device *periplex;
	int ret = 0;
	int device_count = 0;

	/* Get 'periplex' node from device tree */
	periplex_node = of_find_compatible_node(NULL, NULL, "vicharak,periplex");
	if (!periplex_node)
	{
		pr_err("periplex_bus_init: Failed to find periplex node\n");
		return -ENODEV;
	}

	periplex = of_find_device_by_node(periplex_node);
	if (!periplex)
	{
		pr_err("periplex_bus_init: Failed to find platform device\n");
		of_node_put(periplex_node);
		return -ENODEV;
	}

	/* Parse DT to find and register periplex devices */
	for_each_child_of_node(periplex_node, np)
	{
		device_count++;

		pdev = kzalloc(sizeof(*pdev), GFP_KERNEL);
		if (!pdev)
		{
			pr_err("periplex_bus_init: Memory allocation failed for device %d\n",
				   device_count);
			of_node_put(np);
			of_node_put(periplex_node);
			return -ENOMEM;
		}

		pdev->dev.of_node = np;
		pdev->dev.parent = &periplex->dev;
		pdev->dev.bus = &periplex_bus_type;

		ret = periplex_register_device(pdev);
		if (ret)
		{
			pr_err("periplex_bus_init: Failed to register device %d (error: %d)\n",
				   device_count, ret);
			kfree(pdev);
			continue;
		}
	}

	of_node_put(periplex_node);
	return 0;
}

/* Callback function to remove devices registered under periplex bus */
static int periplex_unregister_device_cb(struct device *dev, void *data)
{
	struct periplex_device *pdev = to_periplex_device(dev);
	periplex_unregister_device(pdev);
	return 0;
}

/* Function to remove devices registered under periplex bus */
static void remove_periplex_bus_devices(void)
{
	bus_for_each_dev(&periplex_bus_type, NULL, NULL, periplex_unregister_device_cb);
}

/*set configuration for a specific peripheral(use in write case) */
void set_periplex_configuration(int peri_id, uint8_t config_id, int configuration)
{
	int ret = 0;
	unsigned long timeout_jiffies = msecs_to_jiffies(3000); // 3 second timeout

	mutex_lock(&periplex_mutex);
	w_config.peri_id = peri_id;
	w_config.configuration_id = config_id;
	w_config.configuration = configuration;

	wait_configuration_queue_flag = 1;
	wake_up_interruptible(&wait_configuration_queue_ioctl);

	wait_done_configuration_queue_flag = 0;
	ret = wait_event_interruptible_timeout(wait_done_configuration_queue_ioctl,
										   wait_done_configuration_queue_flag != 0,
										   timeout_jiffies);

	if (ret == 0)
	{
		// Timeout occurred
		pr_err("periplex_bus: set_periplex_configuration: Timeout waiting for completion (3000ms)\n");
		wait_configuration_queue_flag = 0;
	}

	mutex_unlock(&periplex_mutex);
}
EXPORT_SYMBOL(set_periplex_configuration);

/*set data for a specific peripheral (use in write case) */
void set_periplex_data(int peri_id, int length, char *message)
{
	int ret = 0;
	unsigned long timeout_jiffies = msecs_to_jiffies(3000);
	mutex_lock(&periplex_mutex);
	w_data.peri_id = peri_id;
	w_data.length = length;

	write_message = kzalloc(length, GFP_KERNEL);
	if (write_message == NULL)
	{
		pr_err("periplex_bus: Memory allocation failed\n");
		mutex_unlock(&periplex_mutex);
		return;
	}

	if (memcpy(write_message, message, length) == NULL)
	{
		pr_err("periplex_bus: Memory copy failed\n");
		kfree(write_message);
		mutex_unlock(&periplex_mutex);
		return;
	}

	wait_data_queue_flag = 1;
	wake_up_interruptible(&wait_data_queue_ioctl);

	wait_done_data_queue_flag = 0;
	ret = wait_event_interruptible_timeout(wait_done_data_queue_ioctl,
										   wait_done_data_queue_flag != 0,
										   timeout_jiffies);

	if (ret == 0)
	{
		// Timeout occurred
		pr_err("periplex_bus: set_periplex_data: Timeout waiting for completion (3000ms)\n");
		if (write_message)
		{
			kfree(write_message);
			write_message = NULL;
		}
		wait_data_queue_flag = 0;
	}

	mutex_unlock(&periplex_mutex);
}
EXPORT_SYMBOL(set_periplex_data);

/*
** Set data for multiple peripherals in one call (burst mode)
** This function sends data for multiple devices simultaneously
*/
int set_periplex_data_burst(struct peripheral_burst_data *burst_array, int device_count)
{
	int ret = 0;
	int i;
	unsigned long timeout_jiffies = msecs_to_jiffies(5000);

	if (!burst_array || device_count <= 0)
	{
		pr_err("periplex_bus: set_periplex_data_burst: Invalid parameters (array=%p, count=%d)\n",
			   burst_array, device_count);
		return -EINVAL;
	}

	mutex_lock(&periplex_mutex);

	/* Allocate kernel memory for burst data array */
	burst_data_array = kzalloc(device_count * sizeof(struct peripheral_burst_data), GFP_KERNEL);
	if (!burst_data_array)
	{
		pr_err("periplex_bus: set_periplex_data_burst: Failed to allocate burst array\n");
		mutex_unlock(&periplex_mutex);
		return -ENOMEM;
	}

	/* Copy device data to kernel burst array */
	for (i = 0; i < device_count; i++)
	{
		burst_data_array[i].periplex_id = burst_array[i].periplex_id;
		burst_data_array[i].total_length = burst_array[i].total_length;

		if (burst_array[i].total_length <= 0)
		{
			pr_warn("periplex_bus: set_periplex_data_burst: Device %d has invalid length %d\n",
					burst_array[i].periplex_id, burst_array[i].total_length);
			burst_data_array[i].whole_data = NULL;
			continue;
		}

		/* Allocate memory for each device's data */
		burst_data_array[i].whole_data = kzalloc(burst_array[i].total_length, GFP_KERNEL);
		if (!burst_data_array[i].whole_data)
		{
			pr_err("periplex_bus: set_periplex_data_burst: Failed to allocate data for device %d\n",
				   burst_array[i].periplex_id);
			/* Cleanup previously allocated data */
			while (--i >= 0)
			{
				if (burst_data_array[i].whole_data)
				{
					kfree(burst_data_array[i].whole_data);
				}
			}
			kfree(burst_data_array);
			burst_data_array = NULL;
			mutex_unlock(&periplex_mutex);
			return -ENOMEM;
		}

		/* Copy actual data */
		memcpy(burst_data_array[i].whole_data, burst_array[i].whole_data,
			   burst_array[i].total_length);
	}

	/* Setup burst data structure */
	w_burst_data.device_count = device_count;
	w_burst_data.data = burst_data_array;

	/* Wake up userspace to receive burst data */
	wait_burst_data_queue_flag = 1;
	wake_up_interruptible(&wait_burst_data_queue_ioctl);

	/* Wait for userspace to acknowledge completion */
	wait_done_burst_data_queue_flag = 0;
	ret = wait_event_interruptible_timeout(wait_done_burst_data_queue_ioctl,
										   wait_done_burst_data_queue_flag != 0,
										   timeout_jiffies);

	if (ret == 0)
	{
		/* Timeout occurred */
		pr_err("periplex_bus: set_periplex_data_burst: Timeout waiting for completion (5000ms)\n");
		wait_burst_data_queue_flag = 0;
		ret = -ETIMEDOUT;
	}
	else if (ret > 0)
	{
		/* Success */
		ret = 0;
	}
	else
	{
		/* Interrupted by signal */
		pr_err("periplex_bus: set_periplex_data_burst: Interrupted (ret=%d)\n", ret);
	}

	/* Cleanup: Free allocated memory */
	if (burst_data_array)
	{
		for (i = 0; i < device_count; i++)
		{
			if (burst_data_array[i].whole_data)
			{
				kfree(burst_data_array[i].whole_data);
			}
		}
		kfree(burst_data_array);
		burst_data_array = NULL;
	}

	w_burst_data.device_count = 0;
	w_burst_data.data = NULL;

	mutex_unlock(&periplex_mutex);

	return ret;
}
EXPORT_SYMBOL(set_periplex_data_burst);

/* Function to register Periplex driver */
int periplex_register_driver(struct periplex_driver *drv)
{
	int ret = 0;

	if (!drv)
	{
		return -EINVAL;
	}

	drv->driver.bus = &periplex_bus_type;

	ret = driver_register(&drv->driver);
	if (ret)
	{
		pr_err("periplex_bus: Periplex driver: Failed to register driver '%s'\n", drv->driver.name);
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(periplex_register_driver);

/* Function to unregister Periplex driver */
void periplex_unregister_driver(struct periplex_driver *drv)
{
	if (!drv)
	{
		pr_err("periplex_bus: Periplex driver: NULL driver pointer during unregistration\n");
		return;
	}

	driver_unregister(&drv->driver);
}
EXPORT_SYMBOL(periplex_unregister_driver);

/* Function to link any Peripheral's to periplex(mandatory) */
int periplex_link_device(struct periplex_device *pdev)
{

	if (register_count > MAXIMUM_DEVICE)
	{
		pr_err("periplex_bus: OUT_OF_RANGE:Your driver is not inserted successfully\n");
		return -EINVAL;
	}

	if (periplex_dev[pdev->periplex_id] == 0)
	{
		periplex_dev[pdev->periplex_id] = (unsigned long)pdev;
		pr_info("periplex_bus: periplex_id register %d\n", pdev->periplex_id);
		register_count++;
		return 0;
	}
	else
	{
		return 1;
	}
}
EXPORT_SYMBOL(periplex_link_device);

/* Function to unlink any Peripheral's to periplex(mandatory) */
void periplex_unlink_device(struct periplex_device *pdev)
{

	int unregister_count = 0;
	for (unregister_count = 0; unregister_count < MAXIMUM_DEVICE; unregister_count++)
	{
		if (periplex_dev[unregister_count])
		{
			if ((unsigned long)pdev == periplex_dev[unregister_count])
			{
				pr_info("periplex_bus: periplex_id unregister %d\n", unregister_count);
				periplex_dev[unregister_count] = 0;
				register_count--;
				break;
			}
		}
	}
}
EXPORT_SYMBOL(periplex_unlink_device);

static int device_open(struct inode *inode, struct file *file)
{
	return 0;
}

static int device_close(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t device_read(struct file *file, char __user *buf, size_t len,
						   loff_t *off)
{
	return 0;
}

static ssize_t device_write(struct file *file, const char __user *buf,
							size_t len, loff_t *off)
{
	return 0;
}

/* Device ioctl function handle read/write operation from/to peripherals*/
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	int ret = 0;
	int done_data = 0;
	int interrupt_signal = 0;
	int done_configuration = 0;
	char *read_message = NULL;
	struct periplex_device *peri_dev;
	struct kernel_buffer local_r_data = {0};
	
	switch (cmd)
	{
	case SEND_INTERRUPT_SIGNAL:
		if (copy_from_user(&interrupt_signal, (int __user *)arg, sizeof(interrupt_signal)))
		{
			pr_err("periplex_bus: SEND_INTERRUPT_SIGNAL: Failed to copy interrupt signal from user space\n");
			return -EFAULT;
		}

		if (interrupt_signal == 1)
		{
			// Wake up waiting threads for configuration
			wait_configuration_queue_flag = -1; // Use -1 to indicate interrupt
			wake_up_interruptible(&wait_configuration_queue_ioctl);

			// Wake up waiting threads for data
			wait_data_queue_flag = -1; // Use -1 to indicate interrupt
			wake_up_interruptible(&wait_data_queue_ioctl);

			// Wake up any other waiting queues if they exist
			wait_done_data_queue_flag = -1;
			wake_up_interruptible(&wait_done_data_queue_ioctl);

			wait_done_configuration_queue_flag = -1;
			wake_up_interruptible(&wait_done_configuration_queue_ioctl);

			// Wake up burst data waiting threads
			wait_burst_data_queue_flag = -1;
			wake_up_interruptible(&wait_burst_data_queue_ioctl);

			wait_done_burst_data_queue_flag = -1;
			wake_up_interruptible(&wait_done_burst_data_queue_ioctl);

			pr_info("SEND_INTERRUPT_SIGNAL: All waiting queues have been notified of interrupt\n");
		}
		else
		{
			pr_warn("periplex_bus: SEND_INTERRUPT_SIGNAL: Unexpected value for interrupt_signal: %d\n", interrupt_signal);
		}
		break;
		
	case SND_READ_STRUCTURE:

		// Lock the mutex
		if (mutex_lock_interruptible(&periplex_read_mutex))
		{
			pr_info("periplex_bus: SND_READ_STRUCTURE: Mutex lock interrupted by signal\n");
			return -ERESTARTSYS;
		}

		// Copy user data
		if (copy_from_user(&local_r_data, (void __user *)arg, sizeof(local_r_data)))
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: Failed to copy user data\n");
			ret = -EFAULT;
			goto unlock_exit;
		}

		// Validate length
		if (local_r_data.length <= 0 || local_r_data.length > MAX_MESSAGE_LENGTH)
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: Invalid message length: %d\n", local_r_data.length);
			ret = -EINVAL;
			goto unlock_exit;
		}

		// Validate user message pointer
		if (!local_r_data.message)
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: Invalid message pointer\n");
			ret = -EINVAL;
			goto unlock_exit;
		}

		// Allocate memory
		read_message = kmalloc(local_r_data.length, GFP_KERNEL);
		if (!read_message)
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: Memory allocation failed\n");
			ret = -ENOMEM;
			goto unlock_exit;
		}

		// Copy message from user
		if (copy_from_user(read_message, local_r_data.message, local_r_data.length))
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: Failed to copy message from user\n");
			ret = -EFAULT;
			goto cleanup_and_unlock;
		}

		// Validate device minor number
		if (local_r_data.minor < 0 || local_r_data.minor >= MAXIMUM_DEVICE)
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: Invalid minor number: %d\n", local_r_data.minor);
			ret = -EINVAL;
			goto cleanup_and_unlock;
		}

		// Check if device is registered
		if (!periplex_dev[local_r_data.minor])
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: No device registered for minor %d\n", local_r_data.minor);
			ret = -ENODEV;
			goto cleanup_and_unlock;
		}

		peri_dev = (struct periplex_device *)periplex_dev[local_r_data.minor];
		if (!peri_dev)
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: Failed to get periplex device for minor %d\n", local_r_data.minor);
			ret = -ENODEV;
			goto cleanup_and_unlock;
		}

		// Check function pointer
		if (!peri_dev->get_periplex_data)
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: get_periplex_data function pointer is NULL for minor %d\n",
				   local_r_data.minor);
			ret = -ENOSYS;
			goto cleanup_and_unlock;
		}

		// Call device function
		ret = peri_dev->get_periplex_data(peri_dev, read_message, local_r_data.length);
		if (ret)
		{
			pr_err("periplex_bus: SND_READ_STRUCTURE: Failed to get periplex data for minor %d (error: %d)\n", local_r_data.minor, ret);
			ret = -EIO;
			goto cleanup_and_unlock;
		}

		// Success - cleanup and return
		kfree(read_message);
		mutex_unlock(&periplex_read_mutex);
		return 0;

	cleanup_and_unlock:
		kfree(read_message);
	unlock_exit:
		mutex_unlock(&periplex_read_mutex);
		return ret;

	case WAIT_FOR_CONFIGURATION:
		wait_configuration_queue_flag = 0;
		ret = wait_event_interruptible(wait_configuration_queue_ioctl, wait_configuration_queue_flag != 0);

		// Check if woken up due to interrupt signal from userspace
		if (wait_configuration_queue_flag == -1)
		{
			pr_info("periplex_bus: WAIT_FOR_CONFIGURATION: Received interrupt signal, exiting\n");
			return -EINTR;
		}

		// Normal case - copy data to user
		if (copy_to_user((struct kernel_config *)arg, &w_config, sizeof(w_config)))
		{
			pr_err("periplex_bus: WAIT_FOR_CONFIGURATION: Not able to copy structure of config");
			return -EFAULT;
		}
		break;

	case WAIT_FOR_STRUCTURE:
		wait_data_queue_flag = 0;
		ret = wait_event_interruptible(wait_data_queue_ioctl, wait_data_queue_flag != 0);

		// Check if woken up due to interrupt signal from userspace
		if (wait_data_queue_flag == -1)
		{
			pr_info("periplex_bus: WAIT_FOR_STRUCTURE: Received interrupt signal, exiting\n");
			return -EINTR;
		}

		// Normal case - copy data to user
		if (copy_to_user((struct kernel_data *)arg, &w_data, sizeof(w_data)))
		{
			pr_err("periplex_bus: WAIT_FOR_STRUCTURE: Not able to pass structure to the user-space\n");
			return -EFAULT;
		}
		break;

	case SND_ADDRESS:
		if (copy_from_user(&write_message_address, (void __user *)arg,
						   sizeof(write_message_address)))
		{
			pr_err("periplex_bus: SND_ADDRESS: address is not copy perfectly");
			kfree(write_message);
			break;
		}
		if (copy_to_user((char *)write_message_address, write_message,
						 w_data.length))
		{
			pr_err("periplex_bus: SND_ADDRESS: message is not passed perfectly");
			kfree(write_message);
			break;
		}
		kfree(write_message);
		break;

	case DONE_DATA:
		if (copy_from_user(&done_data, (int __user *)arg, sizeof(done_data)))
		{
			pr_err("periplex_bus: DONE_DATA: Failed to copy data from user space\n");
			return -EFAULT;
		}
		if (done_data == 1)
		{
			wait_done_data_queue_flag = 1;
			wake_up_interruptible(&wait_done_data_queue_ioctl);
		}
		else
		{
			pr_warn("periplex_bus: DONE_DATA: Unexpected value for done_data: %d\n", done_data);
		}
		break;

	case DONE_CONFIGURATION:
		if (copy_from_user(&done_configuration, (int __user *)arg, sizeof(done_configuration)))
		{
			pr_err("periplex_bus: Done_CONFIGURATION: Failed to copy data from user space\n");
			return -EFAULT;
		}
		if (done_configuration == 1)
		{
			wait_done_configuration_queue_flag = 1;
			wake_up_interruptible(&wait_done_configuration_queue_ioctl);
		}
		else
		{
			pr_warn("periplex_bus: DONE_CONFIGURATION: Unexpected value for "
					"done_configuration: %d\n",
					done_configuration);
		}
		break;

	case WAIT_FOR_BURST_STRUCTURE:
		wait_burst_data_queue_flag = 0;
		ret = wait_event_interruptible(wait_burst_data_queue_ioctl,
									   wait_burst_data_queue_flag != 0);

		/* Check if interrupted by signal */
		if (ret == -ERESTARTSYS)
		{
			pr_info("periplex_bus: WAIT_FOR_BURST_STRUCTURE: Interrupted by signal\n");
			return -EINTR;
		}

		/* Check if woken up due to interrupt signal from userspace */
		if (wait_burst_data_queue_flag == -1)
		{
			pr_info("periplex_bus: WAIT_FOR_BURST_STRUCTURE: Received interrupt signal, exiting\n");
			return -EINTR;
		}

		/* Copy burst data structure to userspace */
		if (copy_to_user((struct kernel_burst_data *)arg, &w_burst_data,
						 sizeof(w_burst_data)))
		{
			pr_err("periplex_bus: WAIT_FOR_BURST_STRUCTURE: Failed to copy burst structure to userspace\n");
			return -EFAULT;
		}

		break;

	case SND_BURST_METADATA:
	{
		unsigned long user_metadata_array_address;
		struct burst_device_metadata metadata;
		int i;

		if (!burst_data_array || w_burst_data.device_count <= 0)
		{
			pr_err("periplex_bus: SND_BURST_METADATA: No burst data available\n");
			return -EINVAL;
		}

		/* Get the address where userspace wants the metadata array */
		if (copy_from_user(&user_metadata_array_address, (void __user *)arg,
						   sizeof(user_metadata_array_address)))
		{
			pr_err("periplex_bus: SND_BURST_METADATA: Failed to get user array address\n");
			ret = -EFAULT;
			break;
		}

		/* Copy metadata for each device to userspace array */
		for (i = 0; i < w_burst_data.device_count; i++)
		{
			metadata.periplex_id = burst_data_array[i].periplex_id;
			metadata.total_length = burst_data_array[i].total_length;

			if (copy_to_user((struct burst_device_metadata *)(user_metadata_array_address +
															  i * sizeof(struct burst_device_metadata)),
							 &metadata, sizeof(struct burst_device_metadata)))
			{
				pr_err("periplex_bus: SND_BURST_METADATA: Failed to copy metadata for device %d\n", i);
				ret = -EFAULT;
				break;
			}
		}

		break;
	}

	case GET_BURST_DEVICE_DATA:
	{
		struct burst_device_request request;
		int device_idx;

		if (!burst_data_array || w_burst_data.device_count <= 0)
		{
			pr_err("periplex_bus: GET_BURST_DEVICE_DATA: No burst data available\n");
			return -EINVAL;
		}

		/* Get request from userspace */
		if (copy_from_user(&request, (struct burst_device_request *)arg,
						   sizeof(request)))
		{
			pr_err("periplex_bus: GET_BURST_DEVICE_DATA: Failed to copy request from userspace\n");
			return -EFAULT;
		}

		device_idx = request.device_index;

		/* Validate device index */
		if (device_idx < 0 || device_idx >= w_burst_data.device_count)
		{
			pr_err("periplex_bus: GET_BURST_DEVICE_DATA: Invalid device index %d (max: %d)\n",
				   device_idx, w_burst_data.device_count - 1);
			return -EINVAL;
		}

		/* Validate buffer size */
		if (request.buffer_size < burst_data_array[device_idx].total_length)
		{
			pr_err("periplex_bus: GET_BURST_DEVICE_DATA: Buffer too small (%d < %d)\n",
				   request.buffer_size, burst_data_array[device_idx].total_length);
			return -EINVAL;
		}

		/* Validate data pointer */
		if (!burst_data_array[device_idx].whole_data)
		{
			pr_err("periplex_bus: GET_BURST_DEVICE_DATA: No data available for device %d\n", device_idx);
			return -EINVAL;
		}

		/* Validate user buffer pointer */
		if (!request.data_buffer)
		{
			pr_err("periplex_bus: GET_BURST_DEVICE_DATA: Invalid user buffer pointer\n");
			return -EINVAL;
		}

		/* Copy device data to userspace buffer */
		if (copy_to_user(request.data_buffer, burst_data_array[device_idx].whole_data,
						 burst_data_array[device_idx].total_length))
		{
			pr_err("periplex_bus: GET_BURST_DEVICE_DATA: Failed to copy data to userspace for device %d\n",
				   device_idx);
			return -EFAULT;
		}

		break;
	}

	case DONE_BURST_DATA:
	{
		int done_burst_data = 0;

		if (copy_from_user(&done_burst_data, (int __user *)arg, sizeof(done_burst_data)))
		{
			pr_err("periplex_bus: DONE_BURST_DATA: Failed to copy data from user space\n");
			return -EFAULT;
		}

		if (done_burst_data == 1)
		{
			wait_done_burst_data_queue_flag = 1;
			wake_up_interruptible(&wait_done_burst_data_queue_ioctl);
		}
		else
		{
			pr_warn("periplex_bus: DONE_BURST_DATA: Unexpected value: %d (expected 1)\n", done_burst_data);
		}
		break;
	}

	default:
		pr_info("periplex_bus: Default\n");
		break;
	}
	return 0;
}

/* device file operations */
struct file_operations periplex_ioctl_ops = {
	.owner = THIS_MODULE,
	.open = device_open,
	.write = device_write,
	.read = device_read,
	.unlocked_ioctl = device_ioctl,
	.release = device_close};

/* ioctl_init function */
static int __init ioctl_init(void)
{
	int ret = 0;

	/* Initialize wait queue for configuration */
	init_waitqueue_head(&wait_configuration_queue_ioctl);

	/* Initialize wait queue for reading */
	init_waitqueue_head(&wait_data_queue_ioctl);

	/* Initialize wait queue for done data */
	init_waitqueue_head(&wait_done_data_queue_ioctl);

	/* Initialize wait queue for done configuration */
	init_waitqueue_head(&wait_done_configuration_queue_ioctl);

	/* Initialize wait queue for burst data */
	init_waitqueue_head(&wait_burst_data_queue_ioctl);

	/* Initialize wait queue for done burst data */
	init_waitqueue_head(&wait_done_burst_data_queue_ioctl);

	/* Initialize mutex */
	mutex_init(&periplex_mutex);

	mutex_init(&periplex_read_mutex);

	/* Allocating Major Number */
	if ((alloc_chrdev_region(&periplex_num, 0, 1, "periplex")) < 0)
	{
		pr_err("periplex_bus: Cannot allocate major number for device\n");
		return ret;
	}

	/* Initialize the cdev structure with fops */
	cdev_init(&periplex_cdev, &periplex_ioctl_ops);

	/* Register a device (cdev structure) with VFS */
	cdev_add(&periplex_cdev, periplex_num, 1);

	/* Creating a device class unser /sys/class */
	if (IS_ERR(periplex_class = class_create(THIS_MODULE, "periplex")))
	{
		pr_err("periplex_bus: Cannot create the struct class\n");
		goto r_class;
	}

	/* Creating device under /dev */
	if (IS_ERR(device_create(periplex_class, NULL, periplex_num, NULL, "periplex")))
	{
		pr_err("periplex_bus: Cannot create the Device\n");
		goto r_device;
	}
	pr_info("periplex_bus: char device inserted Successfully\n");

	ret = bus_register(&periplex_bus_type);
	if (ret)
	{
		pr_err("periplex_bus: Failed to register bus\n");
		return ret;
	}

	ret = periplex_bus_init();
	if (ret)
	{
		pr_err("periplex_bus: Failed to initialize bus\n");
		bus_unregister(&periplex_bus_type);
		return ret;
	}
	pr_info("periplex_bus: bus inserted successfully\n");
	return 0;

r_device:

	/* Destroy class and device */
	device_destroy(periplex_class, periplex_num);
	class_destroy(periplex_class);

r_class:

	/* Unregister device major number */
	unregister_chrdev_region(periplex_num, 1);
	return ret;
}

/* Module exit function named as ioctl_exit */
static void __exit ioctl_exit(void)
{
	device_destroy(periplex_class, periplex_num);
	class_destroy(periplex_class);
	unregister_chrdev_region(periplex_num, 1);
	pr_info("periplex_bus: Module is removed successfully...\n");
	remove_periplex_bus_devices();
	bus_unregister(&periplex_bus_type);
	pr_info("periplex_bus: bus is removed successfully...\n");
}

/*  Module insert and exit */
module_init(ioctl_init);
module_exit(ioctl_exit);

MODULE_ALIAS("periplex:bus");
MODULE_AUTHOR("Vatsal Kevadiya <vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("periplex: handle read/write operation from/to peripheral's");
MODULE_LICENSE("GPL");
