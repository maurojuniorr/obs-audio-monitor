#include "audio-monitor-mac.h"
#include <AudioToolbox/AudioQueue.h>
#include <CoreFoundation/CFString.h>
#include <obs-module.h>
#include <util/threading.h>
#include <math.h>
#include <errno.h>
#include <time.h>

#define QUEUE_BUFFERS 3
#define MAX_QUEUE_FAILURES 3

struct audio_monitor {
	pthread_mutex_t mutex;
	pthread_cond_t wake;
	pthread_t worker;
	bool worker_created;
	bool quit;
	bool enabled;
	bool accepting;
	uint64_t generation;
	AudioQueueRef queue;
	AudioQueueBufferRef buffers[QUEUE_BUFFERS];
	bool ready[QUEUE_BUFFERS];
	char *device_id;
	uint32_t channels;
	uint32_t sample_rate;
	size_t buffer_frames;
	size_t capacity;
	size_t read_frame;
	size_t frames;
	float *samples;
	bool playing;
	size_t preroll_frames;
	size_t preroll_wait_frames;
	size_t fade_frames;
	size_t fade_remaining;
	float last_sample[MAX_AUDIO_CHANNELS];
	uint64_t underruns;
	float volume;
	bool mono;
	float balance;
};

/* Callbacks return buffer ownership only. All AudioQueue API calls belong to
 * the worker, never to OBS capture threads or CoreAudio callbacks. */
static void buffer_audio(void *data, AudioQueueRef queue, AudioQueueBufferRef buffer)
{
	struct audio_monitor *m = data;
	pthread_mutex_lock(&m->mutex);
	if (m->accepting && m->queue == queue) {
		for (size_t i = 0; i < QUEUE_BUFFERS; i++) {
			if (m->buffers[i] == buffer) {
				m->ready[i] = true;
				break;
			}
		}
		pthread_cond_signal(&m->wake);
	}
	pthread_mutex_unlock(&m->mutex);
}

static void close_queue(struct audio_monitor *m, AudioQueueRef queue)
{
	pthread_mutex_lock(&m->mutex);
	m->accepting = false;
	m->queue = NULL;
	m->frames = 0;
	m->read_frame = 0;
	m->playing = false;
	m->preroll_wait_frames = 0;
	m->fade_remaining = 0;
	memset(m->last_sample, 0, sizeof(m->last_sample));
	memset(m->buffers, 0, sizeof(m->buffers));
	memset(m->ready, 0, sizeof(m->ready));
	pthread_mutex_unlock(&m->mutex);
	/* Synchronous disposal waits for callbacks and frees allocated buffers.
	 * Do not hold the callback mutex across this call. */
	if (queue)
		AudioQueueDispose(queue, true);
}

static OSStatus open_queue(struct audio_monitor *m, AudioQueueRef *queue, uint64_t generation)
{
	AudioStreamBasicDescription desc = {
		.mSampleRate = m->sample_rate,
		.mFormatID = kAudioFormatLinearPCM,
		.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
		.mBytesPerPacket = sizeof(float) * m->channels,
		.mFramesPerPacket = 1,
		.mBytesPerFrame = sizeof(float) * m->channels,
		.mChannelsPerFrame = m->channels,
		.mBitsPerChannel = sizeof(float) * 8,
	};
	OSStatus status = AudioQueueNewOutput(&desc, buffer_audio, m, NULL, NULL, 0, queue);
	if (status != noErr)
		return status;
	if (strcmp(m->device_id, "default") != 0) {
		CFStringRef uid = CFStringCreateWithCString(NULL, m->device_id, kCFStringEncodingUTF8);
		if (!uid)
			return kAudioQueueErr_InvalidDevice;
		status = AudioQueueSetProperty(*queue, kAudioQueueProperty_CurrentDevice, &uid, sizeof(uid));
		CFRelease(uid);
		if (status != noErr)
			return status;
	}
	AudioQueueBufferRef buffers[QUEUE_BUFFERS] = {0};
	size_t bytes = m->buffer_frames * m->channels * sizeof(float);
	for (size_t i = 0; i < QUEUE_BUFFERS; i++) {
		status = AudioQueueAllocateBuffer(*queue, (UInt32)bytes, &buffers[i]);
		if (status != noErr)
			return status;
		memset(buffers[i]->mAudioData, 0, bytes);
		buffers[i]->mAudioDataByteSize = (UInt32)bytes;
		status = AudioQueueEnqueueBuffer(*queue, buffers[i], 0, NULL);
		if (status != noErr)
			return status;
	}
	pthread_mutex_lock(&m->mutex);
	bool current = !m->quit && m->enabled && m->generation == generation;
	if (current) {
		m->queue = *queue;
		memcpy(m->buffers, buffers, sizeof(buffers));
		m->accepting = true;
	}
	pthread_mutex_unlock(&m->mutex);
	if (!current)
		return noErr;
	return AudioQueueStart(*queue, NULL);
}

