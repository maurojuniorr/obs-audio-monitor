/* Deterministic regression test: real worker/DSP code, simulated AudioQueue.
 * No sound is played and no OBS configuration is changed. */
#include <AudioToolbox/AudioQueue.h>
#include <obs-module.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

static pthread_t producer;
static atomic_int live_queues, starts, enqueues, wrong_thread, fail_start;
struct fake_queue {
	AudioQueueOutputCallback callback;
	void *data;
	AudioQueueBufferRef buffers[3];
	int count;
	bool started;
};
static void check_thread(void)
{
	if (pthread_equal(producer, pthread_self())) atomic_fetch_add(&wrong_thread, 1);
}
static OSStatus fake_new(const AudioStreamBasicDescription *desc, AudioQueueOutputCallback cb,
		void *data, CFRunLoopRef loop, CFStringRef mode, UInt32 flags, AudioQueueRef *result)
{
	(void)desc; (void)loop; (void)mode; (void)flags;
	check_thread();
	struct fake_queue *q = calloc(1, sizeof(*q));
	q->callback = cb; q->data = data;
	*result = (AudioQueueRef)q;
	atomic_fetch_add(&live_queues, 1);
	return noErr;
}
static OSStatus fake_allocate(AudioQueueRef queue, UInt32 size, AudioQueueBufferRef *buffer)
{
	check_thread();
	struct fake_queue *q = (void *)queue;
	*buffer = calloc(1, sizeof(**buffer));
	AudioQueueBuffer initial = {.mAudioData = calloc(1, size)};
	memcpy(*buffer, &initial, sizeof(initial));
	q->buffers[q->count++] = *buffer;
	return noErr;
}
static OSStatus fake_enqueue(AudioQueueRef queue, AudioQueueBufferRef buffer, UInt32 n,
		const AudioStreamPacketDescription *descriptions)
{
	(void)n; (void)descriptions;
	check_thread();
	struct fake_queue *q = (void *)queue;
	usleep(1000);
	atomic_fetch_add(&enqueues, 1);
	if (q->started) q->callback(q->data, queue, buffer);
	return noErr;
}
static OSStatus fake_start(AudioQueueRef queue, const AudioTimeStamp *timestamp)
{
	(void)timestamp;
	check_thread();
	atomic_fetch_add(&starts, 1);
	/* Reproduce a slow synchronous start and reentrant callbacks. */
	usleep(50000);
	if (atomic_load(&fail_start)) return kAudioQueueErr_CannotStart;
	struct fake_queue *q = (void *)queue;
	q->started = true;
	for (int i = 0; i < q->count; i++) q->callback(q->data, queue, q->buffers[i]);
	return noErr;
}
static OSStatus fake_dispose(AudioQueueRef queue, Boolean immediate)
{
	(void)immediate;
	check_thread();
	struct fake_queue *q = (void *)queue;
	for (int i = 0; i < q->count; i++) {
		q->callback(q->data, queue, q->buffers[i]);
		free(q->buffers[i]->mAudioData); free(q->buffers[i]);
	}
	free(q);
	atomic_fetch_sub(&live_queues, 1);
	return noErr;
}
static bool fake_info(struct obs_audio_info *info)
{
	info->samples_per_sec = 48000; info->speakers = SPEAKERS_STEREO;
	return true;
}
#define AudioQueueNewOutput fake_new
#define AudioQueueAllocateBuffer fake_allocate
#define AudioQueueEnqueueBuffer fake_enqueue
#define AudioQueueStart fake_start
#define AudioQueueDispose fake_dispose
#define obs_get_audio_info fake_info
#include "../audio-monitor-mac.c"

