/*
** periplex_can.c - Periplex CAN Device Driver
**
** Significant features:
** 1. Register multiple CAN devices using Device Tree (DT).
** 2. Provide open, close, and transmit operations for CAN devices.
** 3. Integrate with Linux CAN subsystem using register_candev and unregister_candev.
** 4. Support for reading and writing CAN frames.
** 5. You can see the can devices in /sys/class/net/can*. and also in ip link show.
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/can/dev.h>
#include <linux/can/netlink.h>
#include <linux/netdevice.h>
#include <linux/mutex.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/delay.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

#define DRIVER_NAME "periplex-can"
#define PERIPLEX_MAX_TX_MBOX 1
#define DEFAULT_BITRATE 125000 // Changed to 125kbps which is more standard
#define CAN_CLOCK 50000000     // Assuming 50MHz CAN clock, adjust as per your hardware
#define MAX_CAN_DATA_SIZE 64   // Maximum CAN data size

// CAN frame flags
#define RTR_ON 0x10
#define RTR_OFF 0x00
#define IDE_ON 0x20
#define IDE_OFF 0x00
#define STATE_QUERY_ON 0x80
#define STATE_QUERY_OFF 0x00

/* 
** Debug flag (can be set via module parameter) 
*/
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* 
** Macro to conditionally print debug info 
*/
#define CAN_DEBUG(fmt, ...)              \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info("periplex_can: "fmt, ##__VA_ARGS__); \
    } while (0)

/*
** Structure to hold CAN device data
*/
struct periplex_can
{
    // Device-specific data
    struct can_priv can;
    struct can_frame *cf;
    struct device_node *np;
    struct net_device *ndev;
    struct work_struct tx_work;
    atomic_t thread_should_stop;
    struct periplex_device *pdev;
    struct workqueue_struct *tx_wq;
    struct task_struct *monitor_thread;

    // Device state
    bool is_open;
    bool state_query;

    // Spinlock for thread safety
    spinlock_t lock;

    //  Error counters
    u8 tx_error_counter;
    u8 rx_error_counter;

    // Device ID and read length
    int periplex_id;
    int periplex_read_length;
    unsigned char read_data_can[MAX_CAN_DATA_SIZE];

    // waitqueue used for internal operations
    wait_queue_head_t wait_queue_can_ioctl;
    int wait_queue_flag_com_can;
    wait_queue_head_t wait_queue_can_ioctl_ack;
    int wait_queue_flag_com_can_ack;
};

/* 
** Define the CAN device structure
*/
static const struct can_bittiming_const periplex_can_bittiming_const = {
    .name = "periplex-can",
    .tseg1_min = 1,
    .tseg1_max = 16,
    .tseg2_min = 1,
    .tseg2_max = 8,
    .sjw_max = 4,
    .brp_min = 1,
    .brp_max = 64,
    .brp_inc = 1,
};

/*
** Forward declarations
*/
static int periplex_can_open(struct net_device *netdev);
static int periplex_can_stop(struct net_device *netdev);
static netdev_tx_t periplex_can_start_xmit(struct sk_buff *skb,
                                           struct net_device *netdev);
static void periplex_tx_work_handler(struct work_struct *work);
static void periplex_can_update_error_counters(struct net_device *dev, u8 tx_err, u8 rx_err);
static int periplex_can_get_berr_counter(const struct net_device *dev,
                                         struct can_berr_counter *bec);


/* 
** Define the CAN net device operations
*/
static const struct net_device_ops periplex_can_netdev_ops = {
    .ndo_open = periplex_can_open,
    .ndo_stop = periplex_can_stop,
    .ndo_start_xmit = periplex_can_start_xmit,
};

/*
** Updated error counter handling
*/
static void periplex_can_update_error_counters(struct net_device *dev, u8 tx_err, u8 rx_err)
{
    struct periplex_can *pcd = netdev_priv(dev);
    CAN_DEBUG("inside the can_update_error_counter\n");

    spin_lock(&pcd->lock);

    pcd->tx_error_counter = tx_err;
    pcd->rx_error_counter = rx_err;

    // Update statistics(this is show at Tx and Rx)
    dev->stats.rx_errors = rx_err;
    dev->stats.tx_errors = tx_err;

    spin_unlock(&pcd->lock);
}

