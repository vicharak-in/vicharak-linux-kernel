/*
** Significant of i2c.c file :
** 1. Make multiple i2c devices with the use of dtso and create the
** i2c-* series into the /dev
** 2. Allow Write/read data in any specific i2c(i2c-*) device
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/wait.h>
#include <linux/of.h>
#include <linux/mutex.h>
#include <linux/moduleparam.h>

#include <asm/uaccess.h>
#include <asm/errno.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

#define DRIVER_NAME "periplex-i2c"
#define FCLK 50000000

/*
** Structure to hold per-adapter data
*/
struct periplex_i2c_data
{
	int periplex_id;
	struct i2c_adapter *adapter;
	struct device *device;

	struct mutex i2c_mutex;

	wait_queue_head_t wait_queue_i2c_ioctl;
	wait_queue_head_t wait_queue_i2c_ioctl_ack;

	int wait_queue_flag_com_i2c;
	int wait_queue_flag_com_i2c_ack;

	char *read_data_i2c;
	int read_length_i2c;
};

/*
** Debug flag (can be set via module parameter)
*/
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define I2C_DEBUG(fmt, ...)              \
	do                                   \
	{                                    \
		if (debug)                       \
			pr_info("periplex_i2c: "fmt, ##__VA_ARGS__); \
	} while (0)

/*
** This function used to get the functionalities that are supported
** by this bus driver.
*/
u32 i2c_func(struct i2c_adapter *adapter)
{
	return (I2C_FUNC_I2C |
			I2C_FUNC_SMBUS_QUICK |
			I2C_FUNC_SMBUS_BYTE |
			I2C_FUNC_SMBUS_BYTE_DATA |
			I2C_FUNC_SMBUS_WORD_DATA |
			I2C_SMBUS_I2C_BLOCK_DATA |
			I2C_FUNC_SMBUS_I2C_BLOCK);
}

/*
** This function waits for an I2C write acknowledgment response
*/
static int wait_for_i2c_write_ack_response(struct periplex_i2c_data *peri_i2c, u16 addr,
										   const char *debug_prefix)
{
	int ret = 0;

	// Reset wait flag before waiting
	peri_i2c->wait_queue_flag_com_i2c = 0;

	// Wait until data is available or interrupted
	ret = wait_event_interruptible_timeout(peri_i2c->wait_queue_i2c_ioctl,
										   peri_i2c->wait_queue_flag_com_i2c != 0,
										   msecs_to_jiffies(1000));

	// Check if wait was interrupted or failed
	if (ret == 0)
	{
		pr_err("periplex_i2c: Timed out waiting in i2c_write_ack_response\n");
		ret = -ETIMEDOUT;
		return ret;
	}
	else if (ret < 0)
	{
		pr_err("periplex_i2c: Wait interrupted with error in i2c_write_ack_response: %d\n", ret);
		return ret;
	}

	// Check if data buffer is valid
	if (peri_i2c->read_data_i2c == NULL)
	{
		pr_err("%s: read_data_i2c is NULL\n", debug_prefix);
		return -EFAULT;
	}

	I2C_DEBUG("%s: read_data_i2c[0] = %d\n", debug_prefix, peri_i2c->read_data_i2c[0]);

	// Validate device presence by checking returned address
	if (peri_i2c->read_data_i2c[0] != addr)
	{
		I2C_DEBUG("%s: No device present at addr 0x%02x\n", debug_prefix, addr);
		ret = -ENXIO; // No such device or address
	}
	else
	{
		I2C_DEBUG("%s: Device present at addr 0x%02x\n", debug_prefix, addr);
		ret = 0;
	}

	// Free the allocated memory
	devm_kfree(peri_i2c->device, peri_i2c->read_data_i2c);
	peri_i2c->read_data_i2c = NULL;
	peri_i2c->read_length_i2c = 0;

	// Acknowledge the read completion
	peri_i2c->wait_queue_flag_com_i2c_ack = 1;
	wake_up_interruptible(&peri_i2c->wait_queue_i2c_ioctl_ack);

	return ret;
}

/*
** this functions is used in read for i2c
*/
int read_data_for_i2c(struct periplex_device *pdev, char *message,
					  const int len)
{
	int ret = 0;
	struct periplex_i2c_data *peri_i2c = periplex_get_drvdata(pdev);

	if (!peri_i2c)
	{
		pr_err("periplex_i2c: No I2C data found for device\n");
		return -ENODEV;
	}

	peri_i2c->read_length_i2c = len;
	I2C_DEBUG("i2c read calling\n");
	I2C_DEBUG("length is %d\n", len);

	peri_i2c->read_data_i2c = devm_kmalloc(peri_i2c->device, len, GFP_KERNEL);
	if (!peri_i2c->read_data_i2c)
	{
		pr_err("periplex_i2c: Failed to allocate memory for read_data_i2c\n");
		return -ENOMEM;
	}

	if (memcpy(peri_i2c->read_data_i2c, message, len) == NULL)
	{
		pr_err("periplex_i2c: Not able to copy\n");
		devm_kfree(peri_i2c->device, peri_i2c->read_data_i2c);
		peri_i2c->read_data_i2c = NULL;
		return -EFAULT;
	}

	peri_i2c->wait_queue_flag_com_i2c_ack = 0;
	peri_i2c->wait_queue_flag_com_i2c = 1;
	wake_up_interruptible(&peri_i2c->wait_queue_i2c_ioctl);

	ret = wait_event_interruptible_timeout(peri_i2c->wait_queue_i2c_ioctl_ack,
										   peri_i2c->wait_queue_flag_com_i2c_ack != 0,
										   msecs_to_jiffies(1000));
	if (ret == 0)
	{
		pr_err("periplex_i2c: Timed out waiting in read_data_for_i2c\n");
		ret = -ETIMEDOUT;
	}
	else if (ret < 0)
	{
		pr_err("periplex_i2c: Wait interrupted with error in read_data_for_i2c: %d\n", ret);
	}
	else
	{
		ret = 0;
	}

	return ret;
}

/*
** This function will be called whenever you call I2C read, wirte APIs like
** i2c_master_send(), i2c_master_recv() etc.
*/
static s32 i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
					int num)
{
	int i = 0;
	int ret = 0;
	char *message = NULL;
	struct periplex_i2c_data *peri_i2c = NULL;

	if (!adap || !adap->dev.driver_data)
	{
		pr_err("periplex_i2c: Invalid adapter or driver data\n");
		return -EINVAL;
	}

	peri_i2c = (struct periplex_i2c_data *)adap->dev.driver_data;

	I2C_DEBUG("periplex_i2c: i2c_xfer called with %d process\n", num);

	mutex_lock(&peri_i2c->i2c_mutex);

	for (i = 0; i < num; i++)
	{
		int j;
		int index = 0;
		struct i2c_msg *msg_temp = &msgs[i];
		bool is_read = (msg_temp->flags & I2C_M_RD) != 0;

		// Check message length limit
		if (msg_temp->len > 255)
		{
			pr_err("periplex_i2c: i2c_xfer: Message length %d exceeds maximum limit of 255 bytes\n",
				   msg_temp->len);
			mutex_unlock(&peri_i2c->i2c_mutex);
			return -EINVAL;
		}

		I2C_DEBUG("[Count: %d] [%s]: [Addr = 0x%x] [Len = %d] [Operation = %s]\n",
				  i, __func__, msg_temp->addr, msg_temp->len,
				  is_read ? "READ" : "WRITE");

		if (is_read)
		{
			int remaining, copy_size;
			I2C_DEBUG("xfer: read call\n");
			message = kmalloc(3, GFP_KERNEL);
			if (!message)
			{
				pr_err("periplex_i2c: Failed to allocate memory for read message\n");
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = msg_temp->addr << 1 | is_read; // slave address + Read/Write bit
			message[1] = 0;								// detect flag
			message[2] = msg_temp->len;					// length

			set_periplex_data(peri_i2c->periplex_id, 3, message);
			kfree(message);

			ret = wait_for_i2c_write_ack_response(peri_i2c, msg_temp->addr, "periplex_i2c: i2c_xfer_read");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			while (index < msg_temp->len)
			{
				// Reset wait flag before waiting
				peri_i2c->wait_queue_flag_com_i2c = 0;
				ret = wait_event_interruptible_timeout(peri_i2c->wait_queue_i2c_ioctl,
													   peri_i2c->wait_queue_flag_com_i2c != 0,
													   msecs_to_jiffies(1000));

				if (ret == 0)
				{
					pr_err("periplex_i2c: Timed out waiting in xfer_read\n");
					ret = -ETIMEDOUT;
					mutex_unlock(&peri_i2c->i2c_mutex);
					return ret;
				}
				else if (ret < 0)
				{
					pr_err("periplex_i2c: Wait interrupted with error in xfer_read: %d\n", ret);
					mutex_unlock(&peri_i2c->i2c_mutex);
					return ret;
				}

				if (!peri_i2c->read_data_i2c)
				{
					pr_err("periplex_i2c: read_data_i2c is NULL\n");
					mutex_unlock(&peri_i2c->i2c_mutex);
					return -EFAULT;
				}

				// Calculate remaining bytes and copy size
				remaining = msg_temp->len - index;
				copy_size = min(remaining, peri_i2c->read_length_i2c);

				memcpy(msg_temp->buf + index, peri_i2c->read_data_i2c, copy_size);
				index += copy_size;

				I2C_DEBUG("Read progress: copied %d bytes, %d remaining\n",
						  copy_size, msg_temp->len - index);

				devm_kfree(peri_i2c->device, peri_i2c->read_data_i2c);
				peri_i2c->read_data_i2c = NULL;
				peri_i2c->read_length_i2c = 0;

				peri_i2c->wait_queue_flag_com_i2c_ack = 1;
				wake_up_interruptible(&peri_i2c->wait_queue_i2c_ioctl_ack);
			}
			I2C_DEBUG("i2c_xfer: read done\n");
		}
		else
		{
			I2C_DEBUG("xfer: write call\n");
			message = kmalloc(msg_temp->len + 3, GFP_KERNEL);
			if (!message)
			{
				pr_err("periplex_i2c: Failed to allocate memory for write message\n");
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = msg_temp->addr << 1 | is_read; // slave address + Read/Write bit
			message[1] = 0;								// detect flag
			message[2] = msg_temp->len;					// length

			// Copy data to message buffer
			for (j = 0; j < msg_temp->len; j++)
			{
				I2C_DEBUG("[0x%02x] \n", msg_temp->buf[j]);
				message[j + 3] = msg_temp->buf[j];
			}

			set_periplex_data(peri_i2c->periplex_id, msg_temp->len + 3, message);
			kfree(message);

			ret = wait_for_i2c_write_ack_response(peri_i2c, msg_temp->addr, "periplex_i2c: i2c_xfer_write");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			I2C_DEBUG("i2c_xfer: write done\n");
		}
	}

	mutex_unlock(&peri_i2c->i2c_mutex);
	return num; // Return number of messages processed successfully
}

/*
** This function will be called whenever you call SMBUS read, wirte APIs
*/
s32 i2c_smbus_xfer(struct i2c_adapter *adap,
				   u16 addr,
				   unsigned short flags,
				   char read_write,
				   u8 command,
				   int size,
				   union i2c_smbus_data *data)
{
	struct periplex_i2c_data *peri_i2c = NULL;
	char *message = NULL;
	int ret = 0;
	int index = 0;
	int length = 1;
	int remaining = 0;
	int data_length = 1;
	int word_length = 2;
	int data_command_length = 2;
	u16 msb = 0;
	u16 lsb = 0;

	if (!adap || !adap->dev.driver_data)
	{
		pr_err("periplex_i2c: Invalid adapter or driver data\n");
		return -EINVAL;
	}

	peri_i2c = (struct periplex_i2c_data *)adap->dev.driver_data;

	I2C_DEBUG("SMBUS XFER: addr=0x%x, rw=%d, command=0x%x, size=%d\n",
			  addr, read_write, command, size);

	mutex_lock(&peri_i2c->i2c_mutex);

	switch (size)
	{
	case I2C_SMBUS_QUICK:
		message = kmalloc(2, GFP_KERNEL);
		if (!message)
		{
			mutex_unlock(&peri_i2c->i2c_mutex);
			return -ENOMEM;
		}

		message[0] = (addr << 1) | read_write; // slave address + Read/Write bit
		message[1] = 1;						   // set 1 detect command flag for quick command

		I2C_DEBUG("Quick command: addr=0x%02x, rw=%d\n", addr, read_write);

		set_periplex_data(peri_i2c->periplex_id, 2, message);
		kfree(message);

		I2C_DEBUG("waiting for I2C_SMBUS_QUICK\n");
		ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_quick");
		if (ret < 0)
		{
			mutex_unlock(&peri_i2c->i2c_mutex);
			return ret;
		}
		break;

	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_WRITE)
		{
			message = kmalloc(4, GFP_KERNEL);
			if (!message)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = (addr << 1) | read_write; // slave address + Read/Write bit
			message[1] = 0;						   // detect flag
			message[2] = length;				   // register_read_bit + length is 1-byte
			message[3] = command;				   // Command/Register address

			set_periplex_data(peri_i2c->periplex_id, 4, message);
			kfree(message);

			I2C_DEBUG("wait in (WRITE) for the I2C_SMBUS_BYTE\n");
			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_byte");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}
		}
		else
		{
			I2C_DEBUG("before: data->byte %02x\n", data->byte);

			message = kmalloc(3, GFP_KERNEL);
			if (!message)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = addr << 1 | read_write; // slave address + Read/write bit
			message[1] = 0;						 // detect flag
			message[2] = length;				 // length is 1-byte

			set_periplex_data(peri_i2c->periplex_id, 3, message);
			kfree(message);

			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_byte");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			I2C_DEBUG("wait in (READ) for the I2C_SMBUS_BYTE\n");

			peri_i2c->wait_queue_flag_com_i2c = 0;
			ret = wait_event_interruptible_timeout(peri_i2c->wait_queue_i2c_ioctl,
												   peri_i2c->wait_queue_flag_com_i2c != 0,
												   msecs_to_jiffies(1000));
			if (ret == 0)
			{
				pr_err("periplex_i2c: Timed out waiting for in I2C_SMBUS_BYTE\n");
				ret = -ETIMEDOUT;
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}
			else if (ret < 0)
			{
				pr_err("periplex_i2c: Wait interrupted with error in I2C_SMBUS_BYTE: %d\n", ret);
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			// check what is the response of one byte
			if (peri_i2c->read_data_i2c)
			{
				data->byte = peri_i2c->read_data_i2c[0];
				I2C_DEBUG("after: data->byte %02x\n", data->byte);
				devm_kfree(peri_i2c->device, peri_i2c->read_data_i2c);
				peri_i2c->read_data_i2c = NULL;
				peri_i2c->read_length_i2c = 0;

				// Acknowledge the read completion
				peri_i2c->wait_queue_flag_com_i2c_ack = 1;
				wake_up_interruptible(&peri_i2c->wait_queue_i2c_ioctl_ack);
				ret = 0;
			}
			else
			{
				ret = -ENXIO;
			}
		}
		break;

	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_WRITE)
		{
			message = kmalloc(5, GFP_KERNEL);
			if (!message)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = addr << 1 | read_write; // slave address + Read/Write bit
			message[1] = (read_write << 7) | 0;	 //	register_read_bit + detect flag
			message[2] = data_command_length;	 // length is 2-byte
			message[3] = command;				 // Command/Register address
			message[4] = data->byte;			 // data to be written

			I2C_DEBUG("data->byte %02x\n", data->byte);

			set_periplex_data(peri_i2c->periplex_id, 5, message);
			kfree(message);

			I2C_DEBUG("wait in (WRITE) for I2C_SMBUS_BYTE_DATA\n");
			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_byte_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}
		}
		else
		{
			I2C_DEBUG("before: data->byte %02x\n", data->byte);

			message = kmalloc(4, GFP_KERNEL);
			if (!message)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = addr << 1 | read_write; // slave address + Read/Write bit
			message[1] = (read_write << 7) | 0;	 // register_read_bit + detect flag
			message[2] = data_length;			 // length is 1-byte
			message[3] = command;				 // Command/Register address

			set_periplex_data(peri_i2c->periplex_id, 4, message);
			kfree(message);

			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_byte_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_byte_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			I2C_DEBUG("wait in (READ) for the I2C_SMBUS_BYTE_DATA\n");
			peri_i2c->wait_queue_flag_com_i2c = 0;
			ret = wait_event_interruptible_timeout(peri_i2c->wait_queue_i2c_ioctl,
												   peri_i2c->wait_queue_flag_com_i2c != 0,
												   msecs_to_jiffies(1000));
			if (ret == 0)
			{
				pr_err("periplex_i2c: Timed out waiting in I2C_SMBUS_BYTE_DATA\n");
				ret = -ETIMEDOUT;
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}
			else if (ret < 0)
			{
				pr_err("periplex_i2c: Wait interrupted with error in I2C_SMBUS_BYTE_DATA: %d\n", ret);
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			if (!peri_i2c->read_data_i2c)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -EFAULT;
			}

			data->byte = peri_i2c->read_data_i2c[0];

			I2C_DEBUG("after: data->byte %02x\n", data->byte);
			devm_kfree(peri_i2c->device, peri_i2c->read_data_i2c);
			peri_i2c->read_data_i2c = NULL;
			peri_i2c->read_length_i2c = 0;

			// Acknowledge the read completion
			peri_i2c->wait_queue_flag_com_i2c_ack = 1;
			wake_up_interruptible(&peri_i2c->wait_queue_i2c_ioctl_ack);
			ret = 0;
		}
		break;

	case I2C_SMBUS_WORD_DATA:
		if (read_write == I2C_SMBUS_WRITE)
		{
			message = kmalloc(6, GFP_KERNEL);
			if (!message)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			I2C_DEBUG("data->word %02x\n", data->word);

			message[0] = addr << 1 | read_write;	   // slave address + Read/Write bit
			message[1] = (read_write << 7) | 0;		   // register_read_bit + detect flag
			message[2] = (word_length + 1);			   // word length is 2-byte
			message[3] = command;					   // Command/Register address
			message[4] = (data->word >> 8) & 0xFF;	   // MSB first
			message[5] = (data->word & 0x00FF) & 0xFF; // LSB second

			I2C_DEBUG("message[4]: %02x\n", message[4]);
			I2C_DEBUG("message[5]: %02x\n", message[5]);

			set_periplex_data(peri_i2c->periplex_id, 6, message);
			kfree(message);

			I2C_DEBUG("wait in (WRITE) for I2C_SMBUS_WORD_DATA\n");
			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_word_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}
		}
		else
		{
			I2C_DEBUG("before: data->word %02x\n", data->byte);

			message = kmalloc(4, GFP_KERNEL);
			if (!message)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = addr << 1 | read_write; // Slave address + Read/Write bit
			message[1] = (read_write << 7) | 0;	 // register_read_bit + detect flag
			message[2] = word_length;			 // word length is 2-byte
			message[3] = command;				 // Command/Register address

			set_periplex_data(peri_i2c->periplex_id, 4, message);
			kfree(message);

			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_word_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_word_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			do
			{
				// Reset wait flag before waiting
				I2C_DEBUG("wait in (READ) for the I2C_SMBUS_WORD_DATA\n");
				peri_i2c->wait_queue_flag_com_i2c = 0;
				ret = wait_event_interruptible_timeout(peri_i2c->wait_queue_i2c_ioctl,
													   peri_i2c->wait_queue_flag_com_i2c != 0,
													   msecs_to_jiffies(1000));

				if (ret == 0)
				{
					pr_err("periplex_i2c: Timed out waiting in I2C_SMBUS_WORD_DATA\n");
					ret = -ETIMEDOUT;
					mutex_unlock(&peri_i2c->i2c_mutex);
					return ret;
				}
				else if (ret < 0)
				{
					pr_err("periplex_i2c: Wait interrupted with error in I2C_SMBUS_WORD_DATA: %d\n", ret);
					mutex_unlock(&peri_i2c->i2c_mutex);
					return ret;
				}

				if (!peri_i2c->read_data_i2c)
				{
					mutex_unlock(&peri_i2c->i2c_mutex);
					return -EFAULT;
				}

				index += peri_i2c->read_length_i2c;
				remaining = word_length - index;

				if (index == 1)
				{
					msb = peri_i2c->read_data_i2c[0];
				}
				else if (index == 2)
				{
					lsb = peri_i2c->read_data_i2c[0];
					data->word = (msb << 8) | lsb;
				}

				I2C_DEBUG("word read: copied %d bytes, %d remaining\n",
						  peri_i2c->read_length_i2c, remaining);

				devm_kfree(peri_i2c->device, peri_i2c->read_data_i2c);
				peri_i2c->read_data_i2c = NULL;
				peri_i2c->read_length_i2c = 0;

				// Acknowledge the read completion
				peri_i2c->wait_queue_flag_com_i2c_ack = 1;
				wake_up_interruptible(&peri_i2c->wait_queue_i2c_ioctl_ack);

			} while (remaining > 0);
			I2C_DEBUG("after: data->word %02x\n", data->word);
			ret = 0;
		}
		break;

	case I2C_SMBUS_BLOCK_DATA:
		dev_err(&adap->dev, "SMBus block read not supported by adapter\n");
		I2C_DEBUG("I2C_SMBUS_BLOCK_DATA not supported\n");
		mutex_unlock(&peri_i2c->i2c_mutex);
		return -EOPNOTSUPP; // Operation not supported

	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (!data)
		{
			mutex_unlock(&peri_i2c->i2c_mutex);
			return -EINVAL;
		}

		if (read_write == I2C_SMBUS_WRITE)
		{
			message = kmalloc(data->block[0] + 4, GFP_KERNEL);
			if (!message)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = addr << 1 | read_write; // Slave address + Read/Write bit
			message[1] = (read_write << 7) | 0;	 // register_read_bit + detect flag
			message[2] = (data->block[0] + 1);	 // length
			message[3] = command;				 // Command/Register address
			memcpy(message + 4, &data->block[1], data->block[0]);

			I2C_DEBUG("%s Block Write: addr=0x%02x, cmd=0x%02x, len=%d\n",
					  size == I2C_SMBUS_BLOCK_DATA ? "SMBus" : "I2C",
					  addr, command, data->block[0]);

			set_periplex_data(peri_i2c->periplex_id, data->block[0] + 4, message);
			kfree(message);

			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_i2c_block_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}
		}
		else
		{
			I2C_DEBUG("wait in (READ) for the I2C_SMBUS_BLOCK_DATA\n");
			I2C_DEBUG("data->block[0] (max bytes to read): %d\n", data->block[0]);

			message = kmalloc(4, GFP_KERNEL);
			if (!message)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return -ENOMEM;
			}

			message[0] = addr << 1 | read_write; // Slave address + Read/Write bit
			message[1] = (read_write << 7) | 0;	 // register_read_bit + detect flag
			message[2] = ((size == I2C_SMBUS_BLOCK_DATA)
							  ? I2C_SMBUS_BLOCK_MAX
							  : data->block[0]); // max length
			message[3] = command;				 // Command/Register address

			set_periplex_data(peri_i2c->periplex_id, 4, message);
			kfree(message);

			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_i2c_block_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			ret = wait_for_i2c_write_ack_response(peri_i2c, addr, "periplex_i2c: smbus_i2c_block_data");
			if (ret < 0)
			{
				mutex_unlock(&peri_i2c->i2c_mutex);
				return ret;
			}

			do
			{
				// Reset wait flag before waiting
				peri_i2c->wait_queue_flag_com_i2c = 0;
				ret = wait_event_interruptible_timeout(peri_i2c->wait_queue_i2c_ioctl,
													   peri_i2c->wait_queue_flag_com_i2c != 0,
													   msecs_to_jiffies(1000));

				if (ret == 0)
				{
					pr_err("periplex_i2c: Timed out waiting in I2C_SMBUS_BLOCK_DATA\n");
					ret = -ETIMEDOUT;
					mutex_unlock(&peri_i2c->i2c_mutex);
					return ret;
				}
				else if (ret < 0)
				{
					pr_err("periplex_i2c: Wait interrupted with error in I2C_SMBUS_BLOCK_DATA: %d\n", ret);
					mutex_unlock(&peri_i2c->i2c_mutex);
					return ret;
				}

				if (!peri_i2c->read_data_i2c)
				{
					mutex_unlock(&peri_i2c->i2c_mutex);
					return -EFAULT;
				}

				remaining = data->block[0] - index;
				if (remaining <= 0)
					break;

				memcpy(data->block + index + 1, peri_i2c->read_data_i2c,
					   min(remaining, peri_i2c->read_length_i2c));
				index += peri_i2c->read_length_i2c;

				I2C_DEBUG("Block read: copied %d bytes, %d remaining\n",
						  peri_i2c->read_length_i2c, remaining - peri_i2c->read_length_i2c);

				devm_kfree(peri_i2c->device, peri_i2c->read_data_i2c);
				peri_i2c->read_data_i2c = NULL;
				peri_i2c->read_length_i2c = 0;

				// Acknowledge the read completion
				peri_i2c->wait_queue_flag_com_i2c_ack = 1;
				wake_up_interruptible(&peri_i2c->wait_queue_i2c_ioctl_ack);

			} while ((remaining - 1) > 0);
			ret = 0;
		}
		break;

	default:
		mutex_unlock(&peri_i2c->i2c_mutex);
		return -EOPNOTSUPP;
	}

	mutex_unlock(&peri_i2c->i2c_mutex);
	return ret;
}

