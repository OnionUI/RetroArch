/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2014 - Hans-Kristian Arntzen
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *  Copyright (C) 2011-2017 - Higor Euripedes
 *  Copyright (C) 2019-2021 - James Leaver
 *  Copyright (C)      2021 - John Parton
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL/SDL.h>
#include <SDL/SDL_video.h>

#include <gfx/video_frame.h>
#include <string/stdstring.h>
#include <encodings/utf.h>
#include <features/features_cpu.h>

#include "gfx.c"
#include "scaler_neon.c"
#include <signal.h>
#include <sys/mman.h>
#include <sys/time.h>

#ifdef HAVE_CONFIG_H
#include "../../config.h"
#endif

#ifdef HAVE_MENU
#include "../../menu/menu_driver.h"
#endif

#include "../../dingux/dingux_utils.h"

#include "../../verbosity.h"
#include "../../gfx/drivers_font_renderer/bitmap.h"
#include "../../configuration.h"
#include "../../file_path_special.h"
#include "../../paths.h"
#include "../../retroarch.h"
#include "../../runloop.h"

#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#define SDL_MIYOOMINI_WIDTH  640
#define SDL_MIYOOMINI_HEIGHT 480
#define RGUI_MENU_WIDTH  320
#define RGUI_MENU_HEIGHT 240
#define SDL_NUM_FONT_GLYPHS 256
#define OSD_TEXT_Y_MARGIN 4
#define OSD_TEXT_LINES_MAX 3	/* 1 .. 7 */
#define OSD_TEXT_LINE_LEN ((uint32_t)(RGUI_MENU_WIDTH / FONT_WIDTH_STRIDE)-1)
#define OSD_TEXT_LEN_MAX (OSD_TEXT_LINE_LEN * OSD_TEXT_LINES_MAX)
#define RGUI_MENU_STRETCH_FILE_PATH "/mnt/SDCARD/.tmp_update/config/RetroArch/.noMenuStretch"
#define FB_DEVICE_FILE_PATH "/dev/fb0"
#define NEW_RES_FILE_PATH "/tmp/new_res_available"

uint32_t res_x, res_y;
bool rgui_menu_stretch = true;
SDL_Rect rgui_menu_dest_rect;
typedef struct sdl_miyoomini_video sdl_miyoomini_video_t;
struct sdl_miyoomini_video
{
   SDL_Surface *screen;
   void (*scale_func)(void* data, void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp);
   /* Scaling/padding/cropping parameters */
   unsigned content_width;
   unsigned content_height;
   unsigned frame_width;
   unsigned frame_height;
   unsigned video_x;
   unsigned video_y;
   unsigned video_w;
   unsigned video_h;
   unsigned rotate;
   bool rgb32;
   bool menu_active;
   bool was_in_menu;
   retro_time_t last_frame_time;
   retro_time_t ff_frame_time_min;
   enum dingux_ipu_filter_type filter_type;
   bool vsync;
   bool keep_aspect;
   bool scale_integer;
   bool quitting;
   bitmapfont_lut_t *osd_font;
   uint32_t font_colour32;
   SDL_Surface *menuscreen;
   SDL_Surface *menuscreen_rgui;
#ifdef HAVE_OVERLAY
   SDL_Surface *overlay_surface;
#endif
   unsigned msg_count;
   char msg_tmp[OSD_TEXT_LEN_MAX];
};

/* Clear OSD text area, without video_rect, rotate180 */
static void sdl_miyoomini_clear_msgarea(void* buf, unsigned x, unsigned y, unsigned w, unsigned h, unsigned lines) {
   if ( ( x == 0 ) && ( w == res_x  ) && ( y == 0 ) && ( h == res_y ) ) return;

   uint32_t x0 = res_x - (x + w); /* left margin , right margin = x */
   uint32_t y0 = res_y - (y + h); /* top margin , bottom margin = y */
   uint32_t sl = x0 * sizeof(uint32_t); /* left buffer size */
   uint32_t sr = x * sizeof(uint32_t); /* right buffer size */
   uint32_t sw = w * sizeof(uint32_t); /* pitch */
   uint32_t ss = res_x * sizeof(uint32_t); /* stride */
   uint32_t vy = OSD_TEXT_Y_MARGIN + 2; /* clear start y offset */
   uint32_t vh = FONT_HEIGHT_STRIDE * 2 * lines - 2; /* clear height */
   uint32_t vh1 = (y0 < vy) ? 0 : (y0 - vy); if (vh1 > vh) vh1 = vh;
   uint32_t vh2 = vh - vh1;
   uint32_t ssl = ss * vh1 + sl;
   uint32_t srl = sr + sl;
   void* ofs = buf + vy * ss;

   if (ssl) memset(ofs, 0, ssl);
   ofs += ssl + sw;
   for (; vh2>1; vh2--, ofs += ss) { if (srl) memset(ofs, 0, srl); }
   if ((vh2) && (sr)) memset(ofs, 0, sr);
}

