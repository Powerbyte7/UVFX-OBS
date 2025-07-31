/*
UVFX for OBS
Copyright (C) 2025 Powerbyte7 joranklaui@gmail.com

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>
#include <graphics/image-file.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <sys/stat.h>
#include <graphics/image-file.h>

#define blog(log_level, format, ...) \
	blog(log_level, "[uvfx_source: '%s'] " format, obs_source_get_name(filter->source), ##__VA_ARGS__)

#define debug(format, ...) blog(LOG_DEBUG, format, ##__VA_ARGS__)
#define info(format, ...) blog(LOG_INFO, format, ##__VA_ARGS__)
#define warn(format, ...) blog(LOG_WARNING, format, ##__VA_ARGS__)

struct uvfx_source {
	obs_source_t *source;
	gs_effect_t *effect;
	char *file;
	bool persistent;
	bool is_slide;
	bool linear_alpha;
	time_t file_timestamp;
	float update_time_elapsed;
	uint64_t last_time;
	bool active;
	bool restart_gif;
	gs_eparam_t *param_multiplier;
	volatile bool file_decoded;
	volatile bool texture_loaded;
	gs_image_file4_t if4;
};

static time_t get_modified_timestamp(const char *filename)
{
	struct stat stats;
	if (os_stat(filename, &stats) != 0)
		return -1;
	return stats.st_mtime;
}

static const char *uvfx_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("UFVFX");
}

void uvfx_source_preload_image(void *data)
{
	struct uvfx_source *filter = data;
	if (os_atomic_load_bool(&filter->file_decoded))
		return;

	filter->file_timestamp = get_modified_timestamp(filter->file);
	gs_image_file4_init(&filter->if4, filter->file,
			    filter->linear_alpha ? GS_IMAGE_ALPHA_PREMULTIPLY_SRGB : GS_IMAGE_ALPHA_PREMULTIPLY);
	os_atomic_set_bool(&filter->file_decoded, true);
}

static void uvfx_source_load_texture(struct uvfx_source *filter)
{
	if (os_atomic_load_bool(&filter->texture_loaded))
		return;

	debug("loading texture '%s'", filter->file);

	obs_enter_graphics();
	gs_image_file4_init_texture(&filter->if4);
	obs_leave_graphics();

	if (!filter->if4.image3.image2.image.loaded)
		warn("failed to load texture '%s'", filter->file);
	filter->update_time_elapsed = 0;
	os_atomic_set_bool(&filter->texture_loaded, true);
}

static void uvfx_source_unload(void *data)
{
	struct uvfx_source *filter = data;
	os_atomic_set_bool(&filter->file_decoded, false);
	os_atomic_set_bool(&filter->texture_loaded, false);

	obs_enter_graphics();
	gs_image_file4_free(&filter->if4);
	obs_leave_graphics();
}

static void uvfx_source_load(struct uvfx_source *filter)
{
	uvfx_source_unload(filter);

	if (filter->file && *filter->file) {
		uvfx_source_preload_image(filter);
		uvfx_source_load_texture(filter);
	}
}

static void uvfx_source_update(void *data, obs_data_t *settings)
{
	struct uvfx_source *filter = data;
	const char *file = obs_data_get_string(settings, "file");
	const bool unload = obs_data_get_bool(settings, "unload");
	const bool linear_alpha = obs_data_get_bool(settings, "linear_alpha");
	const bool is_slide = obs_data_get_bool(settings, "is_slide");

	if (filter->file)
		bfree(filter->file);
	filter->file = bstrdup(file);
	filter->persistent = !unload;
	filter->linear_alpha = linear_alpha;
	filter->is_slide = is_slide;

	if (is_slide)
		return;

	/* Load the image if the source is persistent or showing */
	if (filter->persistent || obs_source_showing(filter->source))
		uvfx_source_load(data);
	else
		uvfx_source_unload(data);
}

static void uvfx_source_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "unload", false);
	obs_data_set_default_bool(settings, "linear_alpha", false);
}

static void uvfx_source_show(void *data)
{
	struct uvfx_source *filter = data;

	if (!filter->persistent && !filter->is_slide)
		uvfx_source_load(filter);
}

static void uvfx_source_hide(void *data)
{
	struct uvfx_source *filter = data;

	if (!filter->persistent && !filter->is_slide)
		uvfx_source_unload(filter);
}