/* mutex held. Wait for a small reserve before starting/resuming consumption;
 * otherwise mismatched producer/consumer block sizes create repeated gaps.
 * CoreAudio continues receiving silence while this reserve accumulates. */
static void fill_buffer(struct audio_monitor *m, AudioQueueBufferRef buffer)
{
	size_t stride = m->channels * sizeof(float);
	float *out = buffer->mAudioData;
	memset(out, 0, m->buffer_frames * stride);
	buffer->mAudioDataByteSize = (UInt32)(m->buffer_frames * stride);
	if (!m->playing) {
		if (!m->frames) {
			m->preroll_wait_frames = 0;
			return;
		}
		/* Do not strand short notifications that never fill the reserve.
		 * Bound pre-roll to six output buffers (180 ms). */
		m->preroll_wait_frames += m->buffer_frames;
		if (m->frames < m->preroll_frames && m->preroll_wait_frames < m->preroll_frames * 2)
			return;
		m->playing = true;
		m->preroll_wait_frames = 0;
		m->fade_remaining = m->fade_frames;
	}
	size_t count = m->frames < m->buffer_frames ? m->frames : m->buffer_frames;
	size_t first = count;
	if (first > m->capacity - m->read_frame)
		first = m->capacity - m->read_frame;
	memcpy(buffer->mAudioData, m->samples + m->read_frame * m->channels, first * stride);
	memcpy((uint8_t *)buffer->mAudioData + first * stride, m->samples, (count - first) * stride);
	m->read_frame = (m->read_frame + count) % m->capacity;
	m->frames -= count;
	for (size_t i = 0; i < count; i++) {
		if (m->fade_remaining) {
			float gain = (float)(m->fade_frames - m->fade_remaining) / m->fade_frames;
			for (uint32_t c = 0; c < m->channels; c++)
				out[i * m->channels + c] *= gain;
			m->fade_remaining--;
		}
		for (uint32_t c = 0; c < m->channels; c++)
			m->last_sample[c] = out[i * m->channels + c];
	}
	if (count < m->buffer_frames) {
		/* Reach zero smoothly even when the source disappears exactly at a
		 * buffer boundary. This tail runs only during a real underrun. */
		size_t end = count + m->fade_frames;
		if (end > m->buffer_frames)
			end = m->buffer_frames;
		size_t begin = end > m->fade_frames ? end - m->fade_frames : 0;
		for (size_t i = begin; i < end; i++) {
			float gain = end - begin > 1 ? (float)(end - i - 1) / (end - begin - 1) : 0.0f;
			for (uint32_t c = 0; c < m->channels; c++)
				out[i * m->channels + c] = (i < count ? out[i * m->channels + c] : m->last_sample[c]) * gain;
		}
		memset(m->last_sample, 0, sizeof(m->last_sample));
		m->playing = false;
		m->fade_remaining = 0;
		m->underruns++;
	}
}

