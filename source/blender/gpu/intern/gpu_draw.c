/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup gpu
 *
 * Utility functions for dealing with OpenGL texture & material context,
 * mipmap generation and light objects.
 *
 * These are some obscure rendering functions shared between the game engine (not anymore)
 * and the blender, in this module to avoid duplication
 * and abstract them away from the rest a bit.
 */

#include <string.h>

#include "BLI_blenlib.h"
#include "BLI_linklist.h"
#include "BLI_math.h"
#include "BLI_threads.h"
#include "BLI_utildefines.h"

#include "DNA_light_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_smoke_types.h"
#include "DNA_view3d_types.h"
#include "DNA_particle_types.h"

#include "MEM_guardedalloc.h"

#include "IMB_imbuf.h"
#include "IMB_imbuf_types.h"

#include "BKE_colorband.h"
#include "BKE_global.h"
#include "BKE_image.h"
#include "BKE_main.h"
#include "BKE_material.h"
#include "BKE_node.h"
#include "BKE_scene.h"

#include "GPU_draw.h"
#include "GPU_extensions.h"
#include "GPU_glew.h"
#include "GPU_material.h"
#include "GPU_matrix.h"
#include "GPU_shader.h"
#include "GPU_texture.h"

#include "PIL_time.h"

#ifdef WITH_SMOKE
#  include "smoke_API.h"
#endif

static void gpu_free_image_immediate(Image *ima);

//* Checking powers of two for images since OpenGL ES requires it */
#ifdef WITH_DDS
static bool is_power_of_2_resolution(int w, int h)
{
	return is_power_of_2_i(w) && is_power_of_2_i(h);
}
#endif

static bool is_over_resolution_limit(GLenum textarget, int w, int h)
{
	int size = (textarget == GL_TEXTURE_2D) ?
	        GPU_max_texture_size() : GPU_max_cube_map_size();
	int reslimit = (U.glreslimit != 0) ?
	        min_ii(U.glreslimit, size) : size;

	return (w > reslimit || h > reslimit);
}

static int smaller_power_of_2_limit(int num)
{
	int reslimit = (U.glreslimit != 0) ?
	        min_ii(U.glreslimit, GPU_max_texture_size()) :
	        GPU_max_texture_size();
	/* take texture clamping into account */
	if (num > reslimit)
		return reslimit;

	return power_of_2_min_i(num);
}

/* Current OpenGL state caching for GPU_set_tpage */

static struct GPUTextureState {
	/* also controls min/mag filtering */
	bool domipmap;
	/* only use when 'domipmap' is set */
	bool linearmipmap;
	/* store this so that new images created while texture painting won't be set to mipmapped */
	bool texpaint;

	float anisotropic;
} GTS = {1, 0, 0, 1.0f};

/* Mipmap settings */

void GPU_set_mipmap(Main *bmain, bool mipmap)
{
	if (GTS.domipmap != mipmap) {
		GPU_free_images(bmain);
		GTS.domipmap = mipmap;
	}
}

void GPU_set_linear_mipmap(bool linear)
{
	if (GTS.linearmipmap != linear) {
		GTS.linearmipmap = linear;
	}
}

bool GPU_get_mipmap(void)
{
	return GTS.domipmap && !GTS.texpaint;
}

bool GPU_get_linear_mipmap(void)
{
	return GTS.linearmipmap;
}

static GLenum gpu_get_mipmap_filter(bool mag)
{
	/* linearmipmap is off by default *when mipmapping is off,
	 * use unfiltered display */
	if (mag) {
		if (GTS.domipmap)
			return GL_LINEAR;
		else
			return GL_NEAREST;
	}
	else {
		if (GTS.domipmap) {
			if (GTS.linearmipmap) {
				return GL_LINEAR_MIPMAP_LINEAR;
			}
			else {
				return GL_LINEAR_MIPMAP_NEAREST;
			}
		}
		else {
			return GL_NEAREST;
		}
	}
}

/* Anisotropic filtering settings */
void GPU_set_anisotropic(Main *bmain, float value)
{
	if (GTS.anisotropic != value) {
		GPU_free_images(bmain);

		/* Clamp value to the maximum value the graphics card supports */
		const float max = GPU_max_texture_anisotropy();
		if (value > max)
			value = max;

		GTS.anisotropic = value;
	}
}

float GPU_get_anisotropic(void)
{
	return GTS.anisotropic;
}

/* Set OpenGL state for an MTFace */

static GPUTexture **gpu_get_image_gputexture(Image *ima, GLenum textarget)
{
	if (textarget == GL_TEXTURE_2D)
		return &ima->gputexture[TEXTARGET_TEXTURE_2D];
	else if (textarget == GL_TEXTURE_CUBE_MAP)
		return &ima->gputexture[TEXTARGET_TEXTURE_CUBE_MAP];

	return NULL;
}

typedef struct VerifyThreadData {
	ImBuf *ibuf;
	float *srgb_frect;
} VerifyThreadData;

static void gpu_verify_high_bit_srgb_buffer_slice(
        float *srgb_frect,
        ImBuf *ibuf,
        const int start_line,
        const int height)
{
	size_t offset = ibuf->channels * start_line * ibuf->x;
	float *current_srgb_frect = srgb_frect + offset;
	float *current_rect_float = ibuf->rect_float + offset;
	IMB_buffer_float_from_float(
	        current_srgb_frect,
	        current_rect_float,
	        ibuf->channels,
	        IB_PROFILE_SRGB,
	        IB_PROFILE_LINEAR_RGB, true,
	        ibuf->x, height,
	        ibuf->x, ibuf->x);
	IMB_buffer_float_unpremultiply(current_srgb_frect, ibuf->x, height);
}

static void verify_thread_do(
        void *data_v,
        int start_scanline,
        int num_scanlines)
{
	VerifyThreadData *data = (VerifyThreadData *)data_v;
	gpu_verify_high_bit_srgb_buffer_slice(
	        data->srgb_frect,
	        data->ibuf,
	        start_scanline,
	        num_scanlines);
}

