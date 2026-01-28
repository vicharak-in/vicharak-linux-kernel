/*
** Significant of pwm.c file :
** 1.Make multiple pwm chip with the use of dtso and create the
** pwmchip* series into the /sys/class/pwm
** 2.Allow to set duty-cycle and period for any specific pwm(pwmchip*) device.
*/

#include <linux/kernel.h>
#include <linux/math64.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/clk.h>
#include <linux/slab.h>
#include <linux/gfp.h>
#include <linux/pwm.h>
#include <asm/errno.h>
#include <linux/moduleparam.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

#define DRIVER_NAME "periplex-pwm"

/* Debug flag (can be set via module parameter) */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Macro to conditionally print debug info */
#define PWM_DEBUG(fmt, ...)              \
    do                                   \
    {                                    \
        if (debug)                       \
            pr_info("periplex_pwm: "fmt, ##__VA_ARGS__); \
    } while (0)

int fre_clk = 0;

struct periplex_pwm
{
    struct pwm_chip chip;
    int periplex_id;
};

int read_data_for_pwm(struct periplex_device *pdev, char *message, const int len)
{
    return 0;
}

static int pwm_periplex_request(struct pwm_chip *chip, struct pwm_device *pwm)
{
    PWM_DEBUG("request\n");
    return 0;
}

static void pwm_periplex_free(struct pwm_chip *chip, struct pwm_device *pwm)
{
    PWM_DEBUG("free\n");
}

static int pwm_periplex_apply(struct pwm_chip *chip, struct pwm_device *pwm,
                              const struct pwm_state *state)
{
    u8 message[8] = {0};
    struct periplex_pwm *pwm_data = container_of(chip, struct periplex_pwm, chip);
    int periplex_id = pwm_data->periplex_id;

    if (state->enabled == 1)
    {
        u64 OFF_time = (state->period - state->duty_cycle) / 20;
        u64 ON_time = state->duty_cycle / 20;
        pr_info("periplex_pwm: OFF_time is %llu\n", OFF_time);
        pr_info("periplex_pwm: ON_time is %llu\n", ON_time);

        message[0] = (OFF_time >> 24) & 0xFF;
        message[1] = (OFF_time >> 16) & 0xFF;
        message[2] = (OFF_time >> 8) & 0xFF;
        message[3] = (OFF_time >> 0) & 0xFF;

        message[4] = (ON_time >> 24) & 0xFF;
        message[5] = (ON_time >> 16) & 0xFF;
        message[6] = (ON_time >> 8) & 0xFF;
        message[7] = (ON_time >> 0) & 0xFF;

        set_periplex_data(periplex_id, 8, (char *)message);
    }
    if (state->enabled == 0)
    {
        memset(message, 0, sizeof(message));
        set_periplex_data(periplex_id, 8, (char *)message);
    }

    return 0;
}

static int pwm_periplex_capture(struct pwm_chip *chip, struct pwm_device *pwm,
                                struct pwm_capture *result, unsigned long timeout)
{
    PWM_DEBUG("capture\n");
    return 0;
}

static void pwm_periplex_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
                                   struct pwm_state *state)
{
    PWM_DEBUG("get_state\n");
}

static const struct pwm_ops pwm_periplex_ops = {
    .request = pwm_periplex_request,
    .free = pwm_periplex_free,
    .apply = pwm_periplex_apply,
    .capture = pwm_periplex_capture,
    .get_state = pwm_periplex_get_state,
};

static int pwm_clk_probe(struct periplex_device *pdev)
{
    int ret = 0;
    struct periplex_pwm *pwm = NULL;

    pwm = kzalloc(sizeof(struct periplex_pwm), GFP_KERNEL);
    if (!pwm)
    {
        ret = -ENOMEM;
        goto err_free_pwm;
    }

    if (device_property_read_u32(&pdev->dev, "periplex-id", &pwm->periplex_id))
    {
        dev_err(&pdev->dev, "Failed to read periplex-id from device tree for pwm\n");
    }

    pwm->chip.dev = &pdev->dev;
    pwm->chip.base = -1; // Let the framework assign a base
    pwm->chip.ops = &pwm_periplex_ops;
    pwm->chip.npwm = 1;

    pdev->periplex_id = pwm->periplex_id;
    pdev->get_periplex_data = read_data_for_pwm;

    periplex_link_device(pdev);
    periplex_set_drvdata(pdev, pwm);

    ret = pwmchip_add(&pwm->chip);
    if (ret < 0)
    {
        dev_err(&pdev->dev, "pwmchip_add() failed: %d\n", ret);
        goto err_free_pwm;
    }

    pr_info("periplex_pwm: pwm driver inserted successfully...\n");
    return 0;

err_free_pwm:
    kfree(pwm);
    return ret;
}

static int pwm_clk_remove(struct periplex_device *pdev)
{
    struct periplex_pwm *pwm = periplex_get_drvdata(pdev);
    pwmchip_remove(&pwm->chip);
    periplex_unlink_device(pdev);
    kfree(pwm);
    pr_info("periplex_pwm: pwm driver removed successfully...\n");
    return 0;
}

static const struct of_device_id periplex_pwm_dt_match[] = {
    {.compatible = "vicharak,periplex-pwm"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_pwm_dt_match);

static struct periplex_driver periplex_pwm_driver = {
    .probe = pwm_clk_probe,
    .remove = pwm_clk_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_pwm_dt_match,
    },
};
module_periplex_driver(periplex_pwm_driver);

MODULE_ALIAS("periplex:pwm");
MODULE_AUTHOR("Vatsal Kevadiya <vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("PWM Driver for the Periplex");
MODULE_LICENSE("GPL");