/* Print OSD text, flip callback, direct draw to framebuffer, 32bpp, 2x, rotate180 */
static void sdl_miyoomini_print_msg(void* data) {
   if (unlikely(!data)) return;
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;

   void *screen_buf;
   const char *str  = vid->msg_tmp;
   uint32_t str_len = strlen_size(str, OSD_TEXT_LEN_MAX);
   if (str_len) {
      screen_buf              = fb_addr + (vinfo.yoffset * res_x * sizeof(uint32_t));
      bool **font_lut         = vid->osd_font->lut;
      uint32_t str_lines      = (uint32_t)((str_len - 1) / OSD_TEXT_LINE_LEN) + 1;
      uint32_t str_counter    = OSD_TEXT_LINE_LEN;
      const int x_pos_def     = res_x - (FONT_WIDTH_STRIDE * 2);
      int x_pos               = x_pos_def;
      int y_pos               = OSD_TEXT_Y_MARGIN - 4 + (FONT_HEIGHT_STRIDE * 2 * str_lines);

      for (; str_len > 0; str_len--) {
         /* Check for out of bounds x coordinates */
         if (!str_counter--) {
            x_pos = x_pos_def; y_pos -= (FONT_HEIGHT_STRIDE * 2); str_counter = OSD_TEXT_LINE_LEN;
         }
         /* Deal with spaces first, for efficiency */
         if (*str == ' ') str++;
         else {
            uint32_t i, j;
            bool *symbol_lut;
            uint32_t symbol = utf8_walk(&str);

            /* Stupid hack: 'oe' ligatures are not really
             * standard extended ASCII, so we have to waste
             * CPU cycles performing a conversion from the
             * unicode values... */
            if (symbol == 339) /* Latin small ligature oe */
               symbol = 156;
            if (symbol == 338) /* Latin capital ligature oe */
               symbol = 140;

            if (symbol >= SDL_NUM_FONT_GLYPHS) continue;

            symbol_lut = font_lut[symbol];

            for (j = 0; j < FONT_HEIGHT; j++) {
               uint32_t buff_offset = ((y_pos - (j * 2) ) * res_x) + x_pos;

               for (i = 0; i < FONT_WIDTH; i++) {
                  if (*(symbol_lut + i + (j * FONT_WIDTH))) {
                     uint32_t *screen_buf_ptr = (uint32_t*)screen_buf + buff_offset - (i * 2);

                     /* Bottom shadow (1) */
                     screen_buf_ptr[+0] = 0;
                     screen_buf_ptr[+1] = 0;
                     screen_buf_ptr[+2] = 0;
                     screen_buf_ptr[+3] = 0;

                     /* Bottom shadow (2) */
                     screen_buf_ptr[res_x+0] = 0;
                     screen_buf_ptr[res_x+1] = 0;
                     screen_buf_ptr[res_x+2] = 0;
                     screen_buf_ptr[res_x+3] = 0;

                     /* Text pixel + right shadow (1) */
                     screen_buf_ptr[(res_x*2)+0] = 0;
                     screen_buf_ptr[(res_x*2)+1] = 0;
                     screen_buf_ptr[(res_x*2)+2] = vid->font_colour32;
                     screen_buf_ptr[(res_x*2)+3] = vid->font_colour32;

                     /* Text pixel + right shadow (2) */
                     screen_buf_ptr[(res_x*3)+0] = 0;
                     screen_buf_ptr[(res_x*3)+1] = 0;
                     screen_buf_ptr[(res_x*3)+2] = vid->font_colour32;
                     screen_buf_ptr[(res_x*3)+3] = vid->font_colour32;
                  }
               }
            }
         }
         x_pos -= FONT_WIDTH_STRIDE * 2;
      }
      vid->msg_count |= (str_lines << 6);
   }
   if (vid->msg_count & 7) {
      /* clear recent OSD text */
      screen_buf = fb_addr;
      uint32_t target_offset = vinfo.yoffset + res_y;
      if (target_offset != res_y * 3) screen_buf += target_offset * res_x * sizeof(uint32_t);
      sdl_miyoomini_clear_msgarea(screen_buf, vid->video_x, vid->video_y, vid->video_w, vid->video_h, vid->msg_count & 7);
   }
   vid->msg_count >>= 3;
}

/* Nearest neighbor scalers */
#define NN_SHIFT 16
void scalenn_16(void* data, void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp) {
   if (unlikely(!data||!sw||!sh)) return;
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   uint32_t dw = vid->video_w;
   uint32_t dh = vid->video_h;

   uint32_t x_step = (sw << NN_SHIFT) / dw + 1;
   uint32_t y_step = (sh << NN_SHIFT) / dh + 1;

   uint32_t in_stride  = sp >> 1;
   uint32_t out_stride = dp >> 1;

   uint16_t* in_ptr  = (uint16_t*)src;
   uint16_t* out_ptr = (uint16_t*)dst;

   uint32_t oy = 0;
   uint32_t y  = 0;

   /* Reading 16bits takes a little time,
      so try not to read as much as possible in the case of 16bpp */
   if (dh > sh) {
      if (dw > sw) {
         do {
            uint32_t col = dw;
            uint32_t ox  = 0;
            uint32_t x   = 0;

            uint16_t* optrtmp1 = out_ptr;

            uint16_t pix = in_ptr[0];
            do {
               uint32_t tx = x >> NN_SHIFT;
               if (tx != ox) {
                  pix = in_ptr[tx];
                  ox  = tx;
               }
               *(out_ptr++) = pix;
               x           += x_step;
            } while (--col);

            y += y_step;
            uint32_t ty = y >> NN_SHIFT;
            uint16_t* optrtmp2 = optrtmp1;
            for(; ty == oy; y += y_step, ty = y >> NN_SHIFT) {
               if (!--dh) return;
               optrtmp2 += out_stride;
               memcpy(optrtmp2, optrtmp1, dw << 1);
            }
            in_ptr += (ty - oy) * in_stride;
            out_ptr = optrtmp2 + out_stride;
            oy      = ty;
         } while (--dh);
      } else {
         do {
            uint32_t col = dw;
            uint32_t x   = 0;

            uint16_t* optrtmp1 = out_ptr;

            do {
               *(out_ptr++) = in_ptr[x >> NN_SHIFT];
               x           += x_step;
            } while (--col);

            y += y_step;
            uint32_t ty = y >> NN_SHIFT;
            uint16_t* optrtmp2 = optrtmp1;
            for(; ty == oy; y += y_step, ty = y >> NN_SHIFT) {
               if (!--dh) return;
               optrtmp2 += out_stride;
               memcpy(optrtmp2, optrtmp1, dw << 1);
            }
            in_ptr += (ty - oy) * in_stride;
            out_ptr = optrtmp2 + out_stride;
            oy      = ty;
         } while (--dh);
      }
   } else {
      if (dw > sw) {
         do {
            uint32_t col = dw;
            uint32_t ox  = 0;
            uint32_t x   = 0;

            uint16_t* optrtmp1 = out_ptr;

            uint16_t pix = in_ptr[0];
            do {
               uint32_t tx = x >> NN_SHIFT;
               if (tx != ox) {
                  pix = in_ptr[tx];
                  ox  = tx;
               }
               *(out_ptr++) = pix;
               x           += x_step;
            } while (--col);

            y += y_step;
            uint32_t ty = y >> NN_SHIFT;
            in_ptr += (ty - oy) * in_stride;
            out_ptr = optrtmp1 + out_stride;
            oy      = ty;
         } while (--dh);
      } else {
         do {
            uint32_t col = dw;
            uint32_t x   = 0;

            uint16_t* optrtmp1 = out_ptr;

            do {
               *(out_ptr++) = in_ptr[x >> NN_SHIFT];
               x           += x_step;
            } while (--col);

            y += y_step;
            uint32_t ty = y >> NN_SHIFT;
            in_ptr += (ty - oy) * in_stride;
            out_ptr = optrtmp1 + out_stride;
            oy      = ty;
         } while (--dh);
      }
   }
}