/*
** I2C algorithm Structure
*/
static struct i2c_algorithm i2c_algorithm_1f = {
	.smbus_xfer = i2c_smbus_xfer,
	.master_xfer = i2c_xfer,
	.functionality = i2c_func,
};

/*
** probe functions for device registration
*/
static int periplex_i2c_probe(struct periplex_device *pdev)
{
	int ret = 0;
	int divider = 0;
	unsigned int speed_hz = 0;
	struct periplex_i2c_data *peri_i2c = NULL;
	struct i2c_timings i2c_timings = {0};

	// Allocate memory for per-adapter data structure
	peri_i2c = devm_kzalloc(&pdev->dev, sizeof(struct periplex_i2c_data), GFP_KERNEL);
	if (!peri_i2c)
	{
		dev_err(&pdev->dev, "Failed to allocate memory for I2C data\n");
		return -ENOMEM;
	}

	// Allocate I2C adapter
	peri_i2c->adapter = devm_kzalloc(&pdev->dev, sizeof(struct i2c_adapter), GFP_KERNEL);
	if (!peri_i2c->adapter)
	{
		dev_err(&pdev->dev, "Failed to allocate I2C adapter\n");
		return -ENOMEM;
	}

	// Initialize per-adapter data structure
	peri_i2c->device = &pdev->dev;
	peri_i2c->read_data_i2c = NULL;
	peri_i2c->read_length_i2c = 0;
	peri_i2c->wait_queue_flag_com_i2c = 0;
	peri_i2c->wait_queue_flag_com_i2c_ack = 0;

	// Initialize wait queues and mutex
	init_waitqueue_head(&peri_i2c->wait_queue_i2c_ioctl);
	init_waitqueue_head(&peri_i2c->wait_queue_i2c_ioctl_ack);
	mutex_init(&peri_i2c->i2c_mutex);

	// Read device tree properties
	if (device_property_read_u32(&pdev->dev, "periplex-id", &peri_i2c->periplex_id))
	{
		dev_err(&pdev->dev, "Failed to read periplex-id from device tree for i2c\n");
		return -EINVAL;
	}

	if (device_property_read_u32(&pdev->dev, "clock-frequency", &speed_hz))
	{
		dev_err(&pdev->dev, "Failed to read clock-frequency from device tree for i2c\n");
		speed_hz = I2C_MAX_STANDARD_MODE_FREQ; // Default to 100kHz if not specified
	}

	// Setup I2C adapter
	peri_i2c->adapter->owner = THIS_MODULE;
	peri_i2c->adapter->class = I2C_CLASS_HWMON;
	peri_i2c->adapter->algo = &i2c_algorithm_1f;
	snprintf(peri_i2c->adapter->name, sizeof(peri_i2c->adapter->name), "I2C-PERIPLEX-%d",
			 peri_i2c->periplex_id);
	peri_i2c->adapter->nr = -1;					   // Auto-assign adapter number
	peri_i2c->adapter->dev.driver_data = peri_i2c; // Store per-adapter data pointer
	peri_i2c->adapter->dev.parent = &pdev->dev;
	peri_i2c->adapter->dev.of_node = pdev->dev.of_node;

	// Setup periplex device
	pdev->periplex_id = peri_i2c->periplex_id;
	pdev->get_periplex_data = read_data_for_i2c;

	// This is mandatory part to register device with periplex
	periplex_link_device(pdev);
	periplex_set_drvdata(pdev, peri_i2c);

	// Configure frequency
	i2c_timings.bus_freq_hz = speed_hz;
	i2c_parse_fw_timings(&peri_i2c->adapter->dev, &i2c_timings, true);
	pr_info("periplex_i2c: I2C Bus Frequency: %u Hz for periplex_id: %d\n", i2c_timings.bus_freq_hz,
			peri_i2c->periplex_id);

	divider = (((FCLK) / (4 * i2c_timings.bus_freq_hz)) - 1);
	set_periplex_configuration(peri_i2c->periplex_id, 0, divider);

	// Register I2C adapter
	ret = i2c_add_numbered_adapter(peri_i2c->adapter);
	if (ret)
	{
		pr_err("periplex_i2c: Failed to add adapter %s\n", peri_i2c->adapter->name);
		goto cleanup;
	}

	pr_info("periplex_i2c: I2C Bus Driver Added: %s (adapter nr: %d, periplex_id: %d)\n",
			peri_i2c->adapter->name, peri_i2c->adapter->nr, peri_i2c->periplex_id);

	return 0;

cleanup:
	if (peri_i2c->adapter)
		i2c_del_adapter(peri_i2c->adapter);
	periplex_unlink_device(pdev);
	return ret;
}

