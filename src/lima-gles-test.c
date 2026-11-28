/* SPDX-License-Identifier: GPL-2.0-only */
/* Offscreen GLES 2.0 texture rendering and readback on the Lima render node. */
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 640, HEIGHT = 480, FRAMES = 60 };

static void require(int good, const char *stage)
{
	if (good)
		return;
	fprintf(stderr, "%s failed (EGL 0x%x GL 0x%x)\n", stage, eglGetError(), glGetError());
	exit(1);
}

static GLuint shader(GLenum type, const char *source)
{
	GLint compiled;
	GLuint object = glCreateShader(type);
	char log[1024];

	glShaderSource(object, 1, &source, NULL);
	glCompileShader(object);
	glGetShaderiv(object, GL_COMPILE_STATUS, &compiled);
	if (!compiled) {
		glGetShaderInfoLog(object, sizeof(log), NULL, log);
		fprintf(stderr, "%s\n", log);
	}
	require(compiled, "shader compilation");
	return object;
}

static EGLDisplay display_for_node(void)
{
	PFNEGLQUERYDEVICESEXTPROC query_devices =
		(PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
	PFNEGLQUERYDEVICESTRINGEXTPROC query_string =
		(PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
	EGLDeviceEXT devices[16];
	EGLint count = 0;

	require(query_devices && query_string && query_devices(16, devices, &count), "eglQueryDevicesEXT");
	for (EGLint i = 0; i < count; ++i) {
		const char *node = query_string(devices[i], EGL_DRM_RENDER_NODE_FILE_EXT);

		if (node && !strcmp(node, "/dev/dri/renderD128")) {
			printf("EGL render node %s\n", node);
			return eglGetPlatformDisplay(EGL_PLATFORM_DEVICE_EXT, devices[i], NULL);
		}
	}
	return EGL_NO_DISPLAY;
}

int main(void)
{
	static const EGLint config_attrs[] = {
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE
	};
	static const EGLint pbuffer[] = {EGL_WIDTH, WIDTH, EGL_HEIGHT, HEIGHT, EGL_NONE};
	static const EGLint ctx_attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	static const GLfloat vertices[] = {-1, -1, 1, -1, 0, 1};
	static const GLubyte texel[] = {17, 85, 204, 255};
	EGLDisplay d = display_for_node();
	EGLConfig config;
	EGLint count;
	GLint linked;
	GLuint program, texture;
	GLint swap;
	const char *renderer;

	require(d != EGL_NO_DISPLAY && eglInitialize(d, NULL, NULL), "eglInitialize");
	require(eglBindAPI(EGL_OPENGL_ES_API), "eglBindAPI");
	require(eglChooseConfig(d, config_attrs, &config, 1, &count) && count == 1, "eglChooseConfig");
	EGLSurface surface = eglCreatePbufferSurface(d, config, pbuffer);
	EGLContext ctx = eglCreateContext(d, config, EGL_NO_CONTEXT, ctx_attrs);

	require(surface != EGL_NO_SURFACE && ctx != EGL_NO_CONTEXT &&
		eglMakeCurrent(d, surface, surface, ctx), "eglMakeCurrent");
	renderer = (const char *)glGetString(GL_RENDERER);
	printf("GL_RENDERER=%s GL_VERSION=%s\n", renderer, glGetString(GL_VERSION));
	require(renderer && strstr(renderer, "Mali450"), "hardware renderer");
	program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER,
		"attribute vec2 pos; varying vec2 uv; void main() {"
		"uv = pos * 0.5 + 0.5; gl_Position = vec4(pos, 0.0, 1.0); }"));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER,
		"precision mediump float; varying vec2 uv; uniform sampler2D tex; uniform float flip;"
		"void main() { vec4 c = texture2D(tex, uv); gl_FragColor = mix(c, c.bgra, flip); }"));
	glBindAttribLocation(program, 0, "pos");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	require(linked, "shader linking");
	glUseProgram(program);
	swap = glGetUniformLocation(program, "flip");
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, vertices);
	glEnableVertexAttribArray(0);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, texel);
	glViewport(0, 0, WIDTH, HEIGHT);
	glClearColor(0, 0, 0, 1);
	for (int i = 0; i < FRAMES; ++i) {
		GLubyte center[4], corner[4];

		glClear(GL_COLOR_BUFFER_BIT);
		glUniform1f(swap, i % 2);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glReadPixels(WIDTH / 2, HEIGHT / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
		glReadPixels(1, HEIGHT - 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, corner);
		require(glGetError() == GL_NO_ERROR && center[0] == texel[i % 2 ? 2 : 0] &&
			center[1] == texel[1] && center[2] == texel[i % 2 ? 0 : 2] && center[3] == 255 &&
			corner[0] == 0 && corner[1] == 0 && corner[2] == 0 && corner[3] == 255,
			"readback");
	}
	eglTerminate(d);
	printf("LIMA_GLES_PASS frames=%d size=%dx%d\n", FRAMES, WIDTH, HEIGHT);
	return 0;
}
