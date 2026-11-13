#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct stb_vorbis stb_vorbis;

typedef struct {
   char *alloc_buffer;
   int   alloc_buffer_length_in_bytes;
} stb_vorbis_alloc;

typedef struct {
   unsigned int sample_rate;
   int channels;
   unsigned int setup_memory_required;
   unsigned int setup_temp_memory_required;
   unsigned int temp_memory_required;
   int max_frame_size;
} stb_vorbis_info;

extern stb_vorbis * stb_vorbis_open_memory(const unsigned char *data, int len, int *error, const stb_vorbis_alloc *alloc_buffer);
extern stb_vorbis_info stb_vorbis_get_info(stb_vorbis *f);
extern int stb_vorbis_get_samples_short_interleaved(stb_vorbis *f, int channels, short *buffer, int num_shorts);
extern int stb_vorbis_seek_start(stb_vorbis *f);
extern void stb_vorbis_close(stb_vorbis *f);

#ifdef __cplusplus
}
#endif
