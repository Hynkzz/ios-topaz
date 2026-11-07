/*
 * eagl_test: iOS's own OpenGL ES entry points reaching Mesa through libsweet-gl.dylib (GPU 7.4a).
 *
 * What CoreAnimation does, in a program of its own: an EAGLContext of API 2 made and made current
 * with iOS's OpenGLES.framework, then glClearColor/glClear/glReadPixels called as iOS's functions,
 * which jump through the context's table (eagl.c, struct eagl). Run with
 *
 *   DYLD_INSERT_LIBRARIES=/var/sweet/lib/libsweet-gl.dylib /var/root/eagl_test
 *
 * Without the library iOS's EAGLContext has no accelerator here and gives no context.
 */
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern void glClearColor(float, float, float, float);
extern void glClear(unsigned);
extern void glReadPixels(int, int, int, int, unsigned, unsigned, void *);
extern unsigned glGetError(void);
extern const unsigned char *glGetString(unsigned);
extern void glFinish(void);

#define GL_COLOR_BUFFER_BIT     0x4000
#define GL_RGBA                 0x1908
#define GL_UNSIGNED_BYTE        0x1401
#define GL_RENDERER             0x1f01
#define GL_VERSION              0x1f02

int
main(void)
{
	Class class = objc_getClass("EAGLContext");
	id context, current;
	BOOL made;
	uint8_t pixel[4] = { 0 };
	const unsigned char *renderer;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (class == Nil) {
		printf("no EAGLContext class (OpenGLES not loaded)\n");
		return 2;
	}
	context = ((id (*)(id, SEL))objc_msgSend)((id)class, sel_registerName("alloc"));
	context = ((id (*)(id, SEL, unsigned long))objc_msgSend)(context, sel_registerName("initWithAPI:"), 2);
	if (context == nil) {
		printf("-[EAGLContext initWithAPI:2]: nil (libsweet-gl.dylib not inserted?)\n");
		return 1;
	}
	made = ((BOOL (*)(id, SEL, id))objc_msgSend)((id)class, sel_registerName("setCurrentContext:"), context);
	current = ((id (*)(id, SEL))objc_msgSend)((id)class, sel_registerName("currentContext"));
	printf("setCurrentContext: %d; currentContext %s the context\n", made, current == context ? "is" : "is NOT");
	renderer = glGetString(GL_RENDERER);
	printf("through iOS's glGetString: renderer \"%s\", version \"%s\"\n", renderer ? (const char *)renderer : "(none)",
	    glGetString(GL_VERSION) ? (const char *)glGetString(GL_VERSION) : "(none)");
	glClearColor(1.0f, 0.5f, 0.25f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
	printf("a cleared pixel: %02x %02x %02x %02x (told ff 80 40 ff); GL error %#x\n", pixel[0], pixel[1], pixel[2],
	    pixel[3], glGetError());
	((BOOL (*)(id, SEL, id))objc_msgSend)((id)class, sel_registerName("setCurrentContext:"), nil);
	if (renderer == NULL || pixel[0] < 0xfd || pixel[3] != 0xff || pixel[1] < 0x7e || pixel[1] > 0x81) {
		printf("EAGL TEST FAILED\n");
		return 1;
	}
	printf("EAGL TEST PASSED\n");
	return 0;
}
