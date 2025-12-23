/*
** Significant of i2s.c file :
** 1. Make multiple I2S devices with the use of DTSO and create the
**    card* series into the /sys/class/sound or /proc/asound directory.
** 2. Implements a Periplex I2S codec driver that can handle playback and capture
**    for multiple channels and sample formats.
** 3. Uses ALSA ASoC framework to register a component and DAI driver.
** 4. Implements an audio queue and workqueue system to buffer and process PCM data
**    in a non-blocking manner.
*/

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/pm_runtime.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <linux/delay.h>
#include <sound/soc.h>
#include <sound/dmaengine_pcm.h>
#include <linux/moduleparam.h>
#include <sound/designware_i2s.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>

/*
** header file through which device can communicate and generated
*/
#include <linux/peripheral.h>

#define DRIVER_NAME "periplex-i2s"

struct mutex i2s_mutex;

#define ROUND_TO_NEAREST(x, y) (((x) + ((y) / 2)) / (y))
#define PERIPLEX_I2S_FCLK 50000000 // 50 MHz clock frequency

/* Queue configuration */
#define AUDIO_QUEUE_SIZE (524288 / 4) // Maximum queue size (for 32-bit samples)
#define MAX_CHUNK_SIZE 2940 // Maximum chunk size for work queue processing