void scalenn_32(void* data, void* __restrict src, void* __restrict dst, uint32_t sw, uint32_t sh, uint32_t sp, uint32_t dp) {
   if (unlikely(!data||!sw||!sh)) return;
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   uint32_t dw = vid->video_w;
   uint32_t dh = vid->video_h;

   uint32_t x_step = (sw << NN_SHIFT) / dw + 1;
   uint32_t y_step = (sh << NN_SHIFT) / dh + 1;

   uint32_t in_stride  = sp >> 2;
   uint32_t out_stride = dp >> 2;

   uint32_t* in_ptr  = (uint32_t*)src;
   uint32_t* out_ptr = (uint32_t*)dst;

   uint32_t oy = 0;
   uint32_t y  = 0;

   /* Reading 32bit is fast when cached,
      so the x-axis is not considered in the case of 32bpp */
   if (dh > sh) {
      do {
         uint32_t col = dw;
         uint32_t x   = 0;

         uint32_t* optrtmp1 = out_ptr;

         do {
            *(out_ptr++) = in_ptr[x >> NN_SHIFT];
            x           += x_step;
         } while (--col);

         y += y_step;
         uint32_t ty = y >> NN_SHIFT;
         uint32_t* optrtmp2 = optrtmp1;
         for(; ty == oy; y += y_step, ty = y >> NN_SHIFT) {
            if (!--dh) return;
            optrtmp2 += out_stride;
            memcpy(optrtmp2, optrtmp1, dw << 2);
         }
         in_ptr += (ty - oy) * in_stride;
         out_ptr = optrtmp2 + out_stride;
         oy      = ty;
      } while (--dh);
   } else {
      do {
         uint32_t col = dw;
         uint32_t x   = 0;

         uint32_t* optrtmp1 = out_ptr;

         do {
            *(out_ptr++) = in_ptr[x >> NN_SHIFT];
            x           += x_step;
         } while (--col);

         y += y_step;
         uint32_t ty = y >> NN_SHIFT;
         in_ptr += (ty - oy) * in_stride;
         out_ptr = optrtmp1 + out_stride;
         oy      = ty;
      } while (--dh);
   }
}

/* Clear border x3 screens for framebuffer (rotate180) */
static void sdl_miyoomini_clear_border(void* buf, unsigned x, unsigned y, unsigned w, unsigned h) {
   if ( (x == 0) && (y == 0) && (w == res_x) && (h == res_y) ) return;
   if ( (w == 0) || (h == 0) ) { memset(buf, 0, res_x * res_y * sizeof(uint32_t) * 3); return; }

   uint32_t x0 = res_x - (x + w); /* left margin , right margin = x */
   uint32_t y0 = res_y - (y + h); /* top margin , bottom margin = y */
   uint32_t sl = x0 * sizeof(uint32_t); /* left buffer size */
   uint32_t sr = x * sizeof(uint32_t); /* right buffer size */
   uint32_t st = y0 * res_x * sizeof(uint32_t); /* top buffer size */
   uint32_t sb = y * res_x * sizeof(uint32_t); /* bottom buffer size */
   uint32_t srl = sr + sl;
   uint32_t stl = st + sl;
   uint32_t srb = sr + sb;
   uint32_t srbtl = srl + sb + st;
   uint32_t sw = w * sizeof(uint32_t); /* pitch */
   uint32_t ss = res_x * sizeof(uint32_t); /* stride */
   uint32_t i;

   if (stl) memset(buf, 0, stl); /* 1st top + 1st left */
   buf += stl + sw;
   for (i=h-1; i>0; i--, buf += ss) {
      if (srl) memset(buf, 0, srl); /* right + left */
   }
   if (srbtl) memset(buf, 0, srbtl); /* last right + bottom + top + 1st left */
   buf += srbtl + sw;
   for (i=h-1; i>0; i--, buf += ss) {
      if (srl) memset(buf, 0, srl); /* right + left */
   }
   if (srbtl) memset(buf, 0, srbtl); /* last right + bottom + top + 1st left */
   buf += srbtl + sw;
   for (i=h-1; i>0; i--, buf += ss) {
      if (srl) memset(buf, 0, srl); /* right + left */
   }
   if (srb) memset(buf, 0, srb); /* last right + last bottom */
}

static FILE *__get_cpuclock_file(void)
{
   FILE *fp = NULL;
   char config_directory[PATH_MAX_LENGTH];
   char cpuclock_config_path[PATH_MAX_LENGTH];
   rarch_system_info_t *system = &runloop_state_get_ptr()->system;
   const char *core_name = system ? system->info.library_name : NULL;

   if (!string_is_empty(core_name)) {
      /* Get base config directory */
      fill_pathname_application_special(config_directory, sizeof(config_directory), APPLICATION_SPECIAL_DIRECTORY_CONFIG);

      // Get core config path for cpuclock.txt
      fill_pathname_join_special_ext(cpuclock_config_path, config_directory, core_name, "cpuclock", ".txt", PATH_MAX_LENGTH);

      fp = fopen(cpuclock_config_path, "r");
      RARCH_LOG("[CPU]: Path %s: %s\n", fp ? "found" : "not found", cpuclock_config_path);
   }
   
   if (!fp) {
      fp = fopen("/proc/self/cwd/cpuclock.txt", "r");
      RARCH_LOG("[CPU]: Path %s: ./cpuclock.txt\n", fp ? "found" : "not found");
   }

   return fp;
}