/*
**Updated berr counter handling
*/
static int periplex_can_get_berr_counter(const struct net_device *dev,
                                         struct can_berr_counter *bec)
{
    struct periplex_can *pcd = netdev_priv(dev);
    CAN_DEBUG("pcd->tx %d and pcd->rx %d\n", pcd->tx_error_counter, pcd->rx_error_counter);

    bec->txerr = pcd->tx_error_counter;
    bec->rxerr = pcd->rx_error_counter;

    return 0;
}

/*
** this functions is used in read for can
*/
int read_data_for_can(struct periplex_device *pdev, char *message, const int len)
{
    int ret;
    struct periplex_can *pcd = periplex_get_drvdata(pdev);
    struct net_device *netdev;
    struct can_frame *cf;
    struct sk_buff *skb;
    enum can_state rx_state, tx_state;
    u8 state_flags, tx_err, rx_err;

    /* Validate device data */
    if (!pcd)
    {
        pr_err("periplex_can: No device data found\n");
        return -ENODEV;
    }

    netdev = pcd->ndev;
    if (!netdev)
    {
        pr_err("periplex_can: No network device found\n");
        return -ENODEV;
    }

    /* Validate input parameters */
    if (!message || len <= 0 || len > MAX_CAN_DATA_SIZE)
    {
        pr_err("periplex_can: Invalid message or length (%d)\n", len);
        return -EINVAL;
    }

    CAN_DEBUG("length is %d\n", len);
    CAN_DEBUG("can read calling\n");

    /* Store length and copy data to per-device buffer */
    pcd->periplex_read_length = len;

    /* Use per-device buffer instead of global read_data_can */
    if (memcpy(pcd->read_data_can, message, len) == NULL)
    {
        pr_err("periplex_can: not able to copy\n");
        return -EFAULT;
    }

    // Extract state values
    state_flags = (u8)pcd->read_data_can[0];
    CAN_DEBUG("State flags: 0x%02X\n", state_flags);

    // Update CAN state based on flags
    if (((state_flags >> 7) == 1) && pcd->state_query)
    {
        CAN_DEBUG("inside the can actual error-count frame\n");

        // Extract error counter values
        tx_err = (u8)pcd->read_data_can[1];
        rx_err = (u8)pcd->read_data_can[2];

        skb = alloc_can_err_skb(netdev, &cf);
        if (!skb)
        {
            pr_err("periplex_can: Cannot allocate error frame\n");
            return -ENOMEM;
        }

        switch (state_flags & 0x03)
        {
        case 0:
            rx_state = rx_err >= tx_err ? CAN_STATE_ERROR_ACTIVE : 0;
            tx_state = rx_err <= tx_err ? CAN_STATE_ERROR_ACTIVE : 0;
            pr_info("periplex_can: CAN State: ERROR_ACTIVE\n");
            break;
        case 1:
            rx_state = rx_err >= tx_err ? CAN_STATE_ERROR_PASSIVE : 0;
            tx_state = rx_err <= tx_err ? CAN_STATE_ERROR_PASSIVE : 0;
            cf->can_id = CAN_ERR_FLAG | CAN_ERR_CRTL;
            cf->can_dlc = CAN_ERR_DLC;
            memset(cf->data, 0, CAN_ERR_DLC);
            cf->data[1] = CAN_ERR_CRTL_TX_PASSIVE;
            pr_info("periplex_can: CAN State: ERROR_PASSIVE\n");
            break;
        case 2:
            rx_state = rx_err >= tx_err ? CAN_STATE_BUS_OFF : 0;
            tx_state = rx_err <= tx_err ? CAN_STATE_BUS_OFF : 0;
            cf->can_id = CAN_ERR_FLAG | CAN_ERR_BUSOFF;
            cf->can_dlc = CAN_ERR_DLC;
            memset(cf->data, 0, CAN_ERR_DLC);
            pr_info("periplex_can: CAN State: BUS_OFF\n");
            break;
        default:
            pr_warn("periplex_can: Unknown CAN state received\n");
            break;
        }

        // Update error counters and state
        periplex_can_update_error_counters(netdev, tx_err, rx_err);
        can_change_state(netdev, cf, tx_state, rx_state);

        CAN_DEBUG("CAN: Updated TX Error: %d, RX Error: %d\n", tx_err, rx_err);
        pcd->state_query = false;
    }
    else
    {
        // Need to handle extened and standard id of read-frame
        CAN_DEBUG("inside the can actual read frame\n");

        pcd->wait_queue_flag_com_can = 1;
        wake_up_interruptible(&pcd->wait_queue_can_ioctl);

        pcd->wait_queue_flag_com_can_ack = 0;
        ret = wait_event_interruptible_timeout(pcd->wait_queue_can_ioctl_ack,
                                               pcd->wait_queue_flag_com_can_ack != 0,
                                               msecs_to_jiffies(5000));
        if (ret == 0)
        {
            // Timeout occurred
            pr_err("periplex_can: Timed out waiting for read data \n");
            ret = -ETIMEDOUT;
            return ret;
        }
        else if (ret < 0)
        {
            // Wait was interrupted
            pr_err("periplex_can: RX wait interrupted: %d\n", ret);
            return ret;
        }
    }
    return 0;
}

