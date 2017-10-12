/*
 * This file is part of libplacebo.
 *
 * libplacebo is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * libplacebo is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with libplacebo. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common.h"
#include "context.h"
#include "ra.h"

void ra_destroy(const struct ra *ra)
{
    if (!ra)
        return;

    ra->impl->destroy(ra);
}

bool ra_fmt_is_ordered(const struct ra_fmt *fmt)
{
    bool ret = true;
    for (int i = 0; i < fmt->num_components; i++)
        ret &= fmt->component_index[i] == i;
    return ret;
}

bool ra_fmt_is_regular(const struct ra_fmt *fmt)
{
    int bits = 0;
    for (int i = 0; i < fmt->num_components; i++) {
        if (fmt->component_index[i] != i || fmt->component_pad[i])
            return false;
        bits += fmt->component_depth[i];
    }

    return bits == fmt->texel_size * 8;
}

const struct ra_fmt *ra_find_fmt(const struct ra *ra, enum ra_fmt_type type,
                                 int num_components, int bits_per_component,
                                 bool regular, enum ra_fmt_caps caps)
{
    for (int n = 0; n < ra->num_formats; n++) {
        const struct ra_fmt *fmt = ra->formats[n];
        if (fmt->type != type || fmt->num_components != num_components)
            continue;
        if ((fmt->caps & caps) != caps)
            continue;
        if (regular && !ra_fmt_is_regular(fmt))
            continue;

        for (int i = 0; i < fmt->num_components; i++) {
            if (fmt->component_depth[i] != bits_per_component)
                goto next_fmt;
        }

        return fmt;

next_fmt: ; // equivalent to `continue`
    }

    // ran out of formats
    PL_DEBUG(ra, "No matching format found");
    return NULL;
}

const struct ra_fmt *ra_find_vertex_fmt(const struct ra *ra,
                                        enum ra_fmt_type type, int comps)
{
    static const size_t sizes[] = {
        [RA_FMT_FLOAT] = sizeof(float),
        [RA_FMT_UNORM] = sizeof(unsigned),
        [RA_FMT_UINT]  = sizeof(unsigned),
        [RA_FMT_SNORM] = sizeof(int),
        [RA_FMT_SINT]  = sizeof(int),
    };

    return ra_find_fmt(ra, type, comps, 8 * sizes[type], true, RA_FMT_CAP_VERTEX);
}

const struct ra_fmt *ra_find_named_fmt(const struct ra *ra, const char *name)
{
    if (!name)
        return NULL;

    for (int i = 0; i < ra->num_formats; i++) {
        const struct ra_fmt *fmt = ra->formats[i];
        if (strcmp(name, fmt->name) == 0)
            return fmt;
    }

    // ran out of formats
    return NULL;
}

const struct ra_tex *ra_tex_create(const struct ra *ra,
                                   const struct ra_tex_params *params)
{
    switch (ra_tex_params_dimension(*params)) {
    case 1:
        assert(params->w > 0);
        assert(params->w <= ra->limits.max_tex_1d_dim);
        assert(!params->renderable);
        break;
    case 2:
        assert(params->w > 0 && params->h > 0);
        assert(params->w <= ra->limits.max_tex_2d_dim);
        assert(params->h <= ra->limits.max_tex_2d_dim);
        break;
    case 3:
        assert(params->w > 0 && params->h > 0 && params->d > 0);
        assert(params->w <= ra->limits.max_tex_3d_dim);
        assert(params->h <= ra->limits.max_tex_3d_dim);
        assert(params->d <= ra->limits.max_tex_3d_dim);
        assert(!params->renderable);
        break;
    }

    const struct ra_fmt *fmt = params->format;
    assert(fmt);
    assert(fmt->caps & RA_FMT_CAP_TEXTURE);
    assert(!params->sampleable || fmt->caps & RA_FMT_CAP_SAMPLEABLE);
    assert(!params->renderable || fmt->caps & RA_FMT_CAP_RENDERABLE);
    assert(!params->storable   || fmt->caps & RA_FMT_CAP_STORABLE);
    assert(!params->blit_src   || fmt->caps & RA_FMT_CAP_BLITTABLE);
    assert(!params->blit_dst   || fmt->caps & RA_FMT_CAP_BLITTABLE);
    assert(params->sample_mode != RA_TEX_SAMPLE_LINEAR || fmt->caps & RA_FMT_CAP_LINEAR);

    return ra->impl->tex_create(ra, params);
}

static bool ra_tex_params_eq(struct ra_tex_params a, struct ra_tex_params b)
{
    return a.w == b.w && a.h == b.h && a.d == b.d &&
           a.format         == b.format &&
           a.sampleable     == b.sampleable &&
           a.renderable     == b.renderable &&
           a.storable       == b.storable &&
           a.blit_src       == b.blit_src &&
           a.blit_dst       == b.blit_dst &&
           a.host_writable  == b.host_writable &&
           a.host_readable  == b.host_readable &&
           a.sample_mode    == b.sample_mode &&
           a.address_mode   == b.address_mode;
}

bool ra_tex_recreate(const struct ra *ra, const struct ra_tex **tex,
                     const struct ra_tex_params *params)
{
    if (*tex && ra_tex_params_eq((*tex)->params, *params))
        return true;

    PL_DEBUG(ra, "ra_tex_recreate: %dx%dx%d", params->w, params->h,params->d);
    ra_tex_destroy(ra, tex);
    *tex = ra_tex_create(ra, params);

    return !!*tex;
}

void ra_tex_destroy(const struct ra *ra, const struct ra_tex **tex)
{
    if (!*tex)
        return;

    ra->impl->tex_destroy(ra, *tex);
    *tex = NULL;
}

void ra_tex_clear(const struct ra *ra, const struct ra_tex *dst,
                  const float color[4])
{
    assert(dst->params.blit_dst);

    ra_tex_invalidate(ra, dst);
    ra->impl->tex_clear(ra, dst, color);
}

void ra_tex_invalidate(const struct ra *ra, const struct ra_tex *tex)
{
    ra->impl->tex_invalidate(ra, tex);
}

static void strip_coords(const struct ra_tex *tex, struct pl_rect3d *rc)
{
    if (!tex->params.d) {
        rc->z0 = 0;
        rc->z1 = 1;
    }

    if (!tex->params.h) {
        rc->y0 = 0;
        rc->y1 = 1;
    }
}

void ra_tex_blit(const struct ra *ra,
                 const struct ra_tex *dst, const struct ra_tex *src,
                 struct pl_rect3d dst_rc, struct pl_rect3d src_rc)
{
    assert(src->params.format->texel_size == dst->params.format->texel_size);
    assert(src->params.blit_src);
    assert(dst->params.blit_dst);
    assert(src_rc.x0 >= 0 && src_rc.x0 < src->params.w);
    assert(src_rc.y0 >= 0 && src_rc.y0 < src->params.h);
    assert(src_rc.z0 >= 0 && src_rc.z0 < src->params.d);
    assert(src_rc.x1 > 0 && src_rc.x1 <= src->params.w);
    assert(src_rc.y1 > 0 && src_rc.y1 <= src->params.h);
    assert(src_rc.z1 > 0 && src_rc.z1 <= src->params.d);
    assert(dst_rc.x0 >= 0 && dst_rc.x0 < dst->params.w);
    assert(dst_rc.y0 >= 0 && dst_rc.y0 < dst->params.h);
    assert(dst_rc.z0 >= 0 && dst_rc.z0 < dst->params.d);
    assert(dst_rc.x1 > 0 && dst_rc.x1 <= dst->params.w);
    assert(dst_rc.y1 > 0 && dst_rc.y1 <= dst->params.h);
    assert(dst_rc.z1 > 0 && dst_rc.z1 <= dst->params.d);

    strip_coords(src, &src_rc);
    strip_coords(dst, &dst_rc);

    struct pl_rect3d full = {0, 0, 0, dst->params.w, dst->params.h, dst->params.d};
    strip_coords(dst, &full);

    if (pl_rect3d_eq(pl_rect3d_normalize(dst_rc), full))
        ra_tex_invalidate(ra, dst);

    ra->impl->tex_blit(ra, dst, src, dst_rc, src_rc);
}

size_t ra_tex_transfer_size(const struct ra_tex_transfer_params *par)
{
    const struct ra_tex *tex = par->tex;

    int texels;
    switch (ra_tex_params_dimension(tex->params)) {
    case 1: texels = pl_rect_w(par->rc); break;
    case 2: texels = pl_rect_h(par->rc) * par->stride_w; break;
    case 3: texels = pl_rect_d(par->rc) * par->stride_w * par->stride_h; break;
    }

    return texels * tex->params.format->texel_size;
}

static void fix_tex_transfer(const struct ra *ra,
                             struct ra_tex_transfer_params *params)
{
    const struct ra_tex *tex = params->tex;
    struct pl_rect3d rc = params->rc;

    // Infer the default values
    if (!rc.x0 && !rc.x1)
        rc.x1 = tex->params.w;
    if (!rc.y0 && !rc.y1)
        rc.y1 = tex->params.h;
    if (!rc.z0 && !rc.z1)
        rc.z1 = tex->params.d;

    if (!params->stride_w)
        params->stride_w = tex->params.w;
    if (!params->stride_h)
        params->stride_h = tex->params.h;

    // Check the parameters for sanity
#ifndef NDEBUG
    switch (ra_tex_params_dimension(tex->params))
    {
    case 3:
        assert(rc.z1 > rc.z0);
        assert(rc.z0 >= 0 && rc.z0 <  tex->params.d);
        assert(rc.z1 >  0 && rc.z1 <= tex->params.d);
        assert(params->stride_h >= pl_rect_h(rc));
        // fall through
    case 2:
        assert(rc.y1 > rc.y0);
        assert(rc.y0 >= 0 && rc.y0 <  tex->params.h);
        assert(rc.y1 >  0 && rc.y1 <= tex->params.h);
        assert(params->stride_w >= pl_rect_w(rc));
        // fall through
    case 1:
        assert(rc.x1 > rc.x0);
        assert(rc.x0 >= 0 && rc.x0 <  tex->params.w);
        assert(rc.x1 >  0 && rc.x1 <= tex->params.w);
        break;
    }

    assert(!params->buf ^ !params->ptr); // exactly one
    if (params->buf) {
        const struct ra_buf *buf = params->buf;
        size_t size = ra_tex_transfer_size(params);
        assert(params->buf_offset == PL_ALIGN2(params->buf_offset, 4));
        assert(params->buf_offset + size <= buf->params.size);
    }
#endif

    // Sanitize superfluous coordinates for the benefit of the RA
    strip_coords(tex, &rc);
    if (!tex->params.w)
        params->stride_w = 1;
    if (!tex->params.h)
        params->stride_h = 1;

    params->rc = rc;
}

bool ra_tex_upload(const struct ra *ra,
                   const struct ra_tex_transfer_params *params)
{
    const struct ra_tex *tex = params->tex;
    assert(tex);
    assert(tex->params.host_writable);

    struct ra_tex_transfer_params fixed = *params;
    fix_tex_transfer(ra, &fixed);
    return ra->impl->tex_upload(ra, &fixed);
}

bool ra_tex_download(const struct ra *ra,
                     const struct ra_tex_transfer_params *params)
{
    const struct ra_tex *tex = params->tex;
    assert(tex);
    assert(tex->params.host_readable);

    struct ra_tex_transfer_params fixed = *params;
    fix_tex_transfer(ra, &fixed);
    return ra->impl->tex_download(ra, &fixed);
}

const struct ra_buf *ra_buf_create(const struct ra *ra,
                                   const struct ra_buf_params *params)
{
    assert(params->size >= 0);
    switch (params->type) {
    case RA_BUF_TEX_TRANSFER:
        assert(ra->limits.max_xfer_size);
        assert(params->size <= ra->limits.max_xfer_size);
        break;
    case RA_BUF_UNIFORM:
        assert(ra->limits.max_ubo_size);
        assert(params->size <= ra->limits.max_ubo_size);
        break;
    case RA_BUF_STORAGE:
        assert(ra->limits.max_ssbo_size);
        assert(params->size <= ra->limits.max_ssbo_size);
        break;
    default: abort();
    }

    const struct ra_buf *buf = ra->impl->buf_create(ra, params);
    assert(buf->data || !params->host_mapped);
    return buf;
}

void ra_buf_destroy(const struct ra *ra, const struct ra_buf **buf)
{
    if (!*buf)
        return;

    ra->impl->buf_destroy(ra, *buf);
    *buf = NULL;
}

void ra_buf_write(const struct ra *ra, const struct ra_buf *buf,
                  size_t buf_offset, const void *data, size_t size)
{
    assert(buf->params.host_writable);
    assert(buf_offset + size <= buf->params.size);
    assert(buf_offset == PL_ALIGN2(buf_offset, 4));
    ra->impl->buf_write(ra, buf, buf_offset, data, size);
}

bool ra_buf_read(const struct ra *ra, const struct ra_buf *buf,
                 size_t buf_offset, void *dest, size_t size)
{
    assert(buf->params.host_readable);
    assert(buf_offset + size <= buf->params.size);
    assert(buf_offset == PL_ALIGN2(buf_offset, 4));
    return ra->impl->buf_read(ra, buf, buf_offset, dest, size);
}

bool ra_buf_poll(const struct ra *ra, const struct ra_buf *buf, uint64_t t)
{
    return ra->impl->buf_poll ? ra->impl->buf_poll(ra, buf, t) : false;
}

size_t ra_var_type_size(enum ra_var_type type)
{
    switch (type) {
    case RA_VAR_SINT:  return sizeof(int);
    case RA_VAR_UINT:  return sizeof(unsigned int);
    case RA_VAR_FLOAT: return sizeof(float);
    default: abort();
    }
}

#define MAX_DIM 4

const char *ra_var_glsl_type_name(struct ra_var var)
{
    static const char *types[RA_VAR_TYPE_COUNT][MAX_DIM+1][MAX_DIM+1] = {
    // float vectors
    [RA_VAR_FLOAT][1][1] = "float",
    [RA_VAR_FLOAT][1][2] = "vec2",
    [RA_VAR_FLOAT][1][3] = "vec3",
    [RA_VAR_FLOAT][1][4] = "vec4",
    // float matrices
    [RA_VAR_FLOAT][2][2] = "mat2",
    [RA_VAR_FLOAT][2][3] = "mat2x3",
    [RA_VAR_FLOAT][2][4] = "mat2x4",
    [RA_VAR_FLOAT][3][2] = "mat3x2",
    [RA_VAR_FLOAT][3][3] = "mat3",
    [RA_VAR_FLOAT][3][4] = "mat3x4",
    [RA_VAR_FLOAT][4][2] = "mat4x2",
    [RA_VAR_FLOAT][4][3] = "mat4x3",
    [RA_VAR_FLOAT][4][4] = "mat4",
    // integer vectors
    [RA_VAR_SINT][1][1] = "int",
    [RA_VAR_SINT][1][2] = "ivec2",
    [RA_VAR_SINT][1][3] = "ivec3",
    [RA_VAR_SINT][1][4] = "ivec4",
    // unsigned integer vectors
    [RA_VAR_UINT][1][1] = "uint",
    [RA_VAR_UINT][1][2] = "uvec2",
    [RA_VAR_UINT][1][3] = "uvec3",
    [RA_VAR_UINT][1][4] = "uvec4",
    };

    if (var.dim_v > MAX_DIM || var.dim_m > MAX_DIM)
        return NULL;

    return types[var.type][var.dim_m][var.dim_v];
}

#define RA_VAR_FV(TYPE, M, V)                           \
    struct ra_var ra_var_##TYPE(const char *name) {     \
        return (struct ra_var) {                        \
            .name  = name,                              \
            .type  = RA_VAR_FLOAT,                      \
            .dim_m = M,                                 \
            .dim_v = V,                                 \
        };                                              \
    }

RA_VAR_FV(float, 1, 1)
RA_VAR_FV(vec2,  1, 2)
RA_VAR_FV(vec3,  1, 3)
RA_VAR_FV(vec4,  1, 4)
RA_VAR_FV(mat2,  2, 2)
RA_VAR_FV(mat3,  3, 3)
RA_VAR_FV(mat4,  4, 4)

struct ra_var_layout ra_var_host_layout(size_t offset, struct ra_var var)
{
    size_t col_size = ra_var_type_size(var.type) * var.dim_v;
    return (struct ra_var_layout) {
        .offset = offset,
        .stride = col_size,
        .size   = col_size * var.dim_m,
    };
}

struct ra_var_layout ra_buf_uniform_layout(const struct ra *ra, size_t offset,
                                           const struct ra_var *var)
{
    if (ra->limits.max_ubo_size) {
        return ra->impl->buf_uniform_layout(ra, offset, var);
    } else {
        return (struct ra_var_layout) {0};
    }
}

struct ra_var_layout ra_buf_storage_layout(const struct ra *ra, size_t offset,
                                           const struct ra_var *var)
{
    if (ra->limits.max_ssbo_size) {
        return ra->impl->buf_storage_layout(ra, offset, var);
    } else {
        return (struct ra_var_layout) {0};
    }
}

struct ra_var_layout ra_push_constant_layout(const struct ra *ra, size_t offset,
                                             const struct ra_var *var)
{
    if (ra->limits.max_pushc_size) {
        return ra->impl->push_constant_layout(ra, offset, var);
    } else {
        return (struct ra_var_layout) {0};
    }
}

int ra_desc_namespace(const struct ra *ra, enum ra_desc_type type)
{
    return ra->impl->desc_namespace(ra, type);
}

const char *ra_desc_access_glsl_name(enum ra_desc_access mode)
{
    switch (mode) {
    case RA_DESC_ACCESS_READWRITE: return "";
    case RA_DESC_ACCESS_READONLY:  return "readonly";
    case RA_DESC_ACCESS_WRITEONLY: return "writeonly";
    default: abort();
    }
}

const struct ra_renderpass *ra_renderpass_create(const struct ra *ra,
                                const struct ra_renderpass_params *params)
{
    assert(params->glsl_shader);
    switch(params->type) {
    case RA_RENDERPASS_RASTER:
        assert(params->vertex_shader);
        for (int i = 0; i < params->num_vertex_attribs; i++) {
            struct ra_vertex_attrib va = params->vertex_attribs[i];
            assert(va.name);
            assert(va.fmt);
            assert(va.fmt->caps & RA_FMT_CAP_VERTEX);
            assert(va.offset + va.fmt->texel_size <= params->vertex_stride);
        }

        assert(params->target_fmt);
        assert(params->target_fmt->caps & RA_FMT_CAP_RENDERABLE);
        assert(!params->enable_blend || params->target_fmt->caps & RA_FMT_CAP_BLENDABLE);
        break;
    case RA_RENDERPASS_COMPUTE:
        assert(ra->caps & RA_CAP_COMPUTE);
        break;
    default: abort();
    }

    for (int i = 0; i < params->num_variables; i++) {
        assert(ra->caps & RA_CAP_INPUT_VARIABLES);
        struct ra_var var = params->variables[i];
        assert(var.name);
        assert(ra_var_glsl_type_name(var));
    }

    for (int i = 0; i < params->num_descriptors; i++) {
        struct ra_desc desc = params->descriptors[i];
        assert(desc.name);
        // TODO: enforce disjoint bindings if possible?
    }

    assert(params->push_constants_size <= ra->limits.max_pushc_size);
    assert(params->push_constants_size == PL_ALIGN2(params->push_constants_size, 4));

    return ra->impl->renderpass_create(ra, params);
}

void ra_renderpass_destroy(const struct ra *ra,
                           const struct ra_renderpass **pass)
{
    if (!*pass)
        return;

    ra->impl->renderpass_destroy(ra, *pass);
    *pass = NULL;
}

void ra_renderpass_run(const struct ra *ra,
                       const struct ra_renderpass_run_params *params)
{
    struct ra_renderpass *pass = params->pass;

#ifndef NDEBUG
    for (int i = 0; i < pass->params.num_descriptors; i++) {
        struct ra_desc desc = pass->params.descriptors[i];
        struct ra_desc_binding db = params->desc_bindings[i];
        assert(db.object);
        switch (desc.type) {
        case RA_DESC_SAMPLED_TEX: {
            const struct ra_tex *tex = db.object;
            assert(tex->params.sampleable);
            break;
        }
        case RA_DESC_STORAGE_IMG: {
            const struct ra_tex *tex = db.object;
            assert(tex->params.storable);
            break;
        }
        case RA_DESC_BUF_UNIFORM: {
            const struct ra_buf *buf = db.object;
            assert(buf->params.type == RA_BUF_UNIFORM);
            break;
        }
        case RA_DESC_BUF_STORAGE: {
            const struct ra_buf *buf = db.object;
            assert(buf->params.type == RA_BUF_STORAGE);
            break;
        }
        default: abort();
        }
    }

    for (int i = 0; i < params->num_var_updates; i++) {
        struct ra_var_update vu = params->var_updates[i];
        assert(ra->caps & RA_CAP_INPUT_VARIABLES);
        assert(vu.index >= 0 && vu.index < pass->params.num_variables);
        assert(vu.data);
    }

    assert(params->push_constants || !pass->params.push_constants_size);

    switch (pass->params.type) {
    case RA_RENDERPASS_RASTER: {
        struct ra_tex *tex = params->target;
        assert(tex);
        assert(ra_tex_params_dimension(tex->params) == 2);
        assert(tex->params.format == pass->params.target_fmt);
        assert(tex->params.renderable);
        struct pl_rect2d vp = params->viewport;
        struct pl_rect2d sc = params->scissors;
        assert(pl_rect2d_eq(vp, pl_rect2d_normalize(vp)));
        assert(pl_rect2d_eq(sc, pl_rect2d_normalize(sc)));
        break;
    }
    case RA_RENDERPASS_COMPUTE:
        for (int i = 0; i < PL_ARRAY_SIZE(params->compute_groups); i++) {
            assert(params->compute_groups[i] >= 0);
            assert(params->compute_groups[i] <= ra->limits.max_dispatch[i]);
        }
        break;
    default: abort();
    }
#endif

    if (params->target && !pass->params.load_target)
        ra_tex_invalidate(ra, params->target);

    return ra->impl->renderpass_run(ra, params);
}

void ra_flush(const struct ra *ra)
{
    if (ra->impl->flush)
        ra->impl->flush(ra);
}

// RA-internal helpers

struct ra_var_layout std140_layout(const struct ra *ra, size_t offset,
                                   const struct ra_var *var)
{
    size_t el_size = ra_var_type_size(var->type);

    // std140 packing rules:
    // 1. The size of generic values is their size in bytes
    // 2. The size of vectors is the vector length * the base count, with the
    // exception of *vec3 which is always the same size as *vec4
    // 3. Matrices are treated like arrays of column vectors
    // 4. The size of array rows is that of the element size rounded up to
    // the nearest multiple of vec4
    // 5. All values are aligned to a multiple of their size (stride for arrays)
    size_t size = el_size * var->dim_v;
    if (var->dim_v == 3)
        size += el_size;
    if (var->dim_m > 1)
        size = PL_ALIGN2(size, sizeof(float[4]));

    return (struct ra_var_layout) {
        .offset = PL_ALIGN2(offset, size),
        .stride = size,
        .size   = size * var->dim_m,
    };
}

struct ra_var_layout std430_layout(const struct ra *ra, size_t offset,
                                   const struct ra_var *var)
{
    size_t el_size = ra_var_type_size(var->type);

    // std430 packing rules: like std140, except arrays/matrices are always
    // "tightly" packed, even arrays/matrices of vec3s
    size_t size = el_size * var->dim_v;
    if (var->dim_v == 3 && var->dim_m == 1)
        size += el_size;

    return (struct ra_var_layout) {
        .offset = PL_ALIGN2(offset, size),
        .stride = size,
        .size   = size * var->dim_m,
    };
}

void ra_buf_pool_uninit(const struct ra *ra, struct ra_buf_pool *pool)
{
    for (int i = 0; i < pool->num_buffers; i++)
        ra_buf_destroy(ra, &pool->buffers[i]);

    talloc_free(pool->buffers);
    *pool = (struct ra_buf_pool){0};
}

static bool ra_buf_params_compatible(const struct ra_buf_params *new,
                                     const struct ra_buf_params *old)
{
    return new->type == old->type &&
           new->size <= old->size &&
           new->host_mapped  == old->host_mapped &&
           new->host_writable == old->host_writable &&
           new->host_readable == old->host_readable;
}

static bool ra_buf_pool_grow(const struct ra *ra, struct ra_buf_pool *pool)
{
    const struct ra_buf *buf = ra_buf_create(ra, &pool->current_params);
    if (!buf)
        return false;

    TARRAY_INSERT_AT(NULL, pool->buffers, pool->num_buffers, pool->index, buf);
    PL_DEBUG(ra, "Resized buffer pool of type %u to size %d",
             pool->current_params.type, pool->num_buffers);
    return true;
}

const struct ra_buf *ra_buf_pool_get(const struct ra *ra,
                                     struct ra_buf_pool *pool,
                                     const struct ra_buf_params *params)
{
    assert(!params->initial_data);

    if (!ra_buf_params_compatible(params, &pool->current_params)) {
        ra_buf_pool_uninit(ra, pool);
        pool->current_params = *params;
    }

    // Make sure we have at least one buffer available
    if (!pool->buffers && !ra_buf_pool_grow(ra, pool))
        return NULL;

    // Make sure the next buffer is available for use
    if (ra_buf_poll(ra, pool->buffers[pool->index], 0) &&
        !ra_buf_pool_grow(ra, pool))
    {
        return NULL;
    }

    const struct ra_buf *buf = pool->buffers[pool->index++];
    pool->index %= pool->num_buffers;

    return buf;
}

bool ra_tex_upload_pbo(const struct ra *ra, struct ra_buf_pool *pbo,
                       const struct ra_tex_transfer_params *params)
{
    if (params->buf)
        return ra_tex_upload(ra, params);

    struct ra_buf_params bufparams = {
        .type = RA_BUF_TEX_TRANSFER,
        .size = ra_tex_transfer_size(params),
        .host_writable = true,
    };

    const struct ra_buf *buf = ra_buf_pool_get(ra, pbo, &bufparams);
    if (!buf)
        return false;

    ra_buf_write(ra, buf, 0, params->ptr, bufparams.size);

    struct ra_tex_transfer_params newparams = *params;
    newparams.buf = buf;
    newparams.ptr = NULL;

    return ra_tex_upload(ra, &newparams);
}

bool ra_tex_download_pbo(const struct ra *ra, struct ra_buf_pool *pbo,
                         const struct ra_tex_transfer_params *params)
{
    if (params->buf)
        return ra_tex_download(ra, params);

    struct ra_buf_params bufparams = {
        .type = RA_BUF_TEX_TRANSFER,
        .size = ra_tex_transfer_size(params),
        .host_readable = true,
    };

    const struct ra_buf *buf = ra_buf_pool_get(ra, pbo, &bufparams);
    if (!buf)
        return false;

    struct ra_tex_transfer_params newparams = *params;
    newparams.buf = buf;
    newparams.ptr = NULL;

    if (!ra_tex_download(ra, &newparams))
        return false;

    if (ra_buf_poll(ra, buf, 0)) {
        PL_TRACE(ra, "ra_tex_download without buffer: blocking (slow path)");
        while (ra_buf_poll(ra, buf, 1000000)) ; // 1 ms
    }

    return ra_buf_read(ra, buf, 0, params->ptr, bufparams.size);
}

struct ra_renderpass_params ra_renderpass_params_copy(void *tactx,
                                    const struct ra_renderpass_params *params)
{
    struct ra_renderpass_params new = *params;
    // FIXME: implement
    return new;
}