static void gpu_verify_high_bit_srgb_buffer(
        float *srgb_frect,
        ImBuf *ibuf)
{
	if (ibuf->y < 64) {
		gpu_verify_high_bit_srgb_buffer_slice(
		        srgb_frect,
		        ibuf,
		        0, ibuf->y);
	}
	else {
		VerifyThreadData data;
		data.ibuf = ibuf;
		data.srgb_frect = srgb_frect;
		IMB_processor_apply_threaded_scanlines(ibuf->y, verify_thread_do, &data);
	}
}

GPUTexture *GPU_texture_from_blender(
        Image *ima,
        ImageUser *iuser,
        int textarget,
        bool is_data)
{
	if (ima == NULL) {
		return NULL;
	}

	/* currently, gpu refresh tagging is used by ima sequences */
	if (ima->gpuflag & IMA_GPU_REFRESH) {
		gpu_free_image_immediate(ima);
		ima->gpuflag &= ~IMA_GPU_REFRESH;
	}

	/* Test if we already have a texture. */
	GPUTexture **tex = gpu_get_image_gputexture(ima, textarget);
	if (*tex) {
		return *tex;
	}

	/* Check if we have a valid image. If not, we return a dummy
	 * texture with zero bindcode so we don't keep trying. */
	uint bindcode = 0;
	if (ima->ok == 0) {
		*tex = GPU_texture_from_bindcode(textarget, bindcode);
		return *tex;
	}

	/* check if we have a valid image buffer */
	ImBuf *ibuf = BKE_image_acquire_ibuf(ima, iuser, NULL);
	if (ibuf == NULL) {
		*tex = GPU_texture_from_bindcode(textarget, bindcode);
		return *tex;
	}

	/* flag to determine whether deep format is used */
	bool use_high_bit_depth = false, do_color_management = false;

	if (ibuf->rect_float) {
		use_high_bit_depth = true;

		/* TODO unneeded when float images are correctly treated as linear always */
		if (!is_data) {
			do_color_management = true;
		}
	}

	const int rectw = ibuf->x;
	const int recth = ibuf->y;
	uint *rect = ibuf->rect;
	float *frect = NULL;
	float *srgb_frect = NULL;

	if (use_high_bit_depth) {
		if (do_color_management) {
			frect = srgb_frect = MEM_mallocN(ibuf->x * ibuf->y * sizeof(*srgb_frect) * 4, "floar_buf_col_cor");
			gpu_verify_high_bit_srgb_buffer(srgb_frect, ibuf);
		}
		else {
			frect = ibuf->rect_float;
		}
	}

	const bool mipmap = GPU_get_mipmap();

#ifdef WITH_DDS
	if (ibuf->ftype == IMB_FTYPE_DDS) {
		GPU_create_gl_tex_compressed(&bindcode, rect, rectw, recth, textarget, mipmap, ima, ibuf);
	}
	else
#endif
	{
		GPU_create_gl_tex(&bindcode, rect, frect, rectw, recth, textarget, mipmap, use_high_bit_depth, ima);
	}

	/* mark as non-color data texture */
	if (bindcode) {
		if (is_data)
			ima->gpuflag |= IMA_GPU_IS_DATA;
		else
			ima->gpuflag &= ~IMA_GPU_IS_DATA;
	}

	/* clean up */
	if (srgb_frect)
		MEM_freeN(srgb_frect);

	BKE_image_release_ibuf(ima, ibuf, NULL);

	*tex = GPU_texture_from_bindcode(textarget, bindcode);
	return *tex;
}

static void **gpu_gen_cube_map(uint *rect, float *frect, int rectw, int recth, bool use_high_bit_depth)
{
	size_t block_size = use_high_bit_depth ? sizeof(float[4]) : sizeof(uchar[4]);
	void **sides = NULL;
	int h = recth / 2;
	int w = rectw / 3;

	if ((use_high_bit_depth && frect == NULL) || (!use_high_bit_depth && rect == NULL) || w != h)
		return sides;

	/* PosX, NegX, PosY, NegY, PosZ, NegZ */
	sides = MEM_mallocN(sizeof(void *) * 6, "");
	for (int i = 0; i < 6; i++)
		sides[i] = MEM_mallocN(block_size * w * h, "");

	/* divide image into six parts */
	/* ______________________
	 * |      |      |      |
	 * | NegX | NegY | PosX |
	 * |______|______|______|
	 * |      |      |      |
	 * | NegZ | PosZ | PosY |
	 * |______|______|______|
	 */
	if (use_high_bit_depth) {
		float (*frectb)[4] = (float(*)[4])frect;
		float (**fsides)[4] = (float(**)[4])sides;

		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				memcpy(&fsides[0][x * h + y], &frectb[(recth - y - 1) * rectw + 2 * w + x], block_size);
				memcpy(&fsides[1][x * h + y], &frectb[(y + h) * rectw + w - 1 - x], block_size);
				memcpy(&fsides[3][y * w + x], &frectb[(recth - y - 1) * rectw + 2 * w - 1 - x], block_size);
				memcpy(&fsides[5][y * w + x], &frectb[(h - y - 1) * rectw + w - 1 - x], block_size);
			}
			memcpy(&fsides[2][y * w], frectb[y * rectw + 2 * w], block_size * w);
			memcpy(&fsides[4][y * w], frectb[y * rectw + w], block_size * w);
		}
	}
	else {
		uint **isides = (uint **)sides;

		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				isides[0][x * h + y] = rect[(recth - y - 1) * rectw + 2 * w + x];
				isides[1][x * h + y] = rect[(y + h) * rectw + w - 1 - x];
				isides[3][y * w + x] = rect[(recth - y - 1) * rectw + 2 * w - 1 - x];
				isides[5][y * w + x] = rect[(h - y - 1) * rectw + w - 1 - x];
			}
			memcpy(&isides[2][y * w], &rect[y * rectw + 2 * w], block_size * w);
			memcpy(&isides[4][y * w], &rect[y * rectw + w], block_size * w);
		}
	}

	return sides;
}

static void gpu_del_cube_map(void **cube_map)
{
	int i;
	if (cube_map == NULL)
		return;
	for (i = 0; i < 6; i++)
		MEM_freeN(cube_map[i]);
	MEM_freeN(cube_map);
}