/* Set cpuclock */
#define	BASE_REG_RIU_PA		(0x1F000000)
#define	BASE_REG_MPLL_PA	(BASE_REG_RIU_PA + 0x103000*2)
#define	PLL_SIZE		(0x1000)
static void set_cpuclock(int clock) {
	sync();
	int fd_mem = open("/dev/mem", O_RDWR);
	void* pll_map = mmap(0, PLL_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, fd_mem, BASE_REG_MPLL_PA);

	uint32_t post_div;
	if (clock >= 800000) post_div = 2;
	else if (clock >= 400000) post_div = 4;
	else if (clock >= 200000) post_div = 8;
	else post_div = 16;

	static const uint64_t divsrc = 432000000llu * 524288;
	uint32_t rate = (clock * 1000)/16 * post_div / 2;
	uint32_t lpf = (uint32_t)(divsrc / rate);
	volatile uint16_t* p16 = (uint16_t*)pll_map;

	uint32_t cur_post_div = (p16[0x232] & 0x0F) + 1;
	uint32_t tmp_post_div = cur_post_div;
	if (post_div > cur_post_div) {
		while (tmp_post_div != post_div) {
			tmp_post_div <<= 1;
			p16[0x232] = (p16[0x232] & 0xF0) | ((tmp_post_div-1) & 0x0F);
		}
	}

	p16[0x2A8] = 0x0000;
	p16[0x2AE] = 0x000F;
	p16[0x2A4] = lpf&0xFFFF;
	p16[0x2A6] = lpf>>16;
	p16[0x2B0] = 0x0001;
	p16[0x2B2] |= 0x1000;
	p16[0x2A8] = 0x0001;
	while( !(p16[0x2BA]&1) );
	p16[0x2A0] = lpf&0xFFFF;
	p16[0x2A2] = lpf>>16;

	if (post_div < cur_post_div) {
		while (tmp_post_div != post_div) {
			tmp_post_div >>= 1;
			p16[0x232] = (p16[0x232] & 0xF0) | ((tmp_post_div-1) & 0x0F);
		}
	}

	munmap(pll_map, PLL_SIZE);
	close(fd_mem);
}

/* Set CPU governor */
enum cpugov { PERFORMANCE = 0, POWERSAVE = 1, ONDEMAND = 2, USERSPACE = 3 };
static void sdl_miyoomini_set_cpugovernor(enum cpugov gov) {
   const char govstr[4][12] = { "performance", "powersave", "ondemand", "userspace" };
   const char fn_min_freq[] = "/sys/devices/system/cpu/cpufreq/policy0/scaling_min_freq";
   const char fn_governor[] = "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor";
   const char fn_setspeed[] = "/sys/devices/system/cpu/cpufreq/policy0/scaling_setspeed";
   static uint32_t minfreq = 0;
   FILE* fp;

   if (!minfreq) {
      /* save min_freq */
      fp = fopen(fn_min_freq, "r");
      if (fp) { fscanf(fp, "%d", &minfreq); fclose(fp); }
      /* set min_freq to lowest */
      fp = fopen(fn_min_freq, "w");
      if (fp) { fprintf(fp, "%d", 0); fclose(fp); }
   }

   if (gov == ONDEMAND) {
      /* revert min_freq */
      fp = fopen(fn_min_freq, "w");
      if (fp) { fprintf(fp, "%d", minfreq); fclose(fp); }
      minfreq = 0;
   }

   /* set cpu clock to value in cpuclock.txt */
   if (gov == PERFORMANCE) {
      fp = __get_cpuclock_file();
      if (fp) {
         int cpuclock = 0;
         fscanf(fp, "%d", &cpuclock); fclose(fp);
         if ((cpuclock >= 100)&&(cpuclock <= 2400)) {
            fp = fopen(fn_governor, "w");
            if (fp) { fwrite(govstr[USERSPACE], 1, strlen(govstr[USERSPACE]), fp); fclose(fp); }
            fp = fopen(fn_setspeed, "w");
            if (fp) { fprintf(fp, "%d", cpuclock * 1000); fclose(fp); }
            set_cpuclock(cpuclock * 1000);
            RARCH_LOG("[CPU]: Set clock: %d MHz\n", cpuclock);
            return;
         }
      }
   }

   /* set governor */
   fp = fopen(fn_governor, "w");
   if (fp) { fwrite(govstr[gov], 1, strlen(govstr[gov]), fp); fclose(fp); }
}

static void sdl_miyoomini_toggle_powersave(bool state) {
   sdl_miyoomini_set_cpugovernor(state ? POWERSAVE : PERFORMANCE);
}

static void sdl_miyoomini_sighandler(int sig) {
   switch (sig) {
   case SIGSTOP:
      sdl_miyoomini_toggle_powersave(true);
      break;
   case SIGCONT:
      sdl_miyoomini_toggle_powersave(false);
      break;
   default:
      break;
   }
}

static void sdl_miyoomini_init_font_color(sdl_miyoomini_video_t *vid) {
   settings_t *settings = config_get_ptr();
   uint32_t red         = 0xFF;
   uint32_t green       = 0xFF;
   uint32_t blue        = 0xFF;

   if (settings) {
      red   = (uint32_t)((settings->floats.video_msg_color_r * 255.0f) + 0.5f) & 0xFF;
      green = (uint32_t)((settings->floats.video_msg_color_g * 255.0f) + 0.5f) & 0xFF;
      blue  = (uint32_t)((settings->floats.video_msg_color_b * 255.0f) + 0.5f) & 0xFF;
   }

   /* Convert to XRGB8888 */
   vid->font_colour32 = (red << 16) | (green << 8) | blue;
}

static void sdl_miyoomini_gfx_free(void *data) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   if (unlikely(!vid)) return;

   if (GFX_GetFlipCallback()) {
      GFX_SetFlipCallback(NULL, NULL); usleep(0x2000); /* wait for finish callback */
   }
   GFX_WaitAllDone();
   if (vid->screen) GFX_FreeSurface(vid->screen);
   if (vid->menuscreen) GFX_FreeSurface(vid->menuscreen);
   if (vid->menuscreen_rgui) GFX_FreeSurface(vid->menuscreen_rgui);
#ifdef HAVE_OVERLAY
   if (vid->overlay_surface) { GFX_SetupOverlaySurface(NULL); GFX_FreeSurface(vid->overlay_surface); }
#endif
   GFX_Quit();

   if (vid->osd_font) bitmapfont_free_lut(vid->osd_font);

   free(vid);

   sdl_miyoomini_set_cpugovernor(ONDEMAND);
}