/*
** This function will run continuously in the background
*/
static int periplex_can_read_monitor_thread(void *data)
{
    struct periplex_can *pcd = (struct periplex_can *)data;
    struct net_device *netdev = pcd->ndev;
    struct net_device_stats *stats = &netdev->stats;
    struct can_frame *cf;
    struct sk_buff *skb;
    unsigned long timeout_jiffies = msecs_to_jiffies(5000);
    int ret;

    /* Set thread to be killable */
    set_current_state(TASK_INTERRUPTIBLE);

    /* Monitor loop */
    while (!atomic_read(&pcd->thread_should_stop) && !kthread_should_stop())
    {
        int i, j, k;
        int index = 0;

        /* Only perform operations if device is open */
        if (!pcd->is_open)
        {
            msleep_interruptible(100);
            continue;
        }

        CAN_DEBUG("waiting: CAN monitor thread\n");
        pcd->wait_queue_flag_com_can = 0;

        ret = wait_event_interruptible_timeout(pcd->wait_queue_can_ioctl,
                                               pcd->wait_queue_flag_com_can != 0 ||
                                                   atomic_read(&pcd->thread_should_stop) ||
                                                   kthread_should_stop(),
                                               timeout_jiffies);

        /* Check for thread termination first */
        if (atomic_read(&pcd->thread_should_stop) || kthread_should_stop())
        {
            pr_info("periplex_can: CAN read monitor thread stopping\n");
            break;
        }

        if (ret == 0)
        {
            CAN_DEBUG("CAN: wait timeout occurred\n");
            continue;
        }
        else if (ret < 0)
        {
            pr_info("periplex_can: wait interrupted by signal %d\n", ret);
            if (ret == -ERESTARTSYS)
            {
                continue;
            }
            break;
        }

        /* Allocate SKB - check for NULL */
        skb = alloc_can_skb(netdev, &cf);
        if (!skb)
        {
            pr_err("periplex_can: Cannot allocate CAN frame\n");
            stats->rx_dropped++;
            continue;
        }

        /* Process CAN frame data */
        if ((pcd->read_data_can[0] & 0x20) == 0x20)
        {
            /* Extended ID processing */
            CAN_DEBUG("inside read of the extended id\n");

            cf->can_id = (((pcd->read_data_can[0] & 0x0F) << 28) |
                          (pcd->read_data_can[1] << 20) |
                          (pcd->read_data_can[2] << 12) |
                          (pcd->read_data_can[3] << 4) |
                          (pcd->read_data_can[4] >> 4));

            cf->can_id &= CAN_EFF_MASK;
            cf->can_id |= CAN_EFF_FLAG;
            cf->can_dlc = pcd->read_data_can[4] & 0x0F;

            /* Validate DLC */
            if (cf->can_dlc > 8)
            {
                cf->can_dlc = 8;
            }

            if (cf->can_dlc >= 5)
            {
                for (i = 0; i < 5 && index < cf->can_dlc; i++)
                {
                    if (pcd->periplex_read_length > 5 + i)
                    {
                        cf->data[index] = pcd->read_data_can[5 + i];
                        index++;
                    }
                }

                /* Signal acknowledgment */
                pcd->wait_queue_flag_com_can_ack = 1;
                wake_up_interruptible(&pcd->wait_queue_can_ioctl_ack);

                /* Wait for additional data if needed */
                if (index < cf->can_dlc)
                {
                    pcd->wait_queue_flag_com_can = 0;
                    ret = wait_event_interruptible_timeout(pcd->wait_queue_can_ioctl,
                                                           pcd->wait_queue_flag_com_can != 0 ||
                                                               atomic_read(&pcd->thread_should_stop) ||
                                                               kthread_should_stop(),
                                                           timeout_jiffies);

                    if (atomic_read(&pcd->thread_should_stop) || kthread_should_stop())
                    {
                        dev_kfree_skb(skb);
                        break;
                    }

                    if (ret > 0)
                    {
                        k = 0;
                        for (j = index; j < cf->can_dlc; j++)
                        {
                            cf->data[j] = pcd->read_data_can[k++];
                        }
                    }
                    else
                    {
                        dev_kfree_skb(skb);
                        continue;
                    }
                }
            }
            else
            {
                for (i = 0; i < cf->can_dlc; i++)
                {
                    cf->data[i] = pcd->read_data_can[5 + i];
                }
            }
        }
        else
        {
            /* Standard ID processing */
            CAN_DEBUG("inside read of the standard id\n");

            cf->can_id = (((pcd->read_data_can[0] & 0x0F) << 8) |
                          pcd->read_data_can[1]) &
                         CAN_SFF_MASK;
            cf->can_dlc = pcd->read_data_can[2] & 0x0F;

            /* Validate DLC */
            if (cf->can_dlc > 8)
            {
                cf->can_dlc = 8;
            }

            if (cf->can_dlc >= 7)
            {
                for (i = 0; i < 7 && index < cf->can_dlc; i++)
                {
                    if (pcd->periplex_read_length > 3 + i)
                    {
                        cf->data[index] = pcd->read_data_can[3 + i];
                        index++;
                    }
                }

                /* Signal acknowledgment */
                pcd->wait_queue_flag_com_can_ack = 1;
                wake_up_interruptible(&pcd->wait_queue_can_ioctl_ack);

                /* Wait for additional data if needed */
                if (index < cf->can_dlc)
                {
                    pcd->wait_queue_flag_com_can = 0;
                    ret = wait_event_interruptible_timeout(pcd->wait_queue_can_ioctl,
                                                           pcd->wait_queue_flag_com_can != 0 ||
                                                               atomic_read(&pcd->thread_should_stop) ||
                                                               kthread_should_stop(),
                                                           timeout_jiffies);

                    if (atomic_read(&pcd->thread_should_stop) || kthread_should_stop())
                    {
                        dev_kfree_skb(skb);
                        break;
                    }

                    if (ret > 0)
                    {
                        k = 0;
                        for (j = index; j < cf->can_dlc; j++)
                        {
                            cf->data[j] = pcd->read_data_can[k++];
                        }
                    }
                    else
                    {
                        dev_kfree_skb(skb);
                        continue;
                    }
                }
            }
            else
            {
                for (i = 0; i < cf->can_dlc; i++)
                {
                    cf->data[i] = pcd->read_data_can[3 + i];
                }
            }
        }

        /* Send frame to network stack */
        skb->protocol = htons(ETH_P_CAN);
        skb->pkt_type = PACKET_BROADCAST;
        skb->dev = netdev;
        skb->ip_summed = CHECKSUM_UNNECESSARY;

        if (netif_rx(skb) == NET_RX_SUCCESS)
        {
            stats->rx_packets++;
            stats->rx_bytes += cf->can_dlc;
        }
        else
        {
            pr_err("periplex_can: Failed to send CAN frame to network stack\n");
            stats->rx_dropped++;
        }

        /* Signal acknowledgment */
        pcd->wait_queue_flag_com_can_ack = 1;
        wake_up_interruptible(&pcd->wait_queue_can_ioctl_ack);
    }

    pr_info("periplex_can: CAN monitor thread stopped cleanly\n");
    return 0;
}

