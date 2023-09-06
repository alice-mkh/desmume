#pragma once

#include <highscore/libhighscore.h>

G_BEGIN_DECLS

#define DESMUME_TYPE_CORE (desmume_core_get_type())

G_DECLARE_FINAL_TYPE (DeSmuMECore, desmume_core, DESMUME, CORE, HsCore)

const char *desmume_hs_get_save_dir (void);

G_MODULE_EXPORT GType hs_get_core_type (void);

G_END_DECLS
