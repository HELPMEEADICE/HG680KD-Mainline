// SPDX-License-Identifier: GPL-2.0-only
#ifndef HI3798_HDMI_MODESET_AUDIO_H
#define HI3798_HDMI_MODESET_AUDIO_H

#include <linux/mutex.h>

struct snd_pcm;

struct hi3798_modeset_audio {
	struct mutex lock;
	void __iomem *tx2, *audio, *reset, *packets;
	struct snd_pcm *pcm;
	bool silent;
};

extern struct hi3798_modeset_audio hi3798_ram_audio;

#endif
