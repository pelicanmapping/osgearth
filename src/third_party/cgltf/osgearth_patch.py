"""Patch upstream cgltf.h and cgltf_write.h with the osgEarth modifications.

Adds the core 2.1 properties (root "files", root "externalAssets", node
"externalAsset") to the single-pass jsmn parser, pointer fixup and free
paths, mirroring the style of the surrounding cgltf code so the patch stays
easy to re-apply on the next upstream release. Also stores node transforms
as doubles, which glTF files with geocentric node matrices need.

Usage: osgearth_patch.py <upstream cgltf.h> <output cgltf.h>
                         <upstream cgltf_write.h> <output cgltf_write.h>
"""
import sys

SRC = sys.argv[1]
DST = sys.argv[2]
WRITE_SRC = sys.argv[3]
WRITE_DST = sys.argv[4]

src = open(SRC, encoding="utf-8", newline="").read()


def rep(old, new, count=1):
    global src
    n = src.count(old)
    assert n == count, ("anchor count mismatch", n, old[:80])
    src = src.replace(old, new)


# ---------------------------------------------------------------- header note
rep(" * Version: 1.15\n",
    " * Version: 1.15\n"
    " *\n"
    " * osgEarth modification: parses the glTF 2.1 core external-asset\n"
    " * properties (\"files\", \"externalAssets\", node \"externalAsset\") into\n"
    " * cgltf_file / cgltf_external_asset. Search for \"glTF 2.1\" to find the\n"
    " * patched regions.\n"
    " *\n"
    " * osgEarth modification: cgltf_node stores its transform as doubles. Search\n"
    " * for \"cgltf_double\" to find the patched regions.\n")

# ---------------------------------------------------------------- structs
rep("""typedef struct cgltf_image
{
	char* name;
	char* uri;
	cgltf_buffer_view* buffer_view;
	char* mime_type;
	cgltf_extras extras;
	cgltf_size extensions_count;
	cgltf_extension* extensions;
} cgltf_image;
""", """typedef struct cgltf_image
{
	char* name;
	char* uri;
	cgltf_buffer_view* buffer_view;
	char* mime_type;
	cgltf_extras extras;
	cgltf_size extensions_count;
	cgltf_extension* extensions;
} cgltf_image;

/* glTF 2.1: an entry of the root-level "files" array. */
typedef struct cgltf_file
{
	char* name;
	char* uri;
	cgltf_buffer_view* buffer_view;
	char* mime_type;
	char** aliases;
	cgltf_size aliases_count;
	cgltf_extras extras;
	cgltf_size extensions_count;
	cgltf_extension* extensions;
} cgltf_file;

/* glTF 2.1: an entry of the root-level "externalAssets" array. */
typedef struct cgltf_external_asset
{
	char* name;
	cgltf_file* file;
	cgltf_extras extras;
	cgltf_size extensions_count;
	cgltf_extension* extensions;
} cgltf_external_asset;
""")

rep("""	cgltf_light* light;
	cgltf_float* weights;
	cgltf_size weights_count;
	cgltf_bool has_translation;
""", """	cgltf_light* light;
	cgltf_external_asset* external_asset; /* glTF 2.1 */
	cgltf_float* weights;
	cgltf_size weights_count;
	cgltf_bool has_translation;
""")

rep("""	cgltf_material_variant* variants;
	cgltf_size variants_count;
""", """	cgltf_material_variant* variants;
	cgltf_size variants_count;

	/* glTF 2.1 */
	cgltf_file* files;
	cgltf_size files_count;

	cgltf_external_asset* external_assets;
	cgltf_size external_assets_count;
""")

# ---------------------------------------------------------------- API
rep("cgltf_size cgltf_image_index(const cgltf_data* data, const cgltf_image* object);\n",
    "cgltf_size cgltf_image_index(const cgltf_data* data, const cgltf_image* object);\n"
    "cgltf_size cgltf_file_index(const cgltf_data* data, const cgltf_file* object); /* glTF 2.1 */\n"
    "cgltf_size cgltf_external_asset_index(const cgltf_data* data, const cgltf_external_asset* object); /* glTF 2.1 */\n")