/* Image *ima can be NULL */
void GPU_create_gl_tex(
        uint *bind, uint *rect, float *frect, int rectw, int recth,
        int textarget, bool mipmap, bool use_high_bit_depth, Image *ima)
{
	ImBuf *ibuf = NULL;

	/* create image */
	glGenTextures(1, (GLuint *)bind);
	glBindTexture(textarget, *bind);

	if (textarget == GL_TEXTURE_2D) {
		if (use_high_bit_depth) {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, rectw, recth, 0, GL_RGBA, GL_FLOAT, frect);
		}
		else {
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, rectw, recth, 0, GL_RGBA, GL_UNSIGNED_BYTE, rect);
		}

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gpu_get_mipmap_filter(1));

		if (GPU_get_mipmap() && mipmap) {
			glGenerateMipmap(GL_TEXTURE_2D);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gpu_get_mipmap_filter(0));
			if (ima)
				ima->gpuflag |= IMA_GPU_MIPMAP_COMPLETE;
		}
		else {
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		}
	}
	else if (textarget == GL_TEXTURE_CUBE_MAP) {
		int w = rectw / 3, h = recth / 2;

		if (h == w && is_power_of_2_i(h) && !is_over_resolution_limit(textarget, h, w)) {
			void **cube_map = gpu_gen_cube_map(rect, frect, rectw, recth, use_high_bit_depth);
			GLenum informat = use_high_bit_depth ? GL_RGBA16F : GL_RGBA8;
			GLenum type = use_high_bit_depth ? GL_FLOAT : GL_UNSIGNED_BYTE;

			if (cube_map)
				for (int i = 0; i < 6; i++)
					glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + i, 0, informat, w, h, 0, GL_RGBA, type, cube_map[i]);

			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, gpu_get_mipmap_filter(1));

			if (GPU_get_mipmap() && mipmap) {
				glGenerateMipmap(GL_TEXTURE_CUBE_MAP);
				glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, gpu_get_mipmap_filter(0));

				if (ima)
					ima->gpuflag |= IMA_GPU_MIPMAP_COMPLETE;
			}
			else {
				glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			}
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

			gpu_del_cube_map(cube_map);
		}
		else {
			printf("Incorrect envmap size\n");
		}
	}

	if (GLEW_EXT_texture_filter_anisotropic)
		glTexParameterf(textarget, GL_TEXTURE_MAX_ANISOTROPY_EXT, GPU_get_anisotropic());

	glBindTexture(textarget, 0);

	if (ibuf)
		IMB_freeImBuf(ibuf);
}

/**
 * GPU_upload_dxt_texture() assumes that the texture is already bound and ready to go.
 * This is so the viewport and the BGE can share some code.
 * Returns false if the provided ImBuf doesn't have a supported DXT compression format
 */
bool GPU_upload_dxt_texture(ImBuf *ibuf)
{
#ifdef WITH_DDS
	GLint format = 0;
	int blocksize, height, width, i, size, offset = 0;

	width = ibuf->x;
	height = ibuf->y;

	if (GLEW_EXT_texture_compression_s3tc) {
		if (ibuf->dds_data.fourcc == FOURCC_DXT1)
			format = GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
		else if (ibuf->dds_data.fourcc == FOURCC_DXT3)
			format = GL_COMPRESSED_RGBA_S3TC_DXT3_EXT;
		else if (ibuf->dds_data.fourcc == FOURCC_DXT5)
			format = GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
	}

	if (format == 0) {
		fprintf(stderr, "Unable to find a suitable DXT compression, falling back to uncompressed\n");
		return false;
	}

	if (!is_power_of_2_resolution(width, height)) {
		fprintf(stderr, "Unable to load non-power-of-two DXT image resolution, falling back to uncompressed\n");
		return false;
	}

	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gpu_get_mipmap_filter(0));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gpu_get_mipmap_filter(1));

	if (GLEW_EXT_texture_filter_anisotropic)
		glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, GPU_get_anisotropic());

	blocksize = (ibuf->dds_data.fourcc == FOURCC_DXT1) ? 8 : 16;
	for (i = 0; i < ibuf->dds_data.nummipmaps && (width || height); ++i) {
		if (width == 0)
			width = 1;
		if (height == 0)
			height = 1;

		size = ((width + 3) / 4) * ((height + 3) / 4) * blocksize;

		glCompressedTexImage2D(
		        GL_TEXTURE_2D, i, format, width, height,
		        0, size, ibuf->dds_data.data + offset);

		offset += size;
		width >>= 1;
		height >>= 1;
	}

	/* set number of mipmap levels we have, needed in case they don't go down to 1x1 */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, i - 1);

	return true;
#else
	(void)ibuf;
	return false;
#endif
}

void GPU_create_gl_tex_compressed(
        uint *bind, uint *pix, int x, int y,
        int textarget, int mipmap, Image *ima, ImBuf *ibuf)
{
#ifndef WITH_DDS
	(void)ibuf;
	/* Fall back to uncompressed if DDS isn't enabled */
	GPU_create_gl_tex(bind, pix, NULL, x, y, textarget, mipmap, 0, ima);
#else
	glGenTextures(1, (GLuint *)bind);
	glBindTexture(textarget, *bind);

	if (textarget == GL_TEXTURE_2D && GPU_upload_dxt_texture(ibuf) == 0) {
		glDeleteTextures(1, (GLuint *)bind);
		GPU_create_gl_tex(bind, pix, NULL, x, y, textarget, mipmap, 0, ima);
	}

	glBindTexture(textarget, 0);
#endif
}

/* these two functions are called on entering and exiting texture paint mode,
 * temporary disabling/enabling mipmapping on all images for quick texture
 * updates with glTexSubImage2D. images that didn't change don't have to be
 * re-uploaded to OpenGL */
