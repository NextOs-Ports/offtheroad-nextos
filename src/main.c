/*
 * main.c -- Off The Road 1.18.2 (DogByte Games) so-loader para NextOS / Mali-450.
 *
 * O que a analise estatica de 23/08/2026 fixou (detalhe em ../STUDY.md):
 *
 *  - engine C++ propria (xGen) + Horde3D + **bgfx** sobre casca Cocos2d-x.
 *    Nao e Unity: sem IL2CPP, sem AssetBundle, sem Addressables.
 *
 *  - QUEM CRIA O CONTEXTO GL (o ponto que decidia o port): o LOADER.
 *    `xGen::cGameEngine::cGameEngine(int)` monta uma `bgfx::PlatformData`
 *    ZERADA e escreve nela o retorno de `eglGetCurrentContext()` antes de
 *    `bgfx::setPlatformData` + `bgfx::init`.  Em `bgfx::gles2::GlContext::create`
 *    o primeiro teste e `if (g_platformData.context != NULL) -> import()`:
 *    com contexto ja corrente a engine ADOTA o nosso e nunca chama
 *    eglGetDisplay/CreateWindowSurface/CreateContext.  Os dois caminhos estao
 *    compilados na lib, mas so o de adocao executa.  Consequencia pratica:
 *    o contexto tem de estar CORRENTE antes de nativeInit, e o swap e nosso.
 *
 *  - CONTROLE NATIVO (regra #19): a lib exporta a API de gamepad inteira --
 *    cocos2d::nativeGamepadConnected / ButtonDown / ButtonUp /
 *    AxisValueChanged.  Os identificadores sao os do Android (KEYCODE_BUTTON_*
 *    e MotionEvent.AXIS_*), porque quem chamava era com/utils/GameControllerInput.
 *    Nada de toque falso para dirigir.
 *
 *  - audio: OpenAL Soft EMBUTIDO na libgame, saindo por OpenSL ES.
 */
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <dlfcn.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengles2.h>

#include "error.h"
#include "imports.h"
#include "jni_shim.h"
#include "opensles_shim.h"
#include "so_util.h"
#include "util.h"

#include "nxgl_frame_proof_adapter.h"
#include "nxgl_provider_discovery_adapter.h"

#define CXX_SO "lib/libc++_shared.so"
#define GAME_SO "lib/libgame.so"
#define CXX_MEM_MB 16
#define GAME_MEM_MB 192

typedef int jint;
typedef unsigned char jboolean;

/* guarda do canario TLS do Bionic (slot tpidr_el0+0x28) */
__attribute__((used, aligned(16))) _Thread_local char g_bionic_guard_pad[256];

/* ---------------------------------------------------------- entry points -- */
static jint (*e_JNI_OnLoad)(void *vm, void *reserved);
static void (*e_nativeSetApkPath)(void *env, void *thiz, void *apkPath);
static void (*e_nativeInit)(void *env, void *thiz, int w, int h);
static void (*e_nativeRender)(void *env, void *thiz);
static void (*e_nativeOnPause)(void *env, void *thiz);
static void (*e_nativeOnResume)(void *env, void *thiz);
static void (*e_nativeOnStart)(void *env, void *thiz);
static void (*e_nativeOnStop)(void *env, void *thiz);
static void (*e_nativeTouchesBegin)(void *env, void *thiz, int id, float x, float y);
static void (*e_nativeTouchesEnd)(void *env, void *thiz, int id, float x, float y);
static void (*e_nativeTouchesMove)(void *env, void *thiz, int id, float x, float y);
static void (*e_nativeKeyDown)(void *env, void *thiz, int keyCode);
static void (*e_nativeKeyUp)(void *env, void *thiz, int keyCode);

/* API de gamepad da propria engine (cocos2d::native*) */
static void (*e_padConnected)(void *env, void *obj, int devId, int type);
static void (*e_padDisconnected)(void *env, void *obj, int devId, int type);
static void (*e_padButtonDown)(void *env, void *obj, int devId, int type, int keyCode);
static void (*e_padButtonUp)(void *env, void *obj, int devId, int type, int keyCode);
static void (*e_padAxis)(void *env, void *obj, int devId, int type, int axis, float v);

static void *g_env = NULL;
static void *g_vm = NULL;
static SDL_Window *g_window = NULL;
static SDL_GLContext g_gl_ctx = NULL;
static SDL_GameController *g_pad = NULL;

static volatile int g_running = 1;
static int g_screen_w = 1280;
static int g_screen_h = 720;

/* identidade do controle virtual que entregamos a engine.  O valor so precisa
 * ser estavel: a engine casa por deviceId no slot de CCControllerDispatcher. */
#define PAD_DEVICE_ID 1
/* Tipo do pad anunciado a engine.  No Java do jogo (GameController.smali) so
 * existem dois valores: 1 = "PLAYSTATION(R)3" (glifos de PlayStation) e
 * 6 = qualquer outro pad (glifos genericos A/B/X/Y).  6 e o nosso padrao;
 * OT_PAD_TYPE=<n> permite experimentar outro valor sem recompilar. */
static int g_pad_type = 6;
#define PAD_TYPE g_pad_type

