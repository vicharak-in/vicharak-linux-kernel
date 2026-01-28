#ifndef _PERIPHERAL_H
#define _PERIPHERAL_H

#include <linux/device.h>
#include <linux/module.h>

#define MAX_MESSAGE_LENGTH 128
#define MAXIMUM_DEVICE 128

/*
** Common Structure for configuration case
*/
struct kernel_config
{
    int peri_id;
    int configuration;
    uint8_t configuration_id;
};

/*
** Common Structure for write case, means use when data are transfer
** from peripheral's(kernel-space) to rah service(user-space)
*/
struct kernel_data
{
    int peri_id;
    int length;
};

/*
** common structure used in read case, means use when data are transfer
** from  rah-service (user-space) to peripheral's (kernel-space)
*/
struct kernel_buffer
{
    char *message;
    int length;
    int minor;
};

struct peripheral_burst_data
{
    int periplex_id;
    int total_length;
    char *whole_data;
};

struct kernel_burst_data
{
    int device_count;
    struct peripheral_burst_data *data;
};

struct burst_device_metadata
{
    int periplex_id;
    int total_length;
};

struct burst_device_request
{
    int device_index;
    char *data_buffer;
    int buffer_size;
};

/*
** set configuration for a specific peripheral(use in write case)
*/
void set_periplex_configuration(int peri_id, uint8_t config_id,
                                int configuration);

/*
** set data for a specific peripheral (use in write case)
*/
void set_periplex_data(int peri_id, int length, char *message);

/*
** set data burst for multiple peripheral (use in write case)
*/
int set_periplex_data_burst(struct peripheral_burst_data *burst_array, int device_count);

/*
** Structure representing periplex device
*/
struct periplex_device
{
    void *data;
    int periplex_id;
    const char *name;
    struct device dev;
    int (*get_periplex_data)(struct periplex_device *pdev,
                             char *message, const int len);
};

/*
** Structure representing periplex driver
*/
struct periplex_driver
{
    struct device_driver driver;
    int id;
    int (*probe)(struct periplex_device *pdev);
    int (*remove)(struct periplex_device *pdev);
};

/*
** periplex bus type
*/
extern struct bus_type periplex_bus_type;

/*
** Function prototypes for bus operations
*/
extern int periplex_link_device(struct periplex_device *pdev);
extern void periplex_unlink_device(struct periplex_device *pdev);
extern int periplex_register_driver(struct periplex_driver *drv);
extern void periplex_unregister_driver(struct periplex_driver *drv);

/*
** Helper macros to convert between types
*/
#define to_periplex_device(d) container_of(d, struct periplex_device, dev)
#define to_periplex_driver(d) container_of(d, struct periplex_driver, driver)

/*
** Macro to simplify driver registration
*/
#define module_periplex_driver(__drv) \
    module_driver(__drv, periplex_register_driver, periplex_unregister_driver)

/*
** methods are use for set/get the driver data
*/
static inline void *periplex_get_drvdata(const struct periplex_device *pdev)
{
    return dev_get_drvdata(&pdev->dev);
}

static inline void periplex_set_drvdata(struct periplex_device *pdev, void *data)
{
    dev_set_drvdata(&pdev->dev, data);
}

#endif /* _PERIPHERAL_H */