static void *monitor_worker(void *data)
{
	struct audio_monitor *m = data;
	uint64_t last_generation = UINT64_MAX;
	unsigned failures = 0;
	pthread_mutex_lock(&m->mutex);
	while (!m->quit) {
		if (last_generation != m->generation) {
			last_generation = m->generation;
			failures = 0;
		}
		if (!m->enabled || failures >= MAX_QUEUE_FAILURES) {
			pthread_cond_wait(&m->wake, &m->mutex);
			continue;
		}
		uint64_t generation = m->generation;
		pthread_mutex_unlock(&m->mutex);
		AudioQueueRef queue = NULL;
		OSStatus status = open_queue(m, &queue, generation);
		pthread_mutex_lock(&m->mutex);
		while (status == noErr && !m->quit && m->enabled && m->generation == generation) {
			size_t index = 0;
			while (index < QUEUE_BUFFERS && !m->ready[index])
				index++;
			if (index == QUEUE_BUFFERS) {
				pthread_cond_wait(&m->wake, &m->mutex);
				continue;
			}
			m->ready[index] = false;
			AudioQueueBufferRef buffer = m->buffers[index];
			fill_buffer(m, buffer);
			pthread_mutex_unlock(&m->mutex);
			status = AudioQueueEnqueueBuffer(queue, buffer, 0, NULL);
			pthread_mutex_lock(&m->mutex);
		}
		pthread_mutex_unlock(&m->mutex);
		close_queue(m, queue);
		pthread_mutex_lock(&m->mutex);
		if (status != noErr && m->enabled && !m->quit && m->generation == generation) {
			failures++;
			pthread_mutex_unlock(&m->mutex);
			blog(LOG_WARNING, "[Audio Monitor — maurojuniorr Rebuild v5] device %s: queue failed: %d (%u/%d)",
			     m->device_id, (int)status, failures, MAX_QUEUE_FAILURES);
			pthread_mutex_lock(&m->mutex);
			struct timespec deadline;
			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_sec++;
			while (!m->quit && m->enabled && m->generation == generation) {
				if (pthread_cond_timedwait(&m->wake, &m->mutex, &deadline) == ETIMEDOUT)
					break;
			}
		}
	}
	pthread_mutex_unlock(&m->mutex);
	return NULL;
}

void audio_monitor_start(struct audio_monitor *m)
{
	if (!m)
		return;
	pthread_mutex_lock(&m->mutex);
	m->enabled = true;
	m->generation++;
	pthread_cond_signal(&m->wake);
	pthread_mutex_unlock(&m->mutex);
}

void audio_monitor_stop(struct audio_monitor *m)
{
	if (!m)
		return;
	pthread_mutex_lock(&m->mutex);
	m->enabled = false;
	m->accepting = false;
	m->generation++;
	m->frames = 0;
	pthread_cond_signal(&m->wake);
	pthread_mutex_unlock(&m->mutex);
}

void audio_monitor_audio(void *data, struct obs_audio_data *audio)
{
	struct audio_monitor *m = data;
	if (!m || !audio || !audio->frames || pthread_mutex_trylock(&m->mutex) != 0)
		return;
	if (!m->enabled || !m->accepting) {
		pthread_mutex_unlock(&m->mutex);
		return;
	}
	/* OBS supplies float planar audio in its output channel layout/rate.
	 * Interleave directly, without allocation or CoreAudio calls. */
	size_t count = audio->frames < m->capacity ? audio->frames : m->capacity;
	size_t skip = audio->frames - count;
	if (m->frames + count > m->capacity) {
		size_t discard = m->frames + count - m->capacity;
		m->read_frame = (m->read_frame + discard) % m->capacity;
		m->frames -= discard;
	}
	size_t write = (m->read_frame + m->frames) % m->capacity;
	float left = 1.0f, right = 1.0f;
	if (m->balance != 0.0f) {
		float balance = (m->balance + 1.0f) / 2.0f;
		left = sinf((1.0f - balance) * (float)(M_PI / 2.0));
		right = sinf(balance * (float)(M_PI / 2.0));
	}
	for (size_t i = 0; i < count; i++) {
		float *out = m->samples + write * m->channels;
		float sum = 0.0f;
		for (uint32_t c = 0; c < m->channels; c++) {
			const float *plane = (const float *)audio->data[c];
			out[c] = plane ? plane[skip + i] * m->volume : 0.0f;
			sum += out[c];
		}
		if (m->mono && m->channels > 1)
			for (uint32_t c = 0; c < m->channels; c++)
				out[c] = sum / m->channels;
		if (m->channels > 1) {
			out[0] *= left;
			out[1] *= right;
		}
		write = (write + 1) % m->capacity;
	}
	m->frames += count;
	pthread_mutex_unlock(&m->mutex);
}

