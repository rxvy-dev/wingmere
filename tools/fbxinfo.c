/* WINGMERE - prints the parts, sizes and materials of an FBX model (development tool). */
#include "../third_party/ufbx.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: fbxinfo model.fbx\n"); return 1; }
    ufbx_load_opts o = {0};
    o.target_axes = ufbx_axes_right_handed_y_up;
    o.target_unit_meters = 1.0f;
    o.space_conversion = UFBX_SPACE_CONVERSION_TRANSFORM_ROOT;
    ufbx_error err;
    ufbx_scene *s = ufbx_load_file(argv[1], &o, &err);
    if (!s) { char b[512]; ufbx_format_error(b, sizeof b, &err); printf("ERR %s\n", b); return 1; }
    printf("creator: %s  unit %.4f  nodes %zu meshes %zu mats %zu textures %zu\n", s->metadata.creator.data, s->settings.unit_meters,
           s->nodes.count, s->meshes.count, s->materials.count, s->textures.count);
    double mn[3] = {1e30, 1e30, 1e30}, mx[3] = {-1e30, -1e30, -1e30};
    for (size_t i = 0; i < s->nodes.count; i++) {
        ufbx_node *n = s->nodes.data[i];
        if (!n->mesh) continue;
        ufbx_mesh *m = n->mesh;
        double a[3] = {1e30, 1e30, 1e30}, b[3] = {-1e30, -1e30, -1e30};
        for (size_t v = 0; v < m->vertices.count; v++) {
            ufbx_vec3 p = ufbx_transform_position(&n->geometry_to_world, m->vertices.data[v]);
            double q[3] = {p.x, p.y, p.z};
            for (int k = 0; k < 3; k++) {
                if (q[k] < a[k]) a[k] = q[k];
                if (q[k] > b[k]) b[k] = q[k];
                if (q[k] < mn[k]) mn[k] = q[k];
                if (q[k] > mx[k]) mx[k] = q[k];
            }
        }
        printf("  node %-34s tris %6zu mats", n->name.data, m->num_triangles);
        for (size_t k = 0; k < m->materials.count; k++) printf(" [%s]", m->materials.data[k]->name.data);
        printf("  box (%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n", a[0], a[1], a[2], b[0], b[1], b[2]);
    }
    printf("  TOTAL box (%.2f %.2f %.2f)-(%.2f %.2f %.2f)  size %.2f x %.2f x %.2f\n", mn[0], mn[1], mn[2], mx[0], mx[1], mx[2],
           mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
    for (size_t i = 0; i < s->materials.count; i++) {
        ufbx_material *m = s->materials.data[i];
        printf("  mat %-28s base (%.2f %.2f %.2f) tex:", m->name.data, m->pbr.base_color.value_vec3.x, m->pbr.base_color.value_vec3.y,
               m->pbr.base_color.value_vec3.z);
        if (m->pbr.base_color.texture) printf(" color=%s", m->pbr.base_color.texture->relative_filename.data);
        if (m->pbr.normal_map.texture) printf(" normal=%s", m->pbr.normal_map.texture->relative_filename.data);
        if (m->pbr.roughness.texture) printf(" rough=%s", m->pbr.roughness.texture->relative_filename.data);
        if (m->pbr.metalness.texture) printf(" metal=%s", m->pbr.metalness.texture->relative_filename.data);
        if (m->fbx.bump.texture) printf(" bump=%s", m->fbx.bump.texture->relative_filename.data);
        printf("\n");
    }
    ufbx_free_scene(s);
    return 0;
}
