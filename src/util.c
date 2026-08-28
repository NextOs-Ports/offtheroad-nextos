#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "util.h"

#define LOG_NAME "debug.log"

int debugPrintf(const char *text, ...) {
  va_list list;

  FILE *f = fopen(LOG_NAME, "a");
  if (f) {
    va_start(list, text);
    vfprintf(f, text, list);
    va_end(list);
    fclose(f);
  }

  va_start(list, text);
  vprintf(text, list);
  va_end(list);

  return 0;
}

uintptr_t read_tls_stack_guard(void) {
#if defined(__aarch64__)
  uintptr_t tls = 0;
  __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tls));
  if (tls)
    return *(uintptr_t *)(tls + 0x28);
#endif
  return 0;
}

const char *resolve_android_path(const char *path) {
  static _Thread_local char alt_path[2048];
  static _Thread_local char asset_path[2048];

  static const char *app_data_prefix = "/data/data/com.dogbytegames.offtheroad/";
  static const char *app_data_prefix_2 = "/data/user/0/com.dogbytegames.offtheroad/";
  static const char *sdcard_prefix =
      "/storage/emulated/0/Android/data/com.dogbytegames.offtheroad/";

  if (!path || path[0] == '\0')
    return path;

  /* Cache binario de shader do bgfx: o jogo grava ~280 arquivos "sh_<hash>"
   * com nome RELATIVO, ou seja na raiz do port.  Mandar para userdata/, que e
   * o unico diretorio gravavel garantido -- e assim a raiz do port continua
   * sendo so o que o pacote instalou. */
  if (strncmp(path, "sh_", 3) == 0 && !strchr(path, '/')) {
    snprintf(alt_path, sizeof(alt_path), "./userdata/%s", path);
    return alt_path;
  }

  /* Dados privados do app (UserDefault.xml, saves) vivem em ./userdata/,
   * que e o diretorio gravavel do port (o launcher garante que existe). */
  if (strncmp(path, app_data_prefix, strlen(app_data_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./userdata/%s",
             path + strlen(app_data_prefix));
    return alt_path;
  }
  if (strncmp(path, app_data_prefix_2, strlen(app_data_prefix_2)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./userdata/%s",
             path + strlen(app_data_prefix_2));
    return alt_path;
  }
  if (strncmp(path, sdcard_prefix, strlen(sdcard_prefix)) == 0) {
    snprintf(alt_path, sizeof(alt_path), "./%s", path + strlen(sdcard_prefix));
    return alt_path;
  }

  if (access(path, F_OK) == 0)
    return path;

  if (path[0] == '/') {
    if (snprintf(alt_path, sizeof(alt_path), ".%s", path) <
            (int)sizeof(alt_path) &&
        access(alt_path, F_OK) == 0) {
      return alt_path;
    }
  }

  const char *basename = strrchr(path, '/');
  basename = basename ? basename + 1 : path;
  if (basename[0] != '\0') {
    if (snprintf(asset_path, sizeof(asset_path), "./assets/%s", basename) <
            (int)sizeof(asset_path) &&
        access(asset_path, F_OK) == 0) {
      return asset_path;
    }
  }

  return path;
}

int ret0(void) { return 0; }
int ret1(void) { return 1; }
int retm1(void) { return -1; }