rep("""cgltf_size cgltf_image_index(const cgltf_data* data, const cgltf_image* object)
{
	assert(object && (cgltf_size)(object - data->images) < data->images_count);
	return (cgltf_size)(object - data->images);
}
""", """cgltf_size cgltf_image_index(const cgltf_data* data, const cgltf_image* object)
{
	assert(object && (cgltf_size)(object - data->images) < data->images_count);
	return (cgltf_size)(object - data->images);
}

/* glTF 2.1 */
cgltf_size cgltf_file_index(const cgltf_data* data, const cgltf_file* object)
{
	assert(object && (cgltf_size)(object - data->files) < data->files_count);
	return (cgltf_size)(object - data->files);
}

/* glTF 2.1 */
cgltf_size cgltf_external_asset_index(const cgltf_data* data, const cgltf_external_asset* object)
{
	assert(object && (cgltf_size)(object - data->external_assets) < data->external_assets_count);
	return (cgltf_size)(object - data->external_assets);
}
""")

# ---------------------------------------------------------------- parsing
rep("static int cgltf_parse_json_images(cgltf_options* options, jsmntok_t const* tokens, int i, const uint8_t* json_chunk, cgltf_data* out_data)\n",
"""/* glTF 2.1 */
static int cgltf_parse_json_file(cgltf_options* options, jsmntok_t const* tokens, int i, const uint8_t* json_chunk, cgltf_file* out_file)
{
	CGLTF_CHECK_TOKTYPE(tokens[i], JSMN_OBJECT);

	int size = tokens[i].size;
	++i;

	for (int j = 0; j < size; ++j)
	{
		CGLTF_CHECK_KEY(tokens[i]);

		if (cgltf_json_strcmp(tokens + i, json_chunk, "uri") == 0)
		{
			i = cgltf_parse_json_string(options, tokens, i + 1, json_chunk, &out_file->uri);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "bufferView") == 0)
		{
			++i;
			out_file->buffer_view = CGLTF_PTRINDEX(cgltf_buffer_view, cgltf_json_to_int(tokens + i, json_chunk));
			++i;
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "mimeType") == 0)
		{
			i = cgltf_parse_json_string(options, tokens, i + 1, json_chunk, &out_file->mime_type);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "aliases") == 0)
		{
			i = cgltf_parse_json_string_array(options, tokens, i + 1, json_chunk, &out_file->aliases, &out_file->aliases_count);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "name") == 0)
		{
			i = cgltf_parse_json_string(options, tokens, i + 1, json_chunk, &out_file->name);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = cgltf_parse_json_extras(options, tokens, i + 1, json_chunk, &out_file->extras);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = cgltf_parse_json_unprocessed_extensions(options, tokens, i, json_chunk, &out_file->extensions_count, &out_file->extensions);
		}
		else
		{
			i = cgltf_skip_json(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

/* glTF 2.1 */
static int cgltf_parse_json_files(cgltf_options* options, jsmntok_t const* tokens, int i, const uint8_t* json_chunk, cgltf_data* out_data)
{
	i = cgltf_parse_json_array(options, tokens, i, json_chunk, sizeof(cgltf_file), (void**)&out_data->files, &out_data->files_count);
	if (i < 0)
	{
		return i;
	}

	for (cgltf_size j = 0; j < out_data->files_count; ++j)
	{
		i = cgltf_parse_json_file(options, tokens, i, json_chunk, &out_data->files[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

/* glTF 2.1 */
static int cgltf_parse_json_external_asset(cgltf_options* options, jsmntok_t const* tokens, int i, const uint8_t* json_chunk, cgltf_external_asset* out_asset)
{
	CGLTF_CHECK_TOKTYPE(tokens[i], JSMN_OBJECT);

	int size = tokens[i].size;
	++i;

	for (int j = 0; j < size; ++j)
	{
		CGLTF_CHECK_KEY(tokens[i]);

		if (cgltf_json_strcmp(tokens + i, json_chunk, "file") == 0)
		{
			++i;
			CGLTF_CHECK_TOKTYPE(tokens[i], JSMN_PRIMITIVE);
			out_asset->file = CGLTF_PTRINDEX(cgltf_file, cgltf_json_to_int(tokens + i, json_chunk));
			++i;
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "name") == 0)
		{
			i = cgltf_parse_json_string(options, tokens, i + 1, json_chunk, &out_asset->name);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "extras") == 0)
		{
			i = cgltf_parse_json_extras(options, tokens, i + 1, json_chunk, &out_asset->extras);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "extensions") == 0)
		{
			i = cgltf_parse_json_unprocessed_extensions(options, tokens, i, json_chunk, &out_asset->extensions_count, &out_asset->extensions);
		}
		else
		{
			i = cgltf_skip_json(tokens, i + 1);
		}

		if (i < 0)
		{
			return i;
		}
	}

	return i;
}

/* glTF 2.1 */
static int cgltf_parse_json_external_assets(cgltf_options* options, jsmntok_t const* tokens, int i, const uint8_t* json_chunk, cgltf_data* out_data)
{
	i = cgltf_parse_json_array(options, tokens, i, json_chunk, sizeof(cgltf_external_asset), (void**)&out_data->external_assets, &out_data->external_assets_count);
	if (i < 0)
	{
		return i;
	}

	for (cgltf_size j = 0; j < out_data->external_assets_count; ++j)
	{
		i = cgltf_parse_json_external_asset(options, tokens, i, json_chunk, &out_data->external_assets[j]);
		if (i < 0)
		{
			return i;
		}
	}
	return i;
}

static int cgltf_parse_json_images(cgltf_options* options, jsmntok_t const* tokens, int i, const uint8_t* json_chunk, cgltf_data* out_data)
""")

