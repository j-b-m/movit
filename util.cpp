#include <epoxy/gl.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale>
#include <sstream>
#include <string>
#include <Eigen/Core>

#include "fp16.h"
#include "init.h"
#include "util.h"

#if defined(__APPLE__)
#include <OpenGL/OpenGL.h>
#elif defined(WIN32)
#include <epoxy/wgl.h>
#else
#include <epoxy/glx.h>
#include <epoxy/egl.h>
#endif

using namespace std;

namespace movit {

extern string *movit_data_directory;

void hsv2rgb(float h, float s, float v, float *r, float *g, float *b)
{
	if (h < 0.0f) {
		h += 2.0f * M_PI;
	}
	float c = v * s;
	float hp = (h * 180.0 / M_PI) / 60.0;
	float x = c * (1 - fabs(fmod(hp, 2.0f) - 1.0f));

	if (hp >= 0 && hp < 1) {
		*r = c;
		*g = x;
		*b = 0.0f;
	} else if (hp >= 1 && hp < 2) {
		*r = x;
		*g = c;
		*b = 0.0f;
	} else if (hp >= 2 && hp < 3) {
		*r = 0.0f;
		*g = c;
		*b = x;
	} else if (hp >= 3 && hp < 4) {
		*r = 0.0f;
		*g = x;
		*b = c;
	} else if (hp >= 4 && hp < 5) {
		*r = x;
		*g = 0.0f;
		*b = c;
	} else {
		*r = c;
		*g = 0.0f;
		*b = x;
	}

	float m = v - c;
	*r += m;
	*g += m;
	*b += m;
}

void hsv2rgb_normalized(float h, float s, float v, float *r, float *g, float *b)
{
	float ref_r, ref_g, ref_b;
	hsv2rgb(h, s, v, r, g, b);
	hsv2rgb(h, 0.0f, v, &ref_r, &ref_g, &ref_b);
	float lum = 0.2126 * *r + 0.7152 * *g + 0.0722 * *b;
	float ref_lum = 0.2126 * ref_r + 0.7152 * ref_g + 0.0722 * ref_b;
	if (lum > 1e-3) {
		float fac = ref_lum / lum;
		*r *= fac;
		*g *= fac;
		*b *= fac;
	}
}

string read_file(const string &filename)
{
	const string full_pathname = *movit_data_directory + "/" + filename;

	FILE *fp = fopen(full_pathname.c_str(), "r");
	if (fp == NULL) {
		perror(full_pathname.c_str());
		exit(1);
	}

	int ret = fseek(fp, 0, SEEK_END);
	if (ret == -1) {
		perror("fseek(SEEK_END)");
		exit(1);
	}

	int size = ftell(fp);

	ret = fseek(fp, 0, SEEK_SET);
	if (ret == -1) {
		perror("fseek(SEEK_SET)");
		exit(1);
	}

	string str;
	str.resize(size);
	ret = fread(&str[0], size, 1, fp);
	if (ret == -1) {
		perror("fread");
		exit(1);
	}
	if (ret == 0) {
		fprintf(stderr, "Short read when trying to read %d bytes from %s\n",
		        size, full_pathname.c_str());
		exit(1);
	}
	fclose(fp);

	return str;
}

string read_version_dependent_file(const string &base, const string &extension)
{
	if (movit_shader_model == MOVIT_GLSL_130) {
		return read_file(base + ".130." + extension);
	} else if (movit_shader_model == MOVIT_GLSL_150) {
		return read_file(base + ".150." + extension);
	} else if (movit_shader_model == MOVIT_ESSL_300) {
		return read_file(base + ".300es." + extension);
	} else {
		assert(false);
	}
}

GLuint compile_shader(const string &shader_src, GLenum type)
{
	GLuint obj = glCreateShader(type);
	const GLchar* source[] = { shader_src.data() };
	const GLint length[] = { (GLint)shader_src.size() };
	glShaderSource(obj, 1, source, length);
	glCompileShader(obj);

	GLchar info_log[4096];
	GLsizei log_length = sizeof(info_log) - 1;
	glGetShaderInfoLog(obj, log_length, &log_length, info_log);
	info_log[log_length] = 0; 
	if (strlen(info_log) > 0) {
		fprintf(stderr, "Shader compile log: %s\n", info_log);
	}

	GLint status;
	glGetShaderiv(obj, GL_COMPILE_STATUS, &status);
	if (status == GL_FALSE) {
		fprintf(stderr, "Failed to compile shader: %s\n", shader_src.c_str());
		exit(1);
	}

	return obj;
}

void print_3x3_matrix(const Eigen::Matrix3d& m)
{
	printf("%6.4f %6.4f %6.4f\n", m(0,0), m(0,1), m(0,2));
	printf("%6.4f %6.4f %6.4f\n", m(1,0), m(1,1), m(1,2));
	printf("%6.4f %6.4f %6.4f\n", m(2,0), m(2,1), m(2,2));
	printf("\n");
}

string output_glsl_mat3(const string &name, const Eigen::Matrix3d &m)
{
	// Use stringstream to be independent of the current locale in a thread-safe manner.
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(8);
	ss << scientific;
	ss << "const mat3 " << name << " = mat3(\n";
	ss << "    " << m(0,0) << ", " << m(1,0) << ", " << m(2,0) << ",\n";
	ss << "    " << m(0,1) << ", " << m(1,1) << ", " << m(2,1) << ",\n";
	ss << "    " << m(0,2) << ", " << m(1,2) << ", " << m(2,2) << ");\n\n";
	return ss.str();
}

string output_glsl_float(const string &name, float x)
{
	// Use stringstream to be independent of the current locale in a thread-safe manner.
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(8);
	ss << scientific;
	ss << "const float " << name << " = " << x << ";\n";
	return ss.str();
}

string output_glsl_vec2(const string &name, float x, float y)
{
	// Use stringstream to be independent of the current locale in a thread-safe manner.
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(8);
	ss << scientific;
	ss << "const vec2 " << name << " = vec2(" << x << ", " << y << ");\n";
	return ss.str();
}

string output_glsl_vec3(const string &name, float x, float y, float z)
{
	// Use stringstream to be independent of the current locale in a thread-safe manner.
	stringstream ss;
	ss.imbue(locale("C"));
	ss.precision(8);
	ss << scientific;
	ss << "const vec3 " << name << " = vec3(" << x << ", " << y << ", " << z << ");\n";
	return ss.str();
}

template<class DestFloat>
void combine_two_samples(float w1, float w2, float pos1, float pos2, float num_subtexels, float inv_num_subtexels,
                         DestFloat *offset, DestFloat *total_weight, float *sum_sq_error)
{
	assert(movit_initialized);
	assert(w1 * w2 >= 0.0f);  // Should not have differing signs.
	float z;  // Normalized 0..1 between pos1 and pos2.
	if (fabs(w1 + w2) < 1e-6) {
		z = 0.5f;
	} else {
		z = w2 / (w1 + w2);
	}

	// Round to the desired precision. Note that this might take z outside the 0..1 range.
	*offset = from_fp32<DestFloat>(pos1 + z * (pos2 - pos1));
	z = (to_fp32(*offset) - pos1) / (pos2 - pos1);

	// Round to the minimum number of bits we have measured earlier.
	// The card will do this for us anyway, but if we know what the real z
	// is, we can pick a better total_weight below.
	z = lrintf(z * num_subtexels) * inv_num_subtexels;
	
	// Choose total weight w so that we minimize total squared error
	// for the effective weights:
	//
	//   e = (w(1-z) - a)² + (wz - b)²
	//
	// Differentiating by w and setting equal to zero:
	//
	//   2(w(1-z) - a)(1-z) + 2(wz - b)z = 0
	//   w(1-z)² - a(1-z) + wz² - bz = 0
	//   w((1-z)² + z²) = a(1-z) + bz
	//   w = (a(1-z) + bz) / ((1-z)² + z²)
	//
	// If z had infinite precision, this would simply reduce to w = w1 + w2.
	*total_weight = from_fp32<DestFloat>((w1 + z * (w2 - w1)) / (z * z + (1 - z) * (1 - z)));

	if (sum_sq_error != NULL) {
		float err1 = to_fp32(*total_weight) * (1 - z) - w1;
		float err2 = to_fp32(*total_weight) * z - w2;
		*sum_sq_error = err1 * err1 + err2 * err2;
	}
}

// Explicit instantiations.
template
void combine_two_samples<float>(float w1, float w2, float pos1, float pos2, float num_subtexels, float inv_num_subtexels,
                                float *offset, float *total_weight, float *sum_sq_error);

template
void combine_two_samples<fp16_int_t>(float w1, float w2, float pos1, float pos2, float num_subtexels, float inv_num_subtexels,
                                     fp16_int_t *offset, fp16_int_t *total_weight, float *sum_sq_error);

GLuint generate_vbo(GLint size, GLenum type, GLsizeiptr data_size, const GLvoid *data)
{
	GLuint vbo;
	glGenBuffers(1, &vbo);
	check_error();
	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	check_error();
	glBufferData(GL_ARRAY_BUFFER, data_size, data, GL_STATIC_DRAW);
	check_error();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	check_error();

	return vbo;
}

GLuint fill_vertex_attribute(GLuint glsl_program_num, const string &attribute_name, GLint size, GLenum type, GLsizeiptr data_size, const GLvoid *data)
{
	int attrib = glGetAttribLocation(glsl_program_num, attribute_name.c_str());
	if (attrib == -1) {
		return -1;
	}

	GLuint vbo = generate_vbo(size, type, data_size, data);

	glBindBuffer(GL_ARRAY_BUFFER, vbo);
	check_error();
	glEnableVertexAttribArray(attrib);
	check_error();
	glVertexAttribPointer(attrib, size, type, GL_FALSE, 0, BUFFER_OFFSET(0));
	check_error();
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	check_error();

	return vbo;
}

void cleanup_vertex_attribute(GLuint glsl_program_num, const string &attribute_name, GLuint vbo)
{
	int attrib = glGetAttribLocation(glsl_program_num, attribute_name.c_str());
	if (attrib == -1) {
		return;
	}

	glDisableVertexAttribArray(attrib);
	check_error();
	glDeleteBuffers(1, &vbo);
	check_error();
}

unsigned div_round_up(unsigned a, unsigned b)
{
	return (a + b - 1) / b;
}

// Algorithm from http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2.
unsigned next_power_of_two(unsigned v)
{
	v--;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	v++;
	return v;
}

void *get_gl_context_identifier()
{
#if defined(__APPLE__)
	return (void *)CGLGetCurrentContext();
#elif defined(WIN32)
	return (void *)wglGetCurrentContext();
#else
	void *ret = (void *)eglGetCurrentContext();
	if (ret != NULL) {
		return ret;
	}
	return (void *)glXGetCurrentContext();
#endif
}

}  // namespace movit