static void sdl_miyoomini_input_driver_init(
      const char *input_drv_name, const char *joypad_drv_name,
      input_driver_t **input, void **input_data) {
   /* Sanity check */
   if (!input || !input_data) return;

   *input      = NULL;
   *input_data = NULL;

   /* If input driver name is empty, cannot
    * initialise anything... */
   if (string_is_empty(input_drv_name)) return;

   signal(SIGSTOP, sdl_miyoomini_sighandler);
   signal(SIGCONT, sdl_miyoomini_sighandler);

   if (string_is_equal(input_drv_name, "sdl_dingux")) {
      *input_data = input_driver_init_wrap(&input_sdl_dingux,
            joypad_drv_name);
      if (*input_data) *input = &input_sdl_dingux;
      return;
   }

#if defined(HAVE_SDL) || defined(HAVE_SDL2)
   if (string_is_equal(input_drv_name, "sdl")) {
      *input_data = input_driver_init_wrap(&input_sdl,
            joypad_drv_name);
      if (*input_data) *input = &input_sdl;
      return;
   }
#endif

#if defined(HAVE_UDEV)
   if (string_is_equal(input_drv_name, "udev")) {
      *input_data = input_driver_init_wrap(&input_udev,
            joypad_drv_name);
      if (*input_data) *input = &input_udev;
      return;
   }
#endif

#if defined(__linux__)
   if (string_is_equal(input_drv_name, "linuxraw")) {
      *input_data = input_driver_init_wrap(&input_linuxraw,
            joypad_drv_name);
      if (*input_data) *input = &input_linuxraw;
      return;
   }
#endif
}

static void sdl_miyoomini_set_output(sdl_miyoomini_video_t* vid, unsigned width, unsigned height, bool rgb32) {
   vid->content_width  = width;
   vid->content_height = height;
   if (vid->rotate & 1) { width = vid->content_height; height = vid->content_width; }

   /* Calculate scaling factor */
   uint32_t xmul = (res_x<<16) / width;
   uint32_t ymul = (res_y<<16) / height;
   uint32_t mul_int = (xmul < ymul ? xmul : ymul)>>16;
   /* Change to aspect/fullscreen scaler when integer & screen size is over (no crop) */
   if (vid->scale_integer && mul_int) {
      /* Integer Scaling */
      vid->video_w = width * mul_int;
      vid->video_h = height * mul_int;
      if (!vid->keep_aspect) {
         if(!(width == res_x && height == res_y)) {
            /* Integer + Fullscreen , keep 4:3 for CRT console emulators */
            uint32_t Wx3 = vid->video_w * 3;
            uint32_t Hx4 = vid->video_h * 4;
            if (Wx3 != Hx4) {
               if (Wx3 > Hx4) vid->video_h = Wx3 / 4;
               else           vid->video_w = Hx4 / 3;
            }
         }
      }
      vid->video_x = (res_x - vid->video_w) >> 1;
      vid->video_y = (res_y - vid->video_h) >> 1;
   } else if (vid->keep_aspect) {
      /* Aspect Scaling */
      if (xmul > ymul) {
         vid->video_w  = (width * res_y) / height;
         vid->video_h = res_y;
         vid->video_x = (res_x - vid->video_w) >> 1;
         vid->video_y = 0;
      } else {
         vid->video_w  = res_x;
         vid->video_h = (height * res_x) / width;
         vid->video_x = 0;
         vid->video_y = (res_y - vid->video_h) >> 1;
      }
   } else {
      /* Fullscreen */
      vid->video_w = res_x;
      vid->video_h = res_y;
      vid->video_x = 0;
      vid->video_y = 0;
   }
   /* align to x4 bytes */
   if (!rgb32) { vid->video_x &= ~1; vid->video_w &= ~1; }

   /* Select scaler to use */
   uint32_t scale_xmul = 0, scale_ymul = 0;
   if ( (vid->filter_type != DINGUX_IPU_FILTER_NEAREST) || (vid->scale_integer && mul_int && vid->keep_aspect) ) {
      scale_xmul = scale_ymul = 1;
      if ( (vid->scale_integer) || (vid->filter_type == DINGUX_IPU_FILTER_BICUBIC) ) {
         // to be at least 80% of the post-scaling size
         scale_xmul = ((vid->video_w<<2)/5 / width) +1;
         scale_ymul = ((vid->video_h<<2)/5 / height) +1;
         if ((scale_xmul == 3)||(scale_xmul > 4)) scale_xmul = 4; // 4x scaler is faster than 3x
         if (scale_ymul > 4) scale_ymul = 4;
      }
   }
   vid->frame_width  = scale_xmul ? vid->content_width  * scale_xmul : vid->video_w;
   vid->frame_height = scale_ymul ? vid->content_height * scale_ymul : vid->video_h;

   static void (* const func[2][3][4])(void*, void* __restrict, void* __restrict, uint32_t, uint32_t, uint32_t, uint32_t) = {
      { { &scale1x1_16, &scale1x2_16, &scale1x3_16, &scale1x4_16 },
        { &scale2x1_16, &scale2x2_16, &scale2x3_16, &scale2x4_16 },
        { &scale4x1_16, &scale4x2_16, &scale4x3_16, &scale4x4_16 } },
      { { &scale1x1_32, &scale1x2_32, &scale1x3_32, &scale1x4_32 },
        { &scale2x1_32, &scale2x2_32, &scale2x3_32, &scale2x4_32 },
        { &scale4x1_32, &scale4x2_32, &scale4x3_32, &scale4x4_32 } }
   };

   if (!scale_xmul) {
      vid->scale_func = rgb32 ? scalenn_32 : scalenn_16;
   } else {
      vid->scale_func = func[rgb32?1:0][(scale_xmul>2)?2:scale_xmul-1][scale_ymul-1];
   }

   //RARCH_LOG("[SCALE] cw:%d ch:%d fw:%d fh:%d x:%d y:%d w:%d h:%d xmul:%d ymul:%d\n",vid->content_width,vid->content_height,
   //   vid->frame_width,vid->frame_height,vid->video_x,vid->video_y,vid->video_w,vid->video_h,scale_xmul,scale_ymul);

   /* Attempt to change video mode */
   GFX_WaitAllDone();
   if (vid->screen) GFX_FreeSurface(vid->screen);
   vid->screen = GFX_CreateRGBSurface(
         0, vid->frame_width, vid->frame_height, rgb32 ? 32 : 16, 0, 0, 0, 0);

   /* Check whether selected display mode is valid */
   if (unlikely(!vid->screen)) RARCH_ERR("[MI_GFX]: Failed to init GFX surface\n");
   /* Clear border */
   else if (!vid->menu_active) sdl_miyoomini_clear_border(fb_addr, vid->video_x, vid->video_y, vid->video_w, vid->video_h);
}