/* Android KeyEvent.KEYCODE_* -- e isso que a engine espera receber */
enum {
  AK_BACK = 4,
  AK_DPAD_UP = 19, AK_DPAD_DOWN = 20, AK_DPAD_LEFT = 21, AK_DPAD_RIGHT = 22,
  AK_BUTTON_A = 96, AK_BUTTON_B = 97, AK_BUTTON_X = 99, AK_BUTTON_Y = 100,
  AK_BUTTON_L1 = 102, AK_BUTTON_R1 = 103,
  AK_BUTTON_L2 = 104, AK_BUTTON_R2 = 105,
  AK_BUTTON_THUMBL = 106, AK_BUTTON_THUMBR = 107,
  AK_BUTTON_START = 108, AK_BUTTON_SELECT = 109,
};

/* Android MotionEvent.AXIS_* */
enum {
  AX_X = 0, AX_Y = 1, AX_Z = 11, AX_RZ = 14,
  AX_HAT_X = 15, AX_HAT_Y = 16, AX_LTRIGGER = 17, AX_RTRIGGER = 18,
  AX_GAS = 22, AX_BRAKE = 23,
};

/* --------------------------------------------------------- crash handler -- */
static int addr_readable(uintptr_t a) {
  if (a < 0x1000) return 0;
  static int fds[2] = {-1, -1};
  if (fds[0] < 0 && pipe(fds) != 0) return 0;
  return write(fds[1], (void *)a, 8) == 8;
}

static void crash_handler(int sig, siginfo_t *info, void *ucv) {
  ucontext_t *uc = (ucontext_t *)ucv;
  uintptr_t pc = 0, lr = 0, sp = 0, fp = 0;
#if defined(__aarch64__)
  pc = uc->uc_mcontext.pc;
  lr = uc->uc_mcontext.regs[30];
  sp = uc->uc_mcontext.sp;
  fp = uc->uc_mcontext.regs[29];
#endif
  uintptr_t base = (uintptr_t)text_base;
  debugPrintf("\n=========== CRASH ===========\n");
  debugPrintf("[crash] sinal=%d addr=%p\n", sig, info ? info->si_addr : NULL);
  debugPrintf("[crash] libgame base=%p (.text=%zu)\n", (void *)base, text_size);
  debugPrintf("[crash] pc=%p lr=%p sp=%p fp=%p\n", (void *)pc, (void *)lr,
              (void *)sp, (void *)fp);
  if (base && pc >= base && pc < base + text_size)
    debugPrintf("[crash] pc = libgame+0x%lx\n", (unsigned long)(pc - base));
  if (base && lr >= base && lr < base + text_size)
    debugPrintf("[crash] lr = libgame+0x%lx\n", (unsigned long)(lr - base));
  for (int i = 0; i < 24 && fp; i++) {
    if (!addr_readable(fp) || !addr_readable(fp + 8)) break;
    uintptr_t next_fp = ((uintptr_t *)fp)[0];
    uintptr_t ret = ((uintptr_t *)fp)[1];
    if (!ret) break;
    if (base && ret >= base && ret < base + text_size)
      debugPrintf("[crash]  #%02d %p  libgame+0x%lx\n", i, (void *)ret,
                  (unsigned long)(ret - base));
    else
      debugPrintf("[crash]  #%02d %p\n", i, (void *)ret);
    if (next_fp <= fp) break;
    fp = next_fp;
  }
  debugPrintf("=============================\n");
  _exit(128 + sig); /* Mali-450: JAMAIS desmontar GL na saida */
}

static void install_crash_handler(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = crash_handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, NULL);
  sigaction(SIGBUS, &sa, NULL);
  sigaction(SIGILL, &sa, NULL);
  sigaction(SIGFPE, &sa, NULL);
  sigaction(SIGABRT, &sa, NULL);
}

/* ---- nxgl (framework): reparo de provider cruzado + prova de imagem ------ */
/* Em dArkOS/ArkOS os SONAMEs graficos podem estar CRUZADOS (stub sem driver
 * atras do nome versionado). A unica condicao segura e' o renderer VAZIO
 * medido no contexto real; o reparo re-executa o processo com o par EGL/GLES
 * provado. Ver framework/nxgl/adapters. */
static SDL_Window *g_repair_window;
static SDL_GLContext g_repair_context;

