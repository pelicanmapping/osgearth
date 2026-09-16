"""Patch upstream cgltf.h with native glTF 2.1 external-asset parsing.

Adds the core 2.1 properties (root "files", root "externalAssets", node
"externalAsset") to the single-pass jsmn parser, pointer fixup and free
paths, mirroring the style of the surrounding cgltf code so the patch stays
easy to re-apply on the next upstream release.
"""
import sys

SRC = sys.argv[1]
DST = sys.argv[2]

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
    " * patched regions.\n")

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

open(DST, "w", encoding="utf-8", newline="").write(src)
print("patched", DST, len(src))