static void *sdl_miyoomini_gfx_init(const video_info_t *video,
      input_driver_t **input, void **input_data) {
   sdl_miyoomini_video_t *vid                    = NULL;
   uint32_t sdl_subsystem_flags                  = SDL_WasInit(0);
   settings_t *settings                          = config_get_ptr();
   const char *input_drv_name                 = settings->arrays.input_driver;
   const char *joypad_drv_name                = settings->arrays.input_joypad_driver;

   sdl_miyoomini_set_cpugovernor(PERFORMANCE);

   if (access(NEW_RES_FILE_PATH, F_OK) == 0) {
      RARCH_LOG("[MI_GFX]: 560p available, changing resolution\n");
      system("/mnt/SDCARD/.tmp_update/script/change_resolution.sh 752x560");
   }

    int fb = open(FB_DEVICE_FILE_PATH, O_RDWR);
    if (fb == -1) {
        RARCH_ERR("Error opening framebuffer device");
        return NULL;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &vinfo)) {
        RARCH_ERR("Error reading variable information");
        close(fb);
        return NULL;
    }

   res_x = vinfo.xres;
   res_y = vinfo.yres;
   close(fb);

   RARCH_LOG("[MI_GFX]: Resolution: %ux%u\n", res_x, res_y);

   if (access(RGUI_MENU_STRETCH_FILE_PATH, F_OK) == 0 || (res_x == 640 && res_y == 480)){
      RARCH_LOG("[MI_GFX]: Menu stretch disabled\n");
      rgui_menu_stretch = false;
   }

   rgui_menu_dest_rect = (SDL_Rect){(res_x - RGUI_MENU_WIDTH * 2) / 2, (res_y - RGUI_MENU_HEIGHT * 2) / 2, RGUI_MENU_WIDTH * 2, RGUI_MENU_HEIGHT * 2};
   /* Initialise graphics subsystem, if required */
   if (sdl_subsystem_flags == 0) {
      if (SDL_Init(SDL_INIT_VIDEO) < 0) return NULL;
   } else if ((sdl_subsystem_flags & SDL_INIT_VIDEO) == 0) {
      if (SDL_InitSubSystem(SDL_INIT_VIDEO) < 0) return NULL;
   }

   vid = (sdl_miyoomini_video_t*)calloc(1, sizeof(*vid));
   if (!vid) return NULL;

   GFX_Init();

   vid->menuscreen = GFX_CreateRGBSurface(
         0, res_x, res_y, 16, 0, 0, 0, 0);
   vid->menuscreen_rgui = GFX_CreateRGBSurface(
         0, RGUI_MENU_WIDTH, RGUI_MENU_HEIGHT, 16, 0, 0, 0, 0);

   if (!vid->menuscreen||!vid->menuscreen_rgui) {
      RARCH_ERR("[MI_GFX]: Failed to init GFX surface\n");
      goto error;
   }

   vid->content_width     = res_x;
   vid->content_height    = res_y;
   vid->rgb32             = video->rgb32;
   vid->vsync             = video->vsync;
   vid->keep_aspect       = settings->bools.video_dingux_ipu_keep_aspect;
   vid->scale_integer     = settings->bools.video_scale_integer;
   vid->filter_type       = (enum dingux_ipu_filter_type)settings->uints.video_dingux_ipu_filter_type;
   vid->menu_active       = false;
   vid->was_in_menu       = false;
   vid->quitting          = false;
   vid->ff_frame_time_min = 16667;

   sdl_miyoomini_set_output(vid, vid->content_width, vid->content_height, vid->rgb32);

   GFX_SetFlipFlags(vid->vsync ? GFX_BLOCKING : 0);

   sdl_miyoomini_input_driver_init(input_drv_name,
         joypad_drv_name, input, input_data);

   /* Initialise OSD font */
   sdl_miyoomini_init_font_color(vid);

   vid->osd_font = bitmapfont_get_lut();

   if (!vid->osd_font ||
       vid->osd_font->glyph_max <
            (SDL_NUM_FONT_GLYPHS - 1)) {
      RARCH_ERR("[SDL1]: Failed to init OSD font\n");
      goto error;
   }

   return vid;

error:
   sdl_miyoomini_gfx_free(vid);
   return NULL;
}