static void ot_video_teardown(void) {
  if (g_repair_context) { SDL_GL_DeleteContext(g_repair_context); g_repair_context = NULL; }
  if (g_repair_window) { SDL_DestroyWindow(g_repair_window); g_repair_window = NULL; }
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

/* glCreateShader separa provider ES2 de um ES1-only. */
static const char *const k_ot_gles2_symbols[] = {"glCreateShader", "glDrawElements",
                                                 "glTexImage2D", "glClear"};

/* -------------------------------------- perfil grafico (OT_DETAIL) -------- */
/* A engine tem 4 niveis internos (0..3) aplicados por
 * cApplication::setupLevelOfDetail(bool) — o nivel mora em this+0 e vira
 * flags de shader + h3dSetOption. O AUTO-benchmark do splash mede FILL-RATE
 * ("benchmark: %.1f fillscreen/sec", cortes 800/1100/1400): num painel
 * pequeno toda GPU enche rapido, o jogo crava nivel 3 e a gameplay real
 * afunda (medido no R36S/G31: 1533 fill/sec -> detail 3 -> ~10 fps). O
 * perfil do port interpoe o PLT de setupLevelOfDetail (todos os call sites
 * internos usam @plt) e forca o nivel ANTES de encaminhar ao real.
 *
 * OT_DETAIL: low(0) | medium(1) | high(2) | ultra(3) | auto (original).
 * Padrao do pacote universal: LOW — handheld primeiro, fps primeiro. */
static void (*real_setupLOD)(void *app, int force);
static int g_detail_level = 0; /* 0..3 quando forcado */
static int g_detail_forced = 1; /* 0 = OT_DETAIL=auto (comportamento do jogo) */

static void my_setupLevelOfDetail(void *app, int force) {
  if (g_detail_forced && app) {
    int cur = *(int *)app;
    if (cur != g_detail_level) {
      debugPrintf("[detail] override OT_DETAIL: %d -> %d\n", cur,
                  g_detail_level);
      *(int *)app = g_detail_level;
    }
  }
  if (real_setupLOD) real_setupLOD(app, force);
}

static void parse_detail_env(void) {
  const char *e = getenv("OT_DETAIL");
  if (!e || !*e) { g_detail_forced = 1; g_detail_level = 0; return; }
  if (strcasecmp(e, "auto") == 0) { g_detail_forced = 0; return; }
  if (strcasecmp(e, "low") == 0) g_detail_level = 0;
  else if (strcasecmp(e, "medium") == 0) g_detail_level = 1;
  else if (strcasecmp(e, "high") == 0) g_detail_level = 2;
  else if (strcasecmp(e, "ultra") == 0) g_detail_level = 3;
  else {
    int v = atoi(e);
    if (v < 0) v = 0;
    if (v > 3) v = 3;
    g_detail_level = v;
  }
  g_detail_forced = 1;
}

static void trigger_exit(const char *reason) {
  if (!g_running) return;
  debugPrintf("[main] saindo: %s\n", reason);
  g_running = 0;
}

/* ------------------------------------------------------------- modulos ---- */
static DynLibFunction *load_module(const char *name, int heap_mb,
                                   DynLibFunction *tbl, int n, int *out_n) {
  size_t hs = (size_t)heap_mb * 1024 * 1024;
  void *heap = mmap(NULL, hs, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (heap == MAP_FAILED) heap = NULL;
  debugPrintf("[loader] carregando %s (heap %p, %d MB)\n", name, heap, heap_mb);
  if (so_load(name, heap, hs) < 0) fatal_error("so_load(%s) falhou", name);
  if (so_relocate() < 0) fatal_error("so_relocate(%s) falhou", name);
  so_resolve(tbl, n, 0);
  so_finalize();
  so_flush_caches();
  so_execute_init_array();
  debugPrintf("[loader] %s pronto: text=%p+%zu data=%p+%zu\n", name, text_base,
              text_size, data_base, data_size);
  if (out_n) return so_snapshot_symbols(out_n);
  return NULL;
}

static DynLibFunction *tbl_concat(DynLibFunction *a, int an, DynLibFunction *b,
                                  int bn, int *out_n) {
  DynLibFunction *c = malloc(sizeof(DynLibFunction) * (size_t)(an + bn));
  memcpy(c, a, sizeof(DynLibFunction) * (size_t)an);
  memcpy(c + an, b, sizeof(DynLibFunction) * (size_t)bn);
  *out_n = an + bn;
  return c;
}

/* --------------------------------------------------------------- cursor --- */
/* Menu e loja do jogo sao de TOQUE.  O overlay abaixo desenha o ponteiro que os
 * analogicos movem; a licao da happywheels vale igual aqui: o cocos guarda em
 * cache programa/atributos ligados, entao o overlay precisa DEVOLVER o estado
 * exato -- zerar por baixo dele faz TUDO desenhar com program=0 (tela preta). */
static GLuint g_cursor_prog = 0, g_cursor_vbo = 0;
static GLint g_cursor_u_pos = -1, g_cursor_u_res = -1, g_cursor_u_color = -1;
static float g_cursor_x = 640.0f, g_cursor_y = 360.0f;
static bool g_cursor_pressed = false;
static bool g_cursor_visible = false;
static bool g_cursor_mode = false;   /* L3 liga/desliga o modo ponteiro */

static void init_cursor_renderer(void) {
  const char *vs_src =
      "attribute vec2 a_pos;\n"
      "uniform vec2 u_pos;\n"
      "uniform vec2 u_res;\n"
      "void main() {\n"
      "  vec2 p = (u_pos + a_pos) / u_res * 2.0 - 1.0;\n"
      "  gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n"
      "}\n";
  const char *fs_src =
      "precision mediump float;\n"
      "uniform vec4 u_color;\n"
      "void main() { gl_FragColor = u_color; }\n";

  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &vs_src, NULL);
  glCompileShader(vs);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &fs_src, NULL);
  glCompileShader(fs);
  g_cursor_prog = glCreateProgram();
  glAttachShader(g_cursor_prog, vs);
  glAttachShader(g_cursor_prog, fs);
  glBindAttribLocation(g_cursor_prog, 0, "a_pos");
  glLinkProgram(g_cursor_prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  g_cursor_u_pos = glGetUniformLocation(g_cursor_prog, "u_pos");
  g_cursor_u_res = glGetUniformLocation(g_cursor_prog, "u_res");
  g_cursor_u_color = glGetUniformLocation(g_cursor_prog, "u_color");

  static const float arrow[] = {
      0.0f, 0.0f, 0.0f, 22.0f, 5.0f, 17.0f,
      0.0f, 0.0f, 5.0f, 17.0f, 16.0f, 16.0f,
      3.0f, 15.0f, 9.0f, 26.0f, 12.5f, 24.0f,
      3.0f, 15.0f, 12.5f, 24.0f, 6.5f, 13.5f,
      1.5f, 2.5f, 1.5f, 18.5f, 5.0f, 15.0f,
      1.5f, 2.5f, 5.0f, 15.0f, 13.5f, 14.5f,
      4.5f, 14.5f, 9.0f, 24.0f, 11.0f, 22.5f,
      4.5f, 14.5f, 11.0f, 22.5f, 6.5f, 13.5f,
  };
  glGenBuffers(1, &g_cursor_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, g_cursor_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(arrow), arrow, GL_STATIC_DRAW);
  glBindBuffer(GL_ARRAY_BUFFER, 0);
}

static void draw_cursor(void) {
  if (!g_cursor_visible || !g_cursor_prog) return;

  GLint prev_prog = 0, prev_vbo = 0, prev_bsrc = GL_ONE, prev_bdst = GL_ZERO;
  GLboolean prev_blend, prev_depth, prev_cull, prev_scissor;
  GLint attr_enabled[4] = {0, 0, 0, 0};
  GLint a0_size = 4, a0_type = GL_FLOAT, a0_norm = 0, a0_stride = 0, a0_buf = 0;
  void *a0_ptr = NULL;

  glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_vbo);
  glGetIntegerv(GL_BLEND_SRC_RGB, &prev_bsrc);
  glGetIntegerv(GL_BLEND_DST_RGB, &prev_bdst);
  prev_blend = glIsEnabled(GL_BLEND);
  prev_depth = glIsEnabled(GL_DEPTH_TEST);
  prev_cull = glIsEnabled(GL_CULL_FACE);
  prev_scissor = glIsEnabled(GL_SCISSOR_TEST);
  for (int i = 0; i < 4; i++)
    glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attr_enabled[i]);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a0_size);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a0_type);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a0_norm);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a0_stride);
  glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a0_buf);
  glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a0_ptr);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_SCISSOR_TEST);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glUseProgram(g_cursor_prog);
  glUniform2f(g_cursor_u_pos, g_cursor_x, g_cursor_y);
  glUniform2f(g_cursor_u_res, (float)g_screen_w, (float)g_screen_h);
  glBindBuffer(GL_ARRAY_BUFFER, g_cursor_vbo);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
  glUniform4f(g_cursor_u_color, 0.05f, 0.05f, 0.05f, 0.95f);
  glDrawArrays(GL_TRIANGLES, 0, 12);
  if (g_cursor_pressed)
    glUniform4f(g_cursor_u_color, 1.0f, 0.85f, 0.2f, 1.0f);
  else
    glUniform4f(g_cursor_u_color, 0.98f, 0.98f, 0.98f, 1.0f);
  glDrawArrays(GL_TRIANGLES, 12, 12);

  glBindBuffer(GL_ARRAY_BUFFER, a0_buf);
  glVertexAttribPointer(0, a0_size, (GLenum)a0_type, (GLboolean)a0_norm,
                        a0_stride, a0_ptr);
  for (int i = 0; i < 4; i++) {
    if (attr_enabled[i]) glEnableVertexAttribArray(i);
    else glDisableVertexAttribArray(i);
  }
  glBindBuffer(GL_ARRAY_BUFFER, prev_vbo);
  glUseProgram(prev_prog);
  glBlendFunc((GLenum)prev_bsrc, (GLenum)prev_bdst);
  if (prev_blend) glEnable(GL_BLEND); else glDisable(GL_BLEND);
  if (prev_depth) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
  if (prev_cull) glEnable(GL_CULL_FACE); else glDisable(GL_CULL_FACE);
  if (prev_scissor) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
}

