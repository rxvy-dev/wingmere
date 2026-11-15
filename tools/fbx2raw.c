/* WINGMERE - dumps an FBX model's triangles, per node and material, to a simple raw file
 * that tools/build_aircraft.py turns into the game's .wmdl format (development tool).
 *
 * raw format (little endian):
 *   "RAW1" u32 nparts
 *   per part: u32 len, node name, u32 len, material name, f32 pivot[3], u32 nverts,
 *             nverts * (f32 px py pz nx ny nz u v)          -- triangles, world space, metres, Y up */
#include "../third_party/ufbx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wstr(FILE *f, const char *s) { unsigned n = (unsigned)strlen(s); fwrite(&n, 4, 1, f); fwrite(s, 1, n, f); }

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: fbx2raw in.fbx out.raw\n"); return 1; }
    ufbx_load_opts o = {0};
    o.target_axes = ufbx_axes_right_handed_y_up;
    o.target_unit_meters = 1.0f;
    o.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
    ufbx_error err;
    ufbx_scene *s = ufbx_load_file(argv[1], &o, &err);
    if (!s) { char b[512]; ufbx_format_error(b, sizeof b, &err); fprintf(stderr, "%s\n", b); return 1; }
    FILE *f = fopen(argv[2], "wb");
    if (!f) return 1;
    unsigned nparts = 0;
    fwrite("RAW1", 1, 4, f);
    long countPos = ftell(f);
    fwrite(&nparts, 4, 1, f);
    size_t maxTri = 0;
    for (size_t i = 0; i < s->meshes.count; i++) if (s->meshes.data[i]->max_face_triangles > maxTri) maxTri = s->meshes.data[i]->max_face_triangles;
    uint32_t *tri = malloc(sizeof(uint32_t) * 3 * (maxTri + 1));
    for (size_t ni = 0; ni < s->nodes.count; ni++) {
        ufbx_node *node = s->nodes.data[ni];
        ufbx_mesh *m = node->mesh;
        if (!m) continue;
        ufbx_matrix nm = ufbx_matrix_for_normals(&node->geometry_to_world);
        size_t nmat = m->materials.count ? m->materials.count : 1;
        for (size_t mi = 0; mi < nmat; mi++) {
            /* count triangles that use this material */
            size_t count = 0;
            for (size_t fi = 0; fi < m->faces.count; fi++) {
                uint32_t fm = m->face_material.count ? m->face_material.data[fi] : 0;
                if (fm == mi && m->faces.data[fi].num_indices >= 3) count += m->faces.data[fi].num_indices - 2;
            }
            if (!count) continue;
            wstr(f, node->name.data);
            wstr(f, m->materials.count ? m->materials.data[mi]->name.data : "default");
            ufbx_vec3 piv = ufbx_transform_position(&node->node_to_world, (ufbx_vec3){0, 0, 0});
            float pv[3] = {(float)piv.x, (float)piv.y, (float)piv.z};
            fwrite(pv, 4, 3, f);
            unsigned nv = (unsigned)(count * 3);
            fwrite(&nv, 4, 1, f);
            for (size_t fi = 0; fi < m->faces.count; fi++) {
                uint32_t fm = m->face_material.count ? m->face_material.data[fi] : 0;
                ufbx_face face = m->faces.data[fi];
                if (fm != mi || face.num_indices < 3) continue;
                uint32_t nt = ufbx_triangulate_face(tri, 3 * (maxTri + 1), m, face);
                for (uint32_t k = 0; k < nt * 3; k++) {
                    uint32_t ix = tri[k];
                    ufbx_vec3 p = ufbx_transform_position(&node->geometry_to_world, ufbx_get_vertex_vec3(&m->vertex_position, ix));
                    ufbx_vec3 n = m->vertex_normal.exists ? ufbx_transform_direction(&nm, ufbx_get_vertex_vec3(&m->vertex_normal, ix)) : (ufbx_vec3){0, 1, 0};
                    double nl = n.x * n.x + n.y * n.y + n.z * n.z;
                    if (nl > 0) { nl = 1.0 / __builtin_sqrt(nl); n.x *= nl; n.y *= nl; n.z *= nl; }
                    ufbx_vec2 uv = m->vertex_uv.exists ? ufbx_get_vertex_vec2(&m->vertex_uv, ix) : (ufbx_vec2){0, 0};
                    float v[8] = {(float)p.x, (float)p.y, (float)p.z, (float)n.x, (float)n.y, (float)n.z, (float)uv.x, (float)uv.y};
                    fwrite(v, 4, 8, f);
                }
            }
            nparts++;
        }
    }
    fseek(f, countPos, SEEK_SET);
    fwrite(&nparts, 4, 1, f);
    fclose(f);
    free(tri);
    ufbx_free_scene(s);
    fprintf(stderr, "%s: %u parts\n", argv[2], nparts);
    return 0;
}