void GPU_paint_set_mipmap(Main *bmain, bool mipmap)
{
	if (!GTS.domipmap)
		return;

	GTS.texpaint = !mipmap;

	if (mipmap) {
		for (Image *ima = bmain->images.first; ima; ima = ima->id.next) {
			if (BKE_image_has_opengl_texture(ima)) {
				if (ima->gpuflag & IMA_GPU_MIPMAP_COMPLETE) {
					if (ima->gputexture[TEXTARGET_TEXTURE_2D]) {
						GPU_texture_bind(ima->gputexture[TEXTARGET_TEXTURE_2D], 0);
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, gpu_get_mipmap_filter(0));
						glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gpu_get_mipmap_filter(1));
						GPU_texture_unbind(ima->gputexture[TEXTARGET_TEXTURE_2D]);
					}
				}
				else
					GPU_free_image(ima);
			}
			else
				ima->gpuflag &= ~IMA_GPU_MIPMAP_COMPLETE;
		}

	}
	else {
		for (Image *ima = bmain->images.first; ima; ima = ima->id.next) {
			if (BKE_image_has_opengl_texture(ima)) {
				if (ima->gputexture[TEXTARGET_TEXTURE_2D]) {
					GPU_texture_bind(ima->gputexture[TEXTARGET_TEXTURE_2D], 0);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
					glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, gpu_get_mipmap_filter(1));
					GPU_texture_unbind(ima->gputexture[TEXTARGET_TEXTURE_2D]);
				}
			}
			else
				ima->gpuflag &= ~IMA_GPU_MIPMAP_COMPLETE;
		}
	}
}


/* check if image has been downscaled and do scaled partial update */
static bool gpu_check_scaled_image(ImBuf *ibuf, Image *ima, float *frect, int x, int y, int w, int h)
{
	if (is_over_resolution_limit(GL_TEXTURE_2D, ibuf->x, ibuf->y)) {
		int x_limit = smaller_power_of_2_limit(ibuf->x);
		int y_limit = smaller_power_of_2_limit(ibuf->y);

		float xratio = x_limit / (float)ibuf->x;
		float yratio = y_limit / (float)ibuf->y;

		/* find new width, height and x,y gpu texture coordinates */

		/* take ceiling because we will be losing 1 pixel due to rounding errors in x,y... */
		int rectw = (int)ceil(xratio * w);
		int recth = (int)ceil(yratio * h);

		x *= xratio;
		y *= yratio;

		/* ...but take back if we are over the limit! */
		if (rectw + x > x_limit) rectw--;
		if (recth + y > y_limit) recth--;

		GPU_texture_bind(ima->gputexture[TEXTARGET_TEXTURE_2D], 0);

		/* float rectangles are already continuous in memory so we can use IMB_scaleImBuf */
		if (frect) {
			ImBuf *ibuf_scale = IMB_allocFromBuffer(NULL, frect, w, h);
			IMB_scaleImBuf(ibuf_scale, rectw, recth);

			glTexSubImage2D(
			        GL_TEXTURE_2D, 0, x, y, rectw, recth, GL_RGBA,
			        GL_FLOAT, ibuf_scale->rect_float);

			IMB_freeImBuf(ibuf_scale);
		}
		/* byte images are not continuous in memory so do manual interpolation */
		else {
			uchar *scalerect = MEM_mallocN(rectw * recth * sizeof(*scalerect) * 4, "scalerect");
			uint *p = (uint *)scalerect;
			int i, j;
			float inv_xratio = 1.0f / xratio;
			float inv_yratio = 1.0f / yratio;
			for (i = 0; i < rectw; i++) {
				float u = (x + i) * inv_xratio;
				for (j = 0; j < recth; j++) {
					float v = (y + j) * inv_yratio;
					bilinear_interpolation_color_wrap(ibuf, (uchar *)(p + i + j * (rectw)), NULL, u, v);
				}
			}

			glTexSubImage2D(
			        GL_TEXTURE_2D, 0, x, y, rectw, recth, GL_RGBA,
			        GL_UNSIGNED_BYTE, scalerect);

			MEM_freeN(scalerect);
		}

		if (GPU_get_mipmap()) {
			glGenerateMipmap(GL_TEXTURE_2D);
		}
		else {
			ima->gpuflag &= ~IMA_GPU_MIPMAP_COMPLETE;
		}

		GPU_texture_unbind(ima->gputexture[TEXTARGET_TEXTURE_2D]);

		return true;
	}

	return false;
}

void GPU_paint_update_image(Image *ima, ImageUser *iuser, int x, int y, int w, int h)
{
	ImBuf *ibuf = BKE_image_acquire_ibuf(ima, iuser, NULL);

	if ((ima->gputexture[TEXTARGET_TEXTURE_2D] == NULL) ||
	    (ibuf == NULL) ||
	    (w == 0) || (h == 0))
	{
		/* these cases require full reload still */
		GPU_free_image(ima);
	}
	else {
		/* for the special case, we can do a partial update
		 * which is much quicker for painting */
		GLint row_length, skip_pixels, skip_rows;

		/* if color correction is needed, we must update the part that needs updating. */
		if (ibuf->rect_float) {
			float *buffer = MEM_mallocN(w * h * sizeof(float) * 4, "temp_texpaint_float_buf");
			bool is_data = (ima->gpuflag & IMA_GPU_IS_DATA) != 0;
			IMB_partial_rect_from_float(ibuf, buffer, x, y, w, h, is_data);

			if (gpu_check_scaled_image(ibuf, ima, buffer, x, y, w, h)) {
				MEM_freeN(buffer);
				BKE_image_release_ibuf(ima, ibuf, NULL);
				return;
			}

			GPU_texture_bind(ima->gputexture[TEXTARGET_TEXTURE_2D], 0);
			glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_FLOAT, buffer);

			MEM_freeN(buffer);

			if (GPU_get_mipmap()) {
				glGenerateMipmap(GL_TEXTURE_2D);
			}
			else {
				ima->gpuflag &= ~IMA_GPU_MIPMAP_COMPLETE;
			}

			GPU_texture_unbind(ima->gputexture[TEXTARGET_TEXTURE_2D]);

			BKE_image_release_ibuf(ima, ibuf, NULL);
			return;
		}

		if (gpu_check_scaled_image(ibuf, ima, NULL, x, y, w, h)) {
			BKE_image_release_ibuf(ima, ibuf, NULL);
			return;
		}

		GPU_texture_bind(ima->gputexture[TEXTARGET_TEXTURE_2D], 0);

		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &row_length);
		glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skip_pixels);
		glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skip_rows);

		glPixelStorei(GL_UNPACK_ROW_LENGTH, ibuf->x);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, x);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, y);

		glTexSubImage2D(
		        GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA,
		        GL_UNSIGNED_BYTE, ibuf->rect);

		glPixelStorei(GL_UNPACK_ROW_LENGTH, row_length);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, skip_pixels);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, skip_rows);

		/* see comment above as to why we are using gpu mipmap generation here */
		if (GPU_get_mipmap()) {
			glGenerateMipmap(GL_TEXTURE_2D);
		}
		else {
			ima->gpuflag &= ~IMA_GPU_MIPMAP_COMPLETE;
		}

		GPU_texture_unbind(ima->gputexture[TEXTARGET_TEXTURE_2D]);
	}

	BKE_image_release_ibuf(ima, ibuf, NULL);
}

