#if defined(MIYOOMINI)

#include "miyoo.h"
#include "configuration.h"
#include "file/config_file.h"
#include "file_path_special.h"
#include "gfx/video_driver.h"
#include "paths.h"
#include "runloop.h"
#include "string/stdstring.h"
#include "verbosity.h"
#include <fcntl.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int res_x = 0, res_y = 0;

/**
 * @brief Displays an on-screen notification of the current scaling option.
 * 
 * The 4 states are:
 * 1. Integer scaling: OFF (Original) - resolution
 * 2. Integer scaling: OFF (4:3) - resolution
 * 3. Integer scaling: ON (Original) - resolution
 * 4. Integer scaling: ON (4:3) - resolution
 * 
 * @param settings 
 */
static void show_miyoo_fullscreen_notification(settings_t *settings)
{
    char msg[PATH_MAX_LENGTH];
    struct retro_message_ext msg_obj = {0};

    msg[0] = '\0';

    if (res_x == 0 || res_y == 0) {
        const char *fb_device = "/dev/fb0";
        int fb = open(fb_device, O_RDWR);

        if (fb == -1) {
            RARCH_ERR("Error opening framebuffer device");
        }
        else {
            struct fb_var_screeninfo vinfo;
            if (ioctl(fb, FBIOGET_VSCREENINFO, &vinfo)) {
                RARCH_ERR("Error reading variable information");
                close(fb);
            }
            else {
                res_x = vinfo.xres;
                res_y = vinfo.yres;
            }
        }
    }

    snprintf(msg, sizeof(msg), "Integer scaling: %s (%s) - %dx%d",
             settings->bools.video_scale_integer ? "ON" : "OFF",
             settings->bools.video_dingux_ipu_keep_aspect ? "Original" : "4:3",
             res_x, res_y);

    msg_obj.msg = msg;
    msg_obj.duration = 1000;
    msg_obj.priority = 3;
    msg_obj.level = RETRO_LOG_INFO;
    msg_obj.target = RETRO_MESSAGE_TARGET_ALL;
    msg_obj.type = RETRO_MESSAGE_TYPE_STATUS;
    msg_obj.progress = -1;

    runloop_environment_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg_obj);
}

/**
 * @brief Get the override path for a specific type
 * 
 * @param override_path Output parameter for override path
 * @param type Override type
 */
static void get_override_path(char *override_path, enum override_type type)
{
    char config_directory[PATH_MAX_LENGTH];
    char content_dir_name[PATH_MAX_LENGTH];
    rarch_system_info_t *system = &runloop_state_get_ptr()->system;
    const char *game_name = NULL;
    const char *core_name = system ? system->info.library_name : NULL;
    const char *rarch_path_basename = path_get(RARCH_PATH_BASENAME);
    bool has_content = !string_is_empty(rarch_path_basename);

    /* > Cannot save an override if we have no core
    * > Cannot save a per-game or per-content-directory
    *   override if we have no content */
    if (string_is_empty(core_name) || (!has_content && type != OVERRIDE_CORE))
        return;

    /* Get base config directory */
    fill_pathname_application_special(config_directory,
                                      sizeof(config_directory),
                                      APPLICATION_SPECIAL_DIRECTORY_CONFIG);

    switch (type) {
    case OVERRIDE_CORE:
        fill_pathname_join_special_ext(override_path,
                                       config_directory, core_name,
                                       core_name,
                                       FILE_PATH_CONFIG_EXTENSION,
                                       PATH_MAX_LENGTH);
        break;
    case OVERRIDE_GAME:
        game_name = path_basename_nocompression(rarch_path_basename);
        fill_pathname_join_special_ext(override_path,
                                       config_directory, core_name,
                                       game_name,
                                       FILE_PATH_CONFIG_EXTENSION,
                                       PATH_MAX_LENGTH);
        break;
    case OVERRIDE_CONTENT_DIR:
        fill_pathname_parent_dir_name(content_dir_name,
                                      rarch_path_basename, sizeof(content_dir_name));
        fill_pathname_join_special_ext(override_path,
                                       config_directory, core_name,
                                       content_dir_name,
                                       FILE_PATH_CONFIG_EXTENSION,
                                       PATH_MAX_LENGTH);
        break;
    case OVERRIDE_NONE:
    default:
        break;
    }
}

/**
 * @brief Checks if a config override of [type] exists and contains either the "keep aspect" or "integer scaling" option
 * 
 * @param override_path Override path
 * @return true Config override contains scaling options - use it
 * @return false Config override does not contain scaling options
 */
static bool check_config_has_scaling(const char *override_path)
{
    bool ret = false;
    config_file_t *conf = NULL;

    if ((conf = config_file_new_from_path_to_string(override_path))) {
        ret = !!config_get_entry(conf, "video_dingux_ipu_keep_aspect") || !!config_get_entry(conf, "video_scale_integer");
        config_file_free(conf);
    }

    return ret;
}

/**
 * @brief Saves the "keep aspect" and "integer scaling" options as a config override.
 * 
 * @param settings 
 * @return true Config override was saved successfully
 * @return false Config override was not saved
 */
static bool write_core_override_aspect_scale(settings_t *settings)
{
    char override_path[PATH_MAX_LENGTH] = {0};
    config_file_t *conf = NULL;

    get_override_path(override_path, OVERRIDE_GAME);

    if (!path_is_directory(override_path) || !check_config_has_scaling(override_path)) {
        get_override_path(override_path, OVERRIDE_CONTENT_DIR);

        if (!path_is_directory(override_path)) {
            get_override_path(override_path, OVERRIDE_CORE);
        }
    }

    if (string_is_empty(override_path))
        return false;

    // Use existing override file, if exists, or create new config file
    if (!(conf = config_file_new_from_path_to_string(override_path)))
        conf = config_file_new_alloc();

    // Set the two overrides - leave everything else as-is
    config_set_string(conf, "video_dingux_ipu_keep_aspect",
                      (settings->bools.video_dingux_ipu_keep_aspect) ? "true" : "false");

    config_set_string(conf, "video_scale_integer",
                      (settings->bools.video_scale_integer) ? "true" : "false");

    // Write override file
    bool ret = config_file_write(conf, override_path, false);
    config_file_free(conf);

    return ret;
}

/**
 * @brief Toggle scaling options.
 * 
 * The 4 states are:
 * 1. Integer scaling: OFF (Original)
 * 2. Integer scaling: OFF (4:3)
 * 3. Integer scaling: ON (Original)
 * 4. Integer scaling: ON (4:3)
 * 
 * @param settings 
 */
void miyoo_event_fullscreen_impl(settings_t *settings)
{
    settings->bools.video_dingux_ipu_keep_aspect = !settings->bools.video_dingux_ipu_keep_aspect;
    if (settings->bools.video_dingux_ipu_keep_aspect) {
        settings->bools.video_scale_integer = !settings->bools.video_scale_integer;
    }

    video_driver_apply_state_changes();

    show_miyoo_fullscreen_notification(settings);
    write_core_override_aspect_scale(settings);
}

#endif