void audio_monitor_set_volume(struct audio_monitor *m, float volume)
{
	if (!m) return;
	pthread_mutex_lock(&m->mutex);
	m->volume = volume;
	pthread_mutex_unlock(&m->mutex);
}

void audio_monitor_set_mono(struct audio_monitor *m, bool mono)
{
	if (!m) return;
	pthread_mutex_lock(&m->mutex);
	m->mono = mono;
	pthread_mutex_unlock(&m->mutex);
}

void audio_monitor_set_balance(struct audio_monitor *m, float balance)
{
	if (!m) return;
	pthread_mutex_lock(&m->mutex);
	m->balance = balance;
	pthread_mutex_unlock(&m->mutex);
}

struct audio_monitor *audio_monitor_create(const char *device_id, const char *source_name, int port)
{
	UNUSED_PARAMETER(source_name);
	UNUSED_PARAMETER(port);
	struct obs_audio_info info;
	if (!device_id || !*device_id || !obs_get_audio_info(&info))
		return NULL;
	struct audio_monitor *m = bzalloc(sizeof(*m));
	m->channels = get_audio_channels(info.speakers);
	m->sample_rate = info.samples_per_sec;
	if (!m->channels || !m->sample_rate) {
		bfree(m);
		return NULL;
	}
	m->buffer_frames = m->sample_rate * 30 / 1000;
	m->preroll_frames = m->buffer_frames * 3;
	m->fade_frames = m->sample_rate * 5 / 1000;
	m->capacity = m->buffer_frames * 16;
	m->samples = bzalloc(m->capacity * m->channels * sizeof(float));
	m->device_id = bstrdup(device_id);
	m->volume = 1.0f;
	pthread_mutex_init(&m->mutex, NULL);
	pthread_cond_init(&m->wake, NULL);
	m->worker_created = pthread_create(&m->worker, NULL, monitor_worker, m) == 0;
	if (!m->worker_created) {
		pthread_cond_destroy(&m->wake);
		pthread_mutex_destroy(&m->mutex);
		bfree(m->samples);
		bfree(m->device_id);
		bfree(m);
		return NULL;
	}
	return m;
}

void audio_monitor_destroy(struct audio_monitor *m)
{
	if (!m) return;
	pthread_mutex_lock(&m->mutex);
	m->quit = true;
	m->enabled = false;
	m->accepting = false;
	pthread_cond_signal(&m->wake);
	pthread_mutex_unlock(&m->mutex);
	pthread_join(m->worker, NULL);
	pthread_cond_destroy(&m->wake);
	pthread_mutex_destroy(&m->mutex);
	bfree(m->samples);
	bfree(m->device_id);
	bfree(m);
}

const char *audio_monitor_get_device_id(struct audio_monitor *m)
{
	return m ? m->device_id : NULL;
}

void audio_monitor_set_format(struct audio_monitor *m, enum audio_format format)
{
	UNUSED_PARAMETER(m);
	UNUSED_PARAMETER(format);
}

void audio_monitor_set_samples_per_sec(struct audio_monitor *m, long long sample_rate)
{
	UNUSED_PARAMETER(m);
	UNUSED_PARAMETER(sample_rate);
}