/* *************************** Transfer functions *************************** */

enum {
	TFUNC_FLAME_SPECTRUM = 0,
	TFUNC_COLOR_RAMP     = 1,
};

#define TFUNC_WIDTH 256

#ifdef WITH_SMOKE
static void create_flame_spectrum_texture(float *data)
{
#define FIRE_THRESH 7
#define MAX_FIRE_ALPHA 0.06f
#define FULL_ON_FIRE 100

	float *spec_pixels = MEM_mallocN(TFUNC_WIDTH * 4 * 16 * 16 * sizeof(float), "spec_pixels");

	blackbody_temperature_to_rgb_table(data, TFUNC_WIDTH, 1500, 3000);

	for (int i = 0; i < 16; i++) {
		for (int j = 0; j < 16; j++) {
			for (int k = 0; k < TFUNC_WIDTH; k++) {
				int index = (j * TFUNC_WIDTH * 16 + i * TFUNC_WIDTH + k) * 4;
				if (k >= FIRE_THRESH) {
					spec_pixels[index] = (data[k * 4]);
					spec_pixels[index + 1] = (data[k * 4 + 1]);
					spec_pixels[index + 2] = (data[k * 4 + 2]);
					spec_pixels[index + 3] = MAX_FIRE_ALPHA * (
					        (k > FULL_ON_FIRE) ? 1.0f : (k - FIRE_THRESH) / ((float)FULL_ON_FIRE - FIRE_THRESH));
				}
				else {
					zero_v4(&spec_pixels[index]);
				}
			}
		}
	}

	memcpy(data, spec_pixels, sizeof(float) * 4 * TFUNC_WIDTH);

	MEM_freeN(spec_pixels);

#undef FIRE_THRESH
#undef MAX_FIRE_ALPHA
#undef FULL_ON_FIRE
}

static void create_color_ramp(const ColorBand *coba, float *data)
{
	for (int i = 0; i < TFUNC_WIDTH; i++) {
		BKE_colorband_evaluate(coba, (float)i / TFUNC_WIDTH, &data[i * 4]);
	}
}

static GPUTexture *create_transfer_function(int type, const ColorBand *coba)
{
	float *data = MEM_mallocN(sizeof(float) * 4 * TFUNC_WIDTH, __func__);

	switch (type) {
		case TFUNC_FLAME_SPECTRUM:
			create_flame_spectrum_texture(data);
			break;
		case TFUNC_COLOR_RAMP:
			create_color_ramp(coba, data);
			break;
	}

	GPUTexture *tex = GPU_texture_create_1D(TFUNC_WIDTH, GPU_RGBA8, data, NULL);

	MEM_freeN(data);

	return tex;
}

static void swizzle_texture_channel_rrrr(GPUTexture *tex)
{
	GPU_texture_bind(tex, 0);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_SWIZZLE_R, GL_RED);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_SWIZZLE_G, GL_RED);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_SWIZZLE_B, GL_RED);
	glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_SWIZZLE_A, GL_RED);
	GPU_texture_unbind(tex);
}

static GPUTexture *create_field_texture(SmokeDomainSettings *sds)
{
	float *field = NULL;

	switch (sds->coba_field) {
		case FLUID_FIELD_DENSITY:    field = smoke_get_density(sds->fluid); break;
		case FLUID_FIELD_HEAT:       field = smoke_get_heat(sds->fluid); break;
		case FLUID_FIELD_FUEL:       field = smoke_get_fuel(sds->fluid); break;
		case FLUID_FIELD_REACT:      field = smoke_get_react(sds->fluid); break;
		case FLUID_FIELD_FLAME:      field = smoke_get_flame(sds->fluid); break;
		case FLUID_FIELD_VELOCITY_X: field = smoke_get_velocity_x(sds->fluid); break;
		case FLUID_FIELD_VELOCITY_Y: field = smoke_get_velocity_y(sds->fluid); break;
		case FLUID_FIELD_VELOCITY_Z: field = smoke_get_velocity_z(sds->fluid); break;
		case FLUID_FIELD_COLOR_R:    field = smoke_get_color_r(sds->fluid); break;
		case FLUID_FIELD_COLOR_G:    field = smoke_get_color_g(sds->fluid); break;
		case FLUID_FIELD_COLOR_B:    field = smoke_get_color_b(sds->fluid); break;
		case FLUID_FIELD_FORCE_X:    field = smoke_get_force_x(sds->fluid); break;
		case FLUID_FIELD_FORCE_Y:    field = smoke_get_force_y(sds->fluid); break;
		case FLUID_FIELD_FORCE_Z:    field = smoke_get_force_z(sds->fluid); break;
		default: return NULL;
	}

	GPUTexture *tex = GPU_texture_create_nD(
	               sds->res[0], sds->res[1], sds->res[2], 3,
	               field, GPU_R8, GPU_DATA_FLOAT, 0, true, NULL);

	swizzle_texture_channel_rrrr(tex);
	return tex;
}

