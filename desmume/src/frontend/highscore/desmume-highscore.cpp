#include "desmume-highscore.h"

#include "../arm_jit.h"
#include "../debug.h"
#include "../GPU.h"
#include "../NDSSystem.h"
#include "../SPU.h"
#include "../path.h"
#include "../rasterize.h"
#include "../render3D.h"
#include "../saves.h"

#include "../OGLRender.h"
#include "../OGLRender_3_2.h"

#ifdef ENABLE_SSE2
#include <emmintrin.h>
#endif

#define MIN_BACKLIGHT 0.025
#define N_BAD_FRAMES 3

volatile bool execute = false;

struct _DeSmuMECore
{
  HsCore parent_instance;

  HsGLContext *gl_context;
  HsSoftwareContext *context;

  char *rom_path;
  char *save_dir;

  int skip_frames;
};

static DeSmuMECore *core;

static void desmume_nintendo_ds_core_init (HsNintendoDsCoreInterface *iface);

G_DEFINE_FINAL_TYPE_WITH_CODE (DeSmuMECore, desmume_core, HS_TYPE_CORE,
                               G_IMPLEMENT_INTERFACE (HS_TYPE_NINTENDO_DS_CORE, desmume_nintendo_ds_core_init))

static int init_sound (int buffer_size) { return 0; }
static void deinit_sound () {}
static void update_audio (s16 *buffer, u32 num_samples) {}
static u32 get_audio_space ();
static void mute_audio () {}
static void unmute_audio () {}
static void set_volume (int volume) {}
static void clear_buffer () {}
static void fetch_samples (s16 *sample_buffer, size_t sample_count, ESynchMode synch_mode, ISynchronizingAudioBuffer *the_synchronizer);

#define SNDCORE_HIGHSCORE 1

static SoundInterface_struct SNDHighscore {
  SNDCORE_HIGHSCORE,
  "highscore",
  init_sound,
  deinit_sound,
  update_audio,
  get_audio_space,
  mute_audio,
  unmute_audio,
  set_volume,
  clear_buffer,
  fetch_samples,
  NULL, // PostProcessSamples
};

SoundInterface_struct *SNDCoreList[] = {
  &SNDDummy,
  &SNDHighscore,
  NULL
};

static void
fetch_samples (s16 *sample_buffer, size_t sample_count, ESynchMode synch_mode, ISynchronizingAudioBuffer *the_synchronizer)
{
  hs_core_play_samples (HS_CORE (core), sample_buffer, sample_count * 2);
}

static u32
get_audio_space ()
{
  return DESMUME_SAMPLE_RATE / 60 + 5;
}

#define GPU3D_SOFTRASTERIZER 1
#define GPU3D_OPENGL_AUTO 2
#define GPU3D_OPENGL 3
#define GPU3D_OPENGL_OLD 4

GPU3DInterface *core3DList[] = {
  &gpu3DNull,
  &gpu3DRasterize,
  &gpu3Dgl,
  &gpu3Dgl_3_2,
  &gpu3DglOld,
  NULL
};

static void
message_info (const char *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  g_autofree char *message = g_strdup_vprintf (fmt, args);
  va_end (args);

  hs_core_log (HS_CORE (core), HS_LOG_INFO, message);
}

static bool
message_confirm (const char *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  g_autofree char *message = g_strdup_vprintf (fmt, args);
  va_end (args);

  hs_core_log (HS_CORE (core), HS_LOG_MESSAGE, message);

  return true;
}

static void
message_error (const char *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  g_autofree char *message = g_strdup_vprintf (fmt, args);
  va_end (args);

  hs_core_log (HS_CORE (core), HS_LOG_CRITICAL, message);
}

static void
message_warn (const char *fmt, ...)
{
  va_list args;
  va_start (args, fmt);
  g_autofree char *message = g_strdup_vprintf (fmt, args);
  va_end (args);

  hs_core_log (HS_CORE (core), HS_LOG_WARNING, message);
}

static msgBoxInterface message_box_highscore = {
  message_info,
  message_confirm,
  message_error,
  message_warn,
};

static bool
highscore_gl_init (void)
{
  return true;
}

static bool
highscore_gl_begin (void)
{
  return true;
}

static void
highscore_gl_end (void)
{
}

static bool
highscore_gl_resize (const bool isFBOSupported, size_t w, size_t h)
{
  return true;
}