rep("""		else if (cgltf_json_strcmp(tokens + i, json_chunk, "textures") == 0)
		{
			i = cgltf_parse_json_textures(options, tokens, i + 1, json_chunk, out_data);
		}
""", """		else if (cgltf_json_strcmp(tokens + i, json_chunk, "textures") == 0)
		{
			i = cgltf_parse_json_textures(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "files") == 0) /* glTF 2.1 */
		{
			i = cgltf_parse_json_files(options, tokens, i + 1, json_chunk, out_data);
		}
		else if (cgltf_json_strcmp(tokens + i, json_chunk, "externalAssets") == 0) /* glTF 2.1 */
		{
			i = cgltf_parse_json_external_assets(options, tokens, i + 1, json_chunk, out_data);
		}
""")

rep("""		else if (cgltf_json_strcmp(tokens+i, json_chunk, "skin") == 0)
		{
			++i;
			CGLTF_CHECK_TOKTYPE(tokens[i], JSMN_PRIMITIVE);
			out_node->skin = CGLTF_PTRINDEX(cgltf_skin, cgltf_json_to_int(tokens + i, json_chunk));
			++i;
		}
""", """		else if (cgltf_json_strcmp(tokens+i, json_chunk, "skin") == 0)
		{
			++i;
			CGLTF_CHECK_TOKTYPE(tokens[i], JSMN_PRIMITIVE);
			out_node->skin = CGLTF_PTRINDEX(cgltf_skin, cgltf_json_to_int(tokens + i, json_chunk));
			++i;
		}
		else if (cgltf_json_strcmp(tokens+i, json_chunk, "externalAsset") == 0) /* glTF 2.1 */
		{
			++i;
			CGLTF_CHECK_TOKTYPE(tokens[i], JSMN_PRIMITIVE);
			out_node->external_asset = CGLTF_PTRINDEX(cgltf_external_asset, cgltf_json_to_int(tokens + i, json_chunk));
			++i;
		}
""")

# ---------------------------------------------------------------- fixup
rep("""	for (cgltf_size i = 0; i < data->images_count; ++i)
	{
		CGLTF_PTRFIXUP(data->images[i].buffer_view, data->buffer_views, data->buffer_views_count);
	}
""", """	for (cgltf_size i = 0; i < data->images_count; ++i)
	{
		CGLTF_PTRFIXUP(data->images[i].buffer_view, data->buffer_views, data->buffer_views_count);
	}

	/* glTF 2.1 */
	for (cgltf_size i = 0; i < data->files_count; ++i)
	{
		CGLTF_PTRFIXUP(data->files[i].buffer_view, data->buffer_views, data->buffer_views_count);
	}

	for (cgltf_size i = 0; i < data->external_assets_count; ++i)
	{
		CGLTF_PTRFIXUP_REQ(data->external_assets[i].file, data->files, data->files_count);
	}
""")

rep("""		CGLTF_PTRFIXUP(data->nodes[i].light, data->lights, data->lights_count);
""", """		CGLTF_PTRFIXUP(data->nodes[i].light, data->lights, data->lights_count);
		CGLTF_PTRFIXUP(data->nodes[i].external_asset, data->external_assets, data->external_assets_count); /* glTF 2.1 */
""")