/*
** set bittiming for periplex can
*/
static int periplex_can_set_bittiming(struct net_device *netdev)
{
    struct periplex_can *pcd = netdev_priv(netdev);
    struct can_bittiming *bt = &pcd->can.bittiming;
    int configuration = (CAN_CLOCK) / (2 * bt->bitrate);

    netdev_info(netdev, "Setting bitrate to %d bps\n", bt->bitrate);

    set_periplex_configuration(pcd->periplex_id, 0, configuration);

    return 0;
}

/*
** Work queue for non-blocking CAN transmission
*/
static void periplex_tx_work_handler(struct work_struct *work)
{
    int i;
    int configuration;
    u8 message[20] = {0};
    struct periplex_can *pcd = container_of(work, struct periplex_can, tx_work);

    CAN_DEBUG("VATSAL CAN TX: ID: 0x%X, DLC: %d, Data: %s\n",
              pcd->cf->can_id,
              pcd->cf->can_dlc,
              message);

    if (pcd->cf->can_dlc == 0)
    {
        CAN_DEBUG("CAN TX: ID: 0x%X, DLC: %d, Data: %s\n",
                  pcd->cf->can_id,
                  pcd->cf->can_dlc,
                  message);

        if ((pcd->cf->can_id >> 11) == 0)
        {
            configuration = 0;
            CAN_DEBUG("CAN_configuration: Frame is using standard identifier\n");
            set_periplex_configuration(pcd->periplex_id, 1, configuration);
        }
        else
        {
            if (((pcd->cf->can_id - CAN_RTR_FLAG) >> 11) == 0)
            {
                pr_info("CAN: RTR is not supported for zero length\n");
            }
            else
            {
                configuration = (pcd->cf->can_id | CAN_EFF_FLAG);
                CAN_DEBUG("configuration is 0x%X\n", configuration);
                CAN_DEBUG("CAN_configuration: Frame is using extended identifier\n");
                set_periplex_configuration(pcd->periplex_id, 1, configuration);
            }
        }
    }
    else
    {
        if (pcd->cf->can_id == 0)
        {
            // data = [0x80], dlc=1 for the state query request
            message[0] = pcd->cf->data[0];
            set_periplex_data(pcd->periplex_id, pcd->cf->can_dlc, (char *)message);
            pcd->state_query = true;
        }
        else
        {
            if ((pcd->cf->can_id >> 11) == 0)
            {
                CAN_DEBUG("inside SFF\n");
                CAN_DEBUG("CAN TX: ID: 0x%X, DLC: %d, Data: %s\n",
                          pcd->cf->can_id,
                          pcd->cf->can_dlc,
                          message);

                message[0] = ((STATE_QUERY_OFF | IDE_OFF) & 0xF0) | (pcd->cf->can_id >> 8);
                message[1] = (pcd->cf->can_id & 0x0FF) & 0xFF;
                message[2] = pcd->cf->can_dlc;

                // Prepare data string for transmission
                for (i = 0; i < pcd->cf->can_dlc; i++)
                {
                    message[i + 3] = pcd->cf->data[i];
                    CAN_DEBUG("data is %c\n", pcd->cf->data[i]);
                }
                set_periplex_data(pcd->periplex_id, pcd->cf->can_dlc + 3, (char *)message);
                CAN_DEBUG("inside 11-bit standard-id for the message\n");
            }
            else
            {
                bool is_rtr = true; // Flag to determine if frame is RTR

                // Check if all data bytes are 0x00 to identify RTR frame
                for (i = 0; i < pcd->cf->can_dlc; i++)
                {
                    if (pcd->cf->data[i] != 0x00)
                    {
                        is_rtr = false;
                        break;
                    }
                    CAN_DEBUG("data is %c\n", pcd->cf->data[i]);
                }

                if (is_rtr)
                {
                    if (((pcd->cf->can_id - CAN_RTR_FLAG) >> 11) == 0)
                    {
                        u32 id = (pcd->cf->can_id - CAN_RTR_FLAG);
                        CAN_DEBUG("CAN TX RTR: ID: 0x%X, DLC: %d\n",
                                  pcd->cf->can_id,
                                  pcd->cf->can_dlc);

                        message[0] = ((STATE_QUERY_OFF | IDE_OFF | RTR_ON) & 0xF0) | (id >> 8);
                        message[1] = (pcd->cf->can_id & 0x0FF) & 0xFF;
                        message[2] = pcd->cf->can_dlc;

                        set_periplex_data(pcd->periplex_id, 3, (char *)message);
                        CAN_DEBUG("inside RTR for 11-bit\n");
                    }
                    else
                    {
                        u32 id = (pcd->cf->can_id - CAN_RTR_FLAG - CAN_EFF_FLAG);
                        CAN_DEBUG("CAN TX RTR: ID: 0x%X, DLC: %d\n",
                                  pcd->cf->can_id,
                                  pcd->cf->can_dlc);

                        message[0] = ((STATE_QUERY_OFF | IDE_ON | RTR_ON) & 0xF0) | (id >> 28);
                        message[1] = ((id & 0x0FF00000) >> 20) & 0xFF;
                        message[2] = ((id & 0x000FF000) >> 12) & 0xFF;
                        message[3] = ((id & 0x00000FF0) >> 4) & 0xFF;
                        message[4] = ((id & 0x0000000F) << 4) | pcd->cf->can_dlc;

                        set_periplex_data(pcd->periplex_id, 5, (char *)message);

                        CAN_DEBUG("inside RTR for 29-bit\n");
                    }
                }
                else
                {
                    u32 id = (pcd->cf->can_id - CAN_EFF_FLAG);
                    CAN_DEBUG("inside EFF\n");
                    CAN_DEBUG("CAN TX EFF: ID: 0x%X, DLC: %d, Data: %s\n",
                              pcd->cf->can_id & CAN_EFF_MASK,
                              pcd->cf->can_dlc,
                              message);

                    message[0] = ((STATE_QUERY_OFF | IDE_ON) & 0xF0) | (id >> 28);
                    message[1] = ((id & 0x0FF00000) >> 20) & 0xFF;
                    message[2] = ((id & 0x000FF000) >> 12) & 0xFF;
                    message[3] = ((id & 0x00000FF0) >> 4) & 0xFF;
                    message[4] = ((id & 0x0000000F) << 4) | pcd->cf->can_dlc;

                    // Prepare data string for transmission
                    for (i = 0; i < pcd->cf->can_dlc; i++)
                    {
                        message[i + 5] = pcd->cf->data[i];
                        CAN_DEBUG("data is %c\n", pcd->cf->data[i]);
                    }
                    set_periplex_data(pcd->periplex_id, pcd->cf->can_dlc + 5, (char *)message);
                    CAN_DEBUG("inside 29-bit standard-id for the message\n");
                }
            }
        }
    }

    // Free allocated frame memory
    kfree(pcd->cf);
    pcd->cf = NULL;
}