static bool sdl_miyoomini_gfx_frame(void *data, const void *frame,
      unsigned width, unsigned height, uint64_t frame_count,
      unsigned pitch, const char *msg, video_frame_info_t *video_info) {
   sdl_miyoomini_video_t* vid = (sdl_miyoomini_video_t*)data;

   /* Return early if:
    * - Input sdl_miyoomini_video_t struct is NULL
    *   (cannot realistically happen)
    * - Menu is inactive and input 'content' frame
    *   data is NULL (may happen when e.g. a running
    *   core skips a frame) */
   if (unlikely(!vid || (!frame && !vid->menu_active))) return true;

   /* If fast forward is currently active, we may
    * push frames at an 'unlimited' rate. Since the
    * display has a fixed refresh rate of 60 Hz, this
    * represents wasted effort. We therefore drop any
    * 'excess' frames in this case.
    * (Note that we *only* do this when fast forwarding.
    * Attempting this trick while running content normally
    * will cause bad frame pacing) */
   if (unlikely(video_info->input_driver_nonblock_state)) {
      retro_time_t current_time = cpu_features_get_time_usec();

      if ((current_time - vid->last_frame_time) < vid->ff_frame_time_min)
         return true;

      vid->last_frame_time = current_time;
   }

#ifdef HAVE_MENU
   menu_driver_frame(video_info->menu_is_alive, video_info);
#endif

   /* Render OSD text at flip */
   if (msg) {
      memcpy(vid->msg_tmp, msg, sizeof(vid->msg_tmp));
      GFX_SetFlipCallback(sdl_miyoomini_print_msg, vid);
   } else if (vid->msg_count) {
      vid->msg_tmp[0] = 0;
      GFX_SetFlipCallback(sdl_miyoomini_print_msg, vid);
   } else {
      GFX_SetFlipCallback(NULL, NULL);
   }

   if (likely(!vid->menu_active)) {
      /* Clear border if we were in the menu on the previous frame */
      if (unlikely(vid->was_in_menu)) {
         sdl_miyoomini_clear_border(fb_addr, vid->video_x, vid->video_y, vid->video_w, vid->video_h);
         vid->was_in_menu = false;
      }
      /* Update video mode if width/height have changed */
      if (unlikely( (vid->content_width  != width ) ||
                    (vid->content_height != height) )) {
         sdl_miyoomini_set_output(vid, width, height, vid->rgb32);
      }
      /* WaitAllDone to make sure the most recent frame is drawn complete */
      MI_GFX_WaitAllDone(FALSE, flipFence);
      /* SW Blit frame to GFX surface with scaling */
      vid->scale_func(vid, (void*)frame, vid->screen->pixels, width, height, pitch, vid->screen->pitch);
      /* HW Blit GFX surface to Framebuffer and Flip */
      GFX_UpdateRect(vid->screen, vid->video_x, vid->video_y, vid->video_w, vid->video_h);
   } else {
      SDL_SoftStretch(vid->menuscreen_rgui, NULL, vid->menuscreen, rgui_menu_stretch ? NULL : &rgui_menu_dest_rect);
      stOpt.eRotate = E_MI_GFX_ROTATE_180;
      GFX_Flip(vid->menuscreen);
      stOpt.eRotate = vid->rotate;
   }
   return true;
}

static void sdl_miyoomini_set_texture_enable(void *data, bool state, bool full_screen) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   if (unlikely(!vid)) return;

   if (state == vid->menu_active) return;
   vid->menu_active = state;

   sdl_miyoomini_toggle_powersave(state);

   if (state) {
      system("playActivity stop_all &");
      vid->was_in_menu = true;
   }
   else {
      system("playActivity resume &");
   }
}

static void sdl_miyoomini_set_texture_frame(void *data, const void *frame, bool rgb32,
      unsigned width, unsigned height, float alpha) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;

   if (unlikely( !vid || rgb32 || (width != RGUI_MENU_WIDTH) || (height != RGUI_MENU_HEIGHT))) return;

   memcpy_neon(vid->menuscreen_rgui->pixels, (void*)frame,
      RGUI_MENU_WIDTH * RGUI_MENU_HEIGHT * sizeof(uint16_t));
}

static void sdl_miyoomini_gfx_set_nonblock_state(void *data, bool toggle,
      bool adaptive_vsync_enabled, unsigned swap_interval) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   if (unlikely(!vid)) return;

   bool vsync            = !toggle;

   /* Check whether vsync status has changed */
   if (vid->vsync != vsync)
   {
      vid->vsync              = vsync;
      GFX_SetFlipFlags(vsync ? GFX_BLOCKING : 0);
   }
}

static void sdl_miyoomini_gfx_check_window(sdl_miyoomini_video_t *vid) {
   SDL_Event event;

   SDL_PumpEvents();
   while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, SDL_QUITMASK))
   {
      if (event.type != SDL_QUIT)
         continue;

      vid->quitting = true;
      sdl_miyoomini_set_cpugovernor(ONDEMAND);
      break;
   }
}

static bool sdl_miyoomini_gfx_alive(void *data) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   if (unlikely(!vid)) return false;

   sdl_miyoomini_gfx_check_window(vid);
   return !vid->quitting;
}

static bool sdl_miyoomini_gfx_focus(void *data) { return true; }
static bool sdl_miyoomini_gfx_suppress_screensaver(void *data, bool enable) { return false; }
static bool sdl_miyoomini_gfx_has_windowed(void *data) { return false; }

static void sdl_miyoomini_gfx_set_rotation(void *data, unsigned rotation) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   if (unlikely(!vid)) return;
   switch (rotation) {
      case 1:
         stOpt.eRotate = E_MI_GFX_ROTATE_90; break;
      case 2:
         stOpt.eRotate = E_MI_GFX_ROTATE_0; break;
      case 3:
         stOpt.eRotate = E_MI_GFX_ROTATE_270; break;
      default:
         stOpt.eRotate = E_MI_GFX_ROTATE_180; break;
   }
   if (vid->rotate != stOpt.eRotate) {
      vid->rotate = stOpt.eRotate;
      sdl_miyoomini_set_output(vid, vid->content_width, vid->content_height, vid->rgb32);
   }
}

static void sdl_miyoomini_gfx_viewport_info(void *data, struct video_viewport *vp) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   if (unlikely(!vid)) return;

   vp->x = vp->y = 0;
   vp->width  = vp->full_width  = vid->content_width;
   vp->height = vp->full_height = vid->content_height;
}

static float sdl_miyoomini_get_refresh_rate(void *data) { return 60.0f; }

static void sdl_miyoomini_set_filtering(void *data, unsigned index, bool smooth, bool ctx_scaling) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   settings_t *settings       = config_get_ptr();
   if (unlikely(!vid || !settings)) return;

   enum dingux_ipu_filter_type ipu_filter_type = (settings) ?
         (enum dingux_ipu_filter_type)settings->uints.video_dingux_ipu_filter_type :
         DINGUX_IPU_FILTER_BICUBIC;

   /* Update software filter setting, if required */
   if (vid->filter_type != ipu_filter_type) {
      vid->filter_type = ipu_filter_type;
      sdl_miyoomini_set_output(vid, vid->content_width, vid->content_height, vid->rgb32);
   }
}

