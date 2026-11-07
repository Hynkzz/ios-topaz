/*
 * gl_triangle: one triangle through Mesa (GPU 7.1, ginkgo).
 *
 * Loads the libOSMesa.8.dylib that .github/workflows/mesa.yml builds
 * (freedreno over the kernel's SweetGPU service, and softpipe), makes an
 * OpenGL ES 2 context of OSMesa's (tools/mesa/osmesa-es-profile.patch) into a
 * picture in this process's memory, clears it to blue, draws one triangle
 * of red, green and blue corners, and reads pixels back. It says which
 * driver drew and how long it took, so that nothing need be seen to be
 * believed. With OSMESA_SOFTWARE=1 in the environment Mesa draws with
 * softpipe instead (the path to compare with).
 *
 *   gl_triangle [LIBRARY [FRAMES [SIZE]]]   default /var/root/libOSMesa.8.dylib, 1 frame, 256 pixels square
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OSMESA_FORMAT                   0x22
#define OSMESA_DEPTH_BITS               0x30
#define OSMESA_STENCIL_BITS             0x31
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_CONTEXT_MINOR_VERSION    0x37
#define OSMESA_ES_PROFILE               0x38
#define GL_RGBA                         0x1908
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_TRIANGLES                    0x0004
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82

static void *(*OSMesaCreateContextAttribs)(const int *, void *);
static unsigned char (*OSMesaMakeCurrent)(void *, void *, unsigned, int, int);
static void (*OSMesaDestroyContext)(void *);
static const unsigned char *(*glGetString)(unsigned);
static unsigned (*glGetError)(void);
static void (*glViewport)(int, int, int, int);
static void (*glClearColor)(float, float, float, float);
static void (*glClear)(unsigned);
static unsigned (*glCreateShader)(unsigned);
static void (*glShaderSource)(unsigned, int, const char *const *, const int *);
static void (*glCompileShader)(unsigned);
static void (*glGetShaderiv)(unsigned, unsigned, int *);
static void (*glGetShaderInfoLog)(unsigned, int, int *, char *);
static unsigned (*glCreateProgram)(void);
static void (*glAttachShader)(unsigned, unsigned);
static void (*glBindAttribLocation)(unsigned, unsigned, const char *);
static void (*glLinkProgram)(unsigned);
static void (*glGetProgramiv)(unsigned, unsigned, int *);
static void (*glUseProgram)(unsigned);
static void (*glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
static void (*glEnableVertexAttribArray)(unsigned);
static void (*glDrawArrays)(unsigned, int, int);
static void (*glFinish)(void);
static void (*glReadPixels)(int, int, int, int, unsigned, unsigned, void *);

static void *library;

/* every byte of `a` within 2 of `b`'s */
static int
near(uint32_t a, uint32_t b)
{
	for (int i = 0; i < 32; i += 8) {
		int d = (int)((a >> i) & 0xff) - (int)((b >> i) & 0xff);

		if (d < -2 || d > 2) {
			return 0;
		}
	}
	return 1;
}

static int
find(void *slot, const char *name)
{
	void *f = dlsym(library, name);

	if (f == NULL) {
		printf("%s: not in the library\n", name);
		return 1;
	}
	memcpy(slot, &f, sizeof(f));
	return 0;
}

#define FIND(name) find(&name, #name)

static const char *vertex_text =
    "attribute vec2 at;\n"
    "attribute vec3 colour;\n"
    "varying vec3 c;\n"
    "void main() { c = colour; gl_Position = vec4(at, 0.0, 1.0); }\n";
static const char *fragment_text =
    "precision mediump float;\n"
    "varying vec3 c;\n"
    "void main() { gl_FragColor = vec4(c, 1.0); }\n";

static unsigned
shader(unsigned type, const char *text)
{
	unsigned s = glCreateShader(type);
	int good = 0;
	char log[512] = "";

	glShaderSource(s, 1, &text, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &good);
	if (!good) {
		glGetShaderInfoLog(s, sizeof(log), NULL, log);
		printf("%s shader does not compile: %s\n", type == GL_VERTEX_SHADER ? "vertex" : "fragment", log);
		return 0;
	}
	return s;
}

static uint64_t
now_us(void)
{
	return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000;
}