/*
** actual can transmit function for periplex_can
*/
static netdev_tx_t periplex_can_start_xmit(struct sk_buff *skb, struct net_device *netdev)
{
    struct periplex_can *pcd = netdev_priv(netdev);
    struct can_frame *cf = (struct can_frame *)skb->data;

    if (can_dropped_invalid_skb(netdev, skb))
        return NETDEV_TX_OK;

    // Copy frame data into private structure for transmission
    pcd->cf = kmemdup(cf, sizeof(struct can_frame), GFP_ATOMIC);
    if (!pcd->cf)
    {
        pr_err("periplex_can: Failed to allocate memory for CAN frame");
        return NETDEV_TX_BUSY;
    }

    queue_work(pcd->tx_wq, &pcd->tx_work);

    netdev->stats.tx_packets++;
    netdev->stats.tx_bytes += cf->can_dlc;

    return NETDEV_TX_OK;
}

/*
** used for open can interface
*/
static int periplex_can_open(struct net_device *netdev)
{
    struct periplex_can *pcd = netdev_priv(netdev);
    int err;

    /* Set default bitrate if not set */
    if (!pcd->can.bittiming.bitrate)
    {
        pcd->can.bittiming.bitrate = DEFAULT_BITRATE;
    }

    /* Open the can device */
    err = open_candev(netdev);
    if (err)
    {
        netdev_err(netdev, "Failed to open CAN device\n");
        return err;
    }

    pcd->is_open = true;
    pcd->can.state = CAN_STATE_ERROR_ACTIVE;
    netif_start_queue(netdev);

    /* Initialize and start monitoring thread */
    atomic_set(&pcd->thread_should_stop, 0);
    pcd->monitor_thread = kthread_run(periplex_can_read_monitor_thread, pcd,
                                      "periplex_can_monitor_%d", pcd->periplex_id);
    if (IS_ERR(pcd->monitor_thread))
    {
        err = PTR_ERR(pcd->monitor_thread);
        netdev_err(netdev, "Failed to start monitoring thread: %d\n", err);
        close_candev(netdev);
        pcd->is_open = false;
        pcd->state_query = false;
        return err;
    }

    netdev_info(netdev, "periplex can device opened with bitrate %d\n",
                pcd->can.bittiming.bitrate);
    return 0;
}

