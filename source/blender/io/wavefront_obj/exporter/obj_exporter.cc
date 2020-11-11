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
 * The Original Code is Copyright (C) 2020 Blender Foundation.
 * All rights reserved.
 */

/** \file
 * \ingroup obj
 */

#include <stdio.h>

#include <memory>

#include "BKE_scene.h"

#include "BLI_path_util.h"
#include "BLI_vector.hh"

#include "DEG_depsgraph_query.h"

#include "DNA_scene_types.h"

#include "ED_object.h"

#include "obj_export_mesh.hh"
#include "obj_export_nurbs.hh"
#include "obj_exporter.hh"

#include "obj_export_file_writer.hh"

namespace blender::io::obj {

OBJDepsgraph::OBJDepsgraph(const bContext *C, const eEvaluationMode eval_mode)
{
  Scene *scene = CTX_data_scene(C);
  Main *bmain = CTX_data_main(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);
  if (eval_mode == DAG_EVAL_RENDER) {
    depsgraph_ = DEG_graph_new(bmain, scene, view_layer, DAG_EVAL_RENDER);
    needs_free_ = true;
    DEG_graph_build_for_all_objects(depsgraph_);
    BKE_scene_graph_evaluated_ensure(depsgraph_, bmain);
  }
  else {
    depsgraph_ = CTX_data_ensure_evaluated_depsgraph(C);
    needs_free_ = false;
  }
}

OBJDepsgraph::~OBJDepsgraph()
{
  if (needs_free_) {
    DEG_graph_free(depsgraph_);
  }
}

Depsgraph *OBJDepsgraph::get()
{
  return depsgraph_;
}

void OBJDepsgraph::update_for_newframe()
{
  BKE_scene_graph_update_for_newframe(depsgraph_);
}

/**
 * Filter supported objects from the Scene.
 *
 * \note Curves are also stored with Meshes if export settings specify so.
 */
static std::pair<Vector<std::unique_ptr<OBJMesh>>, Vector<std::unique_ptr<OBJCurve>>>
find_exportable_objects(Depsgraph *depsgraph, const OBJExportParams &export_params)
{
  Vector<std::unique_ptr<OBJMesh>> r_exportable_meshes;
  Vector<std::unique_ptr<OBJCurve>> r_exportable_nurbs;
  const ViewLayer *view_layer = DEG_get_input_view_layer(depsgraph);
  LISTBASE_FOREACH (const Base *, base, &view_layer->object_bases) {
    Object *object_in_layer = base->object;
    if (export_params.export_selected_objects && !(object_in_layer->base_flag & BASE_SELECTED)) {
      continue;
    }
    switch (object_in_layer->type) {
      case OB_SURF:
        /* Export in mesh form: vertices and polygons. */
        ATTR_FALLTHROUGH;
      case OB_MESH: {
        r_exportable_meshes.append(
            std::make_unique<OBJMesh>(depsgraph, export_params, object_in_layer));
        break;
      }
      case OB_CURVE: {
        Curve *curve = static_cast<Curve *>(object_in_layer->data);
        Nurb *nurb{static_cast<Nurb *>(curve->nurb.first)};
        switch (nurb->type) {
          case CU_NURBS: {
            if (export_params.export_curves_as_nurbs) {
              /* Export in parameter form: control points. */
              r_exportable_nurbs.append(
                  std::make_unique<OBJCurve>(depsgraph, export_params, object_in_layer));
            }
            else {
              /* Export in mesh form: edges and vertices. */
              r_exportable_meshes.append(
                  std::make_unique<OBJMesh>(depsgraph, export_params, object_in_layer));
            }
            break;
          }
          case CU_BEZIER: {
            /* Always export in mesh form: edges and vertices. */
            r_exportable_meshes.append(
                std::make_unique<OBJMesh>(depsgraph, export_params, object_in_layer));
            break;
          }
          default: {
            /* Other curve types are not supported. */
            break;
          }
        }
      }
      default: {
        /* Other object types are not supported. */
        break;
      }
    }
  }
  return {std::move(r_exportable_meshes), std::move(r_exportable_nurbs)};
}

static void write_mesh_objects(Vector<std::unique_ptr<OBJMesh>> exportable_as_mesh,
                               OBJWriter &obj_writer,
                               const OBJExportParams &export_params)
{
  std::unique_ptr<MTLWriter> mtl_writer = nullptr;
  if (export_params.export_materials) {
    mtl_writer = std::make_unique<MTLWriter>(export_params.filepath);
    if (mtl_writer->good()) {
      obj_writer.write_mtllib_name(mtl_writer->mtl_file_path());
    }
  }

  /* Smooth groups and UV vertex indices may make huge memory allocations, so they should be freed
   * right after they're written, instead of waiting for #blender::Vector to clean them up after
   * all the objects are exported. */
  for (Steal<OBJMesh> obj_mesh : exportable_as_mesh) {
    obj_writer.write_object_name(*obj_mesh);
    obj_writer.write_vertex_coords(*obj_mesh);

    if (obj_mesh->tot_polygons() > 0) {
      if (export_params.export_smooth_groups) {
        obj_mesh->calc_smooth_groups(export_params.smooth_groups_bitflags);
      }
      if (export_params.export_normals) {
        obj_writer.write_poly_normals(*obj_mesh);
      }
      if (export_params.export_uv) {
        obj_writer.write_uv_coords(*obj_mesh);
      }
      if (mtl_writer->good()) {
        mtl_writer->append_materials(*obj_mesh);
      }
      obj_writer.write_poly_elements(*obj_mesh);
    }
    obj_writer.write_edges_indices(*obj_mesh);

    obj_writer.update_index_offsets(*obj_mesh);
  }
}

/**
 * Export NURBS Curves in parameter form, not as vertices and edges.
 */
static void write_nurbs_curve_objects(const Vector<std::unique_ptr<OBJCurve>> &exportable_as_nurbs,
                                      const OBJWriter &obj_writer)
{
  /* #OBJCurve doesn't have any dynamically allocated memory, so it's fine
   * to wait for #blender::Vector to clean the objects up. */
  for (const std::unique_ptr<OBJCurve> &obj_curve : exportable_as_nurbs) {
    obj_writer.write_nurbs_curve(*obj_curve);
  }
}

/**
 * Export a single frame to a .OBJ file.
 *
 * Conditionally write a .MTL file also.
 */
static void export_frame(Depsgraph *depsgraph,
                         const OBJExportParams &export_params,
                         const char *filepath)
{
  OBJWriter frame_writer(export_params);
  if (!frame_writer.init_writer(filepath)) {
    return;
  }
  //
  //  /* Meshes, and curves to be exported in mesh form. */
  //  Vector<std::unique_ptr<OBJMesh>> exportable_as_mesh;
  //  /* NURBS to be exported in parameter form. */
  //  Vector<std::unique_ptr<OBJCurve>> exportable_as_nurbs;
  auto [exportable_as_mesh, exportable_as_nurbs] = find_exportable_objects(depsgraph,
                                                                           export_params);

  write_mesh_objects(std::move(exportable_as_mesh), frame_writer, export_params);
  write_nurbs_curve_objects(std::move(exportable_as_nurbs), frame_writer);
}

/**
 * Append the current frame number in the .OBJ file name.
 *
 * \return Whether the filepath is in #FILE_MAX limits.
 */
static bool append_frame_to_filename(const char *filepath,
                                     const int frame,
                                     char *r_filepath_with_frames)
{
  BLI_strncpy(r_filepath_with_frames, filepath, FILE_MAX);
  BLI_path_extension_replace(r_filepath_with_frames, FILE_MAX, "");
  const int digits = frame == 0 ? 1 : integer_digits_i(abs(frame));
  BLI_path_frame(r_filepath_with_frames, frame, digits);
  return BLI_path_extension_replace(r_filepath_with_frames, FILE_MAX, ".obj");
}

/**
 * Central internal function to call Scene update & writer functions.
 */
void exporter_main(bContext *C, const OBJExportParams &export_params)
{
  ED_object_mode_set(C, OB_MODE_OBJECT);
  OBJDepsgraph obj_depsgraph(C, export_params.export_eval_mode);
  Scene *scene = DEG_get_input_scene(obj_depsgraph.get());
  const char *filepath = export_params.filepath;

  /* Single frame export, i.e. no animation. */
  if (!export_params.export_animation) {
    fprintf(stderr, "Writing to %s\n", filepath);
    export_frame(obj_depsgraph.get(), export_params, filepath);
    return;
  }

  char filepath_with_frames[FILE_MAX];
  /* Used to reset the Scene to its original state. */
  const int original_frame = CFRA;

  for (int frame = export_params.start_frame; frame <= export_params.end_frame; frame++) {
    const bool filepath_ok = append_frame_to_filename(filepath, frame, filepath_with_frames);
    if (!filepath_ok) {
      fprintf(stderr, "Error: File Path too long.\n%s\n", filepath_with_frames);
      return;
    }

    CFRA = frame;
    obj_depsgraph.update_for_newframe();
    fprintf(stderr, "Writing to %s\n", filepath_with_frames);
    export_frame(obj_depsgraph.get(), export_params, filepath_with_frames);
  }
  CFRA = original_frame;
}
}  // namespace blender::io::obj