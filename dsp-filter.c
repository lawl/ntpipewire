/* 
 * PipeWire
 * Copyright © 2019 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <signal.h>

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include "./rnnoise/rnnoise.h"

#include "ringbuf.h"

struct data;

struct port
{
    struct data *data;
};

struct data
{
    struct pw_main_loop *loop;
    struct pw_filter *filter;
    struct port *in_port;
    struct port *out_port;
};

//globals
DenoiseState *st;
ringbuf_t in_buf;
ringbuf_t out_buf;
int32_t remaining_grace_period = 0;
bool init = true;

/* our data processing function is in general:
 *
 *  struct pw_buffer *b;
 *  in = pw_filter_dequeue_buffer(filter, in_port);
 *  out = pw_filter_dequeue_buffer(filter, out_port);
 *
 *  .. do stuff with buffers ...
 *
 *  pw_filter_queue_buffer(filter, in_port, in);
 *  pw_filter_queue_buffer(filter, out_port, out);
 *
 *  For DSP ports, there is a shortcut to directly dequeue, get
 *  the data and requeue the buffer with pw_filter_get_dsp_buffer().
 *
 *
 */
static void on_process(void *userdata, struct spa_io_position *position)
{
    struct data *data = userdata;
    float *in, *out;
    uint32_t n_samples = position->clock.duration;

    if (!st)
    {
        st = rnnoise_create(0);
    }


    in = pw_filter_get_dsp_buffer(data->in_port, n_samples);
    out = pw_filter_get_dsp_buffer(data->out_port, n_samples);


    for (int i = 0; i < n_samples; i++)
    {
        in[i] = in[i] * 32767;
    }

    ringbuf_memcpy_into(in_buf, in, n_samples * sizeof(float));

    if (init)
    {
        init = false;
        for (int i = 0; i < n_samples; i++)
        {
            out[i] = 0.f;
        }
        return;
    }


    const size_t n_frames = ringbuf_bytes_used(in_buf) / sizeof(float) / 480;
    float tmpin[n_frames * 480];
    ringbuf_memcpy_from(tmpin, in_buf, 480 * sizeof(float) * n_frames);

    for (int i = 0; i < n_frames; i++)
    {
        float tmp[480];
        float vad_prob = rnnoise_process_frame(st, tmp, tmpin + (i * 480));
        if (vad_prob > 0.95)
        {
            remaining_grace_period = 20;
        }


        if (remaining_grace_period >= 0)
        {
            remaining_grace_period--;
        }
        else
        {
            for (int i = 0; i < 480; i++)
            {
                tmp[i] = 0.f;
            }
        }
        ringbuf_memcpy_into(out_buf, tmp, 480 * sizeof(float));
    }

    if (ringbuf_bytes_used(out_buf) < n_samples * sizeof(float))
    {
        for (int i = 0; i < n_samples; i++)
        {
            out[i] = 0.f;
        }
        pw_log_warn("[NoiseTorch] Output buffer underrun. Should not happen?\n");
        return;
    }
    else
    {
        ringbuf_memcpy_from(out, out_buf, n_samples * sizeof(float));
    }

    for (int i = 0; i < n_samples; i++)
    {
        out[i] = out[i] / 32767;
    }
}

static const struct pw_filter_events filter_events = {
    PW_VERSION_FILTER_EVENTS,
    .process = on_process,
};

static void do_quit(void *userdata, int signal_number)
{
    struct data *data = userdata;
    pw_main_loop_quit(data->loop);
}

int main(int argc, char *argv[])
{
    struct data data = {
        0,
    };

    pw_init(NULL, NULL);


    //480 floats = 10ms audio = 1 sample for rnnoise
    in_buf = ringbuf_new(480 * sizeof(float) * 100);
    out_buf = ringbuf_new(480 * sizeof(float) * 100);

    /* make a main loop. If you already have another main loop, you can add
	 * the fd of this pipewire mainloop to it. */
    data.loop = pw_main_loop_new(NULL);

    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGINT, do_quit, &data);
    pw_loop_add_signal(pw_main_loop_get_loop(data.loop), SIGTERM, do_quit, &data);

    /* Create a simple filter, the simple filter manages the core and remote
	 * objects for you if you don't need to deal with them.
	 *
	 * Pass your events and a user_data pointer as the last arguments. This
	 * will inform you about the filter state. The most important event
	 * you need to listen to is the process event where you need to process
	 * the data.
	 */

  

    data.filter = pw_filter_new_simple(
        pw_main_loop_get_loop(data.loop),
        "NoiseTorch pipewire POC",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_MEDIA_ROLE, "DSP",
            NULL),
        &filter_events,
        &data);

    
    /* make an audio DSP input port */
    data.in_port = pw_filter_add_port(data.filter,
                                      PW_DIRECTION_INPUT,
                                      PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                      sizeof(struct port),
                                      pw_properties_new(
                                          PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                          PW_KEY_PORT_NAME, "input",
                                          NULL),
                                      NULL, 0);

    /* make an audio DSP output port */
    data.out_port = pw_filter_add_port(data.filter,
                                       PW_DIRECTION_OUTPUT,
                                       PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                       sizeof(struct port),
                                       pw_properties_new(
                                           PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                           PW_KEY_PORT_NAME, "output",
                                           NULL),
                                       NULL, 0);
  

    /* Now connect this filter. We ask that our process function is
	 * called in a realtime thread. */
    if (pw_filter_connect(data.filter,
                          PW_FILTER_FLAG_RT_PROCESS,
                          NULL, 0) < 0)
    {
        fprintf(stderr, "can't connect\n");
        return -1;
    }

    /* and wait while we let things run */
    pw_main_loop_run(data.loop);

    pw_filter_destroy(data.filter);
    pw_main_loop_destroy(data.loop);
    pw_deinit();

    rnnoise_destroy(st);
    ringbuf_free(&in_buf);
    ringbuf_free(&out_buf);

    return 0;
}