/*
** used for stop can interface
*/
static int periplex_can_stop(struct net_device *netdev)
{
    struct periplex_can *pcd = netdev_priv(netdev);
    int ret;

    pr_info("periplex_can: Stopping CAN device...\n");

    netif_stop_queue(netdev);
    pcd->is_open = false;
    pcd->state_query = false;

    /* Signal thread to stop */
    atomic_set(&pcd->thread_should_stop, 1);

    /* Wake up the thread if it's waiting */
    pcd->wait_queue_flag_com_can = 1;
    wake_up_interruptible(&pcd->wait_queue_can_ioctl);

    pcd->wait_queue_flag_com_can_ack = 1;
    wake_up_interruptible(&pcd->wait_queue_can_ioctl_ack);

    /* Cancel any pending work first */
    if (pcd->tx_wq)
    {
        cancel_work_sync(&pcd->tx_work);
    }

    /* Stop the monitor thread and wait for it to exit */
    if (pcd->monitor_thread)
    {
        CAN_DEBUG("periplex_can: Stopping monitor thread...\n");
        ret = kthread_stop(pcd->monitor_thread);
        if (ret)
        {
            pr_warn("periplex_can: kthread_stop returned %d\n", ret);
        }
        pcd->monitor_thread = NULL;
        CAN_DEBUG("periplex_can: Monitor thread stopped\n");
    }

    close_candev(netdev);
    netdev_info(netdev, "periplex can device stopped\n");
    return 0;
}