static void wait_starts(int target)
{
	for (int i = 0; i < 5000 && atomic_load(&starts) < target; i++) usleep(1000);
	assert(atomic_load(&starts) >= target);
}
static void test_cadence_and_fades(void)
{
	struct audio_monitor m = {.channels = 2, .capacity = 23040, .buffer_frames = 1440,
		.preroll_frames = 4320, .fade_frames = 240, .enabled = true, .accepting = true, .volume = 1};
	m.samples = calloc(m.capacity * 2, sizeof(float));
	pthread_mutex_init(&m.mutex, NULL);
	float input[1024], output[2880];
	for (size_t i = 0; i < 1024; i++) input[i] = 0.25f;
	struct obs_audio_data audio = {.frames = 1024};
	audio.data[0] = audio.data[1] = (uint8_t *)input;
	AudioQueueBuffer buffer = {.mAudioData = output};
	size_t next_input = 0, packet = 0, old_frames = 0, old_missing = 0;
	bool began = false;
	/* Ten virtual seconds: 1024-frame capture packets, 1440-frame output
	 * requests and occasional 10 ms arrival jitter. Compare with v4's
	 * immediate consumption policy using the exact same event schedule. */
	for (size_t t = 0; t < 480000; t++) {
		if (t == next_input) {
			audio_monitor_audio(&m, &audio);
			old_frames += 1024;
			packet++;
			next_input = packet * 1024 + (packet % 7 == 0 ? 480 : 0);
		}
		if (t % 1440 == 0) {
			if (old_frames < 1440) { old_missing += 1440 - old_frames; old_frames = 0; }
			else old_frames -= 1440;
			fill_buffer(&m, &buffer);
			if (m.playing) {
				if (!began) {
					assert(output[0] == 0.0f);
					for (size_t i = 1; i < 240; i++) assert(output[2*i] >= output[2*(i-1)]);
				}
				for (size_t i = began ? 0 : 240; i < 1440; i++) {
					assert(output[2*i] == 0.25f && output[2*i+1] == 0.25f);
				}
				began = true;
			} else assert(!began);
		}
	}
	assert(began && m.underruns == 0 && old_missing > 0);
	printf("Cadence: v4 policy inserted %zu silent frames; v5 has zero underruns after preroll\n", old_missing);
	/* Source stops: drain reserve, then decay the last sample to zero. */
	while (m.frames >= 1440) fill_buffer(&m, &buffer);
	fill_buffer(&m, &buffer);
	assert(!m.playing && output[2878] == 0.0f && output[2879] == 0.0f);
	fill_buffer(&m, &buffer);
	for (size_t i = 0; i < 2880; i++) assert(output[i] == 0.0f);
	for (int i = 0; i < 5; i++) audio_monitor_audio(&m, &audio);
	fill_buffer(&m, &buffer);
	assert(m.playing && output[0] == 0.0f && output[480] == 0.25f);
	/* An underrun one frame before the boundary must also fade smoothly. */
	m.frames = 1439; m.read_frame = 0; m.fade_remaining = 0;
	for (size_t i = 0; i < 2878; i++) m.samples[i] = 0.25f;
	fill_buffer(&m, &buffer);
	for (size_t i = 1201; i < 1440; i++) assert(fabsf(output[2*i] - output[2*(i-1)]) < 0.002f);
	assert(output[2878] == 0.0f);
	/* A single short notification must not remain buffered forever. */
	m.frames = 0;
	audio_monitor_audio(&m, &audio);
	bool heard_short_clip = false;
	for (int i = 0; i < 6; i++) {
		fill_buffer(&m, &buffer);
		for (size_t j = 0; j < 2880; j++) heard_short_clip |= output[j] != 0.0f;
	}
	assert(heard_short_clip && m.frames == 0);
	pthread_mutex_destroy(&m.mutex);
	free(m.samples);
}
int main(void)
{
	producer = pthread_self();
	test_cadence_and_fades();
	/* Validate DSP, wraparound, overflow and silence without a worker. */
	struct audio_monitor ring = {.channels = 2, .capacity = 4, .buffer_frames = 4,
		.enabled = true, .accepting = true, .volume = 1.0f};
	float storage[8] = {0}, output[8] = {0};
	float left[6] = {1,2,3,4,5,6}, right[6] = {10,20,30,40,50,60};
	ring.samples = storage;
	pthread_mutex_init(&ring.mutex, NULL);
	struct obs_audio_data audio = {.frames = 6};
	audio.data[0] = (uint8_t *)left; audio.data[1] = (uint8_t *)right;
	audio_monitor_audio(&ring, &audio);
	assert(ring.frames == 4);
	AudioQueueBuffer buffer = {.mAudioData = output};
	fill_buffer(&ring, &buffer);
	assert(output[0] == 3 && output[1] == 30 && output[6] == 6 && output[7] == 60);
	fill_buffer(&ring, &buffer);
	for (int i = 0; i < 8; i++) assert(output[i] == 0);
	ring.mono = true; ring.volume = 0.5f;
	audio.frames = 2;
	audio_monitor_audio(&ring, &audio); audio_monitor_audio(&ring, &audio);
	fill_buffer(&ring, &buffer);
	assert(output[0] == 2.75f && output[1] == 2.75f && output[6] == 5.5f);
	pthread_mutex_destroy(&ring.mutex);

	struct audio_monitor *a = audio_monitor_create("default", "test", 0);
	struct audio_monitor *b = audio_monitor_create("default", "test", 0);
	assert(a && b);
	audio_monitor_start(a); audio_monitor_start(b);
	wait_starts(2);
	for (int i = 0; i < 20; i++) {
		int before = atomic_load(&starts);
		audio_monitor_stop(a); audio_monitor_start(a);
		wait_starts(before + 1);
		/* Producer remains available while fake Start sleeps for 50 ms. */
		for (int j = 0; j < 100; j++) {
			audio_monitor_audio(a, &audio); audio_monitor_audio(b, &audio);
		}
	}
	audio_monitor_destroy(a); audio_monitor_destroy(b);
	assert(atomic_load(&live_queues) == 0);
	assert(atomic_load(&wrong_thread) == 0);
	atomic_store(&fail_start, 1);
	int before = atomic_load(&starts);
	a = audio_monitor_create("default", "failure", 0);
	audio_monitor_start(a);
	wait_starts(before + 3);
	usleep(1200000);
	assert(atomic_load(&starts) == before + 3);
	atomic_store(&fail_start, 0);
	audio_monitor_stop(a); audio_monitor_start(a);
	wait_starts(before + 4);
	audio_monitor_destroy(a);
	assert(atomic_load(&live_queues) == 0);
	assert(atomic_load(&wrong_thread) == 0);
	printf("PASS: DSP/overflow/silence, two outputs, 20 reactivations, reentrant callbacks, bounded retries, cleanup, worker-only APIs\n");
}
