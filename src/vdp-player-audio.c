/* SPDX-License-Identifier: GPL-2.0-only */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <sound/asound.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libswresample/swresample.h>

#include "vdp-player-audio.h"

enum { RATE = 48000, CHANNELS = 2, PERIOD = 480, PERIODS = 8, BUFFER = PERIOD * PERIODS };
static const AVRational sample_time = { 1, RATE };

struct player_audio {
	int fd, index;
	AVFormatContext *input;
	AVCodecContext *decoder;
	AVPacket *packet;
	AVFrame *frame;
	SwrContext *swr;
	int16_t *converted;
	unsigned int capacity;
	int64_t origin_us, base_us, base_sample, tone_frames;
	int64_t lead_frames;
	int64_t last_frame, last_progress_us;
	pthread_t thread;
	bool thread_active, started, used;
	atomic_bool quit;
	atomic_int error;
	atomic_int_fast64_t written, end;
};

static int pcm_ioctl(int fd, unsigned long command, void *arg)
{
	int ret;

	do {
		ret = ioctl(fd, command, arg);
	} while (ret < 0 && errno == EINTR);
	return ret < 0 ? AVERROR(errno) : ret;
}

static void pcm_mask(struct snd_pcm_hw_params *hw, unsigned int param, unsigned int value)
{
	struct snd_mask *mask = &hw->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];

	memset(mask, 0, sizeof(*mask));
	mask->bits[value / 32] = 1U << (value % 32);
}

static void pcm_interval(struct snd_pcm_hw_params *hw, unsigned int param, unsigned int value)
{
	struct snd_interval *i = &hw->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];

	*i = (struct snd_interval){ .min = value, .max = value, .integer = 1 };
}

static int pcm_open(struct player_audio *a, const char *path)
{
	struct snd_pcm_hw_params hw = { .rmask = ~0U };
	struct snd_pcm_sw_params sw = {
		.period_step = 1,
		.avail_min = PERIOD,
		.xfer_align = 1,
		.stop_threshold = BUFFER,
	};
	int ret;

	a->fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
	if (a->fd < 0)
		return AVERROR(errno);
	memset(hw.masks, 0xff, sizeof(hw.masks));
	for (unsigned int i = 0; i < sizeof(hw.intervals) / sizeof(hw.intervals[0]); i++)
		hw.intervals[i].max = ~0U;
	pcm_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
	pcm_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
	pcm_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
	pcm_interval(&hw, SNDRV_PCM_HW_PARAM_RATE, RATE);
	pcm_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, CHANNELS);
	pcm_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, PERIOD);
	pcm_interval(&hw, SNDRV_PCM_HW_PARAM_PERIODS, PERIODS);
	pcm_interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, BUFFER);
	ret = pcm_ioctl(a->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw);
	if (ret < 0)
		return ret;
	sw.boundary = BUFFER;
	while (sw.boundary <= (LONG_MAX - BUFFER) / 2)
		sw.boundary *= 2;
	sw.start_threshold = sw.boundary;
	return pcm_ioctl(a->fd, SNDRV_PCM_IOCTL_SW_PARAMS, &sw);
}

static void sleep_ms(void)
{
	const struct timespec delay = { .tv_nsec = 1000000 };

	nanosleep(&delay, NULL);
}

static int64_t monotonic_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int pcm_write(struct player_audio *a, const int16_t *samples, int frames)
{
	while (frames > 0 && !atomic_load(&a->quit)) {
		struct snd_xferi io = { .buf = (void *)samples, .frames = frames };
		int ret = pcm_ioctl(a->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &io);

		if (ret == AVERROR(EAGAIN) || (!ret && !io.result)) {
			struct pollfd p = { .fd = a->fd, .events = POLLOUT };

			if (poll(&p, 1, 10) > 0)
				sleep_ms();
			continue;
		}
		if (ret < 0)
			return ret;
		if (io.result < 0)
			return io.result;
		atomic_fetch_add(&a->written, io.result);
		samples += io.result * CHANNELS;
		frames -= io.result;
	}
	return 0;
}