/* ---------------------------------------------------------------- input --- */
static bool g_btn_prev[SDL_CONTROLLER_BUTTON_MAX];
static float g_axis_prev[32];
static bool g_touch_down = false;
static int g_test_drive = 0;
static unsigned long g_test_exit_frame = 0;

static void pad_axis(int axis, float v) {
  if (axis < 0 || axis >= 32) return;
  if (fabsf(v - g_axis_prev[axis]) < 0.004f) return;
  g_axis_prev[axis] = v;
  if (e_padAxis) e_padAxis(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE, axis, v);
}

static void pad_button(int keycode, bool down) {
  if (down) {
    if (e_padButtonDown) e_padButtonDown(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE, keycode);
    if (e_nativeKeyDown) e_nativeKeyDown(g_env, NULL, keycode);
  } else {
    if (e_padButtonUp) e_padButtonUp(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE, keycode);
    if (e_nativeKeyUp) e_nativeKeyUp(g_env, NULL, keycode);
  }
}

static int sdl_btn_to_android(int b) {
  switch (b) {
    /* medido no device (23/08): com o pad anunciado como GENERICO (tipo 6) o
     * jogo le os keycodes retos — traducao 1:1.  (Com tipo 1/PS3 a engine
     * remapeava internamente e A/X, B/Y saiam trocados; nao reintroduzir o
     * cruzamento da 1.0.3.) */
    case SDL_CONTROLLER_BUTTON_A: return AK_BUTTON_A;
    case SDL_CONTROLLER_BUTTON_B: return AK_BUTTON_B;
    case SDL_CONTROLLER_BUTTON_X: return AK_BUTTON_X;
    case SDL_CONTROLLER_BUTTON_Y: return AK_BUTTON_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return AK_BUTTON_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return AK_BUTTON_R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK: return AK_BUTTON_THUMBL;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return AK_BUTTON_THUMBR;
    case SDL_CONTROLLER_BUTTON_START: return AK_BUTTON_START;
    case SDL_CONTROLLER_BUTTON_BACK: return AK_BUTTON_SELECT;
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return AK_DPAD_UP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return AK_DPAD_DOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return AK_DPAD_LEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return AK_DPAD_RIGHT;
    default: return 0;
  }
}

