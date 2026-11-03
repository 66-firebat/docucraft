#pragma once

#ifdef HARDCOPY_HB_MUPDF

#define hb_buffer_create                    fzhb_buffer_create
#define hb_buffer_destroy                   fzhb_buffer_destroy
#define hb_buffer_add_utf8                  fzhb_buffer_add_utf8
#define hb_buffer_set_direction             fzhb_buffer_set_direction
#define hb_buffer_guess_segment_properties  fzhb_buffer_guess_segment_properties
#define hb_buffer_get_glyph_infos           fzhb_buffer_get_glyph_infos
#define hb_buffer_get_glyph_positions       fzhb_buffer_get_glyph_positions
#define hb_shape                            fzhb_shape
#define hb_feature_from_string              fzhb_feature_from_string
#define hb_ft_font_create_referenced        fzhb_ft_font_create_referenced
#define hb_ft_font_set_funcs                fzhb_ft_font_set_funcs
#define hb_font_destroy                     fzhb_font_destroy

#endif