static void restart_gif(void *data)
{
	struct uvfx_source *filter = data;

	if (filter->if4.image3.image2.image.is_animated_gif) {
		filter->if4.image3.image2.image.cur_frame = 0;
		filter->if4.image3.image2.image.cur_loop = 0;
		filter->if4.image3.image2.image.cur_time = 0;

		obs_enter_graphics();
		gs_image_file4_update_texture(&filter->if4);
		obs_leave_graphics();

		filter->restart_gif = false;
	}
}

static void uvfx_source_activate(void *data)
{
	struct uvfx_source *filter = data;
	filter->restart_gif = true;
}

static void *uvfx_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct uvfx_source *filter = bzalloc(sizeof(struct uvfx_source));
	filter->source = source;

	char *effect_path = obs_module_file("uvfx.effect");

	if (effect_path == NULL) {
		warn("Effect not found!");

		warn("Maybe try %s", obs_get_module_data_path(obs_current_module()));
		
	}


	obs_enter_graphics();
	filter->effect = gs_effect_create_from_file(effect_path, NULL);
	obs_leave_graphics();

	bfree(effect_path);

	if (!filter->effect) {
		bfree(filter);
		return NULL;
	}

	filter->param_multiplier = gs_effect_get_param_by_name(filter->effect, "multiplier");

	uvfx_source_update(filter, settings);
	return filter;
}

static void uvfx_source_destroy(void *data)
{
	struct uvfx_source *filter = data;

	uvfx_source_unload(filter);

	if (filter->file)
		bfree(filter->file);
	bfree(filter);
}

static uint32_t uvfx_source_getwidth(void *data)
{
	struct uvfx_source *filter = data;
	return filter->if4.image3.image2.image.cx;
}

static uint32_t uvfx_source_getheight(void *data)
{
	struct uvfx_source *filter = data;
	return filter->if4.image3.image2.image.cy;
}

static const char *get_tech_name_and_multiplier(enum gs_color_space current_space, enum gs_color_space source_space,
						float *multiplier)
{
	const char *tech_name = "Draw";
	*multiplier = 1.f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawTonemap";
			break;
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
			break;
		case GS_CS_709_EXTENDED:
			break;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_SCRGB:
			break;
		}
	}

	return tech_name;
}

static void uvfx_source_render(void *data, gs_effect_t *effect)
{
	struct uvfx_source *filter = data;
	if (!os_atomic_load_bool(&filter->texture_loaded))
		return;

	struct gs_image_file *const uvs = &filter->if4.image3.image2.image;
	gs_texture_t *const uv_texture = uvs->texture;
	if (!uv_texture)
		return;

	obs_source_t *target = obs_filter_get_target(filter->source);
	uint32_t base_cx = obs_source_get_base_width(target);
	uint32_t base_cy = obs_source_get_base_height(target);

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};

	const enum gs_color_space source_space =
		obs_source_get_color_space(target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	float multiplier;
	const char *technique = get_tech_name_and_multiplier(gs_get_color_space(), source_space, &multiplier);
	const enum gs_color_format format = gs_get_format_from_space(source_space);

	if (obs_source_process_filter_begin_with_color_space(filter->source, format, source_space,
							     OBS_NO_DIRECT_RENDERING)) {
		gs_effect_set_float(filter->param_multiplier, multiplier);

		gs_eparam_t *const param = gs_effect_get_param_by_name(filter->effect, "uv_texture");
		gs_effect_set_texture(param, uv_texture);

		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

		obs_source_process_filter_tech_end(filter->source, filter->effect, base_cx, base_cy, technique);

		gs_blend_state_pop();
	}
}

