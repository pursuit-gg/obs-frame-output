#include <stdio.h>
#include <shlobj.h>
#include <inttypes.h>
#include <util/threading.h>
#include <obs-module.h>
#include <libjpeg/jpeglib.h>

OBS_DECLARE_MODULE()

struct frame_output_data {
  obs_output_t *output;

  wchar_t *save_path;
  wchar_t *current_folder;
  int frame_count;
  uint32_t width;
  uint32_t height;
  pthread_mutex_t write_mutex;
  bool active;
};

static const char *frame_output_name(void *unused)
{
  UNUSED_PARAMETER(unused);
  return "Pursuit Frame Output";
}

static void frame_output_update(void *data, obs_data_t *settings)
{
  UNUSED_PARAMETER(data);
  UNUSED_PARAMETER(settings);
}

static obs_properties_t *frame_output_properties(void *data)
{
  struct frame_output_data *output = data;
  obs_properties_t *props = obs_properties_create();

  return props;
}

void frame_output_defaults(obs_data_t* defaults) {
  UNUSED_PARAMETER(defaults);
}

static void generate_folder(SYSTEMTIME systemtime, wchar_t *folder, wchar_t *save_path)
{
  wchar_t dirname[MAX_PATH];

  swprintf_s(folder, sizeof(wchar_t) * 18, L"%04d%02d%02d%02d%02d%02d%03d", systemtime.wYear, systemtime.wMonth, systemtime.wDay, systemtime.wHour, systemtime.wMinute, systemtime.wSecond, systemtime.wMilliseconds);
  wcscpy_s(dirname, sizeof(dirname), save_path);
  wcscat_s(dirname, sizeof(dirname), L"/");
  wcscat_s(dirname, sizeof(dirname), folder);

  _wmkdir(dirname);
}

static void generate_filename(SYSTEMTIME systemtime, wchar_t *fname, wchar_t *folder, wchar_t *save_path)
{
  wchar_t timestring[18];

  swprintf_s(timestring, sizeof(timestring), L"%04d%02d%02d%02d%02d%02d%03d", systemtime.wYear, systemtime.wMonth, systemtime.wDay, systemtime.wHour, systemtime.wMinute, systemtime.wSecond, systemtime.wMilliseconds);
  wcscpy_s(fname, sizeof(wchar_t) * MAX_PATH, save_path);
  wcscat_s(fname, sizeof(wchar_t) * MAX_PATH, L"/");
  wcscat_s(fname, sizeof(wchar_t) * MAX_PATH, folder);
  wcscat_s(fname, sizeof(wchar_t) * MAX_PATH, L"/");
  wcscat_s(fname, sizeof(wchar_t) * MAX_PATH, timestring);
  wcscat_s(fname, sizeof(wchar_t) * MAX_PATH, L".jpeg");
}

static void finish_folder(wchar_t *folder, wchar_t *save_path)
{
  wchar_t fname[MAX_PATH];

  if (folder) {
    wcscpy_s(fname, sizeof(fname), save_path);
    wcscat_s(fname, sizeof(fname), L"/");
    wcscat_s(fname, sizeof(fname), folder);
    wcscat_s(fname, sizeof(fname), L"/done");

    FILE *f = _wfopen(fname, L"wb");
    fclose(f);
  }
}

static bool frame_output_start(void *data)
{
  struct frame_output_data *output = data;
  if (output->active)
    return false;

  video_t *video = obs_output_video(output->output);
  int format = video_output_get_format(video);
  if (format != VIDEO_FORMAT_RGBA) {
    blog(LOG_DEBUG, "invalid pixel format used for pursuit frame capture output, must be VIDEO_FORMAT_RGBA");
    return false;
  }

  output->width = video_output_get_width(video);
  output->height = video_output_get_height(video);
  output->frame_count = 0;
  bfree(output->current_folder);
  output->current_folder = NULL;
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
    finish_folder(output->current_folder, output->save_path);
    output->active = false;
  }
  pthread_mutex_unlock(&output->write_mutex);
}

static void *frame_output_create(obs_data_t *settings, obs_output_t *output)
{
  struct frame_output_data *data = bzalloc(sizeof(*data));
  wchar_t *appDataPath = bzalloc(sizeof(wchar_t) * MAX_PATH);
  wchar_t *foundPath = 0;
  HRESULT hr = SHGetKnownFolderPath(&FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, NULL, &foundPath);
  if (hr != S_OK) {
    return NULL;
  }
  wcscpy_s(appDataPath, sizeof(wchar_t) * MAX_PATH, foundPath);
  wcscat_s(appDataPath, sizeof(wchar_t) * MAX_PATH, L"/Pursuit");
  _wmkdir(appDataPath);
  wcscat_s(appDataPath, sizeof(wchar_t) * MAX_PATH, L"/Captures");
  _wmkdir(appDataPath);
  CoTaskMemFree(foundPath);

  pthread_mutex_init_value(&data->write_mutex);
  if (pthread_mutex_init(&data->write_mutex, NULL) != 0) {
    pthread_mutex_destroy(&data->write_mutex);
    bfree(data);
    return NULL;
  }
  data->output = output;
  data->active = false;
  data->save_path = appDataPath;
  data->frame_count = 0;
  data->current_folder = NULL;

  frame_output_update(data, settings);
  return data;
}

static void frame_output_destroy(void *data)
{
  struct frame_output_data *output = data;

  if (output) {
    frame_output_stop(output, 0);
    pthread_mutex_destroy(&output->write_mutex);
    bfree(output->current_folder);
    bfree(output->save_path);
    bfree(output);
  }
}

static void save_frame(struct video_data *frame, uint32_t width, uint32_t height, wchar_t *fname)
{
  FILE *f = _wfopen(fname, L"wb");

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
  jpeg_set_quality(&cinfo, 90, true);
  jpeg_start_compress(&cinfo, true);

  JSAMPROW row_ptr[1];
  uint8_t *row_buf = bzalloc(sizeof(uint8_t) * cinfo.image_width * 3);
  row_ptr[0] = &row_buf[0];

  while (cinfo.next_scanline < cinfo.image_height) {
    uint32_t offset = cinfo.next_scanline * frame->linesize[0];
    for (uint32_t i = 0; i < cinfo.image_width; i++) {
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

  SYSTEMTIME systemtime;
  wchar_t fname[MAX_PATH];
  GetSystemTime(&systemtime);

  pthread_mutex_lock(&output->write_mutex);
  if (output->current_folder == NULL || output->frame_count == 60) {
    finish_folder(output->current_folder, output->save_path);
    wchar_t *folder = bzalloc(sizeof(wchar_t) * 18);
    generate_folder(systemtime, folder, output->save_path);
    bfree(output->current_folder);
    output->current_folder = folder;
    output->frame_count = 0;
  }
  generate_filename(systemtime, fname, output->current_folder, output->save_path);
  save_frame(frame, output->width, output->height, fname);
  output->frame_count = output->frame_count + 1;
  pthread_mutex_unlock(&output->write_mutex);
}

extern struct obs_output_info frame_output = {
  .id = "pursuit_frame_output",
  .flags = OBS_OUTPUT_VIDEO,
  .get_name = frame_output_name,
  .get_properties = frame_output_properties,
  .get_defaults = frame_output_defaults,
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