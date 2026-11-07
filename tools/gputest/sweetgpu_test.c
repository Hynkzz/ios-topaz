/*
 * sweetgpu_test: a program's view of SweetGPU (ginkgo, GPU 6d).
 *
 * Opens the service, speaks the msm requests as selectors the way Mesa's
 * freedreno will (iokit/Kernel/arm/SweetGPU.cpp in xnu/patches/0225):
 *   GET_PARAM (chip ID, GMEM), GEM_NEW and GEM_INFO (address), the buffers
 *   mapped into this process (client memory of the handle's type), commands
 *   written into a command buffer by this process, GEM_SUBMIT, WAIT_FENCE,
 *   the results read back here, GEM_CLOSE.
 * The commands: CP_MEM_WRITE of four words, then a 2D-engine fill of a
 * 64x64 picture (the register writes of Mesa's fd6_clear_surface for the
 * A610, as the kernel's step 13).
 *
 * Usage: sweetgpu_test [rounds]   (default 1; exit 0 when every round passed)
 */
#include <IOKit/IOKitLib.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
	kDrmGemClose = 0x09, kMsmGetParam = 0x40, kMsmGemNew = 0x42, kMsmGemInfo = 0x43,
	kMsmGemSubmit = 0x46, kMsmWaitFence = 0x47, kMsmSubmitQueueNew = 0x4a,
};
struct msm_timespec { int64_t seconds, nanoseconds; };
struct msm_param { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct msm_gem_new { uint64_t size; uint32_t flags, handle; };
struct msm_gem_info { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct msm_gem_submit_cmd { uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs; };
struct msm_gem_submit_bo { uint32_t flags, handle; uint64_t presumed; };
struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct msm_wait_fence { uint32_t fence, flags; struct msm_timespec timeout; uint32_t queueid; };
struct msm_submitqueue { uint32_t flags, prio, id; };
struct drm_gem_close { uint32_t handle, pad; };

#define MSM_INFO_GET_IOVA       1
#define MSM_SUBMIT_CMD_BUF      1
#define MSM_SUBMIT_BO_READ      1
#define MSM_SUBMIT_BO_WRITE     2

/* PM4 (Adreno 6xx) */
#define CP_WAIT_FOR_IDLE        0x26
#define CP_BLIT                 0x2c
#define CP_MEM_WRITE            0x3d
#define CP_EVENT_WRITE          0x46
#define CP_SET_MARKER           0x65
#define EVENT_FLUSH_COLOR       0x1d
#define RB_CCU_CNTL             0x8e07
#define A610_CCU_CNTL_BYPASS    (0x10000u >> 12 << 23)
#define GRAS_2D_BLIT_CNTL       0x8400
#define GRAS_2D_DST_TL          0x8405
#define GRAS_2D_DST_BR          0x8406
#define RB_2D_BLIT_CNTL         0x8c00
#define RB_2D_PIXEL_CNTL        0x8c01
#define RB_2D_DST_INFO          0x8c17
#define RB_2D_DST               0x8c18
#define RB_2D_DST_PITCH         0x8c1a
#define RB_2D_SRC_SOLID_C0      0x8c2c
#define SP_2D_DST_FORMAT        0xacc0
#define FORMAT_8888             0x30
#define BLIT_SOLID_8888         (1u << 7 | FORMAT_8888 << 8 | 0xfu << 20 | 0x10u << 24)
#define DST_BGRA_LINEAR         (FORMAT_8888 | 1u << 10)

#define PICTURE_SIZE            65536u          /* 64 x 64 pixels, rows of 256 bytes, then words for CP_MEM_WRITE */
#define PICTURE_ROW             256u
#define WRITTEN_AT              (PICTURE_ROW * 64)
#define FLUSHED_AT              (WRITTEN_AT + 64)

static io_connect_t gpu;
static uint32_t *emit, emitted;

static uint32_t
odd_parity(uint32_t v)
{
	return 0x9669 >> (0xf & (v ^ v >> 4 ^ v >> 8 ^ v >> 12 ^ v >> 16 ^ v >> 20 ^ v >> 24 ^ v >> 28)) & 1;
}

static void put(uint32_t word) { emit[emitted++] = word; }

static void
command(uint32_t opcode, uint32_t words)
{
	put(7u << 28 | words | odd_parity(words) << 15 | opcode << 16 | odd_parity(opcode) << 23);
}

static void
reg(uint32_t number, uint32_t value)
{
	put(4u << 28 | 1 | odd_parity(1) << 7 | number << 8 | odd_parity(number) << 27);
	put(value);
}

static kern_return_t
call(uint32_t selector, void *request, size_t size)
{
	size_t out = size;

	return IOConnectCallStructMethod(gpu, selector, request, size, request, &out);
}

static int
buffer(uint32_t size, uint32_t *handle, uint64_t *address, uint32_t **memory)
{
	struct msm_gem_new made = { .size = size };
	struct msm_gem_info info = { 0 };
	mach_vm_address_t at = 0;
	mach_vm_size_t mapped = 0;
	kern_return_t kr;

	printf("GEM_NEW %u bytes ...\n", size);
	if ((kr = call(kMsmGemNew, &made, sizeof(made))) != KERN_SUCCESS) {
		printf("GEM_NEW %u bytes: %#x\n", size, kr);
		return -1;
	}
	info.handle = made.handle;
	info.info = MSM_INFO_GET_IOVA;
	if ((kr = call(kMsmGemInfo, &info, sizeof(info))) != KERN_SUCCESS) {
		printf("GEM_INFO %u: %#x\n", made.handle, kr);
		return -1;
	}
	printf("buffer %u at %#llx: mapping it here ...\n", made.handle, (unsigned long long)info.value);
	if ((kr = IOConnectMapMemory64(gpu, made.handle, mach_task_self(), &at, &mapped, kIOMapAnywhere)) != KERN_SUCCESS) {
		printf("map buffer %u: %#x\n", made.handle, kr);
		return -1;
	}
	*handle = made.handle;
	*address = info.value;
	*memory = (uint32_t *)(uintptr_t)at;
	printf("buffer %u: %u bytes at GPU address %#llx, here at %p (%llu bytes mapped)\n", made.handle, size,
	    (unsigned long long)info.value, *memory, (unsigned long long)mapped);
	return 0;
}

static int
round_once(int round)
{
	uint32_t pic_handle, cmd_handle, *pic, fence;
	uint64_t pic_at, cmd_at, ns;
	struct msm_gem_submit_bo bos[2];
	struct msm_gem_submit_cmd cmd = { 0 };
	struct msm_gem_submit submit = { 0 };
	struct msm_wait_fence wait = { 0 };
	struct drm_gem_close closing = { 0 };
	struct timespec began, ended;
	kern_return_t kr;
	int bad = 0;

	if (buffer(PICTURE_SIZE, &pic_handle, &pic_at, &pic) || buffer(4096, &cmd_handle, &cmd_at, &emit)) {
		return 1;
	}
	emitted = 0;
	/* four words, by the command processor */
	command(CP_MEM_WRITE, 2 + 4);
	put((uint32_t)(pic_at + WRITTEN_AT));
	put((uint32_t)((pic_at + WRITTEN_AT) >> 32));
	put(0x600dc0de);
	put(0x5ee6f00d);
	put((uint32_t)round);
	put(0xfeedface);
	/* a 64 x 64 fill, by the 2D engine (B 30 G 90 R e0 A ff) */
	{
		static const uint32_t gen1_magic[][2] = {
			{ 0x9804, 0xf }, { 0x9805, 0 }, { 0xa0f8, 0 }, { 0xb600, 0 }, { 0x8600, 0 }, { 0xae03, 0 }, { 0xae00, 0 },
			{ 0xbe04, 0 }, { 0x9600, 0 }, { 0x0e12, 0x10000000 }, { 0x8e01, 0x1 }, { 0x8e04, 0x04100000 },
		};

		for (unsigned i = 0; i < sizeof(gen1_magic) / sizeof(gen1_magic[0]); i++) {
			reg(gen1_magic[i][0], gen1_magic[i][1]);
		}
	}
	command(CP_SET_MARKER, 1);
	put(12);
	reg(RB_CCU_CNTL, A610_CCU_CNTL_BYPASS | 1);
	reg(GRAS_2D_DST_TL, 0);
	reg(GRAS_2D_DST_BR, 63 | 63u << 16);
	reg(RB_2D_SRC_SOLID_C0, 0xe0);
	reg(RB_2D_SRC_SOLID_C0 + 1, 0x90);
	reg(RB_2D_SRC_SOLID_C0 + 2, 0x30);
	reg(RB_2D_SRC_SOLID_C0 + 3, 255);
	reg(RB_2D_BLIT_CNTL, BLIT_SOLID_8888);
	reg(GRAS_2D_BLIT_CNTL, BLIT_SOLID_8888);
	reg(SP_2D_DST_FORMAT, 1 | FORMAT_8888 << 3 | 0xfu << 12);
	reg(RB_2D_PIXEL_CNTL, 0);
	reg(RB_2D_DST_INFO, DST_BGRA_LINEAR);
	reg(RB_2D_DST, (uint32_t)pic_at);
	reg(RB_2D_DST + 1, (uint32_t)(pic_at >> 32));
	reg(RB_2D_DST_PITCH, PICTURE_ROW >> 6);
	command(CP_WAIT_FOR_IDLE, 0);
	command(CP_BLIT, 1);
	put(3);
	/* PC_CCU_FLUSH_COLOR_TS: a timestamp event, so it takes where to write when done (as the kernel's ring_2d_end) */
	command(CP_EVENT_WRITE, 4);
	put(EVENT_FLUSH_COLOR);
	put((uint32_t)(pic_at + FLUSHED_AT));
	put((uint32_t)((pic_at + FLUSHED_AT) >> 32));
	put(0x0f1a5bed);

	bos[0] = (struct msm_gem_submit_bo){ .flags = MSM_SUBMIT_BO_READ | MSM_SUBMIT_BO_WRITE, .handle = pic_handle };
	bos[1] = (struct msm_gem_submit_bo){ .flags = MSM_SUBMIT_BO_READ, .handle = cmd_handle };
	cmd.type = MSM_SUBMIT_CMD_BUF;
	cmd.submit_idx = 1;
	cmd.size = 4 * emitted;
	submit.nr_bos = 2;
	submit.nr_cmds = 1;
	submit.bos = (uint64_t)(uintptr_t)bos;
	submit.cmds = (uint64_t)(uintptr_t)&cmd;
	printf("commands written (%u words): submitting ...\n", emitted);
	clock_gettime(CLOCK_MONOTONIC_RAW, &began);
	if ((kr = call(kMsmGemSubmit, &submit, sizeof(submit))) != KERN_SUCCESS) {
		printf("GEM_SUBMIT: %#x\n", kr);
		return 1;
	}
	fence = submit.fence;
	ns = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + 2000000000ull;      /* = mach_continuous_time in ns */
	wait.fence = fence;
	wait.timeout.seconds = (int64_t)(ns / 1000000000ull);
	wait.timeout.nanoseconds = (int64_t)(ns % 1000000000ull);
	kr = call(kMsmWaitFence, &wait, sizeof(wait));
	clock_gettime(CLOCK_MONOTONIC_RAW, &ended);
	printf("submission %u (%u words): WAIT_FENCE %#x after %ld us\n", fence, emitted, kr,
	    (long)((ended.tv_sec - began.tv_sec) * 1000000 + (ended.tv_nsec - began.tv_nsec) / 1000));
	if (kr != KERN_SUCCESS) {
		return 1;
	}
	printf("words: %#x %#x %#x %#x (told 0x600dc0de 0x5ee6f00d %#x 0xfeedface)\n", pic[WRITTEN_AT / 4],
	    pic[WRITTEN_AT / 4 + 1], pic[WRITTEN_AT / 4 + 2], pic[WRITTEN_AT / 4 + 3], round);
	printf("flush word %#x (told 0xf1a5bed)\n", pic[FLUSHED_AT / 4]);
	printf("pixels: first %#x, middle %#x, last %#x (told 0xffe09030)\n", pic[0], pic[32 * 64 + 32], pic[64 * 64 - 1]);
	bad |= pic[WRITTEN_AT / 4] != 0x600dc0de || pic[WRITTEN_AT / 4 + 2] != (uint32_t)round;
	bad |= pic[0] != 0xffe09030 || pic[32 * 64 + 32] != 0xffe09030 || pic[64 * 64 - 1] != 0xffe09030;
	IOConnectUnmapMemory64(gpu, pic_handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)pic);
	IOConnectUnmapMemory64(gpu, cmd_handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)emit);
	printf("giving the buffers back ...\n");
	closing.handle = pic_handle;
	kr = call(kDrmGemClose, &closing, sizeof(closing));
	closing.handle = cmd_handle;
	kr |= call(kDrmGemClose, &closing, sizeof(closing));
	if (kr != KERN_SUCCESS) {
		printf("GEM_CLOSE: %#x\n", kr);
		bad = 1;
	}
	return bad;
}

