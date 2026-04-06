/***************************************************************************
 * HiBy-specific helpers for the hosted ALSA PCM backend
 ****************************************************************************/

#include "autoconf.h"

#include "pcm-alsa-hiby.h"

#include <ctype.h>
#include <string.h>

static char hiby_pcm_bt_mac[18];

void hiby_pcm_set_bt_mac(const char *mac)
{
    size_t i;

    if (!mac || !mac[0])
    {
        hiby_pcm_bt_mac[0] = '\0';
        return;
    }

    for (i = 0; mac[i] != '\0' && i + 1 < sizeof(hiby_pcm_bt_mac); i++)
    {
        char c = mac[i];
        if (c == ':')
            c = '_';
        hiby_pcm_bt_mac[i] = (char)toupper((unsigned char)c);
    }
    hiby_pcm_bt_mac[i] = '\0';
}

const char *hiby_pcm_get_bt_mac(void)
{
    return hiby_pcm_bt_mac[0] ? hiby_pcm_bt_mac : NULL;
}

bool hiby_pcm_is_bluealsa_device(const char *device)
{
    return device && strstr(device, "bluealsa:") != NULL;
}

void hiby_pcm_adjust_bt_buffering(snd_pcm_sframes_t *period_size,
                                  snd_pcm_sframes_t *buffer_size,
                                  bool bluealsa_active)
{
    if (!bluealsa_active || !period_size || !buffer_size)
        return;

    *period_size *= 2;
    if (*period_size < 2048)
        *period_size = 2048;
    *buffer_size = *period_size * 8;
}

unsigned int hiby_pcm_calc_poll_interval(unsigned int sample_rate,
                                         snd_pcm_sframes_t period_size,
                                         bool bluealsa_active)
{
    unsigned int interval_us;

    if (sample_rate == 0 || period_size <= 0)
        return 10000;

    interval_us = (unsigned int)((period_size * 1000000ULL) / sample_rate);
    if (bluealsa_active)
        interval_us /= 2;
    if (interval_us < 2000)
        interval_us = 2000;
    if (interval_us > 50000)
        interval_us = 50000;
    return interval_us;
}

snd_pcm_uframes_t hiby_pcm_start_threshold(snd_pcm_sframes_t buffer_size,
                                           snd_pcm_sframes_t period_size,
                                           bool bluealsa_active)
{
    if (bluealsa_active && buffer_size > period_size)
        return (snd_pcm_uframes_t)(buffer_size - period_size);

    return (snd_pcm_uframes_t)(buffer_size / 2);
}