int
main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/var/root/libOSMesa.8.dylib";
	int frames = argc > 2 ? atoi(argv[2]) : 1, size = argc > 3 ? atoi(argv[3]) : 256, good = 0, bad = 0;
	const int attributes[] = {
		OSMESA_FORMAT, GL_RGBA, OSMESA_DEPTH_BITS, 0, OSMESA_STENCIL_BITS, 0, OSMESA_PROFILE, OSMESA_ES_PROFILE,
		OSMESA_CONTEXT_MAJOR_VERSION, 2, OSMESA_CONTEXT_MINOR_VERSION, 0, 0
	};
	static const float corners[] = {
		-0.8f, -0.8f, 1, 0, 0,
		0.8f, -0.8f, 0, 1, 0,
		0.0f, 0.8f, 0, 0, 1,
	};
	unsigned vs, fs, program;
	uint32_t *picture;
	uint64_t began, first = 0, all;
	void *context;

	setvbuf(stdout, NULL, _IONBF, 0);       /* every line out before a panic can take it */
	if (frames < 1 || size < 16 || size > 4096) {
		printf("usage: gl_triangle [LIBRARY [FRAMES [SIZE]]]\n");
		return 2;
	}
	began = now_us();
	if ((library = dlopen(path, RTLD_NOW | RTLD_LOCAL)) == NULL) {
		printf("dlopen %s: %s\n", path, dlerror());
		return 2;
	}
	printf("%s loaded in %llu ms\n", path, (unsigned long long)(now_us() - began) / 1000);
	if (FIND(OSMesaCreateContextAttribs) | FIND(OSMesaMakeCurrent) | FIND(OSMesaDestroyContext) | FIND(glGetString) |
	    FIND(glGetError) | FIND(glViewport) | FIND(glClearColor) | FIND(glClear) | FIND(glCreateShader) |
	    FIND(glShaderSource) | FIND(glCompileShader) | FIND(glGetShaderiv) | FIND(glGetShaderInfoLog) |
	    FIND(glCreateProgram) | FIND(glAttachShader) | FIND(glBindAttribLocation) | FIND(glLinkProgram) |
	    FIND(glGetProgramiv) | FIND(glUseProgram) | FIND(glVertexAttribPointer) | FIND(glEnableVertexAttribArray) |
	    FIND(glDrawArrays) | FIND(glFinish) | FIND(glReadPixels)) {
		return 2;
	}
	picture = calloc((size_t)size * size, 4);
	began = now_us();
	if ((context = OSMesaCreateContextAttribs(attributes, NULL)) == NULL) {
		printf("OSMesaCreateContextAttribs: no context of OpenGL ES 2\n");
		return 1;
	}
	if (!OSMesaMakeCurrent(context, picture, GL_UNSIGNED_BYTE, size, size)) {
		printf("OSMesaMakeCurrent: refused\n");
		return 1;
	}
	printf("context in %llu ms: vendor \"%s\", renderer \"%s\", version \"%s\"\n",
	    (unsigned long long)(now_us() - began) / 1000, glGetString(GL_VENDOR), glGetString(GL_RENDERER),
	    glGetString(GL_VERSION));

	began = now_us();
	if ((vs = shader(GL_VERTEX_SHADER, vertex_text)) == 0 || (fs = shader(GL_FRAGMENT_SHADER, fragment_text)) == 0) {
		return 1;
	}
	program = glCreateProgram();
	glAttachShader(program, vs);
	glAttachShader(program, fs);
	glBindAttribLocation(program, 0, "at");
	glBindAttribLocation(program, 1, "colour");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	if (!good) {
		printf("the program does not link\n");
		return 1;
	}
	glUseProgram(program);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 5 * sizeof(float), corners);
	glVertexAttribPointer(1, 3, GL_FLOAT, 0, 5 * sizeof(float), corners + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	printf("shaders compiled and linked in %llu ms\n", (unsigned long long)(now_us() - began) / 1000);

	began = now_us();
	for (int i = 0; i < frames; i++) {
		glViewport(0, 0, size, size);
		glClearColor(0.0f, 0.0f, 0.25f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish();
		if (i == 0) {
			first = now_us() - began;
		}
	}
	all = now_us() - began;
	printf("%d frame(s) of %dx%d: the first in %llu us, all in %llu us (%llu us a frame); GL error %#x\n", frames, size,
	    size, (unsigned long long)first, (unsigned long long)all, (unsigned long long)(all / frames), glGetError());

	/* OSMesa's picture: rows from the bottom; RGBA bytes, so 0xAABBGGRR as a word */
	{
		uint32_t corner = picture[0], middle = picture[(size / 2) * size + size / 2];
		uint32_t left = picture[(size / 10 + size / 50) * size + size / 10 + size / 25], top = picture[(size - 2) * size + size / 2];

		printf("pixels: corner %#010x (told 0xff400000, the clear), middle %#010x (a mix of the three), "
		    "near the red corner %#010x, above the top %#010x (the clear)\n", corner, middle, left, top);
		bad |= !near(corner, 0xff400000) || !near(top, 0xff400000);
		bad |= near(middle, 0xff400000) || (middle >> 24) != 0xff;
		bad |= (left & 0xff) < 0x80;    /* red */
	}
	OSMesaMakeCurrent(NULL, NULL, 0, 0, 0);
	OSMesaDestroyContext(context);
	printf(bad ? "GL TRIANGLE FAILED\n" : "GL TRIANGLE PASSED\n");
	return bad;
}