int
main(int argc, char **argv)
{
	int rounds = argc > 1 ? atoi(argv[1]) : 1, failed = 0;
	io_service_t service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("SweetGPU"));
	struct msm_param param = { .pipe = 1 };
	struct msm_submitqueue queue = { 0 };
	kern_return_t kr;

	setvbuf(stdout, NULL, _IONBF, 0);       /* every line out before a panic can take it */
	if (service == IO_OBJECT_NULL) {
		printf("no SweetGPU service (boot-arg sweetgpu=1 or 2 on ginkgo?)\n");
		return 2;
	}
	if ((kr = IOServiceOpen(service, mach_task_self(), 0, &gpu)) != KERN_SUCCESS) {
		printf("IOServiceOpen: %#x (the GPU not kept on yet? sweetgpu=2 waits 20 s, or sysctl kern.sweetgpu=15)\n", kr);
		return 2;
	}
	for (uint32_t p = 2; p <= 6; p++) {
		param.param = p;
		param.value = 0;
		kr = call(kMsmGetParam, &param, sizeof(param));
		printf("GET_PARAM %u: %#x -> %#llx\n", p, kr, (unsigned long long)param.value);
	}
	kr = call(kMsmSubmitQueueNew, &queue, sizeof(queue));
	printf("SUBMITQUEUE_NEW: %#x -> %u\n", kr, queue.id);
	for (int i = 1; i <= rounds; i++) {
		int bad = round_once(i);

		printf("round %d: %s\n", i, bad ? "FAILED" : "passed");
		failed += bad;
	}
	IOServiceClose(gpu);
	printf(failed ? "SWEETGPU TEST FAILED (%d of %d rounds)\n" : "SWEETGPU TEST PASSED (%d failed of %d rounds)\n", failed, rounds);
	return failed ? 1 : 0;
}
