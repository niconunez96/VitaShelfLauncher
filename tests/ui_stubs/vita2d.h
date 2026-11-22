#ifndef VP_TEST_VITA2D_H
#define VP_TEST_VITA2D_H
#include <stddef.h>
#include <stdint.h>
typedef struct { void *surface; int w, h; int raw; } vita2d_texture;
typedef struct { int unused; } vita2d_pgf;
typedef struct { void *face; void *cairo_face; } vita2d_font;
typedef struct { float x,y,z; unsigned int color; } vita2d_color_vertex;
#define SCE_GXM_TEXTURE_FILTER_LINEAR 1
#define SCE_GXM_PRIMITIVE_TRIANGLE_FAN 6
void *vita2d_pool_memalign(unsigned int,unsigned int);
void vita2d_draw_array(int,const vita2d_color_vertex *,size_t);
void vita2d_draw_rectangle(float,float,float,float,unsigned int);
void vita2d_draw_fill_circle(float,float,float,unsigned int);
void vita2d_draw_line(float,float,float,float,unsigned int);
void vita2d_draw_texture_tint_scale(const vita2d_texture *,float,float,float,float,unsigned int);
void vita2d_draw_texture_tint_part_scale(const vita2d_texture *,float,float,float,float,float,float,float,float,unsigned int);
unsigned int vita2d_texture_get_width(const vita2d_texture *);
unsigned int vita2d_texture_get_height(const vita2d_texture *);
vita2d_texture *vita2d_load_PNG_file(const char *);
void vita2d_texture_set_filters(vita2d_texture *,int,int);
void vita2d_free_texture(vita2d_texture *);
int vita2d_pgf_text_width(vita2d_pgf *,float,const char *);
int vita2d_pgf_draw_text(vita2d_pgf *,int,int,unsigned int,float,const char *);
void vita2d_wait_rendering_done(void);
void vita2d_set_clip_rectangle(int,int,int,int);
void vita2d_enable_clipping(void);
void vita2d_disable_clipping(void);
vita2d_font *vita2d_load_font_file(const char *);
void vita2d_free_font(vita2d_font *);
int vita2d_font_text_width(vita2d_font *,unsigned int,const char *);
int vita2d_font_draw_text(vita2d_font *,int,int,unsigned int,unsigned int,const char *);
vita2d_texture *vita2d_create_empty_texture(unsigned int,unsigned int);
unsigned int vita2d_texture_get_stride(const vita2d_texture *);
void *vita2d_texture_get_datap(const vita2d_texture *);
#endif
