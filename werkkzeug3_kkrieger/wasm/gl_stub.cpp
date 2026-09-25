// GENERATED: no-op GL for the headless (node) build. See wasm/build_headless.sh.
#include <GLES3/gl3.h>
#include <string.h>
extern "C" {
// Sanitizer options: emscripten does not forward ASAN_OPTIONS from the host
// environment, so set them here. Keep going after the first report and skip
// leak detection (scanning a 1 GB heap in wasm takes minutes).
const char *__asan_default_options() { return "halt_on_error=0:detect_leaks=0:print_stats=0"; }
void glActiveTexture(GLenum texture) {}
void glAttachShader(GLuint program, GLuint shader) {}
void glBindAttribLocation(GLuint program, GLuint index, const GLchar *name) {}
void glBindBuffer(GLenum target, GLuint buffer) {}
void glBindFramebuffer(GLenum target, GLuint framebuffer) {}
void glBindRenderbuffer(GLenum target, GLuint renderbuffer) {}
void glBindTexture(GLenum target, GLuint texture) {}
void glBindVertexArray(GLuint array) {}
void glBlendFunc(GLenum sfactor, GLenum dfactor) {}
void glBufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage) {}
void glClear(GLbitfield mask) {}
void glClearColor(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) {}
void glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {}
void glCompileShader(GLuint shader) {}
GLuint glCreateProgram(void) {
  static unsigned next = 1; return next++;
}
GLuint glCreateShader(GLenum type) {
  static unsigned next = 1; return next++;
}
void glCullFace(GLenum mode) {}
void glDeleteBuffers(GLsizei n, const GLuint *buffers) {}
void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) {}
void glDeleteProgram(GLuint program) {}
void glDeleteRenderbuffers(GLsizei n, const GLuint *renderbuffers) {}
void glDeleteShader(GLuint shader) {}
void glDeleteTextures(GLsizei n, const GLuint *textures) {}
void glDeleteVertexArrays(GLsizei n, const GLuint *arrays) {}
void glDepthFunc(GLenum func) {}
void glDepthMask(GLboolean flag) {}
void glDisable(GLenum cap) {}
void glDisableVertexAttribArray(GLuint index) {}
void glDrawArrays(GLenum mode, GLint first, GLsizei count) {}
void glDrawElements(GLenum mode, GLsizei count, GLenum type, const void *indices) {}
void glEnable(GLenum cap) {}
void glEnableVertexAttribArray(GLuint index) {}
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {}
void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {}
void glFrontFace(GLenum mode) {}
void glGenBuffers(GLsizei n, GLuint *buffers) {
  static unsigned next = 1; for(int i=0;i<n;i++) buffers[i] = next++;
}
void glGenFramebuffers(GLsizei n, GLuint *framebuffers) {
  static unsigned next = 1; for(int i=0;i<n;i++) framebuffers[i] = next++;
}
void glGenRenderbuffers(GLsizei n, GLuint *renderbuffers) {
  static unsigned next = 1; for(int i=0;i<n;i++) renderbuffers[i] = next++;
}
void glGenTextures(GLsizei n, GLuint *textures) {
  static unsigned next = 1; for(int i=0;i<n;i++) textures[i] = next++;
}
void glGenVertexArrays(GLsizei n, GLuint *arrays) {
  static unsigned next = 1; for(int i=0;i<n;i++) arrays[i] = next++;
}
void glGenerateMipmap(GLenum target) {}
GLenum glGetError(void) {
  return 0;
}
void glGetIntegerv(GLenum pname, GLint *data) {
  if(data) { data[0]=0; data[1]=0; data[2]=1024; data[3]=576; }
}
void glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
  if(length) *length = 0;
}
void glGetProgramiv(GLuint program, GLenum pname, GLint *params) {
  if(params) *params = 1;
}
void glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog) {
  if(length) *length = 0;
}
void glGetShaderiv(GLuint shader, GLenum pname, GLint *params) {
  if(params) *params = 1;
}
GLint glGetUniformLocation(GLuint program, const GLchar *name) {
  return 0;
}
void glLinkProgram(GLuint program) {}
void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels) {
  if(pixels) memset(pixels,0,(size_t)width*height*4);
}
void glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width, GLsizei height) {}
void glScissor(GLint x, GLint y, GLsizei width, GLsizei height) {}
void glShaderSource(GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length) {}
void glStencilFunc(GLenum func, GLint ref, GLuint mask) {}
void glStencilMask(GLuint mask) {}
void glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) {}
void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels) {}
void glTexImage3D(GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels) {}
void glTexParameteri(GLenum target, GLenum pname, GLint param) {}
void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels) {}
void glUniform1i(GLint location, GLint v0) {}
void glUniform4fv(GLint location, GLsizei count, const GLfloat *value) {}
void glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat *value) {}
void glUseProgram(GLuint program) {}
void glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer) {}
void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {}
}
