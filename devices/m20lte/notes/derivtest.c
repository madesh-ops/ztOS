// derivtest: are screen-space derivatives right on the M20's G71? WebRender sizes every
// anti-aliased edge (rounded corners, borders, shadows) with fwidth(); on this GPU those
// edges come out as wide blurs. Draw a full-screen quad and read back what dFdx, dFdy and
// fwidth return for a varying that grows by exactly 1 per pixel in x and 2 per pixel in y.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>

#define W 128
#define H 128

static const char *VS = "#version 300 es\nin vec2 pos; out vec2 v;\n"
	"void main(){ v = (pos*0.5+0.5) * vec2(128.0, 256.0); gl_Position = vec4(pos,0.0,1.0); }";
/* 0: dFdx(v.x) want 1, 1: dFdy(v.y) want 2, 2: fwidth(v.x) want 1, 3: fwidth(v.y) want 2 */
static const char *FS = "#version 300 es\nprecision %s float; in vec2 v; out vec4 o; uniform int which;\n"
	"void main(){ float d = which == 0 ? dFdx(v.x) : which == 1 ? dFdy(v.y) : which == 2 ? fwidth(v.x) : fwidth(v.y);"
	" o = vec4(d / 8.0, 0.0, 0.0, 1.0); }";

static GLuint build(const char *precision) {
	char fs[1024];
	snprintf(fs, sizeof(fs), FS, precision);
	GLuint p = glCreateProgram();
	const char *src[2] = { VS, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	for (int i = 0; i < 2; i++) {
		GLuint s = glCreateShader(type[i]);
		glShaderSource(s, 1, &src[i], NULL);
		glCompileShader(s);
		GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
		if (!ok) { char log[512]; glGetShaderInfoLog(s, 512, NULL, log); fprintf(stderr, "%s\n", log); exit(1); }
		glAttachShader(p, s);
	}
	glBindAttribLocation(p, 0, "pos");
	glLinkProgram(p);
	return p;
}

int main(void) {
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
	eglInitialize(d, NULL, NULL);
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint a[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
	EGLContext c = eglCreateContext(d, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, a);
	eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c);
	printf("%s\n", glGetString(GL_RENDERER));

	GLuint tex, fb;
	glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &fb); glBindFramebuffer(GL_FRAMEBUFFER, fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glViewport(0, 0, W, H);
	static const float q[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, q);
	glEnableVertexAttribArray(0);

	const char *names[4] = { "dFdx(x)  ", "dFdy(y)  ", "fwidth(x)", "fwidth(y)" };
	const float want[4] = { 1, 2, 1, 2 };
	const char *precisions[2] = { "highp", "mediump" };
	for (int pi = 0; pi < 2; pi++) {
		GLuint p = build(precisions[pi]);
		glUseProgram(p);
		for (int w = 0; w < 4; w++) {
			glUniform1i(glGetUniformLocation(p, "which"), w);
			glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			static unsigned char px[W * H * 4];
			glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);
			if (pi == 0 && w == 0) {
				printf("dFdx(x), pixels x 0..7 by rows 0..3 (want 1.00; 8.00 = 8 or more):\n");
				for (int y = 0; y < 4; y++) {
					for (int x = 0; x < 8; x++) printf(" %5.2f", px[(y * W + x) * 4] / 255.0f * 8.0f);
					printf("\n");
				}
			}
			float mn = 99, mx = -99;
			for (int i = 0; i < W * H; i++) {
				float v = px[i * 4] / 255.0f * 8.0f;
				if (v < mn) mn = v;
				if (v > mx) mx = v;
			}
			printf("%-7s %s want %.0f, got %.2f .. %.2f %s\n", precisions[pi], names[w], want[w], mn, mx,
				(mn > want[w] - 0.1f && mx < want[w] + 0.1f) ? "ok" : "WRONG");
		}
	}
	return 0;
}