static int pcm_silence(struct player_audio *a, int64_t frames)
{
	const int16_t silence[PERIOD * CHANNELS] = {};

	while (frames > 0 && !atomic_load(&a->quit)) {
		int n = frames < PERIOD ? frames : PERIOD;
		int ret = pcm_write(a, silence, n);

		if (ret < 0)
			return ret;
		frames -= n;
	}
	return 0;
}

static int audio_buffer(struct player_audio *a, int frames)
{
	void *buffer;

	if (frames < 0)
		return frames;
	if (!frames)
		return 0;
	buffer = av_fast_realloc(a->converted, &a->capacity,
				 (size_t)frames * CHANNELS * sizeof(*a->converted));
	if (!buffer)
		return AVERROR(ENOMEM);
	a->converted = buffer;
	return 0;
}

static int resample(struct player_audio *a, const AVFrame *frame, int64_t *stamp)
{
	int ret, frames;
	uint8_t *out;

	if (frame && !a->swr) {
		AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
		AVChannelLayout input = {};

		if (frame->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
			av_channel_layout_default(&input, frame->ch_layout.nb_channels);
		else if ((ret = av_channel_layout_copy(&input, &frame->ch_layout)) < 0)
			return ret;
		ret = swr_alloc_set_opts2(&a->swr, &stereo, AV_SAMPLE_FMT_S16, RATE,
					  &input, frame->format, frame->sample_rate, 0, NULL);
		av_channel_layout_uninit(&input);
		if (ret < 0)
			return ret;
		if ((ret = swr_init(a->swr)) < 0)
			return ret;
	}
	*stamp = AV_NOPTS_VALUE;
	if (!a->swr)
		return 0;
	if (frame && frame->best_effort_timestamp != AV_NOPTS_VALUE) {
		*stamp = av_rescale_q(frame->best_effort_timestamp,
				      a->input->streams[a->index]->time_base, sample_time) -
			 swr_get_delay(a->swr, RATE);
	}
	frames = swr_get_out_samples(a->swr, frame ? frame->nb_samples : 0);
	ret = audio_buffer(a, frames);
	if (ret < 0)
		return ret;
	out = (uint8_t *)a->converted;
	return swr_convert(a->swr, &out, frames,
			   frame ? (const uint8_t **)frame->extended_data : NULL,
			   frame ? frame->nb_samples : 0);
}

static int play_samples(struct player_audio *a, int64_t stamp, int frames)
{
	int64_t expected = a->base_sample + atomic_load(&a->written) - a->lead_frames;
	int64_t gap = stamp == AV_NOPTS_VALUE ? 0 : stamp - expected;
	int ret, trim = 0;

	/* Matroska's millisecond PTS rounds a continuous sample sequence. */
	if (gap > RATE / 1000) {
		ret = pcm_silence(a, gap);
		if (ret < 0)
			return ret;
	} else if (gap < -RATE / 1000) {
		trim = -gap < frames ? -gap : frames;
	}
	return pcm_write(a, a->converted + trim * CHANNELS, frames - trim);
}

static int audio_next(struct player_audio *a)
{
	for (;;) {
		int ret = avcodec_receive_frame(a->decoder, a->frame);

		if (ret != AVERROR(EAGAIN) || atomic_load(&a->quit))
			return ret;
		do {
			av_packet_unref(a->packet);
			ret = av_read_frame(a->input, a->packet);
		} while (ret >= 0 && a->packet->stream_index != a->index &&
			 !atomic_load(&a->quit));
		if (ret < 0 && ret != AVERROR_EOF)
			return ret;
		ret = avcodec_send_packet(a->decoder, ret == AVERROR_EOF ? NULL : a->packet);
		if (ret < 0)
			return ret;
	}
}

static int decode_audio(struct player_audio *a)
{
	int ret = 0;

	while (!atomic_load(&a->quit)) {
		int64_t stamp;
		int frames;

		ret = audio_next(a);
		if (ret < 0 && ret != AVERROR_EOF)
			break;
		frames = resample(a, ret == AVERROR_EOF ? NULL : a->frame, &stamp);
		av_frame_unref(a->frame);
		if (frames < 0)
			return frames;
		if (ret == AVERROR_EOF && !frames)
			return 0;
		if (frames && (ret = play_samples(a, stamp, frames)) < 0)
			break;
	}
	return ret;
}

static int generate_tone(struct player_audio *a)
{
	int16_t samples[PERIOD * CHANNELS];

	for (int64_t pos = 0; pos < a->tone_frames && !atomic_load(&a->quit); pos += PERIOD) {
		int n = a->tone_frames - pos < PERIOD ? a->tone_frames - pos : PERIOD;
		int ret;

		for (int i = 0; i < n; i++) {
			int64_t fade = pos + i < PERIOD ? pos + i : a->tone_frames - pos - i - 1;
			double gain = fade < PERIOD ? (double)fade / PERIOD : 1;
			int16_t sample = lrint(8192 * gain * sin(2 * M_PI * ((pos + i) % 48) / 48));

			samples[2 * i] = samples[2 * i + 1] = sample;
		}
		ret = pcm_write(a, samples, n);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static void *audio_worker(void *data)
{
	struct player_audio *a = data;
	int ret = pcm_silence(a, a->lead_frames);

	if (!ret)
		ret = a->input ? decode_audio(a) : generate_tone(a);
	if (!ret) {
		atomic_store(&a->end, atomic_load(&a->written));
		/* Keep the clock alive for the last vblank and a shorter audio track. */
		while (!atomic_load(&a->quit) && !ret)
			ret = pcm_silence(a, PERIOD);
	}
	if (ret < 0 && !atomic_load(&a->quit))
		atomic_store(&a->error, ret);
	return NULL;
}

int player_audio_open(struct player_audio **audio, const char *file, const char *pcm,
		      int64_t video_origin_us)
{
	const AVCodec *codec;
	struct player_audio *a = calloc(1, sizeof(*a));
	int ret;

	if (!a)
		return AVERROR(ENOMEM);
	a->fd = -1;
	a->origin_us = video_origin_us;
	*audio = a;
	ret = avformat_open_input(&a->input, file, NULL, NULL);
	if (ret < 0)
		goto fail;
	ret = av_find_best_stream(a->input, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
	if (ret < 0) {
		if (ret == AVERROR_STREAM_NOT_FOUND)
			ret = 0;
		goto fail;
	}
	a->index = ret;
	a->decoder = avcodec_alloc_context3(codec);
	a->packet = av_packet_alloc();
	a->frame = av_frame_alloc();
	if (!a->decoder || !a->packet || !a->frame) {
		ret = AVERROR(ENOMEM);
		goto fail;
	}
	ret = avcodec_parameters_to_context(a->decoder, a->input->streams[a->index]->codecpar);
	if (ret < 0)
		goto fail;
	a->decoder->pkt_timebase = a->input->streams[a->index]->time_base;
	ret = avcodec_open2(a->decoder, codec, NULL);
	if (ret < 0)
		goto fail;
	ret = pcm_open(a, pcm);
	if (ret >= 0)
		return 0;
fail:
	player_audio_close(audio);
	return ret;
}

int player_audio_tone(struct player_audio **audio, const char *pcm, int64_t us)
{
	struct player_audio *a = calloc(1, sizeof(*a));
	int ret;

	if (!a)
		return AVERROR(ENOMEM);
	a->tone_frames = av_rescale_q(us, AV_TIME_BASE_Q, sample_time);
	*audio = a;
	ret = pcm_open(a, pcm);
	if (ret < 0)
		player_audio_close(audio);
	return ret;
}

int player_audio_prime(struct player_audio *a, int64_t pts_us, int64_t lead_us,
		       const volatile sig_atomic_t *stop)
{
	int ret;

	a->base_us = pts_us;
	a->lead_frames = av_rescale_q(lead_us, AV_TIME_BASE_Q, sample_time);
	a->base_sample = av_rescale_q(a->origin_us + pts_us, AV_TIME_BASE_Q, sample_time);
	if (a->input && (a->used || pts_us)) {
		/* A video keyframe can follow audio packets covering the same
		 * PTS. Demux from before the target and trim in sample units.
		 */
		int64_t preroll = AV_TIME_BASE / 4;
		int64_t seek_us = pts_us > preroll ? pts_us - preroll : 0;

		ret = av_seek_frame(a->input, -1, a->origin_us + seek_us, AVSEEK_FLAG_BACKWARD);
		if (ret < 0)
			return ret;
		avcodec_flush_buffers(a->decoder);
		av_packet_unref(a->packet);
		av_frame_unref(a->frame);
		swr_free(&a->swr);
	}
	a->used = true;
	ret = pcm_ioctl(a->fd, SNDRV_PCM_IOCTL_PREPARE, NULL);
	if (ret < 0)
		return ret;
	atomic_store(&a->quit, false);
	atomic_store(&a->error, 0);
	atomic_store(&a->written, 0);
	atomic_store(&a->end, -1);
	ret = pthread_create(&a->thread, NULL, audio_worker, a);
	if (ret)
		return AVERROR(ret);
	a->thread_active = true;
	while (!*stop && !atomic_load(&a->error) && atomic_load(&a->written) < 3 * PERIOD)
		sleep_ms();
	return atomic_load(&a->error);
}

int player_audio_start(struct player_audio *a)
{
	int ret = pcm_ioctl(a->fd, SNDRV_PCM_IOCTL_START, NULL);

	a->started = ret >= 0;
	a->last_frame = 0;
	a->last_progress_us = monotonic_us();
	return ret;
}

static int hardware_position(struct player_audio *a, int64_t *frames)
{
	struct snd_pcm_sync_ptr sync = {
		.flags = SNDRV_PCM_SYNC_PTR_HWSYNC | SNDRV_PCM_SYNC_PTR_APPL |
			 SNDRV_PCM_SYNC_PTR_AVAIL_MIN,
	};
	int ret = atomic_load(&a->error);

	if (ret < 0)
		return ret;
	ret = pcm_ioctl(a->fd, SNDRV_PCM_IOCTL_SYNC_PTR, &sync);
	if (ret < 0)
		return ret;
	if (sync.s.status.state == SNDRV_PCM_STATE_XRUN)
		return AVERROR(EPIPE);
	*frames = sync.s.status.hw_ptr;
	if (*frames != a->last_frame) {
		a->last_frame = *frames;
		a->last_progress_us = monotonic_us();
	} else if (a->started && monotonic_us() - a->last_progress_us > 1000000) {
		return AVERROR(ETIMEDOUT);
	}
	return 0;
}

int player_audio_clock(struct player_audio *a, int64_t *pts_us)
{
	int64_t frames;
	int ret = hardware_position(a, &frames);

	if (!ret)
		*pts_us = a->base_us + av_rescale_q(frames - a->lead_frames, sample_time, AV_TIME_BASE_Q);
	return ret;
}

int player_audio_finish(struct player_audio *a, const volatile sig_atomic_t *stop)
{
	if (!a->started)
		return 0;
	while (!*stop) {
		int64_t end = atomic_load(&a->end);
		int64_t frames;
		int ret = hardware_position(a, &frames);

		if (ret < 0)
			return ret;
		if (end >= 0 && frames >= end) {
			printf("ALSA played=%lld frames rate=%d xruns=0\n",
			       (long long)(end - a->lead_frames), RATE);
			break;
		}
		sleep_ms();
	}
	return 0;
}

void player_audio_flush(struct player_audio *a)
{
	if (!a->thread_active)
		return;
	atomic_store(&a->quit, true);
	pthread_join(a->thread, NULL);
	pcm_ioctl(a->fd, SNDRV_PCM_IOCTL_DROP, NULL);
	a->thread_active = false;
	a->started = false;
}

void player_audio_close(struct player_audio **audio)
{
	struct player_audio *a = *audio;

	if (!a)
		return;
	player_audio_flush(a);
	if (a->fd >= 0)
		close(a->fd);
	avcodec_free_context(&a->decoder);
	avformat_close_input(&a->input);
	av_packet_free(&a->packet);
	av_frame_free(&a->frame);
	swr_free(&a->swr);
	av_free(a->converted);
	free(a);
	*audio = NULL;
}