#define I2S_DEBUG(fmt, ...)                               \
    do                                                    \
    {                                                     \
        if (debug)                                        \
            pr_info("periplex_i2s: " fmt, ##__VA_ARGS__); \
    } while (0)

static const char *format_names[] = {
    [SNDRV_PCM_FORMAT_S8] = "S8",
    [SNDRV_PCM_FORMAT_S16_LE] = "S16_LE",
    [SNDRV_PCM_FORMAT_S24_LE] = "S24_LE",
    [SNDRV_PCM_FORMAT_S32_LE] = "S32_LE",
};

/* Debug flag */
static int debug = 0;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable or disable debug mode");

/* Audio data queue structure */
struct audio_queue
{
    char *buffer;       // Circular buffer
    unsigned int head;  // Write position
    unsigned int tail;  // Read position
    unsigned int size;  // Total buffer size
    unsigned int count; // Current data count
    spinlock_t lock;    // Queue protection
    bool overflow;      // Overflow flag
};

/* Structure to hold private data */
struct periplex_i2s_dev
{
    int periplex_id;
    struct clk *mod_clk;
    unsigned int rate;
    unsigned int format;
    unsigned int channels;
    unsigned int data_width;
    unsigned int width;
    struct periplex_device *pdev;
    struct snd_pcm_substream *substream;
    snd_pcm_uframes_t pointer_offset;
    unsigned int offset_bytes;
    snd_pcm_uframes_t hw_ptr;
    snd_pcm_uframes_t appl_ptr;

    // Playback state tracking
    bool playback_active;
    bool playback_complete;

    // Timer for hardware simulation (if no real interrupts)
    struct timer_list playback_timer;

    // Current substream
    struct snd_pcm_substream *current_substream;

    // Synchronization
    spinlock_t lock;

    // Audio queue and work queue
    struct audio_queue queue;
    struct workqueue_struct *audio_wq;
    struct work_struct audio_work;
    bool work_scheduled;
    atomic_t work_pending;
};

/*periplex_pcm_hardware for hardware configuration */
static const struct snd_pcm_hardware periplex_pcm_hardware = {
    .info = SNDRV_PCM_INFO_INTERLEAVED |
            SNDRV_PCM_INFO_BLOCK_TRANSFER |
            SNDRV_PCM_INFO_PAUSE |
            SNDRV_PCM_INFO_RESUME,

    .formats = SNDRV_PCM_FMTBIT_S16_LE |
               SNDRV_PCM_FMTBIT_S24_LE |
               SNDRV_PCM_FMTBIT_S32_LE,

    .rates = SNDRV_PCM_RATE_8000 |
             SNDRV_PCM_RATE_16000 |
             SNDRV_PCM_RATE_32000 |
             SNDRV_PCM_RATE_44100 |
             SNDRV_PCM_RATE_48000,

    .rate_min = 8000,
    .rate_max = 48000,

    .channels_min = 1,
    .channels_max = 2,

    // Constrain buffer and period sizes for better control
    .buffer_bytes_max = 131072, 

    .period_bytes_min = 29400,
    .period_bytes_max = 29400,

    .periods_min = 2,
    .periods_max = 2,

    .fifo_size = 0,
};

/* I2S data read function (used while capture) */
int read_data_for_i2s(struct periplex_device *pdev, char *message, const int len)
{
    // here need to implement the capture data handling
    I2S_DEBUG("Reading data for I2S, length: %d\n", len);
    return 0;
}

/* Schedule work queue if data is available and not already scheduled */
static void schedule_audio_work(struct periplex_i2s_dev *i2s)
{
    unsigned long flags;
    bool has_data;

    spin_lock_irqsave(&i2s->queue.lock, flags);
    has_data = (i2s->queue.count > 0);
    spin_unlock_irqrestore(&i2s->queue.lock, flags);

    if (has_data && i2s->playback_active && !atomic_read(&i2s->work_pending))
    {
        atomic_set(&i2s->work_pending, 1);
        queue_work(i2s->audio_wq, &i2s->audio_work);
        I2S_DEBUG("Audio work scheduled\n");
    }
}

/* Audio queue init */
static int audio_queue_init(struct audio_queue *queue, unsigned int size)
{
    queue->buffer = kzalloc(size, GFP_KERNEL);
    if (!queue->buffer)
        return -ENOMEM;

    queue->head = 0;
    queue->tail = 0;
    queue->size = size;
    queue->count = 0;
    queue->overflow = false;
    spin_lock_init(&queue->lock);

    return 0;
}

/* audio queue cleanup */
static void audio_queue_cleanup(struct audio_queue *queue)
{
    kfree(queue->buffer);
    queue->buffer = NULL;
}

/* Audio queue space available functions */
static unsigned int audio_queue_space_available(struct audio_queue *queue)
{
    return queue->size - queue->count;
}

/* Audio queue data available functions */
static unsigned int audio_queue_data_available(struct audio_queue *queue)
{
    return queue->count;
}

/* Audio queue write functions */
static int audio_queue_write(struct audio_queue *queue, struct periplex_i2s_dev *i2s, const char *data, unsigned int len)
{
    unsigned long flags;
    unsigned int space_available;
    unsigned int first_chunk, second_chunk;
    const unsigned int KEEP_AFTER_OVERFLOW = 14700; // Keep this much data after overflow

    spin_lock_irqsave(&queue->lock, flags);

    space_available = audio_queue_space_available(queue);

    // If queue is full or would overflow, keep only KEEP_AFTER_OVERFLOW bytes
    if (len > space_available)
    {
        I2S_DEBUG("Queue overflow detected, keeping %u bytes (was: %u, requested: %u)\n",
                  KEEP_AFTER_OVERFLOW, queue->count, len);

        if (queue->count > KEEP_AFTER_OVERFLOW)
        {
            // Calculate how many bytes to drop from the beginning
            unsigned int bytes_to_drop = queue->count - KEEP_AFTER_OVERFLOW;

            // Move tail forward to drop old data
            queue->tail = (queue->tail + bytes_to_drop) % queue->size;
            queue->count = KEEP_AFTER_OVERFLOW;

            // Update head to maintain circular buffer integrity
            queue->head = (queue->tail + queue->count) % queue->size;
        }

        queue->overflow = true;
        // Reset pending work so it can be re-triggered
        atomic_set(&i2s->work_pending, 0);
    }

    // Now write the new data (same logic as before)
    space_available = audio_queue_space_available(queue);

    // Make sure we have enough space now
    if (len > space_available)
    {
        // Still not enough space, drop more old data
        unsigned int additional_drop = len - space_available;
        queue->tail = (queue->tail + additional_drop) % queue->size;
        queue->count -= additional_drop;
    }

    // Calculate chunks for circular buffer write
    if (queue->head + len <= queue->size)
    {
        // Single chunk write
        memcpy(queue->buffer + queue->head, data, len);
        queue->head = (queue->head + len) % queue->size;
    }
    else
    {
        // Two chunk write (wrap around)
        first_chunk = queue->size - queue->head;
        second_chunk = len - first_chunk;

        memcpy(queue->buffer + queue->head, data, first_chunk);
        memcpy(queue->buffer, data + first_chunk, second_chunk);
        queue->head = second_chunk;
    }

    queue->count += len;
    I2S_DEBUG("Queue level: %u/%u bytes used\n", queue->count, queue->size);

    spin_unlock_irqrestore(&queue->lock, flags);

    // Always re-trigger work after writing
    schedule_audio_work(i2s);

    return len;
}

/* Audio queue read functions */
static int audio_queue_read(struct audio_queue *queue, char *data, unsigned int len)
{
    unsigned long flags;
    unsigned int available;
    unsigned int first_chunk, second_chunk;
    unsigned int to_read;

    spin_lock_irqsave(&queue->lock, flags);

    available = audio_queue_data_available(queue);
    to_read = min(len, available);

    if (to_read == 0)
    {
        spin_unlock_irqrestore(&queue->lock, flags);
        return 0;
    }

    // Calculate chunks for circular buffer read
    if (queue->tail + to_read <= queue->size)
    {
        // Single chunk read
        memcpy(data, queue->buffer + queue->tail, to_read);
        queue->tail = (queue->tail + to_read) % queue->size;
    }
    else
    {
        // Two chunk read (wrap around)
        first_chunk = queue->size - queue->tail;
        second_chunk = to_read - first_chunk;

        memcpy(data, queue->buffer + queue->tail, first_chunk);
        memcpy(data + first_chunk, queue->buffer, second_chunk);
        queue->tail = second_chunk;
    }

    queue->count -= to_read;
    queue->overflow = false; // Clear overflow flag after successful read
    I2S_DEBUG("Queue level after read: %u/%u\n", queue->count, queue->size);

    spin_unlock_irqrestore(&queue->lock, flags);

    return to_read;
}

/* Work queue function to process audio data */
static void periplex_audio_work_func(struct work_struct *work)
{
    struct periplex_i2s_dev *i2s = container_of(work, struct periplex_i2s_dev, audio_work);
    char *work_buffer;
    unsigned int bytes_read;
    bool queue_has_data = true;
    unsigned long flags;
    work_buffer = kzalloc(MAX_CHUNK_SIZE, GFP_KERNEL);
    if (!work_buffer)
    {
        pr_err("periplex_i2s: Failed to allocate work buffer\n");
        atomic_set(&i2s->work_pending, 0);
        return;
    }

    I2S_DEBUG("Audio work function started\n");

    // Process data while queue has data and device is active
    while (queue_has_data)
    {
        mutex_lock(&i2s_mutex);
        bytes_read = audio_queue_read(&i2s->queue, work_buffer, MAX_CHUNK_SIZE);

        if (bytes_read > 0)
        {

            // Send data to hardware (blocking call)
            set_periplex_data(i2s->periplex_id, bytes_read, work_buffer);
            I2S_DEBUG("Processing %u bytes from queue\n", bytes_read);

            // Check if more data is available
            spin_lock_irqsave(&i2s->queue.lock, flags);
            queue_has_data = (i2s->queue.count > 0);
            spin_unlock_irqrestore(&i2s->queue.lock, flags);
        }
        else
        {
            queue_has_data = false;
        }

        // Yield CPU if needed
        cond_resched();
        mutex_unlock(&i2s_mutex);
    }

    kfree(work_buffer);
    atomic_set(&i2s->work_pending, 0);

    I2S_DEBUG("Audio work function completed\n");
}

static int periplex_pcm_open(struct snd_soc_component *component,
                             struct snd_pcm_substream *substream)
{
    struct snd_pcm_runtime *runtime = substream->runtime;
    runtime->hw = periplex_pcm_hardware;
    snd_pcm_hw_constraint_integer(runtime, SNDRV_PCM_HW_PARAM_PERIODS);
    pr_info("periplex_i2s: periplex_pcm_open\n");
    return 0;
}

static int periplex_pcm_close(struct snd_soc_component *component,
                              struct snd_pcm_substream *substream)
{
    struct periplex_i2s_dev *i2s = snd_soc_component_get_drvdata(component);

    // Flush work queue and wait for completion
    if (i2s->audio_wq)
    {
        flush_workqueue(i2s->audio_wq);
    }

    pr_info("periplex_i2s: periplex_pcm_close\n");
    return 0;
}

static int periplex_pcm_hw_params(struct snd_soc_component *component,
                                  struct snd_pcm_substream *substream,
                                  struct snd_pcm_hw_params *params)
{
    int configuration = 0;
    int sample_bits = 32;
    int width = 0;
    unsigned char message[4] = {0};
    unsigned int rate, channels, format, data_width, clock_counter, denominator;
    struct periplex_i2s_dev *i2s = snd_soc_component_get_drvdata(component);
    rate = params_rate(params);
    channels = params_channels(params);
    format = params_format(params);

    I2S_DEBUG("HW Params:\n"
              "Rate: %u Hz\n"
              "Channels: %u\n"
              "Format: %s\n",
              rate,
              channels, format_names[format]);

    switch (format)
    {
    case SNDRV_PCM_FORMAT_S8:
        data_width = 8;
        width = 1;
        I2S_DEBUG("8-bit format\n");
        break;
    case SNDRV_PCM_FORMAT_S16_LE:
        data_width = 16;
        width = 2;
        I2S_DEBUG("16-bit format\n");
        break;

    case SNDRV_PCM_FORMAT_S24_LE:
        data_width = 24;
        width = 3;
        I2S_DEBUG("24-bit format\n");
        break;

    case SNDRV_PCM_FORMAT_S32_LE:
        data_width = 32;
        width = 4;
        I2S_DEBUG("32-bit format\n");
        break;

    default:
        I2S_DEBUG("designware-i2s: unsupported PCM fmt");
        return -EINVAL;
    }

    switch (channels)
    {
    // case EIGHT_CHANNEL_SUPPORT:
    // case SIX_CHANNEL_SUPPORT:
    // case FOUR_CHANNEL_SUPPORT:
    case TWO_CHANNEL_SUPPORT:
        break;
    default:
        I2S_DEBUG("channel not supported\n");
        return -EINVAL;
    }

    denominator = 2 * (2 * rate * sample_bits);
    clock_counter = ROUND_TO_NEAREST(PERIPLEX_I2S_FCLK, denominator);

    I2S_DEBUG("clock_counter %u\n", clock_counter);

    message[0] = 0;
    message[1] = 8; // set according to the start song (before we use 0)
    message[2] = ((width << 4) | 4);
    message[3] = clock_counter;

    configuration |= (message[0] << 24); // Set the most significant byte
    configuration |= (message[1] << 16); // Set the second byte
    configuration |= (message[2] << 8);  // Set the third byte
    configuration |= (message[3]);       // Set the least significant byte

    i2s->rate = rate;
    i2s->channels = channels;
    i2s->format = format;
    i2s->data_width = data_width;
    i2s->width = width;

    set_periplex_configuration(i2s->periplex_id, 0, configuration);
    return 0;
}

static int periplex_pcm_hw_free(struct snd_soc_component *component,
                                struct snd_pcm_substream *substream)
{
    struct periplex_i2s_dev *i2s = snd_soc_component_get_drvdata(component);
    int configuration = 0;
    unsigned char message[4] = {0};
    unsigned int denominator, clock_counter;
    denominator = 2 * (2 * i2s->rate * 32);
    clock_counter = ROUND_TO_NEAREST(PERIPLEX_I2S_FCLK, denominator);

    message[0] = 0;
    message[1] = 0; // set according to the stop song
    message[2] = 4;
    message[3] = clock_counter;

    configuration |= (message[0] << 24); // Set the most significant byte
    configuration |= (message[1] << 16); // Set the second byte
    configuration |= (message[2] << 8);  // Set the third byte
    configuration |= (message[3]);       // Set the least significant byte

    set_periplex_configuration(i2s->periplex_id, 0, configuration);
    I2S_DEBUG("periplex_pcm_hw_free\n");
    return 0;
}

static int periplex_pcm_prepare(struct snd_soc_component *component,
                                struct snd_pcm_substream *substream)
{
    struct periplex_i2s_dev *i2s = snd_soc_component_get_drvdata(component);
    struct snd_pcm_runtime *runtime = substream->runtime;
    i2s->pointer_offset = 0;

    I2S_DEBUG("periplex_pcm_prepare Stream: %s\n",
              substream->stream == SNDRV_PCM_STREAM_PLAYBACK ? "Playback" : "Capture");

    I2S_DEBUG("Prepare: rate=%d, channels=%d, format=%d, period_size=%ld, "
              "buffer_size=%ld, state=%d\n",
              runtime->rate, runtime->channels, runtime->format,
              runtime->period_size, runtime->buffer_size,
              runtime->status->state);
    return 0;
}

static int periplex_pcm_trigger(struct snd_soc_component *component,
                                struct snd_pcm_substream *substream, int cmd)
{
    struct periplex_i2s_dev *i2s = snd_soc_component_get_drvdata(component);
    struct snd_pcm_runtime *runtime = substream->runtime;
    unsigned long flags;

    spin_lock_irqsave(&i2s->lock, flags);

    switch (cmd)
    {
    case SNDRV_PCM_TRIGGER_START:
    case SNDRV_PCM_TRIGGER_RESUME:
    case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
        i2s->playback_active = true;
        i2s->playback_complete = false;
        i2s->current_substream = substream;
        i2s->hw_ptr = 0; // Reset hardware pointer on start

        // Start the timer for first period
        if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
        {
            unsigned long period_ms = (runtime->period_size * 1000) / runtime->rate;
            mod_timer(&i2s->playback_timer, jiffies + msecs_to_jiffies(period_ms - 16));
        }

        // Schedule work if queue has data
        spin_unlock_irqrestore(&i2s->lock, flags);
        schedule_audio_work(i2s);
        spin_lock_irqsave(&i2s->lock, flags);

        I2S_DEBUG("periplex: Playback started, period_ms=%lu\n",
                  (runtime->period_size * 1000) / runtime->rate);
        break;

    case SNDRV_PCM_TRIGGER_STOP:
    case SNDRV_PCM_TRIGGER_SUSPEND:
    case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
        i2s->playback_active = false;
        i2s->playback_complete = true;
        del_timer(&i2s->playback_timer);

        // Wait for work to complete
        spin_unlock_irqrestore(&i2s->lock, flags);
        if (i2s->audio_wq)
        {
            flush_workqueue(i2s->audio_wq);
        }
        spin_lock_irqsave(&i2s->lock, flags);

        I2S_DEBUG("periplex: Playback stopped\n");
        break;

    default:
        spin_unlock_irqrestore(&i2s->lock, flags);
        return -EINVAL;
    }

    spin_unlock_irqrestore(&i2s->lock, flags);
    return 0;
}

// Enhanced copy_user function with queue integration
static int periplex_pcm_copy_user(struct snd_soc_component *component,
                                  struct snd_pcm_substream *substream,
                                  int channel, unsigned long pos,
                                  void __user *buf, unsigned long bytes)
{
    struct periplex_i2s_dev *i2s = snd_soc_component_get_drvdata(component);
    struct snd_pcm_runtime *runtime = substream->runtime;
    char *kernel_buf;
    unsigned long flags;
    snd_pcm_uframes_t frames;
    snd_pcm_uframes_t pos_frames;
    int bytes_written;

    if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
    {
        kernel_buf = kmalloc(bytes, GFP_KERNEL);
        if (!kernel_buf)
            return -ENOMEM;

        if (copy_from_user(kernel_buf, buf, bytes))
        {
            kfree(kernel_buf);
            return -EFAULT;
        }

        // Write data to queue instead of directly to hardware
        bytes_written = audio_queue_write(&i2s->queue, i2s, kernel_buf, bytes);

        if (bytes_written != bytes)
        {
            I2S_DEBUG("Queue write partial: requested=%lu, written=%d\n", bytes, bytes_written);
        }

        kfree(kernel_buf);

        // Calculate frames and position
        frames = bytes / (runtime->channels * (runtime->sample_bits / 8));
        pos_frames = pos / (runtime->channels * (runtime->sample_bits / 8));

        spin_lock_irqsave(&i2s->lock, flags);

        // Track the highest application pointer position
        if (pos_frames + frames > i2s->appl_ptr)
        {
            i2s->appl_ptr = pos_frames + frames;
        }
        i2s->current_substream = substream;
        i2s->playback_active = true;
        i2s->playback_complete = false;
        spin_unlock_irqrestore(&i2s->lock, flags);

        // Schedule work queue to process the data
        schedule_audio_work(i2s);
    }
    else
    {
        // Capture logic
        frames = bytes / (runtime->channels * (runtime->sample_bits / 8));
        pos_frames = pos / (runtime->channels * (runtime->sample_bits / 8));
        spin_lock_irqsave(&i2s->lock, flags);
        i2s->appl_ptr = pos_frames + frames;
        spin_unlock_irqrestore(&i2s->lock, flags);
    }

    I2S_DEBUG("periplex_pcm_copy_user - buffer_size: %ld, period_size: %ld, frame_bits: %u, bytes: %lu, "
                    "pos: %lu, frames: %lu\n",
              runtime->buffer_size, runtime->period_size, runtime->frame_bits, bytes, pos, frames);

    return 0;
}

static void periplex_playback_timer_callback(struct timer_list *t)
{
    struct periplex_i2s_dev *i2s = from_timer(i2s, t, playback_timer);
    unsigned long flags;

    spin_lock_irqsave(&i2s->lock, flags);

    if (i2s->playback_active && i2s->current_substream)
    {
        struct snd_pcm_runtime *runtime = i2s->current_substream->runtime;

        // Advance hardware pointer by one period
        i2s->hw_ptr += runtime->period_size;
        if (i2s->hw_ptr >= runtime->buffer_size)
        {
            i2s->hw_ptr = i2s->hw_ptr % runtime->buffer_size;
        }

        I2S_DEBUG("periplex_timer: hw_ptr=%lu, appl_ptr=%lu, buffer_size=%lu\n",
                  i2s->hw_ptr, i2s->appl_ptr, runtime->buffer_size);

        // Schedule next period if we haven't caught up with application pointer
        if (i2s->hw_ptr < i2s->appl_ptr && i2s->playback_active)
        {
            // Calculate time for one period
            unsigned long period_ms = (runtime->period_size * 1000) / runtime->rate;
            mod_timer(&i2s->playback_timer, jiffies + msecs_to_jiffies(period_ms - 16));

            I2S_DEBUG("periplex: Playback started, period_ms=%lu\n",
                      (runtime->period_size * 1000) / runtime->rate);

            spin_unlock_irqrestore(&i2s->lock, flags);

            // Notify ALSA of period completion
            snd_pcm_period_elapsed(i2s->current_substream);
            return;
        }
        else
        {
            // Playback completed
            i2s->playback_complete = true;
            i2s->playback_active = false;
            I2S_DEBUG("periplex: Playback completed, hw_ptr=%lu, appl_ptr=%lu\n",
                      i2s->hw_ptr, i2s->appl_ptr);
        }
    }

    spin_unlock_irqrestore(&i2s->lock, flags);
}

snd_pcm_uframes_t periplex_pcm_pointer(struct snd_soc_component *component,
                                       struct snd_pcm_substream *substream)
{
    struct periplex_i2s_dev *i2s = snd_soc_component_get_drvdata(component);
    struct snd_pcm_runtime *runtime = substream->runtime;
    unsigned long flags;
    snd_pcm_uframes_t hw_ptr;

    spin_lock_irqsave(&i2s->lock, flags);
    hw_ptr = i2s->hw_ptr;
    spin_unlock_irqrestore(&i2s->lock, flags);

    // Ensure we don't exceed buffer boundaries
    hw_ptr = hw_ptr % runtime->buffer_size;

    I2S_DEBUG("periplex_pcm_pointer: hw_ptr = %lu, active=%d, complete=%d\n",
              hw_ptr, i2s->playback_active, i2s->playback_complete);

    return hw_ptr;
}

static int periplex_pcm_set_sysclk(struct snd_soc_component *component,
                                   int clk_id, int source, unsigned int freq, int dir)
{
    I2S_DEBUG("periplex pcm set sysclk\n");
    return 0;
}

static unsigned int periplex_pcm_read(struct snd_soc_component *component,
                                      unsigned int reg)
{
    I2S_DEBUG("periplex pcm read\n");
    return 0;
}

static int periplex_pcm_ioctl(struct snd_soc_component *component,
                              struct snd_pcm_substream *substream,
                              unsigned int cmd, void *arg)
{
    I2S_DEBUG("periplex pcm ioctl\n");
    return 0;
}

snd_pcm_sframes_t periplex_pcm_delay(struct snd_soc_component *component,
                                     struct snd_pcm_substream *substream)
{
    return 0;
}

/* Initialize the audio queue and work queue system */
static int periplex_audio_queue_init(struct periplex_i2s_dev *i2s)
{
    int ret;

    // Initialize audio queue
    ret = audio_queue_init(&i2s->queue, AUDIO_QUEUE_SIZE);
    if (ret)
    {
        pr_err("periplex_i2s: Failed to initialize audio queue: %d\n", ret);
        return ret;
    }

    // Create dedicated work queue
    i2s->audio_wq = create_singlethread_workqueue("periplex_audio_wq");
    if (!i2s->audio_wq)
    {
        pr_err("periplex_i2s: Failed to create audio work queue\n");
        audio_queue_cleanup(&i2s->queue);
        return -ENOMEM;
    }

    // Initialize work
    INIT_WORK(&i2s->audio_work, periplex_audio_work_func);
    atomic_set(&i2s->work_pending, 0);

    I2S_DEBUG("Audio queue and work queue initialized successfully\n");
    return 0;
}

/* Cleanup the audio queue and work queue system */
static void periplex_audio_queue_cleanup(struct periplex_i2s_dev *i2s)
{
    // Destroy work queue (this also flushes pending work)
    if (i2s->audio_wq)
    {
        destroy_workqueue(i2s->audio_wq);
        i2s->audio_wq = NULL;
    }

    // Cleanup audio queue
    audio_queue_cleanup(&i2s->queue);

    I2S_DEBUG("Audio queue and work queue cleaned up\n");
}

static const struct snd_soc_component_driver periplex_component_driver = {
    .name = DRIVER_NAME,
    .read = periplex_pcm_read,
    .open = periplex_pcm_open,
    .close = periplex_pcm_close,
    .hw_params = periplex_pcm_hw_params,
    .hw_free = periplex_pcm_hw_free,
    .prepare = periplex_pcm_prepare,
    .trigger = periplex_pcm_trigger,
    .copy_user = periplex_pcm_copy_user,
    .set_sysclk = periplex_pcm_set_sysclk,
    .pointer = periplex_pcm_pointer,
    .ioctl = periplex_pcm_ioctl,
};

static int periplex_i2s_trigger(struct snd_pcm_substream *substream, int cmd,
                                struct snd_soc_dai *dai)
{
    I2S_DEBUG("periplex_i2s_delay\n");
    return 0;
}

static int periplex_i2s_hw_params(struct snd_pcm_substream *substream,
                                  struct snd_pcm_hw_params *params,
                                  struct snd_soc_dai *dai)
{
    I2S_DEBUG("periplex_i2s_hw_params\n");
    return 0;
}

static int periplex_i2s_hw_free(struct snd_pcm_substream *substream,
                                struct snd_soc_dai *dai)
{
    I2S_DEBUG("periplex_i2s_hw_free\n");
    return 0;
}

static int periplex_i2s_startup(struct snd_pcm_substream *substream,
                                struct snd_soc_dai *dai)
{
    I2S_DEBUG("periplex_i2s_startup\n");
    return 0;
}

static void periplex_i2s_shutdown(struct snd_pcm_substream *substream,
                                  struct snd_soc_dai *dai)
{
    I2S_DEBUG("periplex_i2s_shutdown\n");
    return;
}

const struct snd_soc_dai_ops periplex_i2s_dai_ops = {
    .trigger = periplex_i2s_trigger,
    .hw_params = periplex_i2s_hw_params,
    .hw_free = periplex_i2s_hw_free,
    .startup = periplex_i2s_startup,
    .shutdown = periplex_i2s_shutdown,

};

static struct snd_soc_dai_driver periplex_i2s_dai = {
    .ops = &periplex_i2s_dai_ops,
    .playback = {
        .stream_name = "Playback",
        .channels_min = 1,
        .channels_max = 2,
        .rate_min = 8000,
        .rate_max = 48000,
        .formats = (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE | SNDRV_PCM_FMTBIT_U16_LE | SNDRV_PCM_FMTBIT_U16_BE |
                    SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_BE | SNDRV_PCM_FMTBIT_U24_LE | SNDRV_PCM_FMTBIT_U24_BE |
                    SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S32_BE | SNDRV_PCM_FMTBIT_U32_LE | SNDRV_PCM_FMTBIT_U32_BE),
        .rates = (SNDRV_PCM_RATE_8000 |
                  SNDRV_PCM_RATE_16000 |
                  SNDRV_PCM_RATE_32000 |
                  SNDRV_PCM_RATE_44100 |
                  SNDRV_PCM_RATE_48000),
    },
    .capture = {
        .stream_name = "Capture",
        .channels_min = 1,
        .channels_max = 2,
        .rate_min = 8000,
        .rate_max = 48000,
        .formats = (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S16_BE | SNDRV_PCM_FMTBIT_U16_LE | SNDRV_PCM_FMTBIT_U16_BE | 
                    SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S24_BE | SNDRV_PCM_FMTBIT_U24_LE | SNDRV_PCM_FMTBIT_U24_BE | 
                    SNDRV_PCM_FMTBIT_S32_LE | SNDRV_PCM_FMTBIT_S32_BE | SNDRV_PCM_FMTBIT_U32_LE | SNDRV_PCM_FMTBIT_U32_BE),
        .rates = (SNDRV_PCM_RATE_8000 | 
                SNDRV_PCM_RATE_16000 | 
                SNDRV_PCM_RATE_32000 | 
                SNDRV_PCM_RATE_44100 | 
                SNDRV_PCM_RATE_48000),
    },
};

/* Modify probe function for non-DMA operation */
static int periplex_i2s_probe(struct periplex_device *pdev)
{
    int ret;
    struct periplex_i2s_dev *i2s;
    struct device *dev = &pdev->dev;

    mutex_init(&i2s_mutex);

    i2s = devm_kzalloc(dev, sizeof(*i2s), GFP_KERNEL);
    if (!i2s)
        return -ENOMEM;

    i2s->pdev = pdev;

    if (device_property_read_u32(dev, "periplex-id", &i2s->periplex_id))
    {
        dev_err(dev, "Failed to read periplex-id from device tree\n");
        return -EINVAL;
    }

    pdev->periplex_id = i2s->periplex_id;
    pdev->get_periplex_data = read_data_for_i2s;

    i2s->hw_ptr = 0;
    i2s->appl_ptr = 0;
    i2s->playback_active = false;
    i2s->playback_complete = false;
    i2s->current_substream = NULL;
    spin_lock_init(&i2s->lock);

    timer_setup(&i2s->playback_timer, periplex_playback_timer_callback, 0);

    ret = periplex_audio_queue_init(i2s);
    if (ret)
    {
        dev_err(&pdev->dev, "Failed to initialize audio queue: %d\n", ret);
        return ret;
    }

    ret = periplex_link_device(pdev);
    if (ret)
    {
        dev_err(dev, "Failed to link device\n");
        return ret;
    }

    periplex_set_drvdata(pdev, i2s);

    ret = devm_snd_soc_register_component(dev, &periplex_component_driver,
                                          &periplex_i2s_dai, 1);
    if (ret)
    {
        dev_err(dev, "Failed to register DAI driver: %d\n", ret);
        return ret;
    }

    pr_info("periplex_i2s: probed I2S driver successfully\n");
    return 0;
}

static int periplex_i2s_remove(struct periplex_device *pdev)
{
    struct periplex_i2s_dev *i2s = periplex_get_drvdata(pdev);
    del_timer_sync(&i2s->playback_timer);
    periplex_audio_queue_cleanup(i2s);
    if (i2s)
    {
        periplex_unlink_device(pdev);
        pr_info("periplex_i2s: Removing Periplex I2S driver\n");
    }

    return 0;
}

static const struct of_device_id periplex_i2s_dt_match[] = {
    {.compatible = "vicharak,periplex-i2s"},
    {},
};
MODULE_DEVICE_TABLE(of, periplex_i2s_dt_match);

static struct periplex_driver periplex_i2s_driver = {
    .probe = periplex_i2s_probe,
    .remove = periplex_i2s_remove,
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = periplex_i2s_dt_match,
    },
};
module_periplex_driver(periplex_i2s_driver);

MODULE_ALIAS("periplex:i2s");
MODULE_AUTHOR("Vatsal Kevadiya <vhkevadiya15@gmail.com>");
MODULE_DESCRIPTION("I2S Driver for the Periplex");
MODULE_LICENSE("GPL");