static GPUTexture *create_density_texture(SmokeDomainSettings *sds, int highres)
{
	float *data = NULL, *source;
	int cell_count = (highres) ? smoke_turbulence_get_cells(sds->wt) : sds->total_cells;
	const bool has_color = (highres) ? smoke_turbulence_has_colors(sds->wt) : smoke_has_colors(sds->fluid);
	int *dim = (highres) ? sds->res_wt : sds->res;
	eGPUTextureFormat format = (has_color) ? GPU_RGBA8 : GPU_R8;

	if (has_color) {
		data = MEM_callocN(sizeof(float) * cell_count * 4, "smokeColorTexture");
	}

	if (highres) {
		if (has_color) {
			smoke_turbulence_get_rgba(sds->wt, data, 0);
		}
		else {
			source = smoke_turbulence_get_density(sds->wt);
		}
	}
	else {
		if (has_color) {
			smoke_get_rgba(sds->fluid, data, 0);
		}
		else {
			source = smoke_get_density(sds->fluid);
		}
	}

	GPUTexture *tex = GPU_texture_create_nD(
	               dim[0], dim[1], dim[2], 3,
	               (has_color) ? data : source,
	               format, GPU_DATA_FLOAT, 0, true, NULL);
	if (data) {
		MEM_freeN(data);
	}

	if (format == GPU_R8) {
		/* Swizzle the RGBA components to read the Red channel so
		 * that the shader stay the same for colored and non color
		 * density textures. */
		swizzle_texture_channel_rrrr(tex);
	}
	return tex;
}

static GPUTexture *create_flame_texture(SmokeDomainSettings *sds, int highres)
{
	float *source = NULL;
	const bool has_fuel = (highres) ? smoke_turbulence_has_fuel(sds->wt) : smoke_has_fuel(sds->fluid);
	int *dim = (highres) ? sds->res_wt : sds->res;

	if (!has_fuel)
		return NULL;

	if (highres) {
		source = smoke_turbulence_get_flame(sds->wt);
	}
	else {
		source = smoke_get_flame(sds->fluid);
	}

	GPUTexture *tex = GPU_texture_create_nD(
	               dim[0], dim[1], dim[2], 3,
	               source, GPU_R8, GPU_DATA_FLOAT, 0, true, NULL);

	swizzle_texture_channel_rrrr(tex);

	return tex;
}
#endif  /* WITH_SMOKE */

void GPU_free_smoke(SmokeModifierData *smd)
{
	if (smd->type & MOD_SMOKE_TYPE_DOMAIN && smd->domain) {
		if (smd->domain->tex)
			GPU_texture_free(smd->domain->tex);
		smd->domain->tex = NULL;

		if (smd->domain->tex_shadow)
			GPU_texture_free(smd->domain->tex_shadow);
		smd->domain->tex_shadow = NULL;

		if (smd->domain->tex_flame)
			GPU_texture_free(smd->domain->tex_flame);
		smd->domain->tex_flame = NULL;

		if (smd->domain->tex_flame_coba)
			GPU_texture_free(smd->domain->tex_flame_coba);
		smd->domain->tex_flame_coba = NULL;

		if (smd->domain->tex_coba)
			GPU_texture_free(smd->domain->tex_coba);
		smd->domain->tex_coba = NULL;

		if (smd->domain->tex_field)
			GPU_texture_free(smd->domain->tex_field);
		smd->domain->tex_field = NULL;
	}
}

void GPU_create_smoke_coba_field(SmokeModifierData *smd)
{
#ifdef WITH_SMOKE
	if (smd->type & MOD_SMOKE_TYPE_DOMAIN) {
		SmokeDomainSettings *sds = smd->domain;

		if (!sds->tex_field) {
			sds->tex_field = create_field_texture(sds);
		}
		if (!sds->tex_coba) {
			sds->tex_coba = create_transfer_function(TFUNC_COLOR_RAMP, sds->coba);
		}
	}
#else // WITH_SMOKE
	smd->domain->tex_field = NULL;
#endif // WITH_SMOKE
}

void GPU_create_smoke(SmokeModifierData *smd, int highres)
{
#ifdef WITH_SMOKE
	if (smd->type & MOD_SMOKE_TYPE_DOMAIN) {
		SmokeDomainSettings *sds = smd->domain;

		if (!sds->tex) {
			sds->tex = create_density_texture(sds, highres);
		}
		if (!sds->tex_flame) {
			sds->tex_flame = create_flame_texture(sds, highres);
		}
		if (!sds->tex_flame_coba && sds->tex_flame) {
			sds->tex_flame_coba = create_transfer_function(TFUNC_FLAME_SPECTRUM, NULL);
		}
		if (!sds->tex_shadow) {
			sds->tex_shadow = GPU_texture_create_nD(
			                      sds->res[0], sds->res[1], sds->res[2], 3,
			                      sds->shadow,
			                      GPU_R8, GPU_DATA_FLOAT, 0, true, NULL);
		}
	}
#else // WITH_SMOKE
	(void)highres;
	smd->domain->tex = NULL;
	smd->domain->tex_flame = NULL;
	smd->domain->tex_flame_coba = NULL;
	smd->domain->tex_shadow = NULL;
#endif // WITH_SMOKE
}

void GPU_create_smoke_velocity(SmokeModifierData *smd)
{
#ifdef WITH_SMOKE
	if (smd->type & MOD_SMOKE_TYPE_DOMAIN) {
		SmokeDomainSettings *sds = smd->domain;

		const float *vel_x = smoke_get_velocity_x(sds->fluid);
		const float *vel_y = smoke_get_velocity_y(sds->fluid);
		const float *vel_z = smoke_get_velocity_z(sds->fluid);

		if (ELEM(NULL, vel_x, vel_y, vel_z)) {
			return;
		}

		if (!sds->tex_velocity_x) {
			sds->tex_velocity_x = GPU_texture_create_3D(sds->res[0], sds->res[1], sds->res[2], GPU_R16F, vel_x, NULL);
			sds->tex_velocity_y = GPU_texture_create_3D(sds->res[0], sds->res[1], sds->res[2], GPU_R16F, vel_y, NULL);
			sds->tex_velocity_z = GPU_texture_create_3D(sds->res[0], sds->res[1], sds->res[2], GPU_R16F, vel_z, NULL);
		}
	}
#else // WITH_SMOKE
	smd->domain->tex_velocity_x = NULL;
	smd->domain->tex_velocity_y = NULL;
	smd->domain->tex_velocity_z = NULL;
#endif // WITH_SMOKE
}

