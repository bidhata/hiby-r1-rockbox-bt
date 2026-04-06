/***************************************************************************
 * HiBy-specific hooks for the shared hosted ALSA backend.
 *
 * This header is included directly by pcm-alsa.c so the poll-thread
 * scheduler can reuse the shared pump logic without forking the backend.
 ****************************************************************************/

#ifndef PCM_ALSA_HIBY_HOOKS_H
#define PCM_ALSA_HIBY_HOOKS_H

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "pcm-alsa-hiby.h"

static void open_hwdev(const char *device, snd_pcm_stream_t mode);
static void pcm_dma_apply_settings_nolock(void);
static void pcm_pump_locked(snd_pcm_t *handle);

static pthread_t hiby_pcm_poll_thread;
static volatile bool hiby_pcm_poll_thread_stop = false;
static bool hiby_pcm_poll_thread_running = false;
static bool hiby_pcm_mutex_initialized = false;
static unsigned int hiby_pcm_poll_interval_us = 10000;

static void hiby_pcm_mutex_init_once(void)
{
    pthread_mutexattr_t attr;

    if (hiby_pcm_mutex_initialized)
        return;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&pcm_mtx, &attr);
    pthread_mutexattr_destroy(&attr);
    hiby_pcm_mutex_initialized = true;
}

static bool hiby_pcm_bt_active(void)
{
    const char *device = current_alsa_device ? current_alsa_device : playback_dev;
    return hiby_pcm_is_bluealsa_device(device);
}

static bool hiby_pcm_params_ready(void)
{
    return period_size > 0 && buffer_size > 0 && frames != NULL;
}

int pcm_alsa_switch_playback_device(const char *device)
{
    int rc;

    if (!device || !*device)
        return -1;

    playback_dev = device;
    if (!handle)
        return 0;

    hiby_pcm_mutex_init_once();
    pthread_mutex_lock(&pcm_mtx);
    open_hwdev(playback_dev, SND_PCM_STREAM_PLAYBACK);
    pcm_dma_apply_settings_nolock();
    rc = (current_alsa_device == playback_dev) ? 0 : -1;
    pthread_mutex_unlock(&pcm_mtx);
    return rc;
}

static bool hiby_pcm_keep_hwdev(const char *device, snd_pcm_stream_t mode)
{
    if (!(handle && device == current_alsa_device
#ifdef HAVE_RECORDING
          && current_alsa_mode == mode
#endif
         ))
    {
        return false;
    }

    return snd_pcm_state(handle) != SND_PCM_STATE_DISCONNECTED;
}

static void hiby_pcm_stop_poll_thread(void)
{
    if (!hiby_pcm_poll_thread_running)
        return;

    hiby_pcm_poll_thread_stop = true;
    pthread_join(hiby_pcm_poll_thread, NULL);
    hiby_pcm_poll_thread_running = false;
}

static void *hiby_pcm_poll_thread_fn(void *arg)
{
    (void)arg;

    while (!hiby_pcm_poll_thread_stop)
    {
        if (handle && pthread_mutex_trylock(&pcm_mtx) == 0)
        {
            pcm_pump_locked(handle);
            pthread_mutex_unlock(&pcm_mtx);
        }
        usleep(hiby_pcm_poll_interval_us);
    }

    return NULL;
}

static void hiby_pcm_start_poll_thread(void)
{
    int err;

    hiby_pcm_mutex_init_once();
    if (hiby_pcm_poll_thread_running)
        return;

    hiby_pcm_poll_thread_stop = false;
    err = pthread_create(&hiby_pcm_poll_thread, NULL, hiby_pcm_poll_thread_fn, NULL);
    if (err == 0)
        hiby_pcm_poll_thread_running = true;
    else
        panicf("Unable to start ALSA poll thread: %s", strerror(err));
}

#endif /* PCM_ALSA_HIBY_HOOKS_H */