static void sdl_miyoomini_apply_state_changes(void *data) {
   sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t*)data;
   settings_t *settings       = config_get_ptr();
   if (unlikely(!vid || !settings)) return;

   bool keep_aspect       = (settings) ? settings->bools.video_dingux_ipu_keep_aspect : true;
   bool integer_scaling   = (settings) ? settings->bools.video_scale_integer : false;

   if ((vid->keep_aspect != keep_aspect) ||
       (vid->scale_integer != integer_scaling)) {
      vid->keep_aspect   = keep_aspect;
      vid->scale_integer = integer_scaling;

      /* Aspect/scaling changes require all frame
       * dimension/padding/cropping parameters to
       * be recalculated. Easiest method is to just
       * (re-)set the current output video mode */
      sdl_miyoomini_set_output(vid, vid->content_width, vid->content_height, vid->rgb32);
   }
}

static uint32_t sdl_miyoomini_get_flags(void *data) { return 0; }

static const video_poke_interface_t sdl_miyoomini_poke_interface = {
   sdl_miyoomini_get_flags,
   NULL, /* load_texture */
   NULL, /* unload_texture */
   NULL, /* set_video_mode */
   sdl_miyoomini_get_refresh_rate,
   sdl_miyoomini_set_filtering,
   NULL, /* get_video_output_size */
   NULL, /* get_video_output_prev */
   NULL, /* get_video_output_next */
   NULL, /* get_current_framebuffer */
   NULL, /* get_proc_address */
   NULL, /* set_aspect_ratio */
   sdl_miyoomini_apply_state_changes,
   sdl_miyoomini_set_texture_frame,
   sdl_miyoomini_set_texture_enable,
   NULL, /* set_osd_msg */
   NULL, /* sdl_show_mouse */
   NULL, /* sdl_grab_mouse_toggle */
   NULL, /* get_current_shader */
   NULL, /* get_current_software_framebuffer */
   NULL, /* get_hw_render_interface */
   NULL, /* set_hdr_max_nits */
   NULL, /* set_hdr_paper_white_nits */
   NULL, /* set_hdr_contrast */
   NULL  /* set_hdr_expand_gamut */
};

static void sdl_miyoomini_get_poke_interface(void *data, const video_poke_interface_t **iface) {
   *iface = &sdl_miyoomini_poke_interface;
}

static bool sdl_miyoomini_gfx_set_shader(void *data,
      enum rarch_shader_type type, const char *path) { return false; }

#ifdef HAVE_OVERLAY

static void sdl_miyoomini_overlay_enable(void *data, bool state) {
	sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t *)data;
	if (!vid) return;

	if ((state)&&(vid->overlay_surface)) GFX_SetupOverlaySurface(vid->overlay_surface);
	else GFX_SetupOverlaySurface(NULL);
}

static bool sdl_miyoomini_overlay_load(void *data, const void *image_data, unsigned num_images) {
	sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t *)data;
	if (!vid) return false;

	struct texture_image *images = (struct texture_image *)image_data;
	void* pixels = images[0].pixels;
	uint32_t width = images[0].width;
	uint32_t height = images[0].height;

	if (vid->overlay_surface) GFX_FreeSurface(vid->overlay_surface);
	SDL_Surface *ostmp = SDL_CreateRGBSurfaceFrom(pixels, width, height, 32, width*4,
				0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	SDL_Surface *ostmp2 = GFX_DuplicateSurface(ostmp);
	SDL_FreeSurface(ostmp);
	vid->overlay_surface = GFX_CreateRGBSurface(0, res_x, res_y, 32,
				0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000);
	ostmp2->flags &= ~SDL_SRCALPHA;
	GFX_BlitSurfaceRotate(ostmp2, NULL, vid->overlay_surface, NULL, 2);
	GFX_FreeSurface(ostmp2);

	settings_t *settings = config_get_ptr();
	vid->overlay_surface->flags |= SDL_SRCALPHA;
	vid->overlay_surface->format->alpha = (settings) ? settings->floats.input_overlay_opacity * 0xFF : 255;
	GFX_SetupOverlaySurface(vid->overlay_surface);

	return true;
}

static void sdl_miyoomini_overlay_tex_geom(void *data, unsigned idx, float x, float y, float w, float h) { }
static void sdl_miyoomini_overlay_vertex_geom(void *data, unsigned idx, float x, float y, float w, float h) { }
static void sdl_miyoomini_overlay_full_screen(void *data, bool enable) { }

static void sdl_miyoomini_overlay_set_alpha(void *data, unsigned idx, float mod) {
	sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t *)data;
	if ((!idx)&&(vid)&&(vid->overlay_surface)) {
		uint8_t value = mod * 0xFF;
		if (!(vid->overlay_surface->flags & SDL_SRCALPHA)||(vid->overlay_surface->format->alpha != value)) {
			vid->overlay_surface->format->alpha = value;
			GFX_SetupOverlaySurface(vid->overlay_surface);
		}
	}
	return;
}

static const video_overlay_interface_t sdl_miyoomini_overlay = {
	sdl_miyoomini_overlay_enable,
	sdl_miyoomini_overlay_load,
	sdl_miyoomini_overlay_tex_geom,
	sdl_miyoomini_overlay_vertex_geom,
	sdl_miyoomini_overlay_full_screen,
	sdl_miyoomini_overlay_set_alpha,
};

void sdl_miyoomini_gfx_get_overlay_interface(void *data, const video_overlay_interface_t **iface)
{
    sdl_miyoomini_video_t *vid = (sdl_miyoomini_video_t *)data;
    if (!vid) return;
    *iface = &sdl_miyoomini_overlay;
}

#endif

video_driver_t video_sdl_dingux = {
   sdl_miyoomini_gfx_init,
   sdl_miyoomini_gfx_frame,
   sdl_miyoomini_gfx_set_nonblock_state,
   sdl_miyoomini_gfx_alive,
   sdl_miyoomini_gfx_focus,
   sdl_miyoomini_gfx_suppress_screensaver,
   sdl_miyoomini_gfx_has_windowed,
   sdl_miyoomini_gfx_set_shader,
   sdl_miyoomini_gfx_free,
   "sdl_dingux",
   NULL, /* set_viewport */
   sdl_miyoomini_gfx_set_rotation,
   sdl_miyoomini_gfx_viewport_info,
   NULL, /* read_viewport  */
   NULL, /* read_frame_raw */
#ifdef HAVE_OVERLAY
   sdl_miyoomini_gfx_get_overlay_interface,
#endif
   sdl_miyoomini_get_poke_interface
};