/* TODO Unify with the other GPU_free_smoke. */
void GPU_free_smoke_velocity(SmokeModifierData *smd)
{
	if (smd->type & MOD_SMOKE_TYPE_DOMAIN && smd->domain) {
		if (smd->domain->tex_velocity_x)
			GPU_texture_free(smd->domain->tex_velocity_x);

		if (smd->domain->tex_velocity_y)
			GPU_texture_free(smd->domain->tex_velocity_y);

		if (smd->domain->tex_velocity_z)
			GPU_texture_free(smd->domain->tex_velocity_z);

		smd->domain->tex_velocity_x = NULL;
		smd->domain->tex_velocity_y = NULL;
		smd->domain->tex_velocity_z = NULL;
	}
}

static LinkNode *image_free_queue = NULL;

static void gpu_queue_image_for_free(Image *ima)
{
	BLI_thread_lock(LOCK_OPENGL);
	BLI_linklist_prepend(&image_free_queue, ima);
	BLI_thread_unlock(LOCK_OPENGL);
}

void GPU_free_unused_buffers(Main *bmain)
{
	if (!BLI_thread_is_main())
		return;

	BLI_thread_lock(LOCK_OPENGL);

	/* images */
	for (LinkNode *node = image_free_queue; node; node = node->next) {
		Image *ima = node->link;

		/* check in case it was freed in the meantime */
		if (bmain && BLI_findindex(&bmain->images, ima) != -1)
			GPU_free_image(ima);
	}

	BLI_linklist_free(image_free_queue, NULL);
	image_free_queue = NULL;

	BLI_thread_unlock(LOCK_OPENGL);
}

static void gpu_free_image_immediate(Image *ima)
{
	for (int i = 0; i < TEXTARGET_COUNT; i++) {
		/* free glsl image binding */
		if (ima->gputexture[i]) {
			GPU_texture_free(ima->gputexture[i]);
			ima->gputexture[i] = NULL;
		}
	}

	ima->gpuflag &= ~(IMA_GPU_MIPMAP_COMPLETE | IMA_GPU_IS_DATA);
}

void GPU_free_image(Image *ima)
{
	if (!BLI_thread_is_main()) {
		gpu_queue_image_for_free(ima);
		return;
	}

	gpu_free_image_immediate(ima);
}

void GPU_free_images(Main *bmain)
{
	if (bmain) {
		for (Image *ima = bmain->images.first; ima; ima = ima->id.next) {
			GPU_free_image(ima);
		}
	}
}

/* same as above but only free animated images */
void GPU_free_images_anim(Main *bmain)
{
	if (bmain) {
		for (Image *ima = bmain->images.first; ima; ima = ima->id.next) {
			if (BKE_image_is_animated(ima)) {
				GPU_free_image(ima);
			}
		}
	}
}


void GPU_free_images_old(Main *bmain)
{
	static int lasttime = 0;
	int ctime = (int)PIL_check_seconds_timer();

	/*
	 * Run garbage collector once for every collecting period of time
	 * if textimeout is 0, that's the option to NOT run the collector
	 */
	if (U.textimeout == 0 || ctime % U.texcollectrate || ctime == lasttime)
		return;

	/* of course not! */
	if (G.is_rendering)
		return;

	lasttime = ctime;

	Image *ima = bmain->images.first;
	while (ima) {
		if ((ima->flag & IMA_NOCOLLECT) == 0 && ctime - ima->lastused > U.textimeout) {
			/* If it's in GL memory, deallocate and set time tag to current time
			 * This gives textures a "second chance" to be used before dying. */
			if (BKE_image_has_opengl_texture(ima)) {
				GPU_free_image(ima);
				ima->lastused = ctime;
			}
			/* Otherwise, just kill the buffers */
			else {
				BKE_image_free_buffers(ima);
			}
		}
		ima = ima->id.next;
	}
}

static void gpu_disable_multisample(void)
{
#ifdef __linux__
	/* changing multisample from the default (enabled) causes problems on some
	 * systems (NVIDIA/Linux) when the pixel format doesn't have a multisample buffer */
	bool toggle_ok = true;

	if (GPU_type_matches(GPU_DEVICE_NVIDIA, GPU_OS_UNIX, GPU_DRIVER_ANY)) {
		int samples = 0;
		glGetIntegerv(GL_SAMPLES, &samples);

		if (samples == 0)
			toggle_ok = false;
	}

	if (toggle_ok) {
		glDisable(GL_MULTISAMPLE);
	}
#else
	glDisable(GL_MULTISAMPLE);
#endif
}

/* Default OpenGL State
 *
 * This is called on startup, for opengl offscreen render.
 * Generally we should always return to this state when
 * temporarily modifying the state for drawing, though that are (undocumented)
 * exceptions that we should try to get rid of. */

void GPU_state_init(void)
{
	GPU_disable_program_point_size();

	glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);

	glDepthFunc(GL_LEQUAL);

	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_COLOR_LOGIC_OP);
	glDisable(GL_STENCIL_TEST);

	glDepthRange(0.0, 1.0);

	glFrontFace(GL_CCW);
	glCullFace(GL_BACK);
	glDisable(GL_CULL_FACE);

	gpu_disable_multisample();
}

void GPU_enable_program_point_size(void)
{
	glEnable(GL_PROGRAM_POINT_SIZE);
}

void GPU_disable_program_point_size(void)
{
	glDisable(GL_PROGRAM_POINT_SIZE);
}

/** \name Framebuffer color depth, for selection codes
 * \{ */


#define STATE_STACK_DEPTH 16

typedef struct {
	eGPUAttrMask mask;

	/* GL_ENABLE_BIT */
	uint is_blend : 1;
	uint is_cull_face : 1;
	uint is_depth_test : 1;
	uint is_dither : 1;
	uint is_lighting : 1;
	uint is_line_smooth : 1;
	uint is_color_logic_op : 1;
	uint is_multisample : 1;
	uint is_polygon_offset_line : 1;
	uint is_polygon_offset_fill : 1;
	uint is_polygon_smooth : 1;
	uint is_sample_alpha_to_coverage : 1;
	uint is_scissor_test : 1;
	uint is_stencil_test : 1;

	bool is_clip_plane[6];

	/* GL_DEPTH_BUFFER_BIT */
	/* uint is_depth_test : 1; */
	int depth_func;
	double depth_clear_value;
	bool depth_write_mask;

	/* GL_SCISSOR_BIT */
	int scissor_box[4];
	/* uint is_scissor_test : 1; */

	/* GL_VIEWPORT_BIT */
	int viewport[4];
	double near_far[2];
}  GPUAttrValues;