static gboolean
try_migrate_upstream_save (const char *rom_path, const char *save_path, GError **error)
{
  g_autoptr (GFile) save_file = g_file_new_for_path (save_path);

  if (!g_file_query_exists (save_file, NULL))
    return TRUE;

  g_autoptr (GFile) rom_file = g_file_new_for_path (rom_path);
  g_autofree char *rom_basename = g_file_get_basename (rom_file);

  g_autofree char *old_save_name = g_strconcat (Path::GetFileNameWithoutExt (rom_basename).c_str (), ".dsv", NULL);
  g_autoptr (GFile) old_save_file = g_file_get_child (save_file, old_save_name);

  if (!g_file_query_exists (old_save_file, NULL))
    return TRUE;

  g_autoptr (GFile) new_save_file = g_file_get_child (save_file, "save.dsv");

  if (g_file_query_exists (new_save_file, NULL)) {
    g_autoptr (GFile) new_save_bak = g_file_get_child (save_file, "save.dsv.bak");

    if (!g_file_move (new_save_file, new_save_bak, G_FILE_COPY_OVERWRITE, NULL, NULL,  NULL, error)) {
      g_autofree char *message = g_strdup_printf ("Failed to back up old save data: %s", (*error)->message);
      hs_core_log (HS_CORE (core), HS_LOG_WARNING, message);
      return FALSE;
    }
  }

  if (!g_file_move (old_save_file, new_save_file, G_FILE_COPY_NONE, NULL, NULL,  NULL, error)) {
    g_autofree char *message = g_strdup_printf ("Failed to migrate upstream save data: %s", (*error)->message);
    hs_core_log (HS_CORE (core), HS_LOG_WARNING, message);
    return FALSE;
  }

  g_autofree char *message = g_strdup_printf ("Migrated '%s' to '%s'",
                                              g_file_peek_path (old_save_file),
                                              g_file_peek_path (new_save_file));
  hs_core_log (HS_CORE (core), HS_LOG_MESSAGE, message);

  return TRUE;
}

static gboolean
desmume_core_load_rom (HsCore      *core,
                       const char **rom_paths,
                       int          n_rom_paths,
                       const char  *save_path,
                       GError     **error)
{
  DeSmuMECore *self = DESMUME_CORE (core);

  g_assert (n_rom_paths == 1);

  if (!try_migrate_upstream_save (rom_paths[0], save_path, error))
    return FALSE;

  g_set_str (&self->rom_path, rom_paths[0]);
  g_set_str (&self->save_dir, save_path);

  if (NDS_Init () != 0) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to init core");
    return FALSE;
  }

  NDS_SetupDefaultFirmware ();

#ifdef HAVE_JIT
  CommonSettings.use_jit = true;
  // CommonSettings.jit_max_block_size = 12;
  arm_jit_sync ();
  arm_jit_reset (CommonSettings.use_jit);
#endif

  CommonSettings.spuInterpolationMode = SPUInterpolation_None;
  SPU_ChangeSoundCore (SNDCORE_HIGHSCORE, 735 * 4);
  SPU_SetSynchMode (ESynchMode_Synchronous, ESynchMethod_N);

  oglrender_init = highscore_gl_init;
  oglrender_beginOpenGL = highscore_gl_begin;
  oglrender_endOpenGL = highscore_gl_end;
  oglrender_framebufferDidResizeCallback = highscore_gl_resize;

  OGLLoadEntryPoints_3_2_Func = OGLLoadEntryPoints_3_2;
  OGLCreateRenderer_3_2_Func = OGLCreateRenderer_3_2;

  self->gl_context = hs_core_create_gl_context (core, HS_GL_PROFILE_CORE, 3, 2, (HsGLFlags) (HS_GL_FLAGS_DEPTH | HS_GL_FLAGS_DIRECT_FB_ACCESS));

  g_autoptr (GError) gl_error = NULL;
  int gl_core = GPU3D_OPENGL_AUTO;

  if (!hs_gl_context_realize (self->gl_context, &gl_error)) {
    hs_core_log (core, HS_LOG_WARNING, "Failed to initialize GL 3.2 context, falling back to 2.1");
    g_clear_object (&self->gl_context);

    self->gl_context = hs_core_create_gl_context (core, HS_GL_PROFILE_LEGACY, 2, 1, (HsGLFlags) (HS_GL_FLAGS_DEPTH | HS_GL_FLAGS_DIRECT_FB_ACCESS));
    gl_core = GPU3D_OPENGL_OLD;

    g_clear_pointer (&gl_error, g_error_free);

    if (!hs_gl_context_realize (self->gl_context, &gl_error)) {
      hs_core_log (core, HS_LOG_WARNING, "Failed to initialize GL 2.1 context");

      g_clear_object (&self->gl_context);
    }
  }

  if (self->gl_context) {
    hs_gl_context_set_size (self->gl_context, GPU_FRAMEBUFFER_NATIVE_WIDTH, GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2);

    if (!GPU->Change3DRendererByID (gl_core)) {
      hs_core_log (core, HS_LOG_WARNING, "Failed to initialize GL renderer, falling back to software rasterizer");
      hs_gl_context_unrealize (self->gl_context);
      g_clear_object (&self->gl_context);

      self->context = hs_core_create_software_context (core, GPU_FRAMEBUFFER_NATIVE_WIDTH, GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2, HS_PIXEL_FORMAT_XRGB8888);
      GPU->Change3DRendererByID (GPU3D_SOFTRASTERIZER);
    }
  } else {
    self->context = hs_core_create_software_context (core, GPU_FRAMEBUFFER_NATIVE_WIDTH, GPU_FRAMEBUFFER_NATIVE_HEIGHT * 2, HS_PIXEL_FORMAT_XRGB8888);
    GPU->Change3DRendererByID (GPU3D_SOFTRASTERIZER);
  }

  if (NDS_LoadROM (self->rom_path) < 0) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load ROM");
    return FALSE;
  }

  return TRUE;
}