/*
** remove functions for device unregistration
*/
static int periplex_i2c_remove(struct periplex_device *pdev)
{
	struct periplex_i2c_data *peri_i2c = periplex_get_drvdata(pdev);

	if (!peri_i2c)
	{
		dev_err(&pdev->dev, "No I2C data found for device\n");
		return -ENODEV;
	}

	I2C_DEBUG("Removing I2C Bus Driver: %s (periplex_id: %d)\n",
			  peri_i2c->adapter ? peri_i2c->adapter->name : "unknown",
			  peri_i2c->periplex_id);

	// Remove I2C adapter
	if (peri_i2c->adapter)
	{
		i2c_del_adapter(peri_i2c->adapter);
		pr_info("periplex_i2c: I2C adapter removed for periplex_id: %d\n", peri_i2c->periplex_id);
	}

	// Clean up any remaining read data
	mutex_lock(&peri_i2c->i2c_mutex);
	if (peri_i2c->read_data_i2c)
	{
		devm_kfree(peri_i2c->device, peri_i2c->read_data_i2c);
		peri_i2c->read_data_i2c = NULL;
		peri_i2c->read_length_i2c = 0;
	}
	mutex_unlock(&peri_i2c->i2c_mutex);

	// Unlink from periplex framework
	periplex_unlink_device(pdev);

	pr_info("periplex_i2c: I2C Bus Driver removed successfully\n");
	return 0;
}

/*
** compitable property match with DTSO of i2c
*/
struct of_device_id periplex_i2c_dt_match[] = {
	{.compatible = "vicharak,periplex-i2c"},
	{},
};
MODULE_DEVICE_TABLE(of, periplex_i2c_dt_match);

struct periplex_driver periplex_i2c_driver = {
	.probe = periplex_i2c_probe,
	.remove = periplex_i2c_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = periplex_i2c_dt_match,
	},
};
module_periplex_driver(periplex_i2c_driver);

MODULE_ALIAS("periplex:i2c");
MODULE_AUTHOR("vatsal Kevadiya<vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("I2C Driver for the Periplex");
MODULE_LICENSE("GPL");