typedef struct {
	GPUAttrValues attr_stack[STATE_STACK_DEPTH];
	uint top;
} GPUAttrStack;

static GPUAttrStack state = {
	.top = 0,
};

#define AttrStack state
#define Attr state.attr_stack[state.top]

/**
 * Replacement for glPush/PopAttributes
 *
 * We don't need to cover all the options of legacy OpenGL
 * but simply the ones used by Blender.
 */
void gpuPushAttr(eGPUAttrMask mask)
{
	Attr.mask = mask;

	if ((mask & GPU_DEPTH_BUFFER_BIT) != 0) {
		Attr.is_depth_test = glIsEnabled(GL_DEPTH_TEST);
		glGetIntegerv(GL_DEPTH_FUNC, &Attr.depth_func);
		glGetDoublev(GL_DEPTH_CLEAR_VALUE, &Attr.depth_clear_value);
		glGetBooleanv(GL_DEPTH_WRITEMASK, (GLboolean *)&Attr.depth_write_mask);
	}

	if ((mask & GPU_ENABLE_BIT) != 0) {
		Attr.is_blend = glIsEnabled(GL_BLEND);

		for (int i = 0; i < 6; i++) {
			Attr.is_clip_plane[i] = glIsEnabled(GL_CLIP_PLANE0 + i);
		}

		Attr.is_cull_face = glIsEnabled(GL_CULL_FACE);
		Attr.is_depth_test = glIsEnabled(GL_DEPTH_TEST);
		Attr.is_dither = glIsEnabled(GL_DITHER);
		Attr.is_line_smooth = glIsEnabled(GL_LINE_SMOOTH);
		Attr.is_color_logic_op = glIsEnabled(GL_COLOR_LOGIC_OP);
		Attr.is_multisample = glIsEnabled(GL_MULTISAMPLE);
		Attr.is_polygon_offset_line = glIsEnabled(GL_POLYGON_OFFSET_LINE);
		Attr.is_polygon_offset_fill = glIsEnabled(GL_POLYGON_OFFSET_FILL);
		Attr.is_polygon_smooth = glIsEnabled(GL_POLYGON_SMOOTH);
		Attr.is_sample_alpha_to_coverage = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
		Attr.is_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
		Attr.is_stencil_test = glIsEnabled(GL_STENCIL_TEST);
	}

	if ((mask & GPU_SCISSOR_BIT) != 0) {
		Attr.is_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
		glGetIntegerv(GL_SCISSOR_BOX, (GLint *)&Attr.scissor_box);
	}

	if ((mask & GPU_VIEWPORT_BIT) != 0) {
		glGetDoublev(GL_DEPTH_RANGE, (GLdouble *)&Attr.near_far);
		glGetIntegerv(GL_VIEWPORT, (GLint *)&Attr.viewport);
	}

	if ((mask & GPU_BLEND_BIT) != 0) {
		Attr.is_blend = glIsEnabled(GL_BLEND);
	}

	BLI_assert(AttrStack.top < STATE_STACK_DEPTH);
	AttrStack.top++;
}

static void restore_mask(GLenum cap, const bool value)
{
	if (value) {
		glEnable(cap);
	}
	else {
		glDisable(cap);
	}
}

void gpuPopAttr(void)
{
	BLI_assert(AttrStack.top > 0);
	AttrStack.top--;

	GLint mask = Attr.mask;

	if ((mask & GPU_DEPTH_BUFFER_BIT) != 0) {
		restore_mask(GL_DEPTH_TEST, Attr.is_depth_test);
		glDepthFunc(Attr.depth_func);
		glClearDepth(Attr.depth_clear_value);
		glDepthMask(Attr.depth_write_mask);
	}

	if ((mask & GPU_ENABLE_BIT) != 0) {
		restore_mask(GL_BLEND, Attr.is_blend);

		for (int i = 0; i < 6; i++) {
			restore_mask(GL_CLIP_PLANE0 + i, Attr.is_clip_plane[i]);
		}

		restore_mask(GL_CULL_FACE, Attr.is_cull_face);
		restore_mask(GL_DEPTH_TEST, Attr.is_depth_test);
		restore_mask(GL_DITHER, Attr.is_dither);
		restore_mask(GL_LINE_SMOOTH, Attr.is_line_smooth);
		restore_mask(GL_COLOR_LOGIC_OP, Attr.is_color_logic_op);
		restore_mask(GL_MULTISAMPLE, Attr.is_multisample);
		restore_mask(GL_POLYGON_OFFSET_LINE, Attr.is_polygon_offset_line);
		restore_mask(GL_POLYGON_OFFSET_FILL, Attr.is_polygon_offset_fill);
		restore_mask(GL_POLYGON_SMOOTH, Attr.is_polygon_smooth);
		restore_mask(GL_SAMPLE_ALPHA_TO_COVERAGE, Attr.is_sample_alpha_to_coverage);
		restore_mask(GL_SCISSOR_TEST, Attr.is_scissor_test);
		restore_mask(GL_STENCIL_TEST, Attr.is_stencil_test);
	}

	if ((mask & GPU_VIEWPORT_BIT) != 0) {
		glViewport(Attr.viewport[0], Attr.viewport[1], Attr.viewport[2], Attr.viewport[3]);
		glDepthRange(Attr.near_far[0], Attr.near_far[1]);
	}

	if ((mask & GPU_SCISSOR_BIT) != 0) {
		restore_mask(GL_SCISSOR_TEST, Attr.is_scissor_test);
		glScissor(Attr.scissor_box[0], Attr.scissor_box[1], Attr.scissor_box[2], Attr.scissor_box[3]);
	}

	if ((mask & GPU_BLEND_BIT) != 0) {
		restore_mask(GL_BLEND, Attr.is_blend);
	}
}

#undef Attr
#undef AttrStack

/** \} */