static void
desmume_core_start (HsCore *core)
{
  DeSmuMECore *self = DESMUME_CORE (core);

  execute = true;
  SPU_Pause (0);

  /* The first couple frames will be bad with GL rendering, skip them */
  self->skip_frames = N_BAD_FRAMES;
}

static void
desmume_core_reset (HsCore *core)
{
  DeSmuMECore *self = DESMUME_CORE (core);

  NDS_Reset ();
  SPU_Pause (0);

  /* The first couple frames will be bad with GL rendering, skip them */
  self->skip_frames = N_BAD_FRAMES;
}

static void
desmume_core_stop (HsCore *core)
{
  DeSmuMECore *self = DESMUME_CORE (core);

  execute = false;

  NDS_DeInit ();

  if (self->gl_context)
    hs_gl_context_unrealize (self->gl_context);

  g_clear_object (&self->gl_context);
  g_clear_object (&self->context);
  g_clear_pointer (&self->rom_path, g_free);
  g_clear_pointer (&self->save_dir, g_free);
}

static void
desmume_core_pause (HsCore *core)
{
  execute = false;
  SPU_Pause (1);
}

static void
desmume_core_resume (HsCore *core)
{
  execute = true;
  SPU_Pause (0);
}

static void
desmume_core_poll_input (HsCore *core, HsInputState *input_state)
{
  u32 buttons = input_state->nintendo_ds.buttons;

  NDS_setPad (
    buttons & 1 << HS_NINTENDO_DS_BUTTON_RIGHT,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_LEFT,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_DOWN,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_UP,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_SELECT,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_START,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_B,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_A,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_Y,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_X,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_L,
    buttons & 1 << HS_NINTENDO_DS_BUTTON_R,
    0, // debug
    0 // lid
  );

  if (input_state->nintendo_ds.touch_pressed) {
    u16 x = (u16) round (input_state->nintendo_ds.touch_x * GPU_FRAMEBUFFER_NATIVE_WIDTH);
    u16 y = (u16) round (input_state->nintendo_ds.touch_y * GPU_FRAMEBUFFER_NATIVE_HEIGHT);

    NDS_setTouchPos (x, y);
  } else {
    NDS_releaseTouch ();
  }

  // NDS_setMic()
}

static void
desmume_core_run_frame (HsCore *core)
{
  DeSmuMECore *self = DESMUME_CORE (core);

  NDS_beginProcessingInput ();
  NDS_endProcessingInput ();

  NDS_exec<false> ();
  SPU_Emulate_user ();

  if (self->skip_frames > 0) {
    self->skip_frames--;
    return;
  }

  const NDSDisplayInfo &display_info = GPU->GetDisplayInfo ();
  const size_t pix_count = GPU_FRAMEBUFFER_NATIVE_WIDTH * GPU_FRAMEBUFFER_NATIVE_HEIGHT;
  u32 *framebuffer;

  if (self->gl_context)
    framebuffer = (u32 *) hs_gl_context_acquire_framebuffer (self->gl_context);
  else
    framebuffer = (u32 *) hs_software_context_get_framebuffer (self->context);

  ColorspaceConvertBuffer555To8888Opaque<false, true, BESwapNone> (display_info.masterNativeBuffer16, framebuffer, pix_count * 2);

  //some games use the backlight for fading effect
  for (int i = NDSDisplayID_Main; i <= NDSDisplayID_Touch; i++) {
    float backlight = MIN_BACKLIGHT + (1 - MIN_BACKLIGHT) * display_info.backlightIntensity[i];
    if (backlight < 1)
      ColorspaceApplyIntensityToBuffer32<false, true> (framebuffer + pix_count * i, pix_count, backlight);
  }

  if (self->gl_context) {
    hs_gl_context_release_framebuffer (self->gl_context);
    hs_gl_context_swap_buffers (self->gl_context);
  }
}