static void uvfx_source_tick(void *data, float seconds)
{
	struct uvfx_source *filter = data;
	if (!os_atomic_load_bool(&filter->texture_loaded)) {
		if (os_atomic_load_bool(&filter->file_decoded))
			uvfx_source_load_texture(filter);
		else
			return;
	}

	uint64_t frame_time = obs_get_video_frame_time();

	filter->update_time_elapsed += seconds;

	if (obs_source_showing(filter->source)) {
		if (filter->update_time_elapsed >= 1.0f) {
			time_t t = get_modified_timestamp(filter->file);
			filter->update_time_elapsed = 0.0f;

			if (filter->file_timestamp != t) {
				uvfx_source_load(filter);
			}
		}
	}

	if (obs_source_showing(filter->source)) {
		if (!filter->active) {
			if (filter->if4.image3.image2.image.is_animated_gif)
				filter->last_time = frame_time;
			filter->active = true;
		}

		if (filter->restart_gif)
			restart_gif(filter);

	} else {
		if (filter->active) {
			restart_gif(filter);
			filter->active = false;
		}

		return;
	}

	if (filter->last_time && filter->if4.image3.image2.image.is_animated_gif) {
		uint64_t elapsed = frame_time - filter->last_time;
		bool updated = gs_image_file4_tick(&filter->if4, elapsed);

		if (updated) {
			obs_enter_graphics();
			gs_image_file4_update_texture(&filter->if4);
			obs_leave_graphics();
		}
	}

	filter->last_time = frame_time;
}

static const char *image_filter =
#ifdef _WIN32
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.jxr *.gif *.psd *.webp);;"
#else
	"All formats (*.bmp *.tga *.png *.jpeg *.jpg *.gif *.psd *.webp);;"
#endif
	"BMP Files (*.bmp);;"
	"Targa Files (*.tga);;"
	"PNG Files (*.png);;"
	"JPEG Files (*.jpeg *.jpg);;"
#ifdef _WIN32
	"JXR Files (*.jxr);;"
#endif
	"GIF Files (*.gif);;"
	"PSD Files (*.psd);;"
	"WebP Files (*.webp);;"
	"All Files (*.*)";

static obs_properties_t *uvfx_source_properties(void *data)
{
	UNUSED_PARAMETER(data);

	obs_properties_t *props = obs_properties_create();

	obs_properties_add_path(props, "file", obs_module_text("File"), OBS_PATH_FILE, image_filter, NULL);
	obs_properties_add_bool(props, "unload", obs_module_text("UnloadWhenNotShowing"));
	obs_properties_add_bool(props, "linear_alpha", obs_module_text("LinearAlpha"));

	return props;
}

uint64_t uvfx_source_get_memory_usage(void *data)
{
	struct uvfx_source *s = data;
	return s->if4.image3.image2.mem_usage;
}

static void missing_file_callback(void *src, const char *new_path, void *data)
{
	struct uvfx_source *s = src;

	obs_source_t *source = s->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_set_string(settings, "file", new_path);
	obs_source_update(source, settings);
	obs_data_release(settings);

	UNUSED_PARAMETER(data);
}

static obs_missing_files_t *uvfx_source_missingfiles(void *data)
{
	struct uvfx_source *s = data;
	obs_missing_files_t *files = obs_missing_files_create();

	if (strcmp(s->file, "") != 0) {
		if (!os_file_exists(s->file)) {
			obs_missing_file_t *file = obs_missing_file_create(s->file, missing_file_callback,
									   OBS_MISSING_FILE_SOURCE, s->source, NULL);

			obs_missing_files_add_file(files, file);
		}
	}

	return files;
}

static enum gs_color_space uvfx_source_get_color_space(void *data, size_t count,
						       const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);

	struct uvfx_source *const s = data;
	gs_image_file4_t *const if4 = &s->if4;
	return if4->image3.image2.image.texture ? if4->space : GS_CS_SRGB;
}

static struct obs_source_info uvfx_source_info = {
	.id = "uvfx_source",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_OUTPUT_VIDEO | OBS_SOURCE_SRGB,
	.get_name = uvfx_source_get_name,
	.create = uvfx_source_create,
	.destroy = uvfx_source_destroy,
	.update = uvfx_source_update,
	.get_defaults = uvfx_source_defaults,
	.show = uvfx_source_show,
	.hide = uvfx_source_hide,
	.get_width = uvfx_source_getwidth,
	.get_height = uvfx_source_getheight,
	.video_render = uvfx_source_render,
	.video_tick = uvfx_source_tick,
	.missing_files = uvfx_source_missingfiles,
	.get_properties = uvfx_source_properties,
	.icon_type = OBS_ICON_TYPE_IMAGE,
	.activate = uvfx_source_activate,
	.video_get_color_space = uvfx_source_get_color_space,

};

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

bool obs_module_load(void)
{
	obs_register_source(&uvfx_source_info);
	obs_log(LOG_INFO, "UVFX for OBS loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "UVFX for OBS unloaded");
}