# ---------------------------------------------------------------- free
rep("""	data->memory.free_func(data->memory.user_data, data->images);
""", """	data->memory.free_func(data->memory.user_data, data->images);

	/* glTF 2.1 */
	for (cgltf_size i = 0; i < data->files_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->files[i].name);
		data->memory.free_func(data->memory.user_data, data->files[i].uri);
		data->memory.free_func(data->memory.user_data, data->files[i].mime_type);

		for (cgltf_size j = 0; j < data->files[i].aliases_count; ++j)
		{
			data->memory.free_func(data->memory.user_data, data->files[i].aliases[j]);
		}
		data->memory.free_func(data->memory.user_data, data->files[i].aliases);

		cgltf_free_extensions(data, data->files[i].extensions, data->files[i].extensions_count);
		cgltf_free_extras(data, &data->files[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->files);

	for (cgltf_size i = 0; i < data->external_assets_count; ++i)
	{
		data->memory.free_func(data->memory.user_data, data->external_assets[i].name);

		cgltf_free_extensions(data, data->external_assets[i].extensions, data->external_assets[i].extensions_count);
		cgltf_free_extras(data, &data->external_assets[i].extras);
	}

	data->memory.free_func(data->memory.user_data, data->external_assets);
""")


# ---------------------------------------------------------------- draco ids
rep("""				CGLTF_PTRFIXUP_REQ(data->meshes[i].primitives[j].draco_mesh_compression.buffer_view, data->buffer_views, data->buffer_views_count);
				for (cgltf_size m = 0; m < data->meshes[i].primitives[j].draco_mesh_compression.attributes_count; ++m)
				{
					CGLTF_PTRFIXUP_REQ(data->meshes[i].primitives[j].draco_mesh_compression.attributes[m].data, data->accessors, data->accessors_count);
				}
""", """				CGLTF_PTRFIXUP_REQ(data->meshes[i].primitives[j].draco_mesh_compression.buffer_view, data->buffer_views, data->buffer_views_count);
				/* osgEarth modification: KHR_draco_mesh_compression attribute values are Draco
				 * unique attribute ids, not accessor indices. They keep the CGLTF_PTRINDEX
				 * encoding (id + 1) in .data and must not be dereferenced. */
""")

# ---------------------------------------------------------------- double node transforms
# Based on upstream PR 239 (https://github.com/jkuhlmann/cgltf/pull/239). A 32-bit
# float rounds an ECEF translation by up to 0.25 m, which opens cracks between the
# tiles of a Google Photorealistic 3D Tiles tileset. Precision is lost at JSON parse
# time, so it has to be fixed here rather than in the caller.
rep("typedef float cgltf_float;\n",
    "typedef float cgltf_float;\n"
    "typedef double cgltf_double; /* osgEarth modification: node transforms */\n")

rep("""	cgltf_bool has_matrix;
	cgltf_float translation[3];
	cgltf_float rotation[4];
	cgltf_float scale[3];
	cgltf_float matrix[16];
""", """	cgltf_bool has_matrix;
	cgltf_double translation[3];
	cgltf_double rotation[4];
	cgltf_double scale[3];
	cgltf_double matrix[16];
""")

rep("""void cgltf_node_transform_local(const cgltf_node* node, cgltf_float* out_matrix);
void cgltf_node_transform_world(const cgltf_node* node, cgltf_float* out_matrix);
""", """void cgltf_node_transform_local(const cgltf_node* node, cgltf_double* out_matrix);
void cgltf_node_transform_world(const cgltf_node* node, cgltf_double* out_matrix);
""")

# Both transform functions do all of their arithmetic in the node's precision.
start = src.index("void cgltf_node_transform_local(const cgltf_node* node, cgltf_float* out_matrix)\n{")
end = src.index("static cgltf_ssize cgltf_component_read_integer(")
transforms = src[start:end]
for old, new in (("cgltf_float", "cgltf_double"), ("sizeof(float)", "sizeof(double)"), ("float ", "double "),
                 ("0.f", "0."), ("1.f", "1.")):
    transforms = transforms.replace(old, new)
assert "float" not in transforms and ".f" not in transforms
src = src[:start] + transforms + src[end:]