static gboolean
desmume_core_reload_save (HsCore      *core,
                          const char  *save_path,
                          GError     **error)
{
  DeSmuMECore *self = DESMUME_CORE (core);

  NDS_FreeROM ();

  g_set_str (&self->save_dir, save_path);

  if (NDS_LoadROM (self->rom_path) < 0) {
    g_set_error (error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load ROM");
    return FALSE;
  }

  return TRUE;
}

static void
desmume_core_load_state (HsCore          *core,
                         const char      *path,
                         HsStateCallback  callback)
{
  DeSmuMECore *self = DESMUME_CORE (core);

  if (!savestate_load (path)) {
    GError *error = NULL;

    g_set_error (&error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load state");
    callback (core, &error);
    return;
  }

  /* The first couple frames will be bad with GL rendering, skip them */
  self->skip_frames = N_BAD_FRAMES;

  callback (core, NULL);
}

static void
desmume_core_save_state (HsCore          *core,
                         const char      *path,
                         HsStateCallback  callback)
{
  if (!savestate_save (path)) {
    GError *error = NULL;

    g_set_error (&error, HS_CORE_ERROR, HS_CORE_ERROR_INTERNAL, "Failed to load state");
    callback (core, &error);
    return;
  }

  callback (core, NULL);
}

static double
desmume_core_get_frame_rate (HsCore *core)
{
  return 59.8261;
}

static double
desmume_core_get_aspect_ratio (HsCore *core)
{
  return GPU_FRAMEBUFFER_NATIVE_WIDTH / (double) GPU_FRAMEBUFFER_NATIVE_HEIGHT / 2.0;
}

static double
desmume_core_get_sample_rate (HsCore *core)
{
  return DESMUME_SAMPLE_RATE;
}

static void
desmume_core_finalize (GObject *object)
{
  DeSmuMECore *self = DESMUME_CORE (object);

  core = NULL;

  G_OBJECT_CLASS (desmume_core_parent_class)->finalize (object);
}

static void
desmume_core_class_init (DeSmuMECoreClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  HsCoreClass *core_class = HS_CORE_CLASS (klass);

  object_class->finalize = desmume_core_finalize;

  core_class->load_rom = desmume_core_load_rom;
  core_class->start = desmume_core_start;
  core_class->reset = desmume_core_reset;
  core_class->stop = desmume_core_stop;
  core_class->pause = desmume_core_pause;
  core_class->resume = desmume_core_resume;
  core_class->poll_input = desmume_core_poll_input;
  core_class->run_frame = desmume_core_run_frame;

  core_class->reload_save = desmume_core_reload_save;
  core_class->load_state = desmume_core_load_state;
  core_class->save_state = desmume_core_save_state;

  core_class->get_frame_rate = desmume_core_get_frame_rate;
  core_class->get_aspect_ratio = desmume_core_get_aspect_ratio;

  core_class->get_sample_rate = desmume_core_get_sample_rate;
}

static void
log_message (HsLogLevel level, const char *message)
{
  g_autofree char *msg = g_strdup (message);

  int len = strlen (msg);
  if (msg[len - 1] == '\n')
    msg[len - 1] = '\0';

  hs_core_log (HS_CORE (core), level, msg);
}

static void
debug_cb (const Logger& logger, const char *message)
{
  log_message (HS_LOG_DEBUG, message);
}

static void
info_cb (const Logger& logger, const char *message)
{
  log_message (HS_LOG_INFO, message);
}

static void
desmume_core_init (DeSmuMECore *self)
{
  g_assert (core == NULL);

  core = self;

  execute = false;

  msgbox = &message_box_highscore;

  for (int i = 0; i < 8; i++)
    Logger::log (i, __FILE__, __LINE__, debug_cb);

  Logger::log (10, __FILE__, __LINE__, info_cb);
}

static void
desmume_nintendo_ds_core_init (HsNintendoDsCoreInterface *iface)
{
}

const char *
desmume_hs_get_save_dir (void)
{
  return core->save_dir;
}

GType
hs_get_core_type (void)
{
  return DESMUME_TYPE_CORE;
}
