#include <stdio.h>
#include <inttypes.h>
#include <time.h>
#include <util/threading.h>
#include <obs-module.h>
#include <libjpeg/jpeglib.h>

OBS_DECLARE_MODULE()

struct frame_output_data {
  obs_output_t *output;
  const char *save_path;
  int quality;
  unsigned width;
  unsigned height;
  pthread_mutex_t write_mutex;
  bool active;
};

static const char *frame_output_name(void *unused)
{
  UNUSED_PARAMETER(unused);
  return "Frame Capture Output";
}

static void frame_output_update(void *data, obs_data_t *settings)
{
  struct frame_output_data *output = data;
  const char *save_path = obs_data_get_string(settings, "save_path");
  int quality = obs_data_get_int(settings, "quality");

  pthread_mutex_lock(&output->write_mutex);
  if (save_path)
    output->save_path = save_path;
  if (quality)
    output->quality = quality;
  pthread_mutex_unlock(&output->write_mutex);
}

static obs_properties_t *frame_output_properties(void *data)
{
  struct frame_output_data *output = data;
  obs_properties_t *props = obs_properties_create();

  obs_properties_add_path(props, "save_path", "SAVE_PATH", OBS_PATH_DIRECTORY, "", output->save_path);
  obs_properties_add_int(props, "quality", "QUALITY", 0, 100, 1);
  return props;
}

static bool frame_output_start(void *data)
{
  struct frame_output_data *output = data;
  if (output->active)
    return false;

  if (!output->save_path) {
    blog(LOG_DEBUG, "save_path must be set for frame capture output");
    return false;
  }

  video_t *video = obs_output_video(output->output);
  int format = video_output_get_format(video);
  if (format != VIDEO_FORMAT_RGBA) {
    blog(LOG_DEBUG, "invalid pixel format used for frame capture output, must be VIDEO_FORMAT_RGBA");
    return false;
  }

  output->width = (unsigned)video_output_get_width(video);
  output->height = (unsigned)video_output_get_height(video);
  output->active = true;

  if (!obs_output_can_begin_data_capture(output->output, OBS_OUTPUT_VIDEO))
    return false;
  obs_output_set_video_conversion(output->output, NULL);
  obs_output_begin_data_capture(output->output, 0);
  return true;
}

static void frame_output_stop(void *data, uint64_t ts)
{
  struct frame_output_data *output = data;
  UNUSED_PARAMETER(ts);

  pthread_mutex_lock(&output->write_mutex);
  if (output->active) {
    obs_output_end_data_capture(output->output);
    output->active = false;
  }
  pthread_mutex_unlock(&output->write_mutex);
}

static void *frame_output_create(obs_data_t *settings, obs_output_t *output)
{
  struct frame_output_data *data = bzalloc(sizeof(*data));
  pthread_mutex_init_value(&data->write_mutex);
  if (pthread_mutex_init(&data->write_mutex, NULL) != 0) {
    pthread_mutex_destroy(&data->write_mutex);
    bfree(data);
    return NULL;
  }
  data->output = output;
  data->quality = 90;
  data->active = false;
  frame_output_update(data, settings);
  return data;
}

static void frame_output_destroy(void *data)
{
  struct frame_output_data *output = data;
  frame_output_stop(output, 0);
  pthread_mutex_destroy(&output->write_mutex);
  bfree(output);
}

static void generate_filename(char *fname, const char *save_path) {
  time_t rawtime;
  struct tm * timeinfo;
  char timestring[18];

  time(&rawtime);
  timeinfo = gmtime(&rawtime);

  sprintf(timestring, "%04d%02d%02d%02d%02d%02d%03d", timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, 0);

  strcpy(fname, save_path);
  strcat(fname, "/");
  strcat(fname, timestring);
  strcat(fname, ".jpeg");
}

static void save_frame(struct video_data *frame, unsigned width, unsigned height, int quality, const char *fname)
{
  FILE *f = fopen(fname, "wb");

  struct jpeg_compress_struct cinfo;
  struct jpeg_error_mgr jerr;
  cinfo.err = jpeg_std_error(&jerr);
  jpeg_create_compress(&cinfo);
  jpeg_stdio_dest(&cinfo, f);

  cinfo.image_width = width;
  cinfo.image_height = height;
  cinfo.input_components = 3;
  cinfo.in_color_space = JCS_RGB;

  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, true);
  jpeg_start_compress(&cinfo, true);

  JSAMPROW row_ptr[1];
  uint8_t *row_buf = bzalloc(sizeof(uint8_t) * cinfo.image_width * 3);
  row_ptr[0] = &row_buf[0];

  while (cinfo.next_scanline < cinfo.image_height) {
    unsigned offset = cinfo.next_scanline * cinfo.image_width * 4;
    for (unsigned i = 0; i < cinfo.image_width; i++) {
      row_buf[i * 3] = frame->data[0][offset + (i * 4)];
      row_buf[(i * 3) + 1] = frame->data[0][offset + (i * 4) + 1];
      row_buf[(i * 3) + 2] = frame->data[0][offset + (i * 4) + 2];
    }
    jpeg_write_scanlines(&cinfo, row_ptr, 1);
  }

  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  bfree(row_buf);
  fclose(f);
}

static void frame_output_video(void *data, struct video_data *frame)
{
  struct frame_output_data *output = data;

  char fname[1024];
  pthread_mutex_lock(&output->write_mutex);
  generate_filename(fname, output->save_path);
  save_frame(frame, output->width, output->height, output->quality, fname);
  pthread_mutex_unlock(&output->write_mutex);
}

extern struct obs_output_info frame_output = {
  .id = "frame_output",
  .flags = OBS_OUTPUT_VIDEO,
  .get_name = frame_output_name,
  .create = frame_output_create,
  .destroy = frame_output_destroy,
  .update = frame_output_update,
  .start = frame_output_start,
  .stop = frame_output_stop,
  .raw_video = frame_output_video,
};

bool obs_module_load(void)
{
  obs_register_output(&frame_output);
  return true;
}