/*
** probe function for periplex can
*/
static int periplex_can_probe(struct periplex_device *pdev)
{
    int ret;
    struct net_device *netdev;
    struct periplex_can *pcd;

    netdev = alloc_candev(sizeof(struct periplex_can), PERIPLEX_MAX_TX_MBOX);
    if (!netdev)
    {
        dev_err(&pdev->dev, "Failed to allocate CAN device\n");
        return -ENOMEM;
    }

    netdev->flags |= IFF_ECHO;
    pcd = netdev_priv(netdev);
    pcd->ndev = netdev;
    pcd->pdev = pdev;
    pcd->np = pdev->dev.of_node;
    pcd->is_open = false;
    pcd->state_query = false;
    pcd->monitor_thread = NULL;

    /* Initialize per-device wait queues */
    init_waitqueue_head(&pcd->wait_queue_can_ioctl);
    init_waitqueue_head(&pcd->wait_queue_can_ioctl_ack);
    pcd->wait_queue_flag_com_can = 0;
    pcd->wait_queue_flag_com_can_ack = 0;

    atomic_set(&pcd->thread_should_stop, 0);

    /* Parse device tree properties */
    if (device_property_read_u32(&pdev->dev, "periplex-id", &pcd->periplex_id))
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tree for can\n");
        ret = -EINVAL;
        goto err_free_candev;
    }

    /* Initialize spinlock */
    spin_lock_init(&pcd->lock);

    /* Initialize workqueue for transmission */
    pcd->tx_wq = alloc_workqueue("periplex_tx_wq", WQ_UNBOUND, 1);
    if (!pcd->tx_wq)
    {
        ret = -ENOMEM;
        goto err_free_candev;
    }
    INIT_WORK(&pcd->tx_work, periplex_tx_work_handler);

    /* Setup network device */
    SET_NETDEV_DEV(netdev, &pdev->dev);
    netdev->netdev_ops = &periplex_can_netdev_ops;

    /* Setup CAN specific features */
    pcd->can.clock.freq = CAN_CLOCK;
    pcd->can.bittiming_const = &periplex_can_bittiming_const;
    pcd->can.do_set_bittiming = periplex_can_set_bittiming;
    pcd->can.do_get_berr_counter = periplex_can_get_berr_counter;
    pcd->can.ctrlmode_supported = CAN_CTRLMODE_BERR_REPORTING |
                                  CAN_CTRLMODE_LOOPBACK |
                                  CAN_CTRLMODE_LISTENONLY |
                                  CAN_CTRLMODE_3_SAMPLES;

    pdev->periplex_id = pcd->periplex_id;
    pdev->get_periplex_data = read_data_for_can;

    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, pcd);
    netif_carrier_on(netdev);

    ret = register_candev(netdev);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to register CAN device\n");
        goto err_destroy_wq;
    }

    pr_info("periplex_can: periplex CAN device registered successfully (ID: %d)\n", pcd->periplex_id);
    return 0;

