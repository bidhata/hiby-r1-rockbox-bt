/***************************************************************************
 * HiBy-specific helpers for the hosted ALSA PCM backend
 ****************************************************************************/

#ifndef PCM_ALSA_HIBY_H
#define PCM_ALSA_HIBY_H

#include <stdbool.h>
#include <alsa/asoundlib.h>

/* HiBy-only runtime playback route control. */
int pcm_alsa_switch_playback_device(const char *device);
void hiby_pcm_set_bt_mac(const char *mac);
const char *hiby_pcm_get_bt_mac(void);

bool hiby_pcm_is_bluealsa_device(const char *device);

void hiby_pcm_adjust_bt_buffering(snd_pcm_sframes_t *period_size,
                                  snd_pcm_sframes_t *buffer_size,
                                  bool bluealsa_active);

unsigned int hiby_pcm_calc_poll_interval(unsigned int sample_rate,
                                         snd_pcm_sframes_t period_size,
                                         bool bluealsa_active);

snd_pcm_uframes_t hiby_pcm_start_threshold(snd_pcm_sframes_t buffer_size,
                                           snd_pcm_sframes_t period_size,
                                           bool bluealsa_active);

#endif /* PCM_ALSA_HIBY_H */