static void open_pad(int which) {
  if (g_pad) return;
  if (!SDL_IsGameController(which)) return;
  g_pad = SDL_GameControllerOpen(which);
  if (!g_pad) return;
  debugPrintf("[input] gamepad: %s\n", SDL_GameControllerName(g_pad));
  {
    char *map = SDL_GameControllerMapping(g_pad);
    debugPrintf("[input] mapping: %s\n", map ? map : "(nenhum)");
    if (map) SDL_free(map);
    {
      /* SDL_GameControllerHasButton e' 2.0.14+; o piso universal e' SDL 2.0.4.
       * Diagnostico apenas: em SDL antiga a linha vira "n/d". */
      typedef SDL_bool (*ot_has_button_fn)(SDL_GameController *,
                                           SDL_GameControllerButton);
      ot_has_button_fn has_button =
          (ot_has_button_fn)dlsym(RTLD_DEFAULT, "SDL_GameControllerHasButton");
      debugPrintf("[input] SELECT(BACK)=%s START=%s\n",
                  !has_button ? "n/d"
                  : has_button(g_pad, SDL_CONTROLLER_BUTTON_BACK) ? "sim" : "NAO",
                  !has_button ? "n/d"
                  : has_button(g_pad, SDL_CONTROLLER_BUTTON_START) ? "sim" : "NAO");
    }
  }
  if (e_padConnected) {
    e_padConnected(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE);
    debugPrintf("[input] nativeGamepadConnected(dev=%d tipo=%d)\n",
                PAD_DEVICE_ID, PAD_TYPE);
  }
}

static void send_touch(bool down, bool move) {
  if (move) {
    if (g_touch_down && e_nativeTouchesMove)
      e_nativeTouchesMove(g_env, NULL, 0, g_cursor_x, g_cursor_y);
    return;
  }
  if (down && !g_touch_down) {
    g_touch_down = true;
    if (e_nativeTouchesBegin) e_nativeTouchesBegin(g_env, NULL, 0, g_cursor_x, g_cursor_y);
  } else if (!down && g_touch_down) {
    g_touch_down = false;
    if (e_nativeTouchesEnd) e_nativeTouchesEnd(g_env, NULL, 0, g_cursor_x, g_cursor_y);
  }
}

static void pump_input(float dt) {
  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    switch (ev.type) {
      case SDL_QUIT: trigger_exit("SDL_QUIT"); break;
      case SDL_KEYDOWN:
        if (ev.key.keysym.sym == SDLK_ESCAPE) trigger_exit("ESC");
        break;
      case SDL_CONTROLLERDEVICEADDED: open_pad(ev.cdevice.which); break;
      case SDL_CONTROLLERDEVICEREMOVED:
        if (g_pad) {
          SDL_GameControllerClose(g_pad);
          g_pad = NULL;
          if (e_padDisconnected) e_padDisconnected(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE);
        }
        break;
      default: break;
    }
  }
  if (!g_pad) return;

  /* SELECT+START = sair.  Lido por ESTADO, nunca por evento (regra #29). */
  if (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_BACK) &&
      SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_START)) {
    trigger_exit("SELECT+START");
    return;
  }

  for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; b++) {
    bool now = SDL_GameControllerGetButton(g_pad, (SDL_GameControllerButton)b) != 0;
    if (now == g_btn_prev[b]) continue;
    g_btn_prev[b] = now;
    int ak = sdl_btn_to_android(b);
    if (ak) pad_button(ak, now);
    /* o dpad tambem chega como eixo HAT nos jogos Android */
    if (b == SDL_CONTROLLER_BUTTON_DPAD_LEFT || b == SDL_CONTROLLER_BUTTON_DPAD_RIGHT) {
      float hx = (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ? 1.0f : 0.0f) -
                 (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) ? 1.0f : 0.0f);
      pad_axis(AX_HAT_X, hx);
    }
    if (b == SDL_CONTROLLER_BUTTON_DPAD_UP || b == SDL_CONTROLLER_BUTTON_DPAD_DOWN) {
      float hy = (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) ? 1.0f : 0.0f) -
                 (SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_DPAD_UP) ? 1.0f : 0.0f);
      pad_axis(AX_HAT_Y, hy);
    }
  }

  float lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX) / 32767.0f;
  float ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY) / 32767.0f;
  float rx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
  float ry = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
  float lt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) / 32767.0f;
  float rt = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
  const float dz = 0.16f;
  if (fabsf(lx) < dz) lx = 0.0f;
  if (fabsf(ly) < dz) ly = 0.0f;
  if (fabsf(rx) < dz) rx = 0.0f;
  if (fabsf(ry) < dz) ry = 0.0f;

  /* modo ponteiro: L3 liga/desliga.  Fora do modo o stick direito e SO a
   * camera do jogo e NENHUM cursor e desenhado (feedback do NextOS: os dois
   * no mesmo stick conflitam).  Dentro do modo o stick direito move o
   * ponteiro, R3 toca, e a camera nao recebe o stick. */
  {
    static bool l3_prev = false;
    bool l3 = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
    if (l3 && !l3_prev) {
      g_cursor_mode = !g_cursor_mode;
      debugPrintf("[input] modo ponteiro: %s\n", g_cursor_mode ? "ON" : "OFF");
      if (!g_cursor_mode && g_touch_down) send_touch(false, false);
    }
    l3_prev = l3;
  }
  g_cursor_visible = g_cursor_mode;

  pad_axis(AX_X, lx);
  pad_axis(AX_Y, ly);
  pad_axis(AX_Z, g_cursor_mode ? 0.0f : rx);
  pad_axis(AX_RZ, g_cursor_mode ? 0.0f : ry);
  pad_axis(AX_LTRIGGER, lt < 0.0f ? 0.0f : lt);
  pad_axis(AX_RTRIGGER, rt < 0.0f ? 0.0f : rt);

  if (g_cursor_mode) {
    if (rx != 0.0f || ry != 0.0f) {
      g_cursor_x += rx * 900.0f * dt;
      g_cursor_y += ry * 900.0f * dt;
      if (g_cursor_x < 0) g_cursor_x = 0;
      if (g_cursor_y < 0) g_cursor_y = 0;
      if (g_cursor_x > g_screen_w - 1) g_cursor_x = (float)g_screen_w - 1;
      if (g_cursor_y > g_screen_h - 1) g_cursor_y = (float)g_screen_h - 1;
      send_touch(false, true);
    }
    bool click = SDL_GameControllerGetButton(g_pad, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0;
    if (click != g_cursor_pressed) {
      g_cursor_pressed = click;
      send_touch(click, false);
    }
  }
}