err_destroy_wq:
    if (pcd->tx_wq)
    {
        destroy_workqueue(pcd->tx_wq);
    }
err_free_candev:
    free_candev(netdev);
    return ret;
}

/*
** remove function for periplex can
*/
static int periplex_can_remove(struct periplex_device *pdev)
{
    struct periplex_can *pcd = periplex_get_drvdata(pdev);

    if (!pcd)
    {
        pr_warn("periplex_can: No device data found during remove\n");
        return -ENODEV;
    }

    pr_info("periplex_can: Removing CAN device (ID: %d)\n", pcd->periplex_id);

    /* Ensure the device is stopped first */
    if (pcd->is_open)
    {
        periplex_can_stop(pcd->ndev);
    }

    /* Double-check thread cleanup */
    if (pcd->monitor_thread)
    {
        pr_warn("periplex_can: Monitor thread still exists during remove, forcing stop\n");
        atomic_set(&pcd->thread_should_stop, 1);
        pcd->wait_queue_flag_com_can = 1;
        wake_up_interruptible(&pcd->wait_queue_can_ioctl);
        kthread_stop(pcd->monitor_thread);
        pcd->monitor_thread = NULL;
    }

    /* Clean up workqueue */
    if (pcd->tx_wq)
    {
        cancel_work_sync(&pcd->tx_work);
        destroy_workqueue(pcd->tx_wq);
        pcd->tx_wq = NULL;
    }

    /* Unregister and free network device */
    unregister_candev(pcd->ndev);
    free_candev(pcd->ndev);

    /* Unlink from periplex framework */
    periplex_unlink_device(pdev);

    pr_info("periplex_can: periplex CAN device removed cleanly\n");
    return 0;
}

static const struct of_device_id periplex_can_dt_match[] = {
    {.compatible = "vicharak,periplex-can"},
    {}};
MODULE_DEVICE_TABLE(of, periplex_can_dt_match);

static struct periplex_driver periplex_can_driver = {
    .probe = periplex_can_probe,
    .remove = periplex_can_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_can_dt_match,
    },
};

module_periplex_driver(periplex_can_driver);

MODULE_ALIAS("periplex:can");
MODULE_AUTHOR("Vatsal Kevadiya <vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("CAN Driver for the Periplex");
MODULE_LICENSE("GPL");