rep("static cgltf_bool cgltf_json_to_bool(jsmntok_t const* tok, const uint8_t* json_chunk)\n",
"""static cgltf_double cgltf_json_to_double(jsmntok_t const* tok, const uint8_t* json_chunk)
{
	CGLTF_CHECK_TOKTYPE(*tok, JSMN_PRIMITIVE);
	char tmp[128];
	int size = (size_t)(tok->end - tok->start) < sizeof(tmp) ? (int)(tok->end - tok->start) : (int)(sizeof(tmp) - 1);
	strncpy(tmp, (const char*)json_chunk + tok->start, size);
	tmp[size] = 0;
	return (cgltf_double)CGLTF_ATOF(tmp);
}

static cgltf_bool cgltf_json_to_bool(jsmntok_t const* tok, const uint8_t* json_chunk)
""")

rep("static int cgltf_parse_json_string(cgltf_options* options, jsmntok_t const* tokens, int i, const uint8_t* json_chunk, char** out_string)\n",
"""static int cgltf_parse_json_double_array(jsmntok_t const* tokens, int i, const uint8_t* json_chunk, cgltf_double* out_array, int size)
{
	CGLTF_CHECK_TOKTYPE(tokens[i], JSMN_ARRAY);
	if (tokens[i].size != size)
	{
		return CGLTF_ERROR_JSON;
	}
	++i;
	for (int j = 0; j < size; ++j)
	{
		CGLTF_CHECK_TOKTYPE(tokens[i], JSMN_PRIMITIVE);
		out_array[j] = cgltf_json_to_double(tokens + i, json_chunk);
		++i;
	}
	return i;
}

static int cgltf_parse_json_string(cgltf_options* options, jsmntok_t const* tokens, int i, const uint8_t* json_chunk, char** out_string)
""")

for field, size in (("translation", 3), ("rotation", 4), ("scale", 3), ("matrix", 16)):
    rep("i = cgltf_parse_json_float_array(tokens, i + 1, json_chunk, out_node->%s, %d);" % (field, size),
        "i = cgltf_parse_json_double_array(tokens, i + 1, json_chunk, out_node->%s, %d);" % (field, size))

open(DST, "w", encoding="utf-8", newline="").write(src)
print("patched", DST, len(src))

# ---------------------------------------------------------------- cgltf_write.h
src = open(WRITE_SRC, encoding="utf-8", newline="").read()

rep(" * Version: 1.15\n",
    " * Version: 1.15\n"
    " *\n"
    " * osgEarth modification: writes cgltf_node transforms as doubles. Search for\n"
    " * \"cgltf_double\" to find the patched regions.\n")

rep("""	#define CGLTF_DECIMAL_DIG 9
#endif
""", """	#define CGLTF_DECIMAL_DIG 9
#endif

#ifdef DBL_DECIMAL_DIG
	// DBL_DECIMAL_DIG is C11
	#define CGLTF_DBL_DECIMAL_DIG (DBL_DECIMAL_DIG)
#else
	#define CGLTF_DBL_DECIMAL_DIG 17
#endif
""")

rep("static bool cgltf_check_floatarray(const float* vals, int dim, float val) {\n",
"""static void cgltf_write_doublearrayprop(cgltf_write_context* context, const char* label, const cgltf_double* vals, cgltf_size dim)
{
	cgltf_write_indent(context);
	CGLTF_SPRINTF("\\"%s\\": [", label);
	for (cgltf_size i = 0; i < dim; ++i)
	{
		if (i != 0)
		{
			CGLTF_SPRINTF(", %.*g", CGLTF_DBL_DECIMAL_DIG, vals[i]);
		}
		else
		{
			CGLTF_SPRINTF("%.*g", CGLTF_DBL_DECIMAL_DIG, vals[i]);
		}
	}
	CGLTF_SPRINTF("]");
	context->needs_comma = 1;
}

static bool cgltf_check_floatarray(const float* vals, int dim, float val) {
""")

for field, size in (("matrix", 16), ("translation", 3), ("rotation", 4), ("scale", 3)):
    rep('cgltf_write_floatarrayprop(context, "%s", node->%s, %d);' % (field, field, size),
        'cgltf_write_doublearrayprop(context, "%s", node->%s, %d);' % (field, field, size))

open(WRITE_DST, "w", encoding="utf-8", newline="").write(src)
print("patched", WRITE_DST, len(src))