/* ------------------------------------------------------------------ main -- */
int main(int argc, char *argv[]) {
  (void)argc;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  install_crash_handler();
  nxgl_frame_proof_launch_receipt();
  g_test_drive = getenv("OT_TEST_DRIVE") != NULL;
  {
    const char *pt = getenv("OT_PAD_TYPE");
    if (pt) g_pad_type = atoi(pt);
    debugPrintf("[input] tipo de pad anunciado: %d\n", g_pad_type);
  }
  {
    const char *e = getenv("OT_TEST_EXIT");
    if (e) g_test_exit_frame = (unsigned long)strtoul(e, NULL, 10);
  }
  debugPrintf("=== Off The Road 1.18.2 -- so-loader NextOS aarch64 / Mali-450 ===\n");

  char cwd[1024];
  if (!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
  debugPrintf("[main] cwd=%s\n", cwd);

  char assets_dir[1200], userdata_dir[1200];
  snprintf(assets_dir, sizeof(assets_dir), "%s/assets/", cwd);
  snprintf(userdata_dir, sizeof(userdata_dir), "%s/userdata/", cwd);
  mkdir(userdata_dir, 0755);
  jni_set_assets_path(assets_dir);
  jni_set_writable_path(userdata_dir);

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER |
               SDL_INIT_TIMER) != 0)
    fatal_error("SDL_Init: %s", SDL_GetError());

  SDL_DisplayMode dm;
  if (SDL_GetDesktopDisplayMode(0, &dm) == 0 && dm.w > 0 && dm.h > 0) {
    g_screen_w = dm.w;
    g_screen_h = dm.h;
  }
  debugPrintf("[main] resolucao do painel: %dx%d\n", g_screen_w, g_screen_h);

  for (int attempt = 0; attempt < 2 && !g_window; attempt++) {
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    g_window = SDL_CreateWindow("Off The Road", SDL_WINDOWPOS_UNDEFINED,
                                SDL_WINDOWPOS_UNDEFINED, g_screen_w, g_screen_h,
                                SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP |
                                    SDL_WINDOW_SHOWN);
    if (!g_window && attempt == 0) {
      /* processo re-executado com par que falhou na TENTATIVA REAL --
       * desamarrar e repetir pela pilha da firmware (ff4/nxgl 0.2.12). */
      nxgl_provider_repair_receipt rollback_receipt;
      debugPrintf("[main] SDL_CreateWindow: %s\n", SDL_GetError());
      if (nxgl_provider_precontext_rollback(&rollback_receipt)) {
        debugPrintf("[nxgl] %s\n", rollback_receipt.text);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        if (SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) break;
        continue;
      }
    }
  }
  if (!g_window) {
    /* A janela nem existiu: sintoma do provedor cruzado sem criterio vivo.
     * Se houver reparo autorizado, a funcao NAO retorna (re-exec). */
    nxgl_provider_repair_options repair;
    nxgl_provider_repair_receipt repair_receipt;
    debugPrintf("[main] SDL_CreateWindow: %s\n", SDL_GetError());
    nxgl_provider_repair_options_init(&repair);
    repair.video_backend = SDL_GetCurrentVideoDriver();
    repair.required_gles_symbols = k_ot_gles2_symbols;
    repair.required_gles_symbol_count =
        sizeof k_ot_gles2_symbols / sizeof k_ot_gles2_symbols[0];
    repair.teardown = ot_video_teardown;
    repair.argv = argv;
    nxgl_provider_repair_precontext(&repair, NXGL_OPEN_STAGE_V2_WINDOW_CREATE,
                                    NXGL_OPEN_REASON_V2_WINDOW_FAILED,
                                    &repair_receipt);
    debugPrintf("[nxgl] %s\n", repair_receipt.text);
    fatal_error("SDL_CreateWindow: %s", SDL_GetError());
  }
  g_gl_ctx = SDL_GL_CreateContext(g_window);
  if (!g_gl_ctx) fatal_error("SDL_GL_CreateContext: %s", SDL_GetError());
  SDL_GL_MakeCurrent(g_window, g_gl_ctx);
  SDL_GL_SetSwapInterval(1);
  SDL_ShowCursor(SDL_DISABLE);

  {
    const GLubyte *r = glGetString(GL_RENDERER);
    const GLubyte *v = glGetString(GL_VERSION);
    debugPrintf("[gl] %s / %s\n", r ? (const char *)r : "?",
                v ? (const char *)v : "?");

    /* Renderer vazio = assinatura do SONAME cruzado (contexto que aceita tudo
     * e nao desenha nada). O framework decide; renderer saudavel = no-op.
     * O loader e a engine compartilham o MESMO libGLESv2.so.2 (imports.c
     * aponta para os simbolos linkados), entao esta medida JA' e' o caminho
     * da engine. */
    nxgl_frame_proof_set_resolver((void *(*)(const char *))SDL_GL_GetProcAddress);
    {
      nxgl_provider_repair_options repair;
      nxgl_provider_repair_receipt repair_receipt;
      int dw = 0, dh = 0;
      SDL_GL_GetDrawableSize(g_window, &dw, &dh);
      g_repair_window = g_window;
      g_repair_context = g_gl_ctx;
      nxgl_provider_repair_options_init(&repair);
      repair.renderer = (const char *)r;
      repair.video_backend = SDL_GetCurrentVideoDriver();
      repair.window_opened = 1;
      repair.context_current = 1;
      repair.drawable_positive = (dw > 0 && dh > 0);
      repair.required_gles_symbols = k_ot_gles2_symbols;
      repair.required_gles_symbol_count =
          sizeof k_ot_gles2_symbols / sizeof k_ot_gles2_symbols[0];
      repair.teardown = ot_video_teardown;
      repair.argv = argv;
      nxgl_provider_repair_if_renderer_broken(&repair, &repair_receipt);
      debugPrintf("[nxgl] %s\n", repair_receipt.text);
    }
    nxgl_frame_proof_set_video_context(g_screen_w, g_screen_h,
                                       SDL_GetCurrentVideoDriver(),
                                       (const char *)r, (const char *)v);
  }
  /* o contexto TEM de estar corrente aqui: e dele que a engine se apropria
   * (eglGetCurrentContext -> bgfx::setPlatformData) dentro de nativeInit. */
  init_cursor_renderer();

  g_cursor_x = g_screen_w * 0.5f;
  g_cursor_y = g_screen_h * 0.5f;

  jni_shim_init(&g_vm, &g_env);

  /* libc++_shared.so PRIMEIRO: libgame.so referencia ~180 simbolos do
   * namespace std::__ndk1 que so existem la (a glibc/libstdc++ do device tem
   * outro ABI e nao serve). */
  int cxx_n = 0;
  DynLibFunction *cxx = load_module(CXX_SO, CXX_MEM_MB, dynlib_functions,
                                    dynlib_functions_count, &cxx_n);
  debugPrintf("[loader] libc++_shared exporta %d simbolos\n", cxx_n);

  DynLibFunction *tbl = dynlib_functions;
  int tbl_n = dynlib_functions_count;
  if (cxx && cxx_n > 0)
    tbl = tbl_concat(dynlib_functions, dynlib_functions_count, cxx, cxx_n, &tbl_n);

  load_module(GAME_SO, GAME_MEM_MB, tbl, tbl_n, NULL);

  /* perfil grafico: interpoe o PLT interno de setupLevelOfDetail */
  parse_detail_env();
  real_setupLOD =
      (void *)so_find_addr_safe("_ZN12cApplication18setupLevelOfDetailEb");
  if (g_detail_forced && real_setupLOD) {
    int n = so_interpose_export("_ZN12cApplication18setupLevelOfDetailEb",
                                (uintptr_t)my_setupLevelOfDetail);
    debugPrintf("[detail] perfil OT_DETAIL=%d (%s), plt slots=%d\n",
                g_detail_level,
                g_detail_level == 0   ? "low"
                : g_detail_level == 1 ? "medium"
                : g_detail_level == 2 ? "high"
                                      : "ultra",
                n);
    if (n == 0) g_detail_forced = 0; /* sem slot = sem override, jogo original */
  } else {
    debugPrintf("[detail] perfil OT_DETAIL=auto (benchmark do jogo decide)\n");
  }

  e_JNI_OnLoad = (void *)so_find_addr_safe("JNI_OnLoad");
  e_nativeSetApkPath = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath");
  e_nativeInit = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit");
  e_nativeRender = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  e_nativeOnPause = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnPause");
  e_nativeOnResume = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnResume");
  e_nativeOnStart = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnStart");
  e_nativeOnStop = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnStop");
  e_nativeTouchesBegin = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
  e_nativeTouchesEnd = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
  e_nativeTouchesMove = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
  e_nativeKeyDown = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyDown");
  e_nativeKeyUp = (void *)so_find_addr_safe("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyUp");

  e_padConnected = (void *)so_find_addr_safe("_ZN7cocos2d22nativeGamepadConnectedEP7_JNIEnvP8_jobjectii");
  e_padDisconnected = (void *)so_find_addr_safe("_ZN7cocos2d25nativeGamepadDisconnectedEP7_JNIEnvP8_jobjectii");
  e_padButtonDown = (void *)so_find_addr_safe("_ZN7cocos2d23nativeGamepadButtonDownEP7_JNIEnvP8_jobjectiii");
  e_padButtonUp = (void *)so_find_addr_safe("_ZN7cocos2d21nativeGamepadButtonUpEP7_JNIEnvP8_jobjectiii");
  e_padAxis = (void *)so_find_addr_safe("_ZN7cocos2d29nativeGamepadAxisValueChangedEP7_JNIEnvP8_jobjectiiif");
  debugPrintf("[input] api nativa de gamepad: conn=%p down=%p up=%p axis=%p\n",
              (void *)e_padConnected, (void *)e_padButtonDown,
              (void *)e_padButtonUp, (void *)e_padAxis);

  if (!e_nativeInit || !e_nativeRender)
    fatal_error("nativeInit/nativeRender nao encontrados");

  if (e_JNI_OnLoad) {
    jint v = e_JNI_OnLoad(g_vm, NULL);
    debugPrintf("[jni] JNI_OnLoad -> 0x%x\n", v);
  }

  if (e_nativeSetApkPath) {
    char apk[1200];
    snprintf(apk, sizeof(apk), "%s/base.apk", cwd);
    debugPrintf("[cocos] nativeSetApkPath(%s)\n", apk);
    e_nativeSetApkPath(g_env, NULL, jni_make_string(strdup(apk)));
  }

  /* lifecycle na ordem nativa: onStart/onResume antes do 1o nativeRender */
  if (e_nativeOnStart) e_nativeOnStart(g_env, NULL);

  debugPrintf("[cocos] nativeInit(%d, %d)\n", g_screen_w, g_screen_h);
  e_nativeInit(g_env, NULL, g_screen_w, g_screen_h);

  if (e_nativeOnResume) e_nativeOnResume(g_env, NULL);

  open_pad(0);
  for (int i = 0; i < SDL_NumJoysticks() && !g_pad; i++) open_pad(i);

  debugPrintf("=== entrando no laco de quadros ===\n");
  unsigned long frames = 0;
  uint32_t fps_t0 = SDL_GetTicks();
  int fps_n = 0;
  uint32_t last = SDL_GetTicks();

  while (g_running) {
    uint32_t now0 = SDL_GetTicks();
    float dt = (now0 - last) / 1000.0f;
    if (dt <= 0.0f || dt > 0.25f) dt = 1.0f / 60.0f;
    last = now0;

    pump_input(dt);

    /* OT_TEST_DRIVE=1: acelera pelo MESMO caminho nativo que o controle usa
     * (nativeGamepadAxisValueChanged), para provar sem dedo no aparelho que a
     * ponte de gamepad move o veiculo. */
    if (g_test_drive) {
      /* 1) dispensa o tutorial modal (botao A e um toque no centro da caixa)
       * 2) acelera a fundo pelo MESMO caminho nativo que o controle usa */
      switch (frames) {
        case 60:  pad_button(AK_BUTTON_A, true); break;   /* confirmar */
        case 66:  pad_button(AK_BUTTON_A, false); break;
        case 120:
          g_cursor_x = g_screen_w * 0.5f;
          g_cursor_y = g_screen_h * 0.88f;
          send_touch(true, false);
          break;
        case 128: send_touch(false, false); break;
        case 200: debugPrintf("[teste] acelerador a fundo\n"); break;
        case 900:
          if (e_padAxis) {
            e_padAxis(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE, AX_RTRIGGER, 0.0f);
            e_padAxis(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE, AX_X, 0.0f);
          }
          debugPrintf("[teste] acelerador solto\n");
          break;
        default: break;
      }
      if (frames >= 200 && frames < 900 && e_padAxis) {
        e_padAxis(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE, AX_RTRIGGER, 1.0f);
        e_padAxis(g_env, NULL, PAD_DEVICE_ID, PAD_TYPE, AX_GAS, 1.0f);
      }
    }

    opensles_shim_pump_callbacks();
    e_nativeRender(g_env, NULL);
    draw_cursor();
    /* prova de imagem CONTINUA: medir ANTES do present (pos-swap o backbuffer
     * e' indefinido em GPU tile-based e a leitura vira falso PRETO). */
    nxgl_frame_proof_before_present(g_screen_w, g_screen_h);
    SDL_GL_SwapWindow(g_window);

    frames++;
    fps_n++;
    if (g_test_exit_frame && frames == g_test_exit_frame)
      trigger_exit("OT_TEST_EXIT (mesmo caminho do SELECT+START)");
    uint32_t now = SDL_GetTicks();
    if (now - fps_t0 >= 5000) {
      debugPrintf("[fps] %.1f (quadro %lu)\n",
                  fps_n * 1000.0f / (float)(now - fps_t0), frames);
      fps_t0 = now;
      fps_n = 0;
    }
    if (frames == 5 || frames == 120) {
      GLint vp[4] = {0, 0, 0, 0};
      glGetIntegerv(GL_VIEWPORT, vp);
      debugPrintf("[gfx] quadro %lu viewport=%d,%d %dx%d glErr=0x%x\n", frames,
                  vp[0], vp[1], vp[2], vp[3], glGetError());
    }
  }

  debugPrintf("[main] salvando e encerrando\n");
  if (e_nativeOnPause) e_nativeOnPause(g_env, NULL);
  if (e_nativeOnStop) e_nativeOnStop(g_env, NULL);
  debugPrintf("[main] encerrado (quadros=%lu)\n", frames);
  nxgl_frame_proof_publish();
  /* Mali-450: NUNCA SDL_GL_DeleteContext/DestroyWindow/SDL_Quit aqui -- o
   * driver Utgard trava no kernel no desmonte e so a tomada resolve. */
  _exit(0);
}
