/**
 * $Id$
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This shader is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version. The Blender
 * Foundation also sells licenses for use in proprietary software under
 * the Blender License.  See http://www.blender.org/BL/ for information
 * about this.
 *
 * This shader is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this shader; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The Original Code is Copyright (C) 2005 Blender Foundation.
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): Brecht Van Lommel.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

#ifndef __GPU_MATERIAL__
#define __GPU_MATERIAL__

struct Image;
struct ImageUser;
struct Material;
struct Object;
struct GPUVertexAttribs;
struct GPUNode;
struct GPUNodeLink;
struct GPUNodeStack;
struct GPUMaterial;

typedef struct GPUNode GPUNode;
typedef struct GPUNodeLink GPUNodeLink;
typedef struct GPUMaterial GPUMaterial;

/* Nodes */

typedef enum GPUType {
	GPU_NONE = 0,
	GPU_FLOAT = 1,
	GPU_VEC2 = 2,
	GPU_VEC3 = 3,
	GPU_VEC4 = 4,
	GPU_MAT3 = 9,
	GPU_MAT4 = 16,
	GPU_TEX1D = 1001,
	GPU_TEX2D = 1002,
	GPU_ATTRIB = 2001
} GPUType;

typedef struct GPUNodeStack {
	GPUType type;
	char *name;
	float vec[4];
	struct GPUNodeLink *link;
	int hasinput;
} GPUNodeStack;

GPUNodeLink *GPU_attribute(int type, char *name);
GPUNodeLink *GPU_uniform(float *num);
GPUNodeLink *GPU_image(struct Image *ima, struct ImageUser *iuser);
GPUNodeLink *GPU_texture(int size, float *pixels);
GPUNodeLink *GPU_socket(GPUNodeStack *sock);

GPUNode *GPU_link(GPUMaterial *mat, char *name, ...);
GPUNode *GPU_stack_link(GPUMaterial *mat, char *name, GPUNodeStack *in, GPUNodeStack *out, ...);

/* Material */

GPUMaterial *GPU_material_construct_begin();
void GPU_material_output_link(GPUMaterial *material, GPUNodeLink *link);
void GPU_material_enable_alpha(GPUMaterial *material);
GPUNodeLink *GPU_blender_material(GPUMaterial *mat, struct Material *ma);
int GPU_material_construct_end(GPUMaterial *material);

void GPU_material_free(GPUMaterial *material);

void GPU_material_bind(struct Object *ob, GPUMaterial *material);
void GPU_material_unbind(GPUMaterial *material);

void GPU_material_vertex_attributes(GPUMaterial *material,
	struct GPUVertexAttribs *attrib);

GPUMaterial *GPU_material_from_blender(struct Material *ma);

#endif /*__GPU_MATERIAL__*/

