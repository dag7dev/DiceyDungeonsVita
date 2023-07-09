/* main.c -- Dicey Dungeon .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2023 Rinnegatamante
 * Copyright (C) 2023 Dag7
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.	See the LICENSE file for details.
 */

// C libs
#include <sys/times.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>

#include <vitasdk.h>
#include <kubridge.h>
#include <vitashark.h>
#include <vitaGL.h>
#include <zlib.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <SDL2/SDL_net.h>
#include <SDL2/SDL_vulkan.h>
#include <SDL2/SDL_system.h>
#include <SDL2/SDL_syswm.h>
#include <SLES/OpenSLES.h>

#include <malloc.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wchar.h>
#include <wctype.h>

#include <math.h>
#include <math_neon.h>

#include <errno.h>
#include <ctype.h>
#include <setjmp.h>
#include <sys/time.h>
#include <sys/stat.h>

#include "main.h"
#include "config.h"
#include "dialog.h"
#include "so_util.h"
#include "sha1.h"

// FNS

void SDL_AndroidBackButton(void)
{
	debugPrintf("SDL_AndroidBackButton\n");
}

int SDL_GetAndroidSDKVersion(void)
{
	return 10;
}

SDL_bool SDL_IsAndroidTV(void)
{
	return SDL_FALSE;
}

SDL_bool SDL_IsChromebook(void)
{
	return SDL_FALSE;
}

SDL_bool SDL_IsDeXMode(void)
{
	return SDL_FALSE;
}

#define ENABLE_DEBUG

#ifdef ENABLE_DEBUG
#define dlog printf
#else
#define dlog
#endif

extern const char *BIONIC_ctype_;
extern const short *BIONIC_tolower_tab_;
extern const short *BIONIC_toupper_tab_;

static char data_path[128];

static char fake_vm[0x1000];
static char fake_env[0x1000];

int framecap = 0;

void *__wrap_calloc(uint32_t nmember, uint32_t size) { return vglCalloc(nmember, size); }
void __wrap_free(void *addr) { vglFree(addr); };
void *__wrap_malloc(uint32_t size) { return vglMalloc(size); };
void *__wrap_memalign(uint32_t alignment, uint32_t size) { return vglMemalign(alignment, size); };
void *__wrap_realloc(void *ptr, uint32_t size) { return vglRealloc(ptr, size); };

int file_exists(const char *path)
{
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;

unsigned int _pthread_stack_default_user = 1 * 1024 * 1024;

so_module diceydungeons_mod, diceydungeonsmain_mod;

void *__wrap_memcpy(void *dest, const void *src, size_t n)
{
	return sceClibMemcpy(dest, src, n);
}

void *__wrap_memmove(void *dest, const void *src, size_t n)
{
	return sceClibMemmove(dest, src, n);
}

int ret4() { return 4; }

void *__wrap_memset(void *s, int c, size_t n)
{
	return sceClibMemset(s, c, n);
}

char *getcwd_hook(char *buf, size_t size)
{
	strcpy(buf, data_path);
	return buf;
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
	*memptr = memalign(alignment, size);
	return 0;
}

int debugPrintf(char *text, ...)
{
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, text);
	vsprintf(string, text, list);
	va_end(list);

	SceUID fd = sceIoOpen("ux0:data/hazard_log.txt", SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND, 0777);
	if (fd >= 0)
	{
		sceIoWrite(fd, string, strlen(string));
		sceIoClose(fd);
	}
#endif
	return 0;
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...)
{
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOG] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_write(int prio, const char *tag, const char *fmt, ...)
{
#ifdef ENABLE_DEBUG
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOGW] %s: %s\n", tag, string);
#endif
	return 0;
}

int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list list)
{
#ifdef ENABLE_DEBUG
	static char string[0x8000];

	vsprintf(string, fmt, list);
	va_end(list);

	printf("[LOGV] %s: %s\n", tag, string);
#endif
	return 0;
}

int ret0(void)
{
	return 0;
}

int ret1(void)
{
	return 1;
}

#define MUTEX_TYPE_NORMAL 0x0000
#define MUTEX_TYPE_RECURSIVE 0x4000
#define MUTEX_TYPE_ERRORCHECK 0x8000

static pthread_t s_pthreadSelfRet;

static void init_static_mutex(pthread_mutex_t **mutex)
{
	pthread_mutex_t *mtxMem = NULL;

	switch ((int)*mutex)
	{
	case MUTEX_TYPE_NORMAL:
	{
		pthread_mutex_t initTmpNormal = PTHREAD_MUTEX_INITIALIZER;
		mtxMem = calloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpNormal, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	case MUTEX_TYPE_RECURSIVE:
	{
		pthread_mutex_t initTmpRec = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
		mtxMem = calloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpRec, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	case MUTEX_TYPE_ERRORCHECK:
	{
		pthread_mutex_t initTmpErr = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER;
		mtxMem = calloc(1, sizeof(pthread_mutex_t));
		sceClibMemcpy(mtxMem, &initTmpErr, sizeof(pthread_mutex_t));
		*mutex = mtxMem;
		break;
	}
	default:
		break;
	}
}

static void init_static_cond(pthread_cond_t **cond)
{
	if (*cond == NULL)
	{
		pthread_cond_t initTmp = PTHREAD_COND_INITIALIZER;
		pthread_cond_t *condMem = calloc(1, sizeof(pthread_cond_t));
		sceClibMemcpy(condMem, &initTmp, sizeof(pthread_cond_t));
		*cond = condMem;
	}
}

int pthread_attr_destroy_soloader(pthread_attr_t **attr)
{
	int ret = pthread_attr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_attr_getstack_soloader(const pthread_attr_t **attr,
								   void **stackaddr,
								   size_t *stacksize)
{
	return pthread_attr_getstack(*attr, stackaddr, stacksize);
}

__attribute__((unused)) int pthread_condattr_init_soloader(pthread_condattr_t **attr)
{
	*attr = calloc(1, sizeof(pthread_condattr_t));

	return pthread_condattr_init(*attr);
}

__attribute__((unused)) int pthread_condattr_destroy_soloader(pthread_condattr_t **attr)
{
	int ret = pthread_condattr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_cond_init_soloader(pthread_cond_t **cond,
							   const pthread_condattr_t **attr)
{
	*cond = calloc(1, sizeof(pthread_cond_t));

	if (attr != NULL)
		return pthread_cond_init(*cond, *attr);
	else
		return pthread_cond_init(*cond, NULL);
}

int pthread_cond_destroy_soloader(pthread_cond_t **cond)
{
	int ret = pthread_cond_destroy(*cond);
	free(*cond);
	return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t **cond)
{
	init_static_cond(cond);
	return pthread_cond_signal(*cond);
}

int pthread_cond_timedwait_soloader(pthread_cond_t **cond,
									pthread_mutex_t **mutex,
									struct timespec *abstime)
{
	init_static_cond(cond);
	init_static_mutex(mutex);
	return pthread_cond_timedwait(*cond, *mutex, abstime);
}

int pthread_create_soloader(pthread_t **thread,
							const pthread_attr_t **attr,
							void *(*start)(void *),
							void *param)
{
	*thread = calloc(1, sizeof(pthread_t));

	if (attr != NULL)
	{
		pthread_attr_setstacksize(*attr, 512 * 1024);
		return pthread_create(*thread, *attr, start, param);
	}
	else
	{
		pthread_attr_t attrr;
		pthread_attr_init(&attrr);
		pthread_attr_setstacksize(&attrr, 512 * 1024);
		return pthread_create(*thread, &attrr, start, param);
	}
}

int pthread_mutexattr_init_soloader(pthread_mutexattr_t **attr)
{
	*attr = calloc(1, sizeof(pthread_mutexattr_t));

	return pthread_mutexattr_init(*attr);
}

int pthread_mutexattr_settype_soloader(pthread_mutexattr_t **attr, int type)
{
	return pthread_mutexattr_settype(*attr, type);
}

int pthread_mutexattr_setpshared_soloader(pthread_mutexattr_t **attr, int pshared)
{
	return pthread_mutexattr_setpshared(*attr, pshared);
}

int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t **attr)
{
	int ret = pthread_mutexattr_destroy(*attr);
	free(*attr);
	return ret;
}

int pthread_mutex_destroy_soloader(pthread_mutex_t **mutex)
{
	int ret = pthread_mutex_destroy(*mutex);
	free(*mutex);
	return ret;
}

int pthread_mutex_init_soloader(pthread_mutex_t **mutex,
								const pthread_mutexattr_t **attr)
{
	*mutex = calloc(1, sizeof(pthread_mutex_t));

	if (attr != NULL)
		return pthread_mutex_init(*mutex, *attr);
	else
		return pthread_mutex_init(*mutex, NULL);
}

int pthread_mutex_lock_soloader(pthread_mutex_t **mutex)
{
	init_static_mutex(mutex);
	return pthread_mutex_lock(*mutex);
}

int pthread_mutex_trylock_soloader(pthread_mutex_t **mutex)
{
	init_static_mutex(mutex);
	return pthread_mutex_trylock(*mutex);
}

int pthread_mutex_unlock_soloader(pthread_mutex_t **mutex)
{
	return pthread_mutex_unlock(*mutex);
}

int pthread_join_soloader(const pthread_t *thread, void **value_ptr)
{
	return pthread_join(*thread, value_ptr);
}

int pthread_cond_wait_soloader(pthread_cond_t **cond, pthread_mutex_t **mutex)
{
	return pthread_cond_wait(*cond, *mutex);
}

int pthread_cond_broadcast_soloader(pthread_cond_t **cond)
{
	return pthread_cond_broadcast(*cond);
}

int pthread_attr_init_soloader(pthread_attr_t **attr)
{
	*attr = calloc(1, sizeof(pthread_attr_t));

	return pthread_attr_init(*attr);
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t **attr, int state)
{
	return pthread_attr_setdetachstate(*attr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t **attr, size_t stacksize)
{
	return pthread_attr_setstacksize(*attr, stacksize);
}

int pthread_attr_setschedparam_soloader(pthread_attr_t **attr,
										const struct sched_param *param)
{
	return pthread_attr_setschedparam(*attr, param);
}

int pthread_attr_setstack_soloader(pthread_attr_t **attr,
								   void *stackaddr,
								   size_t stacksize)
{
	return pthread_attr_setstack(*attr, stackaddr, stacksize);
}

int pthread_setschedparam_soloader(const pthread_t *thread, int policy,
								   const struct sched_param *param)
{
	return pthread_setschedparam(*thread, policy, param);
}

int pthread_getschedparam_soloader(const pthread_t *thread, int *policy,
								   struct sched_param *param)
{
	return pthread_getschedparam(*thread, policy, param);
}

int pthread_detach_soloader(const pthread_t *thread)
{
	return pthread_detach(*thread);
}

int pthread_getattr_np_soloader(pthread_t *thread, pthread_attr_t *attr)
{
	fprintf(stderr, "[WARNING!] Not implemented: pthread_getattr_np\n");
	return 0;
}

int pthread_equal_soloader(const pthread_t *t1, const pthread_t *t2)
{
	if (t1 == t2)
		return 1;
	if (!t1 || !t2)
		return 0;
	return pthread_equal(*t1, *t2);
}

pthread_t *pthread_self_soloader()
{
	s_pthreadSelfRet = pthread_self();
	return &s_pthreadSelfRet;
}

#ifndef MAX_TASK_COMM_LEN
#define MAX_TASK_COMM_LEN 16
#endif

int pthread_setname_np_soloader(const pthread_t *thread, const char *thread_name)
{
	if (thread == 0 || thread_name == NULL)
	{
		return EINVAL;
	}
	size_t thread_name_len = strlen(thread_name);
	if (thread_name_len >= MAX_TASK_COMM_LEN)
	{
		return ERANGE;
	}

	// TODO: Implement the actual name setting if possible
	fprintf(stderr, "PTHR: pthread_setname_np with name %s\n", thread_name);

	return 0;
}

int clock_gettime_hook(int clk_id, struct timespec *t)
{
	struct timeval now;
	int rv = gettimeofday(&now, NULL);
	if (rv)
		return rv;
	t->tv_sec = now.tv_sec;
	t->tv_nsec = now.tv_usec * 1000;

	return 0;
}

int GetCurrentThreadId(void)
{
	return sceKernelGetThreadId();
}

extern void *__aeabi_ldiv0;

int GetEnv(void *vm, void **env, int r2)
{
	*env = fake_env;
	return 0;
}

void throw_exc(char **str, void *a, int b)
{
	printf("throwing %s\n", *str);
}

FILE *fopen_hook(char *fname, char *mode)
{
	FILE *f;
	char real_fname[256];
	// printf("fopen(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4))
	{
		sprintf(real_fname, "%s/%s", data_path, fname);
		f = fopen(real_fname, mode);
	}
	else
	{
		f = fopen(fname, mode);
	}
	return f;
}

int open_hook(const char *fname, int flags, mode_t mode)
{
	int f;
	char real_fname[256];
	// printf("open(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4))
	{
		sprintf(real_fname, "%s/%s", data_path, fname);
		f = open(real_fname, flags, mode);
	}
	else
	{
		f = open(fname, flags, mode);
	}
	return f;
}

extern void *__aeabi_atexit;
extern void *__aeabi_ddiv;
extern void *__aeabi_dmul;
extern void *__aeabi_dadd;
extern void *__aeabi_i2d;
extern void *__aeabi_idiv;
extern void *__aeabi_idivmod;
extern void *__aeabi_ldivmod;
extern void *__aeabi_uidiv;
extern void *__aeabi_uidivmod;
extern void *__aeabi_uldivmod;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__cxa_call_unexpected;
extern void *__gnu_unwind_frame;
extern void *__stack_chk_fail;
int open(const char *pathname, int flags);

static int __stack_chk_guard_fake = 0x42424242;

static FILE __sF_fake[0x1000][3];

int stat_hook(const char *pathname, void *statbuf)
{
	// dlog("stat(%s)\n", pathname);
	struct stat st;
	int res = stat(pathname, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
	return memalign(length, 0x1000);
}

int munmap(void *addr, size_t length)
{
	free(addr);
	return 0;
}

int fstat_hook(int fd, void *statbuf)
{
	struct stat st;
	int res = fstat(fd, &st);
	if (res == 0)
		*(uint64_t *)(statbuf + 0x30) = st.st_size;
	return res;
}

extern void *__cxa_guard_acquire;
extern void *__cxa_guard_release;

void *sceClibMemclr(void *dst, SceSize len)
{
	if (!dst)
	{
		printf("memclr on NULL\n");
		return NULL;
	}
	return sceClibMemset(dst, 0, len);
}

void *sceClibMemset2(void *dst, SceSize len, int ch)
{
	return sceClibMemset(dst, ch, len);
}

void *Android_JNI_GetEnv()
{
	return fake_env;
}

char *SDL_AndroidGetExternalStoragePath()
{
	return "ux0:data/diceydungeons";
}

char *SDL_AndroidGetInternalStoragePath()
{
	return "ux0:data/diceydungeons";
}

char *SDL_GetPrefPath_hook(const char *org, const char *app)
{
	char *r = SDL_GetPrefPath(org, app);
	printf("Pref Path: %s\n", r);
	r[strlen(r) - 1] = 0;
	return r;
}

int g_SDL_BufferGeometry_w;
int g_SDL_BufferGeometry_h;

void abort_hook()
{
	// dlog("ABORT CALLED!!!\n");
	uint8_t *p = NULL;
	p[0] = 1;
}

int ret99()
{
	return 99;
}

int chdir_hook(const char *path)
{
	return 0;
}

GLint glGetUniformLocation_fake(GLuint program, const GLchar *name)
{
	if (!strcmp(name, "texture"))
		return glGetUniformLocation(program, "_texture");
	return glGetUniformLocation(program, name);
}

static so_default_dynlib gl_hook[] = {
	{"glPixelStorei", (uintptr_t)&ret0},
};
static size_t gl_numhook = sizeof(gl_hook) / sizeof(*gl_hook);

void *SDL_GL_GetProcAddress_fake(const char *symbol)
{
	dlog("looking for symbol %s\n", symbol);
	for (size_t i = 0; i < gl_numhook; ++i)
	{
		if (!strcmp(symbol, gl_hook[i].symbol))
		{
			return (void *)gl_hook[i].func;
		}
	}
	void *r = vglGetProcAddress(symbol);
	if (!r)
	{
		dlog("Cannot find symbol %s\n", symbol);
	}
	return r;
}

#define SCE_ERRNO_MASK 0xFF

#define DT_DIR 4
#define DT_REG 8

struct android_dirent
{
	char pad[18];
	unsigned char d_type;
	char d_name[256];
};

typedef struct
{
	SceUID uid;
	struct android_dirent dir;
} android_DIR;

int closedir_fake(android_DIR *dirp)
{
	if (!dirp || dirp->uid < 0)
	{
		errno = EBADF;
		return -1;
	}

	int res = sceIoDclose(dirp->uid);
	dirp->uid = -1;

	free(dirp);

	if (res < 0)
	{
		errno = res & SCE_ERRNO_MASK;
		return -1;
	}

	errno = 0;
	return 0;
}

android_DIR *opendir_fake(const char *dirname)
{
	printf("opendir(%s)\n", dirname);
	SceUID uid = sceIoDopen(dirname);

	if (uid < 0)
	{
		errno = uid & SCE_ERRNO_MASK;
		return NULL;
	}

	android_DIR *dirp = calloc(1, sizeof(android_DIR));

	if (!dirp)
	{
		sceIoDclose(uid);
		errno = ENOMEM;
		return NULL;
	}

	dirp->uid = uid;

	errno = 0;
	return dirp;
}

struct android_dirent *readdir_fake(android_DIR *dirp)
{
	if (!dirp)
	{
		errno = EBADF;
		return NULL;
	}

	SceIoDirent sce_dir;
	int res = sceIoDread(dirp->uid, &sce_dir);

	if (res < 0)
	{
		errno = res & SCE_ERRNO_MASK;
		return NULL;
	}

	if (res == 0)
	{
		errno = 0;
		return NULL;
	}

	dirp->dir.d_type = SCE_S_ISDIR(sce_dir.d_stat.st_mode) ? DT_DIR : DT_REG;
	strcpy(dirp->dir.d_name, sce_dir.d_name);
	return &dirp->dir;
}

SDL_Surface *IMG_Load_hook(const char *file)
{
	char real_fname[256];
	printf("loading %s\n", file);
	if (strncmp(file, "ux0:", 4))
	{
		sprintf(real_fname, "%s/assets/%s", data_path, file);
		return IMG_Load(real_fname);
	}
	return IMG_Load(file);
}

SDL_Texture *IMG_LoadTexture_hook(SDL_Renderer *renderer, const char *file)
{
	char real_fname[256];
	// printf("loading %s\n", file);
	if (strncmp(file, "ux0:", 4))
	{
		sprintf(real_fname, "%s/assets/%s", data_path, file);
		return IMG_LoadTexture(renderer, real_fname);
	}
	return IMG_LoadTexture(renderer, file);
}

SDL_RWops *SDL_RWFromFile_hook(const char *fname, const char *mode)
{
	SDL_RWops *f;
	char real_fname[256];
	printf("SDL_RWFromFile(%s,%s)\n", fname, mode);
	if (strncmp(fname, "ux0:", 4))
	{
		sprintf(real_fname, "%s/assets/%s", data_path, fname);
		printf("SDL_RWFromFile patched to %s\n", real_fname);
		f = SDL_RWFromFile(real_fname, mode);
	}
	else
	{
		f = SDL_RWFromFile(fname, mode);
	}
	return f;
}

Mix_Music *Mix_LoadMUS_hook(const char *fname)
{
	Mix_Music *f;
	char real_fname[256];
	printf("Mix_LoadMUS(%s)\n", fname);
	if (strncmp(fname, "ux0:", 4))
	{
		sprintf(real_fname, "%s/assets/%s", data_path, fname);
		f = Mix_LoadMUS(real_fname);
	}
	else
	{
		f = Mix_LoadMUS(fname);
	}
	return f;
}

int Mix_OpenAudio_hook(int frequency, Uint16 format, int channels, int chunksize)
{
	return Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024);
}

extern void SDL_ResetKeyboard(void);

size_t __strlen_chk(const char *s, size_t s_len)
{
	return strlen(s);
}

SDL_Window *SDL_CreateWindow_hook(const char *title, int x, int y, int w, int h, Uint32 flags)
{
	return SDL_CreateWindow("hazard", 0, 0, SCREEN_W, SCREEN_H, flags);
}

uint64_t lseek64(int fd, uint64_t offset, int whence)
{
	return lseek(fd, offset, whence);
}

char *SDL_GetBasePath_hook()
{
	void *ret = malloc(256);
	sprintf(ret, "%s/assets/", data_path);
	printf("SDL_GetBasePath\n");
	return ret;
}

void SDL_GetVersion_fake(SDL_version *ver)
{
	ver->major = 2;
	ver->minor = 0;
	ver->patch = 10;
}

const char *SDL_JoystickName_fake(SDL_Joystick *joystick)
{
	return "Totally PS4 Controller ( ͡° ͜ʖ ͡°)";
}

void glBindAttribLocation_fake(GLuint program, GLuint index, const GLchar *name)
{
	if (index == 2)
	{
		glBindAttribLocation(program, 2, "extents");
		glBindAttribLocation(program, 2, "vertcol");
	}
	glBindAttribLocation(program, index, name);
}

int SDL_OpenAudio_fake(SDL_AudioSpec *desired, SDL_AudioSpec *obtained)
{
	desired->freq = 44100;
	return SDL_OpenAudio(desired, obtained);
}

static so_default_dynlib default_dynlib[] = {
	{"glTexParameteri", (uintptr_t)&glTexParameteri},
	{"glGetError", (uintptr_t)&ret0},
	{"glReadPixels", (uintptr_t)&glReadPixels},
	{"glShaderSource", (uintptr_t)&glShaderSource},
	{"glGetUniformLocation", (uintptr_t)&glGetUniformLocation_fake},
	{"glBindAttribLocation", (uintptr_t)&glBindAttribLocation_fake},
	{"SDL_GetPlatform", (uintptr_t)&SDL_GetPlatform},
	{"sincosf", (uintptr_t)&sincosf},
	{"opendir", (uintptr_t)&opendir_fake},
	{"readdir", (uintptr_t)&readdir_fake},
	{"closedir", (uintptr_t)&closedir_fake},
	{"g_SDL_BufferGeometry_w", (uintptr_t)&g_SDL_BufferGeometry_w},
	{"g_SDL_BufferGeometry_h", (uintptr_t)&g_SDL_BufferGeometry_h},
	{"__aeabi_memclr", (uintptr_t)&sceClibMemclr},
	{"__aeabi_memclr4", (uintptr_t)&sceClibMemclr},
	{"__aeabi_memclr8", (uintptr_t)&sceClibMemclr},
	{"__aeabi_memcpy4", (uintptr_t)&sceClibMemcpy},
	{"__aeabi_memcpy8", (uintptr_t)&sceClibMemcpy},
	{"__aeabi_memmove4", (uintptr_t)&memmove},
	{"__aeabi_memmove8", (uintptr_t)&memmove},
	{"__aeabi_memcpy", (uintptr_t)&sceClibMemcpy},
	{"__aeabi_memmove", (uintptr_t)&memmove},
	{"__aeabi_memset", (uintptr_t)&sceClibMemset2},
	{"__aeabi_memset4", (uintptr_t)&sceClibMemset2},
	{"__aeabi_memset8", (uintptr_t)&sceClibMemset2},
	{"__aeabi_atexit", (uintptr_t)&__aeabi_atexit},
	{"__aeabi_uidiv", (uintptr_t)&__aeabi_uidiv},
	{"__aeabi_uidivmod", (uintptr_t)&__aeabi_uidivmod},
	{"__aeabi_ldivmod", (uintptr_t)&__aeabi_ldivmod},
	{"__aeabi_idivmod", (uintptr_t)&__aeabi_idivmod},
	{"__aeabi_idiv", (uintptr_t)&__aeabi_idiv},
	{"__aeabi_ddiv", (uintptr_t)&__aeabi_ddiv},
	{"__aeabi_dmul", (uintptr_t)&__aeabi_dmul},
	{"__aeabi_dadd", (uintptr_t)&__aeabi_dadd},
	{"__aeabi_i2d", (uintptr_t)&__aeabi_i2d},
	{"__android_log_print", (uintptr_t)&__android_log_print},
	{"__android_log_vprint", (uintptr_t)&__android_log_vprint},
	{"__android_log_write", (uintptr_t)&__android_log_write},
	{"__cxa_atexit", (uintptr_t)&__cxa_atexit},
	{"__cxa_call_unexpected", (uintptr_t)&__cxa_call_unexpected},
	{"__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire},
	{"__cxa_guard_release", (uintptr_t)&__cxa_guard_release},
	{"__cxa_finalize", (uintptr_t)&__cxa_finalize},
	{"__errno", (uintptr_t)&__errno},
	{"__strlen_chk", (uintptr_t)&__strlen_chk},
	{"__gnu_unwind_frame", (uintptr_t)&__gnu_unwind_frame},
	{"__gnu_Unwind_Find_exidx", (uintptr_t)&ret0},
	{"dl_unwind_find_exidx", (uintptr_t)&ret0},
	{"__sF", (uintptr_t)&__sF_fake},
	{"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},
	{"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
	{"_ctype_", (uintptr_t)&BIONIC_ctype_},
	{"_tolower_tab_", (uintptr_t)&BIONIC_tolower_tab_},
	{"_toupper_tab_", (uintptr_t)&BIONIC_toupper_tab_},
	{"abort", (uintptr_t)&abort_hook},
	{"accept", (uintptr_t)&accept},
	{"access", (uintptr_t)&access},
	{"acos", (uintptr_t)&acos},
	{"acosh", (uintptr_t)&acosh},
	{"asctime", (uintptr_t)&asctime},
	{"acosf", (uintptr_t)&acosf},
	{"acosl", (uintptr_t)&acosl},
	{"asin", (uintptr_t)&asin},
	{"asinh", (uintptr_t)&asinh},
	{"asinf", (uintptr_t)&asinf},
	{"atan", (uintptr_t)&atan},
	{"atanh", (uintptr_t)&atanh},
	{"atanl", (uintptr_t)&atanl},
	{"atan2l", (uintptr_t)&atan2l},
	{"atan2", (uintptr_t)&atan2},
	{"atan2f", (uintptr_t)&atan2f},
	{"atanf", (uintptr_t)&atanf},
	{"atoi", (uintptr_t)&atoi},
	{"atol", (uintptr_t)&atol},
	{"atoll", (uintptr_t)&atoll},
	{"basename", (uintptr_t)&basename},
	{"bind", (uintptr_t)&bind},
	{"bsd_signal", (uintptr_t)&ret0},
	{"bsearch", (uintptr_t)&bsearch},
	{"btowc", (uintptr_t)&btowc},
	{"calloc", (uintptr_t)&calloc},
	{"ceil", (uintptr_t)&ceil},
	{"ceilf", (uintptr_t)&ceilf},
	{"chdir", (uintptr_t)&chdir_hook},
	{"clearerr", (uintptr_t)&clearerr},
	{"clock", (uintptr_t)&clock},
	{"clock_gettime", (uintptr_t)&clock_gettime_hook},
	{"close", (uintptr_t)&close},
	{"closedir", (uintptr_t)&close},
	{"connect", (uintptr_t)&connect},
	{"cos", (uintptr_t)&cos},
	{"cosf", (uintptr_t)&cosf},
	{"cosh", (uintptr_t)&cosh},
	{"crc32", (uintptr_t)&crc32},
	{"deflate", (uintptr_t)&deflate},
	{"deflateEnd", (uintptr_t)&deflateEnd},
	{"deflateInit_", (uintptr_t)&deflateInit_},
	{"deflateInit2_", (uintptr_t)&deflateInit2_},
	{"deflateReset", (uintptr_t)&deflateReset},
	{"dlopen", (uintptr_t)&ret0},
	{"dlclose", (uintptr_t)&ret0},
	{"dlerror", (uintptr_t)&ret0},
	{"dlsym", (uintptr_t)&ret1},
	{"exit", (uintptr_t)&exit},
	{"exit", (uintptr_t)&exit},
	{"exp", (uintptr_t)&exp},
	{"exp2", (uintptr_t)&exp2},
	{"expl", (uintptr_t)&expl},
	{"exp2l", (uintptr_t)&exp2l},
	{"expf", (uintptr_t)&expf},
	{"fabsf", (uintptr_t)&fabsf},
	{"fclose", (uintptr_t)&fclose},
	{"fcntl", (uintptr_t)&ret0},
	// { "fdopen", (uintptr_t)&fdopen },
	{"feof", (uintptr_t)&feof},
	{"ferror", (uintptr_t)&ferror},
	{"fflush", (uintptr_t)&fflush},
	{"fgets", (uintptr_t)&fgets},
	{"floor", (uintptr_t)&floor},
	{"fileno", (uintptr_t)&fileno},
	{"floor", (uintptr_t)&floor},
	{"floorf", (uintptr_t)&floorf},
	{"fmod", (uintptr_t)&fmod},
	{"fmodf", (uintptr_t)&fmodf},
	{"fork", (uintptr_t)&fork},
	{"fopen", (uintptr_t)&fopen_hook},
	{"free", (uintptr_t)&free},
	{"open", (uintptr_t)&open_hook},
	{"fprintf", (uintptr_t)&fprintf},
	{"fputc", (uintptr_t)&fputc},
	// { "fputwc", (uintptr_t)&fputwc },
	// { "fputs", (uintptr_t)&fputs },
	{"fread", (uintptr_t)&fread},
	{"freeaddrinfo", (uintptr_t)&freeaddrinfo},
	{"frexp", (uintptr_t)&frexp},
	{"frexpf", (uintptr_t)&frexpf},
	// { "fscanf", (uintptr_t)&fscanf },
	{"fseek", (uintptr_t)&fseek},
	{"fseeko", (uintptr_t)&fseeko},
	{"fstat", (uintptr_t)&fstat_hook},
	{"ftell", (uintptr_t)&ftell},
	{"ftello", (uintptr_t)&ftello},
	// { "ftruncate", (uintptr_t)&ftruncate },
	{"fwrite", (uintptr_t)&fwrite},
	{"getc", (uintptr_t)&getc},
	{"getaddrinfo", (uintptr_t)&getaddrinfo},
	{"getpid", (uintptr_t)&ret0},
	{"getchar", (uintptr_t)&getchar},
	{"getcwd", (uintptr_t)&getcwd_hook},
	{"getenv", (uintptr_t)&ret0},
	{"getwc", (uintptr_t)&getwc},
	{"gettimeofday", (uintptr_t)&gettimeofday},
	{"gmtime", (uintptr_t)&gmtime},
	{"gmtime_r", (uintptr_t)&gmtime_r},
	{"gzopen", (uintptr_t)&gzopen},
	{"inflate", (uintptr_t)&inflate},
	{"inflateEnd", (uintptr_t)&inflateEnd},
	{"inflateInit_", (uintptr_t)&inflateInit_},
	{"inflateInit2_", (uintptr_t)&inflateInit2_},
	{"inflateReset", (uintptr_t)&inflateReset},
	{"isascii", (uintptr_t)&isascii},
	//{"ioctl", (uintptr_t)&ioctl},
	{"isalnum", (uintptr_t)&isalnum},
	{"isalpha", (uintptr_t)&isalpha},
	{"iscntrl", (uintptr_t)&iscntrl},
	{"isgraph", (uintptr_t)&isgraph},
	{"isdigit", (uintptr_t)&isdigit},
	{"islower", (uintptr_t)&islower},
	{"isupper", (uintptr_t)&isupper},
	{"ispunct", (uintptr_t)&ispunct},
	{"isprint", (uintptr_t)&isprint},
	{"isspace", (uintptr_t)&isspace},
	{"iswalpha", (uintptr_t)&iswalpha},
	{"iswcntrl", (uintptr_t)&iswcntrl},
	{"iswctype", (uintptr_t)&iswctype},
	{"iswdigit", (uintptr_t)&iswdigit},
	{"iswdigit", (uintptr_t)&iswdigit},
	{"iswlower", (uintptr_t)&iswlower},
	{"iswprint", (uintptr_t)&iswprint},
	{"iswpunct", (uintptr_t)&iswpunct},
	{"iswspace", (uintptr_t)&iswspace},
	{"iswupper", (uintptr_t)&iswupper},
	{"iswxdigit", (uintptr_t)&iswxdigit},
	{"isxdigit", (uintptr_t)&isxdigit},
	{"ldexp", (uintptr_t)&ldexp},
	{"ldexpf", (uintptr_t)&ldexpf},
	{"listen", (uintptr_t)&listen},
	{"localtime", (uintptr_t)&localtime},
	{"localtime_r", (uintptr_t)&localtime_r},
	{"log", (uintptr_t)&log},
	{"logl", (uintptr_t)&logl},
	{"logf", (uintptr_t)&logf},
	{"log10", (uintptr_t)&log10},
	{"log10f", (uintptr_t)&log10f},
	{"longjmp", (uintptr_t)&longjmp},
	{"lrand48", (uintptr_t)&lrand48},
	{"lrint", (uintptr_t)&lrint},
	{"lrintf", (uintptr_t)&lrintf},
	{"lseek", (uintptr_t)&lseek},
	{"lseek64", (uintptr_t)&lseek64},
	{"malloc", (uintptr_t)&malloc},
	{"mbrtowc", (uintptr_t)&mbrtowc},
	{"memalign", (uintptr_t)&memalign},
	{"memchr", (uintptr_t)&sceClibMemchr},
	{"memcmp", (uintptr_t)&memcmp},
	{"memcpy", (uintptr_t)&sceClibMemcpy},
	{"memmove", (uintptr_t)&memmove},
	{"memset", (uintptr_t)&sceClibMemset},
	{"mkdir", (uintptr_t)&mkdir},
	{"mktime", (uintptr_t)&mktime},
	{"nanosleep", (uintptr_t)&nanosleep},
	// { "mmap", (uintptr_t)&mmap},
	// { "munmap", (uintptr_t)&munmap},
	{"modf", (uintptr_t)&modf},
	{"modff", (uintptr_t)&modff},
	{"opendir", (uintptr_t)&opendir},
	{"pipe", (uintptr_t)&pipe},
	{"poll", (uintptr_t)&poll},
	{"pow", (uintptr_t)&pow},
	{"powf", (uintptr_t)&powf},
	{"printf", (uintptr_t)&printf},
	{"pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader},
	{"pthread_attr_getstack", (uintptr_t)&pthread_attr_getstack_soloader},
	{"pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader},
	{"pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_soloader},
	{"pthread_attr_setschedparam", (uintptr_t)&pthread_attr_setschedparam_soloader},
	{"pthread_attr_setstack", (uintptr_t)&pthread_attr_setstack_soloader},
	{"pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_soloader},
	{"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_soloader},
	{"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_soloader},
	{"pthread_cond_init", (uintptr_t)&pthread_cond_init_soloader},
	{"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_soloader},
	{"pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_soloader},
	{"pthread_cond_wait", (uintptr_t)&pthread_cond_wait_soloader},
	{"pthread_create", (uintptr_t)&pthread_create_soloader},
	{"pthread_detach", (uintptr_t)&pthread_detach_soloader},
	{"pthread_equal", (uintptr_t)&pthread_equal_soloader},
	{"pthread_exit", (uintptr_t)&pthread_exit},
	{"pthread_getattr_np", (uintptr_t)&pthread_getattr_np_soloader},
	{"pthread_getschedparam", (uintptr_t)&pthread_getschedparam_soloader},
	{"pthread_getspecific", (uintptr_t)&pthread_getspecific},
	{"pthread_key_create", (uintptr_t)&pthread_key_create},
	{"pthread_key_delete", (uintptr_t)&pthread_key_delete},
	{"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader},
	{"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader},
	{"pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader},
	{"pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader},
	{"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader},
	{"pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_soloader},
	{"pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_soloader},
	{"pthread_mutexattr_setpshared", (uintptr_t)&pthread_mutexattr_setpshared_soloader},
	{"pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_soloader},
	{"pthread_once", (uintptr_t)&pthread_once},
	{"pthread_self", (uintptr_t)&pthread_self_soloader},
	{"pthread_setschedparam", (uintptr_t)&pthread_setschedparam_soloader},
	{"pthread_setspecific", (uintptr_t)&pthread_setspecific},
	{"sched_get_priority_min", (uintptr_t)&ret0},
	{"sched_get_priority_max", (uintptr_t)&ret99},
	{"putc", (uintptr_t)&putc},
	{"puts", (uintptr_t)&puts},
	{"putwc", (uintptr_t)&putwc},
	{"qsort", (uintptr_t)&qsort},
	{"rand", (uintptr_t)&rand},
	{"read", (uintptr_t)&read},
	{"realpath", (uintptr_t)&realpath},
	{"realloc", (uintptr_t)&realloc},
	{"recv", (uintptr_t)&recv},
	{"recvfrom", (uintptr_t)&recvfrom},
	{"rename", (uintptr_t)&rename},
	{"rmdir", (uintptr_t)&rmdir},
	{"roundf", (uintptr_t)&roundf},
	{"rint", (uintptr_t)&rint},
	{"rintf", (uintptr_t)&rintf},
	{"send", (uintptr_t)&send},
	{"sendto", (uintptr_t)&sendto},
	{"scalbn", (uintptr_t)&scalbn},
	{"select", (uintptr_t)&select},
	{"setbuf", (uintptr_t)&setbuf},
	{"setenv", (uintptr_t)&ret0},
	{"setjmp", (uintptr_t)&setjmp},
	{"setlocale", (uintptr_t)&ret0},
	{"setsockopt", (uintptr_t)&setsockopt},
	{"setvbuf", (uintptr_t)&setvbuf},
	{"shutdown", (uintptr_t)&shutdown},
	{"sin", (uintptr_t)&sin},
	{"sinf", (uintptr_t)&sinf},
	{"sinh", (uintptr_t)&sinh},
	//{ "sincos", (uintptr_t)&sincos },
	{"snprintf", (uintptr_t)&snprintf},
	{"socket", (uintptr_t)&socket},
	{"sprintf", (uintptr_t)&sprintf},
	{"sqrt", (uintptr_t)&sqrt},
	{"sqrtf", (uintptr_t)&sqrtf},
	{"srand", (uintptr_t)&srand},
	{"srand48", (uintptr_t)&srand48},
	{"sscanf", (uintptr_t)&sscanf},
	{"stat", (uintptr_t)&stat_hook},
	{"strcasecmp", (uintptr_t)&strcasecmp},
	{"strcasestr", (uintptr_t)&strstr},
	{"strcat", (uintptr_t)&strcat},
	{"strchr", (uintptr_t)&strchr},
	{"strcmp", (uintptr_t)&sceClibStrcmp},
	{"strcoll", (uintptr_t)&strcoll},
	{"strcpy", (uintptr_t)&strcpy},
	{"strcspn", (uintptr_t)&strcspn},
	{"strdup", (uintptr_t)&strdup},
	{"strerror", (uintptr_t)&strerror},
	{"strftime", (uintptr_t)&strftime},
	{"strlcpy", (uintptr_t)&strlcpy},
	{"strlen", (uintptr_t)&strlen},
	{"strncasecmp", (uintptr_t)&sceClibStrncasecmp},
	{"strncat", (uintptr_t)&sceClibStrncat},
	{"strncmp", (uintptr_t)&sceClibStrncmp},
	{"strncpy", (uintptr_t)&sceClibStrncpy},
	{"strpbrk", (uintptr_t)&strpbrk},
	{"strrchr", (uintptr_t)&sceClibStrrchr},
	{"strstr", (uintptr_t)&sceClibStrstr},
	{"strtod", (uintptr_t)&strtod},
	{"strtol", (uintptr_t)&strtol},
	{"strtoul", (uintptr_t)&strtoul},
	{"strtoll", (uintptr_t)&strtoll},
	{"strtoull", (uintptr_t)&strtoull},
	{"strtok", (uintptr_t)&strtok},
	{"strxfrm", (uintptr_t)&strxfrm},
	{"sysconf", (uintptr_t)&ret0},
	{"system", (uintptr_t)&system},
	{"tan", (uintptr_t)&tan},
	{"tanf", (uintptr_t)&tanf},
	{"tanh", (uintptr_t)&tanh},
	{"time", (uintptr_t)&time},
	{"times", (uintptr_t)&times},
	{"tolower", (uintptr_t)&tolower},
	{"toupper", (uintptr_t)&toupper},
	{"towlower", (uintptr_t)&towlower},
	{"towupper", (uintptr_t)&towupper},
	{"ungetc", (uintptr_t)&ungetc},
	{"ungetwc", (uintptr_t)&ungetwc},
	{"unlink", (uintptr_t)&unlink},
	{"usleep", (uintptr_t)&usleep},
	{"vasprintf", (uintptr_t)&vasprintf},
	{"vfprintf", (uintptr_t)&vfprintf},
	{"vprintf", (uintptr_t)&vprintf},
	{"vsnprintf", (uintptr_t)&vsnprintf},
	{"vsscanf", (uintptr_t)&vsscanf},
	{"vsprintf", (uintptr_t)&vsprintf},
	{"vswprintf", (uintptr_t)&vswprintf},
	{"wcrtomb", (uintptr_t)&wcrtomb},
	{"wcscoll", (uintptr_t)&wcscoll},
	{"wcscmp", (uintptr_t)&wcscmp},
	{"wcsncpy", (uintptr_t)&wcsncpy},
	{"wcsftime", (uintptr_t)&wcsftime},
	{"wcslen", (uintptr_t)&wcslen},
	{"wcsxfrm", (uintptr_t)&wcsxfrm},
	{"wctob", (uintptr_t)&wctob},
	{"wctype", (uintptr_t)&wctype},
	{"wmemchr", (uintptr_t)&wmemchr},
	{"wmemcmp", (uintptr_t)&wmemcmp},
	{"wmemcpy", (uintptr_t)&wmemcpy},
	{"wmemmove", (uintptr_t)&wmemmove},
	{"wmemset", (uintptr_t)&wmemset},
	{"write", (uintptr_t)&write},
	{"sigaction", (uintptr_t)&ret0},
	{"zlibVersion", (uintptr_t)&zlibVersion},
	// { "writev", (uintptr_t)&writev },
	{"SDL_AndroidGetActivityClass", (uintptr_t)&ret0},
	{"SDL_IsTextInputActive", (uintptr_t)&SDL_IsTextInputActive},
	{"SDL_GameControllerEventState", (uintptr_t)&SDL_GameControllerEventState},
	{"SDL_WarpMouseInWindow", (uintptr_t)&SDL_WarpMouseInWindow},

	{"SDL_AndroidBackButton", (uintptr_t)&SDL_AndroidBackButton},
	//{"SDL_DYNAPI_entry", (uintptr_t)&SDL_DYNAPI_entry},
	{"SDL_GetAndroidSDKVersion", (uintptr_t)&SDL_GetAndroidSDKVersion},
	{"SDL_IsAndroidTV", (uintptr_t)&SDL_IsAndroidTV},
	{"SDL_IsChromebook", (uintptr_t)&SDL_IsChromebook},
	{"SDL_IsDeXMode", (uintptr_t)&SDL_IsDeXMode},

	{"SDL_Vulkan_CreateSurface", (uintptr_t)&SDL_Vulkan_CreateSurface},
	{"SDL_Vulkan_GetDrawableSize", (uintptr_t)&SDL_Vulkan_GetDrawableSize},
	{"SDL_Vulkan_GetInstanceExtensions", (uintptr_t)&SDL_Vulkan_GetInstanceExtensions},
	{"SDL_Vulkan_GetVkGetInstanceProcAddr", (uintptr_t)&SDL_Vulkan_GetVkGetInstanceProcAddr},
	{"SDL_Vulkan_LoadLibrary", (uintptr_t)&SDL_Vulkan_LoadLibrary},
	{"SDL_Vulkan_UnloadLibrary", (uintptr_t)&SDL_Vulkan_UnloadLibrary},
	{"SDL_AndroidGetExternalStoragePath", (uintptr_t)&SDL_AndroidGetExternalStoragePath},
	{"SDL_AndroidGetInternalStoragePath", (uintptr_t)&SDL_AndroidGetInternalStoragePath},
	{"SDL_Android_Init", (uintptr_t)&ret1},
	{"SDL_AddTimer", (uintptr_t)&SDL_AddTimer},
	{"SDL_CondSignal", (uintptr_t)&SDL_CondSignal},
	{"SDL_CondWait", (uintptr_t)&SDL_CondWait},
	{"SDL_ConvertSurfaceFormat", (uintptr_t)&SDL_ConvertSurfaceFormat},
	{"SDL_CreateCond", (uintptr_t)&SDL_CreateCond},
	{"SDL_CreateMutex", (uintptr_t)&SDL_CreateMutex},
	{"SDL_CreateRenderer", (uintptr_t)&SDL_CreateRenderer},
	{"SDL_CreateRGBSurface", (uintptr_t)&SDL_CreateRGBSurface},
	{"SDL_CreateTexture", (uintptr_t)&SDL_CreateTexture},
	{"SDL_CreateTextureFromSurface", (uintptr_t)&SDL_CreateTextureFromSurface},
	{"SDL_CreateThread", (uintptr_t)&SDL_CreateThread},
	{"SDL_CreateWindow", (uintptr_t)&SDL_CreateWindow_hook},
	{"SDL_Delay", (uintptr_t)&SDL_Delay},
	{"SDL_DestroyMutex", (uintptr_t)&SDL_DestroyMutex},
	{"SDL_DestroyRenderer", (uintptr_t)&SDL_DestroyRenderer},
	{"SDL_DestroyTexture", (uintptr_t)&SDL_DestroyTexture},
	{"SDL_DestroyWindow", (uintptr_t)&SDL_DestroyWindow},
	{"SDL_FillRect", (uintptr_t)&SDL_FillRect},
	{"SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface},
	{"SDL_GetCurrentDisplayMode", (uintptr_t)&SDL_GetCurrentDisplayMode},
	{"SDL_GetDisplayMode", (uintptr_t)&SDL_GetDisplayMode},
	{"SDL_GetError", (uintptr_t)&SDL_GetError},
	{"SDL_GetModState", (uintptr_t)&SDL_GetModState},
	{"SDL_GetMouseState", (uintptr_t)&SDL_GetMouseState},
	{"SDL_GetRGBA", (uintptr_t)&SDL_GetRGBA},
	{"SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW},
	{"SDL_GetNumDisplayModes", (uintptr_t)&SDL_GetNumDisplayModes},
	{"SDL_GetRendererInfo", (uintptr_t)&SDL_GetRendererInfo},
	{"SDL_GetTextureBlendMode", (uintptr_t)&SDL_GetTextureBlendMode},
	{"SDL_GetPrefPath", (uintptr_t)&SDL_GetPrefPath},
	{"SDL_GetTextureColorMod", (uintptr_t)&SDL_GetTextureColorMod},
	{"SDL_GetTicks", (uintptr_t)&SDL_GetTicks},
	{"SDL_GetVersion", (uintptr_t)&SDL_GetVersion_fake},
	{"SDL_GL_BindTexture", (uintptr_t)&SDL_GL_BindTexture},
	{"SDL_GL_GetCurrentContext", (uintptr_t)&SDL_GL_GetCurrentContext},
	{"SDL_GL_MakeCurrent", (uintptr_t)&SDL_GL_MakeCurrent},
	{"SDL_GL_SetAttribute", (uintptr_t)&SDL_GL_SetAttribute},
	{"SDL_Init", (uintptr_t)&SDL_Init},
	{"SDL_InitSubSystem", (uintptr_t)&SDL_InitSubSystem},
	{"SDL_IntersectRect", (uintptr_t)&SDL_IntersectRect},
	{"SDL_LockMutex", (uintptr_t)&SDL_LockMutex},
	{"SDL_LockSurface", (uintptr_t)&SDL_LockSurface},
	{"SDL_Log", (uintptr_t)&ret0},
	{"SDL_LogError", (uintptr_t)&ret0},
	{"SDL_LogSetPriority", (uintptr_t)&ret0},
	{"SDL_MapRGB", (uintptr_t)&SDL_MapRGB},
	{"SDL_JoystickInstanceID", (uintptr_t)&SDL_JoystickInstanceID},
	{"SDL_GameControllerGetAxis", (uintptr_t)&SDL_GameControllerGetAxis},
	{"SDL_MinimizeWindow", (uintptr_t)&SDL_MinimizeWindow},
	{"SDL_PeepEvents", (uintptr_t)&SDL_PeepEvents},
	{"SDL_PumpEvents", (uintptr_t)&SDL_PumpEvents},
	{"SDL_PushEvent", (uintptr_t)&SDL_PushEvent},
	{"SDL_PollEvent", (uintptr_t)&SDL_PollEvent},
	{"SDL_QueryTexture", (uintptr_t)&SDL_QueryTexture},
	{"SDL_Quit", (uintptr_t)&SDL_Quit},
	{"SDL_RemoveTimer", (uintptr_t)&SDL_RemoveTimer},
	{"SDL_RenderClear", (uintptr_t)&SDL_RenderClear},
	{"SDL_RenderCopy", (uintptr_t)&SDL_RenderCopy},
	{"SDL_RenderFillRect", (uintptr_t)&SDL_RenderFillRect},
	{"SDL_RenderPresent", (uintptr_t)&SDL_RenderPresent},
	{"SDL_RWFromFile", (uintptr_t)&SDL_RWFromFile_hook},
	{"SDL_RWread", (uintptr_t)&SDL_RWread},
	{"SDL_RWwrite", (uintptr_t)&SDL_RWwrite},
	{"SDL_RWclose", (uintptr_t)&SDL_RWclose},
	{"SDL_RWsize", (uintptr_t)&SDL_RWsize},
	{"SDL_RWFromMem", (uintptr_t)&SDL_RWFromMem},
	{"SDL_SetColorKey", (uintptr_t)&SDL_SetColorKey},
	{"SDL_SetEventFilter", (uintptr_t)&SDL_SetEventFilter},
	{"SDL_SetHint", (uintptr_t)&SDL_SetHint},
	{"SDL_SetMainReady_REAL", (uintptr_t)&SDL_SetMainReady},
	{"SDL_SetRenderDrawBlendMode", (uintptr_t)&SDL_SetRenderDrawBlendMode},
	{"SDL_SetRenderDrawColor", (uintptr_t)&SDL_SetRenderDrawColor},
	{"SDL_SetRenderTarget", (uintptr_t)&SDL_SetRenderTarget},
	{"SDL_SetTextureBlendMode", (uintptr_t)&SDL_SetTextureBlendMode},
	{"SDL_SetTextureColorMod", (uintptr_t)&SDL_SetTextureColorMod},
	{"SDL_ShowCursor", (uintptr_t)&SDL_ShowCursor},
	{"SDL_ShowSimpleMessageBox", (uintptr_t)&SDL_ShowSimpleMessageBox},
	{"SDL_StartTextInput", (uintptr_t)&SDL_StartTextInput},
	{"SDL_StopTextInput", (uintptr_t)&SDL_StopTextInput},
	{"SDL_strdup", (uintptr_t)&SDL_strdup},
	{"SDL_UnlockMutex", (uintptr_t)&SDL_UnlockMutex},
	{"SDL_UnlockSurface", (uintptr_t)&SDL_UnlockSurface},
	{"SDL_UpperBlit", (uintptr_t)&SDL_UpperBlit},
	{"SDL_WaitThread", (uintptr_t)&SDL_WaitThread},
	{"SDL_GetKeyFromScancode", (uintptr_t)&SDL_GetKeyFromScancode},
	{"SDL_GetNumVideoDisplays", (uintptr_t)&SDL_GetNumVideoDisplays},
	{"SDL_GetDisplayBounds", (uintptr_t)&SDL_GetDisplayBounds},
	{"SDL_UnionRect", (uintptr_t)&SDL_UnionRect},
	{"SDL_GetKeyboardFocus", (uintptr_t)&SDL_GetKeyboardFocus},
	{"SDL_GetRelativeMouseMode", (uintptr_t)&SDL_GetRelativeMouseMode},
	{"SDL_NumJoysticks", (uintptr_t)&SDL_NumJoysticks},
	{"SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize},
	{"SDL_GameControllerOpen", (uintptr_t)&SDL_GameControllerOpen},
	{"SDL_GameControllerGetJoystick", (uintptr_t)&SDL_GameControllerGetJoystick},
	{"SDL_HapticOpenFromJoystick", (uintptr_t)&SDL_HapticOpenFromJoystick},
	{"SDL_GetPerformanceFrequency", (uintptr_t)&SDL_GetPerformanceFrequency},
	{"SDL_GetPerformanceCounter", (uintptr_t)&SDL_GetPerformanceCounter},
	{"SDL_GetMouseFocus", (uintptr_t)&SDL_GetMouseFocus},
	{"SDL_ShowMessageBox", (uintptr_t)&SDL_ShowMessageBox},
	{"SDL_RaiseWindow", (uintptr_t)&SDL_RaiseWindow},
	{"SDL_GL_GetAttribute", (uintptr_t)&SDL_GL_GetAttribute},
	{"SDL_GL_CreateContext", (uintptr_t)&SDL_GL_CreateContext},
	{"SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress_fake},
	{"SDL_GL_DeleteContext", (uintptr_t)&SDL_GL_DeleteContext},
	{"SDL_GetDesktopDisplayMode", (uintptr_t)&SDL_GetDesktopDisplayMode},
	{"SDL_SetWindowData", (uintptr_t)&SDL_SetWindowData},
	{"SDL_GetWindowFlags", (uintptr_t)&SDL_GetWindowFlags},
	{"SDL_GetWindowSize", (uintptr_t)&SDL_GetWindowSize},
	{"SDL_GetWindowDisplayIndex", (uintptr_t)&SDL_GetWindowDisplayIndex},
	{"SDL_SetWindowFullscreen", (uintptr_t)&SDL_SetWindowFullscreen},
	{"SDL_SetWindowSize", (uintptr_t)&SDL_SetWindowSize},
	{"SDL_SetWindowPosition", (uintptr_t)&SDL_SetWindowPosition},
	{"SDL_GL_GetCurrentWindow", (uintptr_t)&SDL_GL_GetCurrentWindow},
	{"SDL_GetWindowData", (uintptr_t)&SDL_GetWindowData},
	{"SDL_GetWindowTitle", (uintptr_t)&SDL_GetWindowTitle},
	{"SDL_ResetKeyboard", (uintptr_t)&SDL_ResetKeyboard},
	{"SDL_SetWindowTitle", (uintptr_t)&SDL_SetWindowTitle},
	{"SDL_GetWindowPosition", (uintptr_t)&SDL_GetWindowPosition},
	{"SDL_GL_SetSwapInterval", (uintptr_t)&ret0},
	{"SDL_IsGameController", (uintptr_t)&SDL_IsGameController},
	{"SDL_JoystickGetDeviceGUID", (uintptr_t)&SDL_JoystickGetDeviceGUID},
	{"SDL_GameControllerNameForIndex", (uintptr_t)&SDL_GameControllerNameForIndex},
	{"SDL_GetWindowFromID", (uintptr_t)&SDL_GetWindowFromID},
	{"SDL_GL_SwapWindow", (uintptr_t)&SDL_GL_SwapWindow},
	{"SDL_SetMainReady", (uintptr_t)&SDL_SetMainReady},
	{"SDL_NumAccelerometers", (uintptr_t)&ret0},
	{"SDL_AndroidGetJNIEnv", (uintptr_t)&Android_JNI_GetEnv},
	{"Android_JNI_GetEnv", (uintptr_t)&Android_JNI_GetEnv},
	{"SDL_RWFromConstMem", (uintptr_t)&SDL_RWFromConstMem},
	{"SDL_ConvertSurface", (uintptr_t)&SDL_ConvertSurface},
	{"SDL_SetError", (uintptr_t)&SDL_SetError},
	{"SDL_MapRGBA", (uintptr_t)&SDL_MapRGBA},
	{"SDL_EventState", (uintptr_t)&SDL_EventState},
	{"SDL_SetSurfaceBlendMode", (uintptr_t)&SDL_SetSurfaceBlendMode},
	{"SDL_UpperBlitScaled", (uintptr_t)&SDL_UpperBlitScaled},
	{"SDL_FreeRW", (uintptr_t)&SDL_FreeRW},
	{"SDL_GetKeyboardState", (uintptr_t)&SDL_GetKeyboardState},
	{"SDL_JoystickNumAxes", (uintptr_t)&ret4},
	{"SDL_JoystickUpdate", (uintptr_t)&SDL_JoystickUpdate},
	{"SDL_JoystickGetAxis", (uintptr_t)&SDL_JoystickGetAxis},
	{"SDL_JoystickGetButton", (uintptr_t)&SDL_JoystickGetButton},
	{"SDL_GetScancodeFromKey", (uintptr_t)&SDL_GetScancodeFromKey},
	{"SDL_GetKeyName", (uintptr_t)&SDL_GetKeyName},
	{"SDL_GetScancodeName", (uintptr_t)&SDL_GetScancodeName},
	{"SDL_JoystickGetHat", (uintptr_t)&SDL_JoystickGetHat},
	{"SDL_JoystickClose", (uintptr_t)&SDL_JoystickClose},
	{"SDL_JoystickOpen", (uintptr_t)&SDL_JoystickOpen},
	{"SDL_JoystickEventState", (uintptr_t)&SDL_JoystickEventState},
	{"SDL_LogSetAllPriority", (uintptr_t)&SDL_LogSetAllPriority},
	{"SDL_LogMessageV", (uintptr_t)&SDL_LogMessageV},
	{"SDL_RWtell", (uintptr_t)&SDL_RWtell},
	{"SDL_AndroidGetActivity", (uintptr_t)&ret0},
	{"SDL_free", (uintptr_t)&SDL_free},
	{"SDL_AtomicAdd", (uintptr_t)&SDL_AtomicAdd},
	{"SDL_AtomicSet", (uintptr_t)&SDL_AtomicSet},
	{"SDL_CreateSystemCursor", (uintptr_t)&SDL_CreateSystemCursor},
	{"SDL_OpenAudio", (uintptr_t)&SDL_OpenAudio},
	{"SDL_CloseAudio", (uintptr_t)&SDL_CloseAudio},
	{"SDL_PauseAudio", (uintptr_t)&SDL_PauseAudio},
	{"SDL_CreateCursor", (uintptr_t)&SDL_CreateCursor},
	{"SDL_SetCursor", (uintptr_t)&SDL_SetCursor},
	{"SDL_GameControllerClose", (uintptr_t)&SDL_GameControllerClose},
	{"SDL_FreeCursor", (uintptr_t)&SDL_FreeCursor},
	{"SDL_CreateColorCursor", (uintptr_t)&SDL_CreateColorCursor},
	{"IMG_Init", (uintptr_t)&IMG_Init},
	{"IMG_Quit", (uintptr_t)&IMG_Quit},
	{"Mix_PauseMusic", (uintptr_t)&Mix_PauseMusic},
	{"Mix_ResumeMusic", (uintptr_t)&Mix_ResumeMusic},
	{"Mix_VolumeMusic", (uintptr_t)&Mix_VolumeMusic},
	{"Mix_LoadMUS", (uintptr_t)&Mix_LoadMUS_hook},
	{"Mix_PlayMusic", (uintptr_t)&Mix_PlayMusic},
	{"Mix_FreeMusic", (uintptr_t)&ret0}, // FIXME
	{"Mix_RewindMusic", (uintptr_t)&Mix_RewindMusic},
	{"Mix_SetMusicPosition", (uintptr_t)&Mix_SetMusicPosition},
	{"Mix_CloseAudio", (uintptr_t)&Mix_CloseAudio},
	{"Mix_OpenAudio", (uintptr_t)&Mix_OpenAudio_hook},
	{"Mix_RegisterEffect", (uintptr_t)&Mix_RegisterEffect},
	{"Mix_Resume", (uintptr_t)&Mix_Resume},
	{"Mix_AllocateChannels", (uintptr_t)&Mix_AllocateChannels},
	{"Mix_ChannelFinished", (uintptr_t)&Mix_ChannelFinished},
	{"Mix_LoadWAV_RW", (uintptr_t)&Mix_LoadWAV_RW},
	{"Mix_FreeChunk", (uintptr_t)&Mix_FreeChunk},
	{"Mix_PausedMusic", (uintptr_t)&Mix_PausedMusic},
	{"Mix_Paused", (uintptr_t)&Mix_Paused},
	{"Mix_PlayingMusic", (uintptr_t)&Mix_PlayingMusic},
	{"Mix_Playing", (uintptr_t)&Mix_Playing},
	{"Mix_Volume", (uintptr_t)&Mix_Volume},
	{"Mix_SetDistance", (uintptr_t)&Mix_SetDistance},
	{"Mix_SetPanning", (uintptr_t)&Mix_SetPanning},
	{"Mix_QuerySpec", (uintptr_t)&Mix_QuerySpec},
	{"Mix_UnregisterEffect", (uintptr_t)&Mix_UnregisterEffect},
	{"Mix_HaltMusic", (uintptr_t)&Mix_HaltMusic},
	{"Mix_HaltChannel", (uintptr_t)&Mix_HaltChannel},
	{"Mix_LoadMUS_RW", (uintptr_t)&Mix_LoadMUS_RW},
	{"Mix_PlayChannelTimed", (uintptr_t)&Mix_PlayChannelTimed},
	{"Mix_Pause", (uintptr_t)&Mix_Pause},
	{"Mix_Init", (uintptr_t)&Mix_Init},
	/*{ "TTF_Quit", (uintptr_t)&TTF_Quit },
	{ "TTF_Init", (uintptr_t)&TTF_Init },
	{ "TTF_RenderText_Blended", (uintptr_t)&TTF_RenderText_Blended },
	{ "TTF_OpenFontRW", (uintptr_t)&TTF_OpenFontRW },
	{ "TTF_SetFontOutline", (uintptr_t)&TTF_SetFontOutline },
	{ "TTF_CloseFont", (uintptr_t)&TTF_CloseFont },
	{ "TTF_GlyphIsProvided", (uintptr_t)&TTF_GlyphIsProvided },*/
	{"IMG_Load", (uintptr_t)&IMG_Load_hook},
	{"IMG_LoadTexture", (uintptr_t)&IMG_LoadTexture_hook},
	{"IMG_Load_RW", (uintptr_t)&IMG_Load_RW},
	{"raise", (uintptr_t)&raise},
	{"posix_memalign", (uintptr_t)&posix_memalign},
	{"swprintf", (uintptr_t)&swprintf},
	{"wcscpy", (uintptr_t)&wcscpy},
	{"wcscat", (uintptr_t)&wcscat},
	{"wcstombs", (uintptr_t)&wcstombs},
	{"wcsstr", (uintptr_t)&wcsstr},
	{"compress", (uintptr_t)&compress},
	{"uncompress", (uintptr_t)&uncompress},
	{"atof", (uintptr_t)&atof},
	{"SDLNet_FreePacket", (uintptr_t)&SDLNet_FreePacket},
	{"SDLNet_Quit", (uintptr_t)&SDLNet_Quit},
	{"SDLNet_GetError", (uintptr_t)&SDLNet_GetError},
	{"SDLNet_Init", (uintptr_t)&SDLNet_Init},
	{"SDLNet_AllocPacket", (uintptr_t)&SDLNet_AllocPacket},
	{"SDLNet_UDP_Recv", (uintptr_t)&SDLNet_UDP_Recv},
	{"SDLNet_UDP_Send", (uintptr_t)&SDLNet_UDP_Send},
	{"SDLNet_GetLocalAddresses", (uintptr_t)&SDLNet_GetLocalAddresses},
	{"SDLNet_UDP_Close", (uintptr_t)&SDLNet_UDP_Close},
	{"SDLNet_ResolveHost", (uintptr_t)&SDLNet_ResolveHost},
	{"SDLNet_UDP_Open", (uintptr_t)&SDLNet_UDP_Open},
	{"remove", (uintptr_t)&remove},
	{"IMG_SavePNG", (uintptr_t)&IMG_SavePNG},
	{"SDL_DetachThread", (uintptr_t)&SDL_DetachThread},
	/*{ "TTF_SetFontHinting", (uintptr_t)&TTF_SetFontHinting },
	{ "TTF_FontHeight", (uintptr_t)&TTF_FontHeight },
	{ "TTF_FontAscent", (uintptr_t)&TTF_FontAscent },
	{ "TTF_FontDescent", (uintptr_t)&TTF_FontDescent },
	{ "TTF_SizeUTF8", (uintptr_t)&TTF_SizeUTF8 },
	{ "TTF_SizeText", (uintptr_t)&TTF_SizeText },
	{ "TTF_SetFontStyle", (uintptr_t)&TTF_SetFontStyle },
	{ "TTF_RenderUTF8_Blended", (uintptr_t)&TTF_RenderUTF8_Blended },*/
	{"SDL_strlen", (uintptr_t)&SDL_strlen},
	{"SDL_LogDebug", (uintptr_t)&ret0},
	{"SDL_HasEvents", (uintptr_t)&SDL_HasEvents},
	{"SDL_RWseek", (uintptr_t)&SDL_RWseek},
	{"SDL_JoystickNameForIndex", (uintptr_t)&SDL_JoystickNameForIndex},
	{"SDL_JoystickNumButtons", (uintptr_t)&SDL_JoystickNumButtons},
	{"SDL_JoystickGetGUID", (uintptr_t)&SDL_JoystickGetGUID},
	{"SDL_JoystickGetGUIDString", (uintptr_t)&SDL_JoystickGetGUIDString},
	{"SDL_JoystickNumHats", (uintptr_t)&SDL_JoystickNumHats},
	{"SDL_JoystickNumBalls", (uintptr_t)&SDL_JoystickNumBalls},
	{"SDL_JoystickName", (uintptr_t)&SDL_JoystickName_fake},
	{"SDL_GetNumRenderDrivers", (uintptr_t)&SDL_GetNumRenderDrivers},
	{"SDL_GetRenderDriverInfo", (uintptr_t)&SDL_GetRenderDriverInfo},
	{"SDL_GetNumVideoDrivers", (uintptr_t)&SDL_GetNumVideoDrivers},
	{"SDL_GetVideoDriver", (uintptr_t)&SDL_GetVideoDriver},
	{"SDL_GetBasePath", (uintptr_t)&SDL_GetBasePath_hook},
	{"SDL_RenderReadPixels", (uintptr_t)&SDL_RenderReadPixels},
	{"SDL_CreateRGBSurfaceFrom", (uintptr_t)&SDL_CreateRGBSurfaceFrom},
	{"SDL_SetWindowBordered", (uintptr_t)&SDL_SetWindowBordered},
	{"SDL_RestoreWindow", (uintptr_t)&SDL_RestoreWindow},
	{"SDL_sqrt", (uintptr_t)&SDL_sqrt},
	{"SDL_ThreadID", (uintptr_t)&SDL_ThreadID},
	{"__system_property_get", (uintptr_t)&ret0},
	{"strnlen", (uintptr_t)&strnlen},
	{"SDL_AddHintCallback", (uintptr_t)&SDL_AddHintCallback},
	{"SDL_AtomicAdd", (uintptr_t)&SDL_AtomicAdd},
	{"SDL_AtomicGet", (uintptr_t)&SDL_AtomicGet},
	{"SDL_AtomicGetPtr", (uintptr_t)&SDL_AtomicGetPtr},
	{"SDL_AtomicSet", (uintptr_t)&SDL_AtomicSet},
	{"SDL_AtomicSetPtr", (uintptr_t)&SDL_AtomicSetPtr},
	{"SDL_AudioStreamAvailable", (uintptr_t)&SDL_AudioStreamAvailable},
	{"SDL_AudioStreamClear", (uintptr_t)&SDL_AudioStreamClear},
	{"SDL_AudioStreamFlush", (uintptr_t)&SDL_AudioStreamFlush},
	{"SDL_AudioStreamGet", (uintptr_t)&SDL_AudioStreamGet},
	{"SDL_AudioStreamPut", (uintptr_t)&SDL_AudioStreamPut},
	{"SDL_CaptureMouse", (uintptr_t)&SDL_CaptureMouse},
	{"SDL_ClearQueuedAudio", (uintptr_t)&SDL_ClearQueuedAudio},
	{"SDL_ComposeCustomBlendMode", (uintptr_t)&SDL_ComposeCustomBlendMode},
	{"SDL_CreateRGBSurfaceWithFormat", (uintptr_t)&SDL_CreateRGBSurfaceWithFormat},
	{"SDL_CreateRGBSurfaceWithFormatFrom", (uintptr_t)&SDL_CreateRGBSurfaceWithFormatFrom},
	{"SDL_CreateThreadWithStackSize", (uintptr_t)&SDL_CreateThreadWithStackSize},
	{"SDL_DelHintCallback", (uintptr_t)&SDL_DelHintCallback},
	{"SDL_DequeueAudio", (uintptr_t)&SDL_DequeueAudio},
	{"SDL_DetachThread", (uintptr_t)&SDL_DetachThread},
	{"SDL_DuplicateSurface", (uintptr_t)&SDL_DuplicateSurface},
	{"SDL_FreeAudioStream", (uintptr_t)&SDL_FreeAudioStream},
	{"SDL_FreeSurface", (uintptr_t)&SDL_FreeSurface},
	{"SDL_FreeWAV", (uintptr_t)&SDL_FreeWAV},
	{"SDL_GL_GetCurrentContext", (uintptr_t)&SDL_GL_GetCurrentContext},
	{"SDL_GL_GetCurrentWindow", (uintptr_t)&SDL_GL_GetCurrentWindow},
	{"SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize},
	{"SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress},
	{"SDL_GL_GetSwapInterval", (uintptr_t)&SDL_GL_GetSwapInterval},
	{"SDL_GL_ResetAttributes", (uintptr_t)&SDL_GL_ResetAttributes},
	{"SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW},
	{"SDL_GameControllerFromInstanceID", (uintptr_t)&SDL_GameControllerFromInstanceID},
	{"SDL_GameControllerFromPlayerIndex", (uintptr_t)&SDL_GameControllerFromPlayerIndex},
	{"SDL_GameControllerGetPlayerIndex", (uintptr_t)&SDL_GameControllerGetPlayerIndex},
	{"SDL_GameControllerGetProduct", (uintptr_t)&SDL_GameControllerGetProduct},
	{"SDL_GameControllerGetProductVersion", (uintptr_t)&SDL_GameControllerGetProductVersion},
	{"SDL_GameControllerGetType", (uintptr_t)&SDL_GameControllerGetType},
	{"SDL_GameControllerGetVendor", (uintptr_t)&SDL_GameControllerGetVendor},
	{"SDL_GameControllerMappingForDeviceIndex", (uintptr_t)&SDL_GameControllerMappingForDeviceIndex},
	{"SDL_GameControllerMappingForIndex", (uintptr_t)&SDL_GameControllerMappingForIndex},
	{"SDL_GameControllerNumMappings", (uintptr_t)&SDL_GameControllerNumMappings},
	{"SDL_GameControllerRumble", (uintptr_t)&SDL_GameControllerRumble},
	{"SDL_GameControllerSetPlayerIndex", (uintptr_t)&SDL_GameControllerSetPlayerIndex},
	{"SDL_GameControllerTypeForIndex", (uintptr_t)&SDL_GameControllerTypeForIndex},
	{"SDL_GetAssertionHandler", (uintptr_t)&SDL_GetAssertionHandler},
	{"SDL_GetBasePath", (uintptr_t)&SDL_GetBasePath},
	{"SDL_GetDefaultAssertionHandler", (uintptr_t)&SDL_GetDefaultAssertionHandler},
	{"SDL_GetDefaultCursor", (uintptr_t)&SDL_GetDefaultCursor},
	{"SDL_GetDisplayDPI", (uintptr_t)&SDL_GetDisplayDPI},
	{"SDL_GetDisplayOrientation", (uintptr_t)&SDL_GetDisplayOrientation},
	{"SDL_GetDisplayUsableBounds", (uintptr_t)&SDL_GetDisplayUsableBounds},
	{"SDL_GetError", (uintptr_t)&SDL_GetError},
	{"SDL_GetEventFilter", (uintptr_t)&SDL_GetEventFilter},
	{"SDL_GetGlobalMouseState", (uintptr_t)&SDL_GetGlobalMouseState},
	{"SDL_GetGrabbedWindow", (uintptr_t)&SDL_GetGrabbedWindow},
	{"SDL_GetHintBoolean", (uintptr_t)&SDL_GetHintBoolean},
	{"SDL_GetMemoryFunctions", (uintptr_t)&SDL_GetMemoryFunctions},
	{"SDL_GetNumAllocations", (uintptr_t)&SDL_GetNumAllocations},
	{"SDL_GetQueuedAudioSize", (uintptr_t)&SDL_GetQueuedAudioSize},
	{"SDL_GetRendererOutputSize", (uintptr_t)&SDL_GetRendererOutputSize},
	{"SDL_GetSystemRAM", (uintptr_t)&SDL_GetSystemRAM},
	{"SDL_GetTextureScaleMode", (uintptr_t)&SDL_GetTextureScaleMode},
	{"SDL_AddHintCallback", (uintptr_t)&SDL_AddHintCallback},
	{"SDL_AtomicAdd", (uintptr_t)&SDL_AtomicAdd},
	{"SDL_AtomicGet", (uintptr_t)&SDL_AtomicGet},
	{"SDL_AtomicGetPtr", (uintptr_t)&SDL_AtomicGetPtr},
	{"SDL_AtomicSet", (uintptr_t)&SDL_AtomicSet},
	{"SDL_AtomicSetPtr", (uintptr_t)&SDL_AtomicSetPtr},
	{"SDL_AudioStreamAvailable", (uintptr_t)&SDL_AudioStreamAvailable},
	{"SDL_AudioStreamClear", (uintptr_t)&SDL_AudioStreamClear},
	{"SDL_AudioStreamFlush", (uintptr_t)&SDL_AudioStreamFlush},
	{"SDL_AudioStreamGet", (uintptr_t)&SDL_AudioStreamGet},
	{"SDL_AudioStreamPut", (uintptr_t)&SDL_AudioStreamPut},
	{"SDL_CaptureMouse", (uintptr_t)&SDL_CaptureMouse},
	{"SDL_ClearQueuedAudio", (uintptr_t)&SDL_ClearQueuedAudio},
	{"SDL_ComposeCustomBlendMode", (uintptr_t)&SDL_ComposeCustomBlendMode},
	{"SDL_CreateRGBSurfaceWithFormat", (uintptr_t)&SDL_CreateRGBSurfaceWithFormat},
	{"SDL_CreateRGBSurfaceWithFormatFrom", (uintptr_t)&SDL_CreateRGBSurfaceWithFormatFrom},
	{"SDL_CreateThreadWithStackSize", (uintptr_t)&SDL_CreateThreadWithStackSize},
	{"SDL_DelHintCallback", (uintptr_t)&SDL_DelHintCallback},
	{"SDL_DequeueAudio", (uintptr_t)&SDL_DequeueAudio},
	{"SDL_DetachThread", (uintptr_t)&SDL_DetachThread},
	{"SDL_DuplicateSurface", (uintptr_t)&SDL_DuplicateSurface},
	{"SDL_GL_GetCurrentContext", (uintptr_t)&SDL_GL_GetCurrentContext},
	{"SDL_GL_GetCurrentWindow", (uintptr_t)&SDL_GL_GetCurrentWindow},
	{"SDL_GL_GetDrawableSize", (uintptr_t)&SDL_GL_GetDrawableSize},
	{"SDL_GL_GetProcAddress", (uintptr_t)&SDL_GL_GetProcAddress},
	{"SDL_GL_GetSwapInterval", (uintptr_t)&SDL_GL_GetSwapInterval},
	{"SDL_GL_ResetAttributes", (uintptr_t)&SDL_GL_ResetAttributes},
	{"SDL_GameControllerAddMappingsFromRW", (uintptr_t)&SDL_GameControllerAddMappingsFromRW},
	{"SDL_GameControllerFromInstanceID", (uintptr_t)&SDL_GameControllerFromInstanceID},
	{"SDL_GameControllerFromPlayerIndex", (uintptr_t)&SDL_GameControllerFromPlayerIndex},
	{"SDL_GameControllerGetPlayerIndex", (uintptr_t)&SDL_GameControllerGetPlayerIndex},
	{"SDL_GameControllerGetProduct", (uintptr_t)&SDL_GameControllerGetProduct},
	{"SDL_GameControllerGetProductVersion", (uintptr_t)&SDL_GameControllerGetProductVersion},
	{"SDL_GameControllerGetType", (uintptr_t)&SDL_GameControllerGetType},
	{"SDL_GameControllerGetVendor", (uintptr_t)&SDL_GameControllerGetVendor},
	{"SDL_GameControllerMappingForDeviceIndex", (uintptr_t)&SDL_GameControllerMappingForDeviceIndex},
	{"SDL_GameControllerMappingForIndex", (uintptr_t)&SDL_GameControllerMappingForIndex},
	{"SDL_GameControllerNumMappings", (uintptr_t)&SDL_GameControllerNumMappings},
	{"SDL_GameControllerRumble", (uintptr_t)&SDL_GameControllerRumble},
	{"SDL_GameControllerSetPlayerIndex", (uintptr_t)&SDL_GameControllerSetPlayerIndex},
	{"SDL_GameControllerTypeForIndex", (uintptr_t)&SDL_GameControllerTypeForIndex},
	{"SDL_GetAssertionHandler", (uintptr_t)&SDL_GetAssertionHandler},
	{"SDL_GetBasePath", (uintptr_t)&SDL_GetBasePath},
	{"SDL_GetDefaultAssertionHandler", (uintptr_t)&SDL_GetDefaultAssertionHandler},
	{"SDL_GetDefaultCursor", (uintptr_t)&SDL_GetDefaultCursor},
	{"SDL_GetDisplayDPI", (uintptr_t)&SDL_GetDisplayDPI},
	{"SDL_GetDisplayOrientation", (uintptr_t)&SDL_GetDisplayOrientation},
	{"SDL_GetDisplayUsableBounds", (uintptr_t)&SDL_GetDisplayUsableBounds},
	{"SDL_GetError", (uintptr_t)&SDL_GetError},
	{"SDL_GetEventFilter", (uintptr_t)&SDL_GetEventFilter},
	{"SDL_GetGlobalMouseState", (uintptr_t)&SDL_GetGlobalMouseState},
	{"SDL_GetGrabbedWindow", (uintptr_t)&SDL_GetGrabbedWindow},
	{"SDL_GetHintBoolean", (uintptr_t)&SDL_GetHintBoolean},
	{"SDL_GetMemoryFunctions", (uintptr_t)&SDL_GetMemoryFunctions},
	{"SDL_GetNumAllocations", (uintptr_t)&SDL_GetNumAllocations},
	{"SDL_GetQueuedAudioSize", (uintptr_t)&SDL_GetQueuedAudioSize},
	{"SDL_GetRendererOutputSize", (uintptr_t)&SDL_GetRendererOutputSize},
	{"SDL_GetSystemRAM", (uintptr_t)&SDL_GetSystemRAM},
	{"SDL_GetTextureScaleMode", (uintptr_t)&SDL_GetTextureScaleMode},
	{"SDL_GetTouchDeviceType", (uintptr_t)&SDL_GetTouchDeviceType},
	{"SDL_GetWindowBordersSize", (uintptr_t)&SDL_GetWindowBordersSize},
	{"SDL_GetWindowOpacity", (uintptr_t)&SDL_GetWindowOpacity},
	{"SDL_GetYUVConversionMode", (uintptr_t)&SDL_GetYUVConversionMode},
	{"SDL_GetYUVConversionModeForResolution", (uintptr_t)&SDL_GetYUVConversionModeForResolution},
	{"SDL_HasARMSIMD", (uintptr_t)&SDL_HasARMSIMD},
	{"SDL_HasAVX", (uintptr_t)&SDL_HasAVX},
	{"SDL_HasAVX2", (uintptr_t)&SDL_HasAVX2},
	{"SDL_HasAVX512F", (uintptr_t)&SDL_HasAVX512F},
	{"SDL_HasColorKey", (uintptr_t)&SDL_HasColorKey},
	{"SDL_HasIntersection", (uintptr_t)&SDL_HasIntersection},
	{"SDL_HasMMX", (uintptr_t)&SDL_HasMMX},
	{"SDL_HasNEON", (uintptr_t)&SDL_HasNEON},
	{"SDL_IsTablet", (uintptr_t)&SDL_IsTablet},
	{"SDL_JoystickCurrentPowerLevel", (uintptr_t)&SDL_JoystickCurrentPowerLevel},
	{"SDL_JoystickFromInstanceID", (uintptr_t)&SDL_JoystickFromInstanceID},
	{"SDL_JoystickFromPlayerIndex", (uintptr_t)&SDL_JoystickFromPlayerIndex},
	{"SDL_JoystickGetAxisInitialState", (uintptr_t)&SDL_JoystickGetAxisInitialState},
	{"SDL_JoystickGetDeviceInstanceID", (uintptr_t)&SDL_JoystickGetDeviceInstanceID},
	{"SDL_JoystickGetDevicePlayerIndex", (uintptr_t)&SDL_JoystickGetDevicePlayerIndex},
	{"SDL_JoystickGetDeviceProduct", (uintptr_t)&SDL_JoystickGetDeviceProduct},
	{"SDL_JoystickGetDeviceProductVersion", (uintptr_t)&SDL_JoystickGetDeviceProductVersion},
	{"SDL_JoystickGetDeviceType", (uintptr_t)&SDL_JoystickGetDeviceType},
	{"SDL_JoystickGetDeviceVendor", (uintptr_t)&SDL_JoystickGetDeviceVendor},
	{"SDL_JoystickGetPlayerIndex", (uintptr_t)&SDL_JoystickGetPlayerIndex},
	{"SDL_JoystickGetProduct", (uintptr_t)&SDL_JoystickGetProduct},
	{"SDL_JoystickGetProductVersion", (uintptr_t)&SDL_JoystickGetProductVersion},
	{"SDL_JoystickGetType", (uintptr_t)&SDL_JoystickGetType},
	{"SDL_JoystickGetVendor", (uintptr_t)&SDL_JoystickGetVendor},
	{"SDL_JoystickRumble", (uintptr_t)&SDL_JoystickRumble},
	{"SDL_JoystickSetPlayerIndex", (uintptr_t)&SDL_JoystickSetPlayerIndex},
	{"SDL_LoadFile", (uintptr_t)&SDL_LoadFile},
	{"SDL_LoadFile_RW", (uintptr_t)&SDL_LoadFile_RW},
	{"SDL_LockJoysticks", (uintptr_t)&SDL_LockJoysticks},
	{"SDL_LockTextureToSurface", (uintptr_t)&SDL_LockTextureToSurface},
	{"SDL_MemoryBarrierAcquireFunction", (uintptr_t)&SDL_MemoryBarrierAcquireFunction},
	{"SDL_MemoryBarrierReleaseFunction", (uintptr_t)&SDL_MemoryBarrierReleaseFunction},
	{"SDL_Metal_CreateView", (uintptr_t)&SDL_Metal_CreateView},
	{"SDL_Metal_DestroyView", (uintptr_t)&SDL_Metal_DestroyView},
	{"SDL_NewAudioStream", (uintptr_t)&SDL_NewAudioStream},
	{"SDL_NumSensors", (uintptr_t)&SDL_NumSensors},
	{"SDL_OnApplicationDidBecomeActive", (uintptr_t)&SDL_OnApplicationDidBecomeActive},
	{"SDL_OnApplicationDidEnterBackground", (uintptr_t)&SDL_OnApplicationDidEnterBackground},
	{"SDL_OnApplicationDidReceiveMemoryWarning", (uintptr_t)&SDL_OnApplicationDidReceiveMemoryWarning},
	{"SDL_OnApplicationWillEnterForeground", (uintptr_t)&SDL_OnApplicationWillEnterForeground},
	{"SDL_OnApplicationWillResignActive", (uintptr_t)&SDL_OnApplicationWillResignActive},
	{"SDL_OnApplicationWillTerminate", (uintptr_t)&SDL_OnApplicationWillTerminate},
	{"SDL_QueueAudio", (uintptr_t)&SDL_QueueAudio},
	{"SDL_Quit", (uintptr_t)&SDL_Quit},
	{"SDL_QuitSubSystem", (uintptr_t)&SDL_QuitSubSystem},
	{"SDL_RWclose", (uintptr_t)&SDL_RWclose},
	{"SDL_RWread", (uintptr_t)&SDL_RWread},
	{"SDL_RWseek", (uintptr_t)&SDL_RWseek},
	{"SDL_RWsize", (uintptr_t)&SDL_RWsize},
	{"SDL_RWtell", (uintptr_t)&SDL_RWtell},
	{"SDL_RWwrite", (uintptr_t)&SDL_RWwrite},
	{"SDL_RenderCopyExF", (uintptr_t)&SDL_RenderCopyExF},
	{"SDL_RenderCopyF", (uintptr_t)&SDL_RenderCopyF},
	{"SDL_RenderDrawLineF", (uintptr_t)&SDL_RenderDrawLineF},
	{"SDL_RenderDrawLinesF", (uintptr_t)&SDL_RenderDrawLinesF},
	{"SDL_RenderDrawPointF", (uintptr_t)&SDL_RenderDrawPointF},
	{"SDL_RenderDrawPointsF", (uintptr_t)&SDL_RenderDrawPointsF},
	{"SDL_RenderDrawRectF", (uintptr_t)&SDL_RenderDrawRectF},
	{"SDL_RenderDrawRectsF", (uintptr_t)&SDL_RenderDrawRectsF},
	{"SDL_RenderFillRectF", (uintptr_t)&SDL_RenderFillRectF},
	{"SDL_RenderFillRectsF", (uintptr_t)&SDL_RenderFillRectsF},
	{"SDL_RenderFlush", (uintptr_t)&SDL_RenderFlush},
	{"SDL_RenderGetClipRect", (uintptr_t)&SDL_RenderGetClipRect},
	{"SDL_RenderGetIntegerScale", (uintptr_t)&SDL_RenderGetIntegerScale},
	{"SDL_RenderGetMetalCommandEncoder", (uintptr_t)&SDL_RenderGetMetalCommandEncoder},
	{"SDL_RenderGetMetalLayer", (uintptr_t)&SDL_RenderGetMetalLayer},
	{"SDL_RenderGetScale", (uintptr_t)&SDL_RenderGetScale},
	{"SDL_RenderIsClipEnabled", (uintptr_t)&SDL_RenderIsClipEnabled},
	{"SDL_RenderReadPixels", (uintptr_t)&SDL_RenderReadPixels},
	{"SDL_RenderSetClipRect", (uintptr_t)&SDL_RenderSetClipRect},
	{"SDL_RenderSetIntegerScale", (uintptr_t)&SDL_RenderSetIntegerScale},
	{"SDL_ReportAssertion", (uintptr_t)&SDL_ReportAssertion},
	{"SDL_SIMDAlloc", (uintptr_t)&SDL_SIMDAlloc},
	{"SDL_SIMDFree", (uintptr_t)&SDL_SIMDFree},
	{"SDL_SIMDGetAlignment", (uintptr_t)&SDL_SIMDGetAlignment},
	{"SDL_SensorClose", (uintptr_t)&SDL_SensorClose},
	{"SDL_SensorFromInstanceID", (uintptr_t)&SDL_SensorFromInstanceID},
	{"SDL_SensorGetData", (uintptr_t)&SDL_SensorGetData},
	{"SDL_SensorGetDeviceInstanceID", (uintptr_t)&SDL_SensorGetDeviceInstanceID},
	{"SDL_SensorGetDeviceName", (uintptr_t)&SDL_SensorGetDeviceName},
	{"SDL_SensorGetDeviceNonPortableType", (uintptr_t)&SDL_SensorGetDeviceNonPortableType},
	{"SDL_SensorGetDeviceType", (uintptr_t)&SDL_SensorGetDeviceType},
	{"SDL_SensorGetInstanceID", (uintptr_t)&SDL_SensorGetInstanceID},
	{"SDL_SensorGetName", (uintptr_t)&SDL_SensorGetName},
	{"SDL_SensorGetNonPortableType", (uintptr_t)&SDL_SensorGetNonPortableType},
	{"SDL_SensorGetType", (uintptr_t)&SDL_SensorGetType},
	{"SDL_SensorOpen", (uintptr_t)&SDL_SensorOpen},
	{"SDL_SensorUpdate", (uintptr_t)&SDL_SensorUpdate},
	{"SDL_SetMainReady", (uintptr_t)&SDL_SetMainReady},
	{"SDL_SetMemoryFunctions", (uintptr_t)&SDL_SetMemoryFunctions},
	{"SDL_SetTextureScaleMode", (uintptr_t)&SDL_SetTextureScaleMode},
	{"SDL_SetWindowHitTest", (uintptr_t)&SDL_SetWindowHitTest},
	{"SDL_SetWindowInputFocus", (uintptr_t)&SDL_SetWindowInputFocus},
	{"SDL_SetWindowModalFor", (uintptr_t)&SDL_SetWindowModalFor},
	{"SDL_SetWindowOpacity", (uintptr_t)&SDL_SetWindowOpacity},
	{"SDL_SetWindowResizable", (uintptr_t)&SDL_SetWindowResizable},
	{"SDL_SetYUVConversionMode", (uintptr_t)&SDL_SetYUVConversionMode},
	{"SDL_TLSCreate", (uintptr_t)&SDL_TLSCreate},
	{"SDL_TLSGet", (uintptr_t)&SDL_TLSGet},
	{"SDL_TLSSet", (uintptr_t)&SDL_TLSSet},
	{"SDL_UnlockJoysticks", (uintptr_t)&SDL_UnlockJoysticks},
	{"SDL_UpdateYUVTexture", (uintptr_t)&SDL_UpdateYUVTexture},
	{"SDL_WarpMouseGlobal", (uintptr_t)&SDL_WarpMouseGlobal},
	{"SDL_acos", (uintptr_t)&SDL_acos},
	{"SDL_acosf", (uintptr_t)&SDL_acosf},
	{"SDL_asin", (uintptr_t)&SDL_asin},
	{"SDL_asinf", (uintptr_t)&SDL_asinf},
	{"SDL_atan", (uintptr_t)&SDL_atan},
	{"SDL_atan2", (uintptr_t)&SDL_atan2},
	{"SDL_atan2f", (uintptr_t)&SDL_atan2f},
	{"SDL_atanf", (uintptr_t)&SDL_atanf},
	{"SDL_ceilf", (uintptr_t)&SDL_ceilf},
	{"SDL_copysign", (uintptr_t)&SDL_copysign},
	{"SDL_copysignf", (uintptr_t)&SDL_copysignf},
	{"SDL_cos", (uintptr_t)&SDL_cos},
	{"SDL_exp", (uintptr_t)&SDL_exp},
	{"SDL_expf", (uintptr_t)&SDL_expf},
	{"SDL_fabs", (uintptr_t)&SDL_fabs},
	{"SDL_fabsf", (uintptr_t)&SDL_fabsf},
	{"SDL_floor", (uintptr_t)&SDL_floor},
	{"SDL_floorf", (uintptr_t)&SDL_floorf},
	{"SDL_fmod", (uintptr_t)&SDL_fmod},
	{"SDL_fmodf", (uintptr_t)&SDL_fmodf},
	{"SDL_islower", (uintptr_t)&SDL_islower},
	{"SDL_isupper", (uintptr_t)&SDL_isupper},
	{"SDL_log", (uintptr_t)&SDL_log},
	{"SDL_log10", (uintptr_t)&SDL_log10},
	{"SDL_log10f", (uintptr_t)&SDL_log10f},
	{"SDL_logf", (uintptr_t)&SDL_logf},
	{"SDL_ltoa", (uintptr_t)&SDL_ltoa},
	{"SDL_pow", (uintptr_t)&SDL_pow},
	{"SDL_powf", (uintptr_t)&SDL_powf},
	{"SDL_scalbn", (uintptr_t)&SDL_scalbn},
	{"SDL_scalbnf", (uintptr_t)&SDL_scalbnf},
	{"SDL_sin", (uintptr_t)&SDL_sin},
	{"SDL_sqrt", (uintptr_t)&SDL_sqrt},
	{"SDL_sqrtf", (uintptr_t)&SDL_sqrtf},
	{"SDL_strtokr", (uintptr_t)&SDL_strtokr},
	{"SDL_tan", (uintptr_t)&SDL_tan},
	{"SDL_tanf", (uintptr_t)&SDL_tanf},
	{"SDL_utf8strlen", (uintptr_t)&SDL_utf8strlen},
	{"SDL_vsnprintf", (uintptr_t)&SDL_vsnprintf},
	{"SDL_vsscanf", (uintptr_t)&SDL_vsscanf},
	{"SDL_wcscmp", (uintptr_t)&SDL_wcscmp},
	{"SDL_wcsdup", (uintptr_t)&SDL_wcsdup},
	{"SDL_wcsncmp", (uintptr_t)&SDL_wcsncmp},
	{"SDL_wcsstr", (uintptr_t)&SDL_wcsstr},
	{"SDL_wcsstr", (uintptr_t)&SDL_wcsstr},

	{"SDL_AddHintCallback_REAL", (uintptr_t)&SDL_AddHintCallback},
	{"SDL_AtomicAdd_REAL", (uintptr_t)&SDL_AtomicAdd},
	{"SDL_AtomicGet_REAL", (uintptr_t)&SDL_AtomicGet},
	{"SDL_AtomicGetPtr_REAL", (uintptr_t)&SDL_AtomicGetPtr},
	{"SDL_AtomicSet_REAL", (uintptr_t)&SDL_AtomicSet},
	{"SDL_AtomicSetPtr_REAL", (uintptr_t)&SDL_AtomicSetPtr},
	{"SDL_AudioStreamAvailable_REAL", (uintptr_t)&SDL_AudioStreamAvailable},
	{"SDL_AudioStreamClear_REAL", (uintptr_t)&SDL_AudioStreamClear},
	{"SDL_AudioStreamFlush_REAL", (uintptr_t)&SDL_AudioStreamFlush},
	{"SDL_AudioStreamGet_REAL", (uintptr_t)&SDL_AudioStreamGet},
	{"SDL_AudioStreamPut_REAL", (uintptr_t)&SDL_AudioStreamPut},
	{"SDL_CaptureMouse_REAL", (uintptr_t)&SDL_CaptureMouse},
	{"SDL_ClearQueuedAudio_REAL", (uintptr_t)&SDL_ClearQueuedAudio},
	{"SDL_ComposeCustomBlendMode_REAL", (uintptr_t)&SDL_ComposeCustomBlendMode},
	{"SDL_CreateRGBSurfaceWithFormat_REAL", (uintptr_t)&SDL_CreateRGBSurfaceWithFormat},
	{"SDL_CreateRGBSurfaceWithFormatFrom_REAL", (uintptr_t)&SDL_CreateRGBSurfaceWithFormatFrom},
	{"SDL_CreateThreadWithStackSize_REAL", (uintptr_t)&SDL_CreateThreadWithStackSize},
	{"SDL_DelHintCallback_REAL", (uintptr_t)&SDL_DelHintCallback},
	{"SDL_DequeueAudio_REAL", (uintptr_t)&SDL_DequeueAudio},
	{"SDL_DetachThread_REAL", (uintptr_t)&SDL_DetachThread},
	{"SDL_DuplicateSurface_REAL", (uintptr_t)&SDL_DuplicateSurface},
	{"SDL_FreeAudioStream_REAL", (uintptr_t)&SDL_FreeAudioStream},
	{"SDL_FreeSurface_REAL", (uintptr_t)&SDL_FreeSurface},
	{"SDL_FreeWAV_REAL", (uintptr_t)&SDL_FreeWAV},
	{"SDL_GL_GetCurrentContext_REAL", (uintptr_t)&SDL_GL_GetCurrentContext},
	{"SDL_GL_GetCurrentWindow_REAL", (uintptr_t)&SDL_GL_GetCurrentWindow},
	{"SDL_GL_GetDrawableSize_REAL", (uintptr_t)&SDL_GL_GetDrawableSize},
	{"SDL_GL_GetProcAddress_REAL", (uintptr_t)&SDL_GL_GetProcAddress},
	{"SDL_GL_GetSwapInterval_REAL", (uintptr_t)&SDL_GL_GetSwapInterval},
	{"SDL_GL_ResetAttributes_REAL", (uintptr_t)&SDL_GL_ResetAttributes},
	{"SDL_GameControllerAddMappingsFromRW_REAL", (uintptr_t)&SDL_GameControllerAddMappingsFromRW},
	{"SDL_GameControllerFromInstanceID_REAL", (uintptr_t)&SDL_GameControllerFromInstanceID},
	{"SDL_GameControllerFromPlayerIndex_REAL", (uintptr_t)&SDL_GameControllerFromPlayerIndex},
	{"SDL_GameControllerGetPlayerIndex_REAL", (uintptr_t)&SDL_GameControllerGetPlayerIndex},
	{"SDL_GameControllerGetProduct_REAL", (uintptr_t)&SDL_GameControllerGetProduct},
	{"SDL_GameControllerGetProductVersion_REAL", (uintptr_t)&SDL_GameControllerGetProductVersion},
	{"SDL_GameControllerGetType_REAL", (uintptr_t)&SDL_GameControllerGetType},
	{"SDL_GameControllerGetVendor_REAL", (uintptr_t)&SDL_GameControllerGetVendor},
	{"SDL_GameControllerMappingForDeviceIndex_REAL", (uintptr_t)&SDL_GameControllerMappingForDeviceIndex},
	{"SDL_GameControllerMappingForIndex_REAL", (uintptr_t)&SDL_GameControllerMappingForIndex},
	{"SDL_GameControllerNumMappings_REAL", (uintptr_t)&SDL_GameControllerNumMappings},
	{"SDL_GameControllerRumble_REAL", (uintptr_t)&SDL_GameControllerRumble},
	{"SDL_GameControllerSetPlayerIndex_REAL", (uintptr_t)&SDL_GameControllerSetPlayerIndex},
	{"SDL_GameControllerTypeForIndex_REAL", (uintptr_t)&SDL_GameControllerTypeForIndex},
	{"SDL_GetAssertionHandler_REAL", (uintptr_t)&SDL_GetAssertionHandler},
	{"SDL_GetBasePath_REAL", (uintptr_t)&SDL_GetBasePath},
	{"SDL_GetDefaultAssertionHandler_REAL", (uintptr_t)&SDL_GetDefaultAssertionHandler},
	{"SDL_GetDefaultCursor_REAL", (uintptr_t)&SDL_GetDefaultCursor},
	{"SDL_GetDisplayDPI_REAL", (uintptr_t)&SDL_GetDisplayDPI},
	{"SDL_GetDisplayOrientation_REAL", (uintptr_t)&SDL_GetDisplayOrientation},
	{"SDL_GetDisplayUsableBounds_REAL", (uintptr_t)&SDL_GetDisplayUsableBounds},
	{"SDL_GetError_REAL", (uintptr_t)&SDL_GetError},
	{"SDL_GetEventFilter_REAL", (uintptr_t)&SDL_GetEventFilter},
	{"SDL_GetGlobalMouseState_REAL", (uintptr_t)&SDL_GetGlobalMouseState},
	{"SDL_GetGrabbedWindow_REAL", (uintptr_t)&SDL_GetGrabbedWindow},
	{"SDL_GetHintBoolean_REAL", (uintptr_t)&SDL_GetHintBoolean},
	{"SDL_GetMemoryFunctions_REAL", (uintptr_t)&SDL_GetMemoryFunctions},
	{"SDL_GetNumAllocations_REAL", (uintptr_t)&SDL_GetNumAllocations},
	{"SDL_GetQueuedAudioSize_REAL", (uintptr_t)&SDL_GetQueuedAudioSize},
	{"SDL_GetRendererOutputSize_REAL", (uintptr_t)&SDL_GetRendererOutputSize},
	{"SDL_GetSystemRAM_REAL", (uintptr_t)&SDL_GetSystemRAM},
	{"SDL_GetTextureScaleMode_REAL", (uintptr_t)&SDL_GetTextureScaleMode},
	{"SDL_AddHintCallback_REAL", (uintptr_t)&SDL_AddHintCallback},
	{"SDL_AtomicAdd_REAL", (uintptr_t)&SDL_AtomicAdd},
	{"SDL_AtomicGet_REAL", (uintptr_t)&SDL_AtomicGet},
	{"SDL_AtomicGetPtr_REAL", (uintptr_t)&SDL_AtomicGetPtr},
	{"SDL_AtomicSet_REAL", (uintptr_t)&SDL_AtomicSet},
	{"SDL_AtomicSetPtr_REAL", (uintptr_t)&SDL_AtomicSetPtr},
	{"SDL_AudioStreamAvailable_REAL", (uintptr_t)&SDL_AudioStreamAvailable},
	{"SDL_AudioStreamClear_REAL", (uintptr_t)&SDL_AudioStreamClear},
	{"SDL_AudioStreamFlush_REAL", (uintptr_t)&SDL_AudioStreamFlush},
	{"SDL_AudioStreamGet_REAL", (uintptr_t)&SDL_AudioStreamGet},
	{"SDL_AudioStreamPut_REAL", (uintptr_t)&SDL_AudioStreamPut},
	{"SDL_CaptureMouse_REAL", (uintptr_t)&SDL_CaptureMouse},
	{"SDL_ClearQueuedAudio_REAL", (uintptr_t)&SDL_ClearQueuedAudio},
	{"SDL_ComposeCustomBlendMode_REAL", (uintptr_t)&SDL_ComposeCustomBlendMode},
	{"SDL_CreateRGBSurfaceWithFormat_REAL", (uintptr_t)&SDL_CreateRGBSurfaceWithFormat},
	{"SDL_CreateRGBSurfaceWithFormatFrom_REAL", (uintptr_t)&SDL_CreateRGBSurfaceWithFormatFrom},
	{"SDL_CreateThreadWithStackSize_REAL", (uintptr_t)&SDL_CreateThreadWithStackSize},
	{"SDL_DelHintCallback_REAL", (uintptr_t)&SDL_DelHintCallback},
	{"SDL_DequeueAudio_REAL", (uintptr_t)&SDL_DequeueAudio},
	{"SDL_DetachThread_REAL", (uintptr_t)&SDL_DetachThread},
	{"SDL_DuplicateSurface_REAL", (uintptr_t)&SDL_DuplicateSurface},
	{"SDL_FreeAudioStream_REAL", (uintptr_t)&SDL_FreeAudioStream},
	{"SDL_FreeSurface_REAL", (uintptr_t)&SDL_FreeSurface},
	{"SDL_FreeWAV_REAL", (uintptr_t)&SDL_FreeWAV},
	{"SDL_GL_GetCurrentContext_REAL", (uintptr_t)&SDL_GL_GetCurrentContext},
	{"SDL_GL_GetCurrentWindow_REAL", (uintptr_t)&SDL_GL_GetCurrentWindow},
	{"SDL_GL_GetDrawableSize_REAL", (uintptr_t)&SDL_GL_GetDrawableSize},
	{"SDL_GL_GetProcAddress_REAL", (uintptr_t)&SDL_GL_GetProcAddress},
	{"SDL_GL_GetSwapInterval_REAL", (uintptr_t)&SDL_GL_GetSwapInterval},
	{"SDL_GL_ResetAttributes_REAL", (uintptr_t)&SDL_GL_ResetAttributes},
	{"SDL_GameControllerAddMappingsFromRW_REAL", (uintptr_t)&SDL_GameControllerAddMappingsFromRW},
	{"SDL_GameControllerFromInstanceID_REAL", (uintptr_t)&SDL_GameControllerFromInstanceID},
	{"SDL_GameControllerFromPlayerIndex_REAL", (uintptr_t)&SDL_GameControllerFromPlayerIndex},
	{"SDL_GameControllerGetPlayerIndex_REAL", (uintptr_t)&SDL_GameControllerGetPlayerIndex},
	{"SDL_GameControllerGetProduct_REAL", (uintptr_t)&SDL_GameControllerGetProduct},
	{"SDL_GameControllerGetProductVersion_REAL", (uintptr_t)&SDL_GameControllerGetProductVersion},
	{"SDL_GameControllerGetType_REAL", (uintptr_t)&SDL_GameControllerGetType},
	{"SDL_GameControllerGetVendor_REAL", (uintptr_t)&SDL_GameControllerGetVendor},
	{"SDL_GameControllerMappingForDeviceIndex_REAL", (uintptr_t)&SDL_GameControllerMappingForDeviceIndex},
	{"SDL_GameControllerMappingForIndex_REAL", (uintptr_t)&SDL_GameControllerMappingForIndex},
	{"SDL_GameControllerNumMappings_REAL", (uintptr_t)&SDL_GameControllerNumMappings},
	{"SDL_GameControllerRumble_REAL", (uintptr_t)&SDL_GameControllerRumble},
	{"SDL_GameControllerSetPlayerIndex_REAL", (uintptr_t)&SDL_GameControllerSetPlayerIndex},
	{"SDL_GameControllerTypeForIndex_REAL", (uintptr_t)&SDL_GameControllerTypeForIndex},
	{"SDL_GetAssertionHandler_REAL", (uintptr_t)&SDL_GetAssertionHandler},
	{"SDL_GetBasePath_REAL", (uintptr_t)&SDL_GetBasePath},
	{"SDL_GetDefaultAssertionHandler_REAL", (uintptr_t)&SDL_GetDefaultAssertionHandler},
	{"SDL_GetDefaultCursor_REAL", (uintptr_t)&SDL_GetDefaultCursor},
	{"SDL_GetDisplayDPI_REAL", (uintptr_t)&SDL_GetDisplayDPI},
	{"SDL_GetDisplayOrientation_REAL", (uintptr_t)&SDL_GetDisplayOrientation},
	{"SDL_GetDisplayUsableBounds_REAL", (uintptr_t)&SDL_GetDisplayUsableBounds},
	{"SDL_GetError_REAL", (uintptr_t)&SDL_GetError},
	{"SDL_GetEventFilter_REAL", (uintptr_t)&SDL_GetEventFilter},
	{"SDL_GetGlobalMouseState_REAL", (uintptr_t)&SDL_GetGlobalMouseState},
	{"SDL_GetGrabbedWindow_REAL", (uintptr_t)&SDL_GetGrabbedWindow},
	{"SDL_GetHintBoolean_REAL", (uintptr_t)&SDL_GetHintBoolean},
	{"SDL_GetMemoryFunctions_REAL", (uintptr_t)&SDL_GetMemoryFunctions},
	{"SDL_GetNumAllocations_REAL", (uintptr_t)&SDL_GetNumAllocations},
	{"SDL_GetQueuedAudioSize_REAL", (uintptr_t)&SDL_GetQueuedAudioSize},
	{"SDL_GetRendererOutputSize_REAL", (uintptr_t)&SDL_GetRendererOutputSize},
	{"SDL_GetSystemRAM_REAL", (uintptr_t)&SDL_GetSystemRAM},
	{"SDL_GetTextureScaleMode_REAL", (uintptr_t)&SDL_GetTextureScaleMode},
	{"SDL_GetTouchDeviceType_REAL", (uintptr_t)&SDL_GetTouchDeviceType},
	{"SDL_GetWindowBordersSize_REAL", (uintptr_t)&SDL_GetWindowBordersSize},
	{"SDL_GetWindowOpacity_REAL", (uintptr_t)&SDL_GetWindowOpacity},
	{"SDL_GetYUVConversionMode_REAL", (uintptr_t)&SDL_GetYUVConversionMode},
	{"SDL_GetYUVConversionModeForResolution_REAL", (uintptr_t)&SDL_GetYUVConversionModeForResolution},
	{"SDL_HasARMSIMD_REAL", (uintptr_t)&SDL_HasARMSIMD},
	{"SDL_HasAVX_REAL", (uintptr_t)&SDL_HasAVX},
	{"SDL_HasAVX2_REAL", (uintptr_t)&SDL_HasAVX2},
	{"SDL_HasAVX512F_REAL", (uintptr_t)&SDL_HasAVX512F},
	{"SDL_HasColorKey_REAL", (uintptr_t)&SDL_HasColorKey},
	{"SDL_HasIntersection_REAL", (uintptr_t)&SDL_HasIntersection},
	{"SDL_HasMMX_REAL", (uintptr_t)&SDL_HasMMX},
	{"SDL_HasNEON_REAL", (uintptr_t)&SDL_HasNEON},
	{"SDL_IsTablet_REAL", (uintptr_t)&SDL_IsTablet},
	{"SDL_JoystickCurrentPowerLevel_REAL", (uintptr_t)&SDL_JoystickCurrentPowerLevel},
	{"SDL_JoystickFromInstanceID_REAL", (uintptr_t)&SDL_JoystickFromInstanceID},
	{"SDL_JoystickFromPlayerIndex_REAL", (uintptr_t)&SDL_JoystickFromPlayerIndex},
	{"SDL_JoystickGetAxisInitialState_REAL", (uintptr_t)&SDL_JoystickGetAxisInitialState},
	{"SDL_JoystickGetDeviceInstanceID_REAL", (uintptr_t)&SDL_JoystickGetDeviceInstanceID},
	{"SDL_JoystickGetDevicePlayerIndex_REAL", (uintptr_t)&SDL_JoystickGetDevicePlayerIndex},
	{"SDL_JoystickGetDeviceProduct_REAL", (uintptr_t)&SDL_JoystickGetDeviceProduct},
	{"SDL_JoystickGetDeviceProductVersion_REAL", (uintptr_t)&SDL_JoystickGetDeviceProductVersion},
	{"SDL_JoystickGetDeviceType_REAL", (uintptr_t)&SDL_JoystickGetDeviceType},
	{"SDL_JoystickGetDeviceVendor_REAL", (uintptr_t)&SDL_JoystickGetDeviceVendor},
	{"SDL_JoystickGetPlayerIndex_REAL", (uintptr_t)&SDL_JoystickGetPlayerIndex},
	{"SDL_JoystickGetProduct_REAL", (uintptr_t)&SDL_JoystickGetProduct},
	{"SDL_JoystickGetProductVersion_REAL", (uintptr_t)&SDL_JoystickGetProductVersion},
	{"SDL_JoystickGetType_REAL", (uintptr_t)&SDL_JoystickGetType},
	{"SDL_JoystickGetVendor_REAL", (uintptr_t)&SDL_JoystickGetVendor},
	{"SDL_JoystickRumble_REAL", (uintptr_t)&SDL_JoystickRumble},
	{"SDL_JoystickSetPlayerIndex_REAL", (uintptr_t)&SDL_JoystickSetPlayerIndex},
	{"SDL_LoadFile_REAL", (uintptr_t)&SDL_LoadFile},
	{"SDL_LoadFile_RW_REAL", (uintptr_t)&SDL_LoadFile_RW},
	{"SDL_LockJoysticks_REAL", (uintptr_t)&SDL_LockJoysticks},
	{"SDL_LockTextureToSurface_REAL", (uintptr_t)&SDL_LockTextureToSurface},
	{"SDL_MemoryBarrierAcquireFunction_REAL", (uintptr_t)&SDL_MemoryBarrierAcquireFunction},
	{"SDL_MemoryBarrierReleaseFunction_REAL", (uintptr_t)&SDL_MemoryBarrierReleaseFunction},
	{"SDL_Metal_CreateView_REAL", (uintptr_t)&SDL_Metal_CreateView},
	{"SDL_Metal_DestroyView_REAL", (uintptr_t)&SDL_Metal_DestroyView},
	{"SDL_NewAudioStream_REAL", (uintptr_t)&SDL_NewAudioStream},
	{"SDL_NumSensors_REAL", (uintptr_t)&SDL_NumSensors},
	{"SDL_OnApplicationDidBecomeActive_REAL", (uintptr_t)&SDL_OnApplicationDidBecomeActive},
	{"SDL_OnApplicationDidEnterBackground_REAL", (uintptr_t)&SDL_OnApplicationDidEnterBackground},
	{"SDL_OnApplicationDidReceiveMemoryWarning_REAL", (uintptr_t)&SDL_OnApplicationDidReceiveMemoryWarning},
	{"SDL_OnApplicationWillEnterForeground_REAL", (uintptr_t)&SDL_OnApplicationWillEnterForeground},
	{"SDL_OnApplicationWillResignActive_REAL", (uintptr_t)&SDL_OnApplicationWillResignActive},
	{"SDL_OnApplicationWillTerminate_REAL", (uintptr_t)&SDL_OnApplicationWillTerminate},
	{"SDL_QueueAudio_REAL", (uintptr_t)&SDL_QueueAudio},
	{"SDL_Quit_REAL", (uintptr_t)&SDL_Quit},
	{"SDL_QuitSubSystem_REAL", (uintptr_t)&SDL_QuitSubSystem},
	{"SDL_RWclose_REAL", (uintptr_t)&SDL_RWclose},
	{"SDL_RWread_REAL", (uintptr_t)&SDL_RWread},
	{"SDL_RWseek_REAL", (uintptr_t)&SDL_RWseek},
	{"SDL_RWsize_REAL", (uintptr_t)&SDL_RWsize},
	{"SDL_RWtell_REAL", (uintptr_t)&SDL_RWtell},
	{"SDL_RWwrite_REAL", (uintptr_t)&SDL_RWwrite},
	{"SDL_RenderCopyExF_REAL", (uintptr_t)&SDL_RenderCopyExF},
	{"SDL_RenderCopyF_REAL", (uintptr_t)&SDL_RenderCopyF},
	{"SDL_RenderDrawLineF_REAL", (uintptr_t)&SDL_RenderDrawLineF},
	{"SDL_RenderDrawLinesF_REAL", (uintptr_t)&SDL_RenderDrawLinesF},
	{"SDL_RenderDrawPointF_REAL", (uintptr_t)&SDL_RenderDrawPointF},
	{"SDL_RenderDrawPointsF_REAL", (uintptr_t)&SDL_RenderDrawPointsF},
	{"SDL_RenderDrawRectF_REAL", (uintptr_t)&SDL_RenderDrawRectF},
	{"SDL_RenderDrawRectsF_REAL", (uintptr_t)&SDL_RenderDrawRectsF},
	{"SDL_RenderFillRectF_REAL", (uintptr_t)&SDL_RenderFillRectF},
	{"SDL_RenderFillRectsF_REAL", (uintptr_t)&SDL_RenderFillRectsF},
	{"SDL_RenderFlush_REAL", (uintptr_t)&SDL_RenderFlush},
	{"SDL_RenderGetClipRect_REAL", (uintptr_t)&SDL_RenderGetClipRect},
	{"SDL_RenderGetIntegerScale_REAL", (uintptr_t)&SDL_RenderGetIntegerScale},
	{"SDL_RenderGetMetalCommandEncoder_REAL", (uintptr_t)&SDL_RenderGetMetalCommandEncoder},
	{"SDL_RenderGetMetalLayer_REAL", (uintptr_t)&SDL_RenderGetMetalLayer},
	{"SDL_RenderGetScale_REAL", (uintptr_t)&SDL_RenderGetScale},
	{"SDL_RenderIsClipEnabled_REAL", (uintptr_t)&SDL_RenderIsClipEnabled},
	{"SDL_RenderReadPixels_REAL", (uintptr_t)&SDL_RenderReadPixels},
	{"SDL_RenderSetClipRect_REAL", (uintptr_t)&SDL_RenderSetClipRect},
	{"SDL_RenderSetIntegerScale_REAL", (uintptr_t)&SDL_RenderSetIntegerScale},
	{"SDL_ReportAssertion_REAL", (uintptr_t)&SDL_ReportAssertion},
	{"SDL_SIMDAlloc_REAL", (uintptr_t)&SDL_SIMDAlloc},
	{"SDL_SIMDFree_REAL", (uintptr_t)&SDL_SIMDFree},
	{"SDL_SIMDGetAlignment_REAL", (uintptr_t)&SDL_SIMDGetAlignment},
	{"SDL_SensorClose_REAL", (uintptr_t)&SDL_SensorClose},
	{"SDL_SensorFromInstanceID_REAL", (uintptr_t)&SDL_SensorFromInstanceID},
	{"SDL_SensorGetData_REAL", (uintptr_t)&SDL_SensorGetData},
	{"SDL_SensorGetDeviceInstanceID_REAL", (uintptr_t)&SDL_SensorGetDeviceInstanceID},
	{"SDL_SensorGetDeviceName_REAL", (uintptr_t)&SDL_SensorGetDeviceName},
	{"SDL_SensorGetDeviceNonPortableType_REAL", (uintptr_t)&SDL_SensorGetDeviceNonPortableType},
	{"SDL_SensorGetDeviceType_REAL", (uintptr_t)&SDL_SensorGetDeviceType},
	{"SDL_SensorGetInstanceID_REAL", (uintptr_t)&SDL_SensorGetInstanceID},
	{"SDL_SensorGetName_REAL", (uintptr_t)&SDL_SensorGetName},
	{"SDL_SensorGetNonPortableType_REAL", (uintptr_t)&SDL_SensorGetNonPortableType},
	{"SDL_SensorGetType_REAL", (uintptr_t)&SDL_SensorGetType},
	{"SDL_SensorOpen_REAL", (uintptr_t)&SDL_SensorOpen},
	{"SDL_SensorUpdate_REAL", (uintptr_t)&SDL_SensorUpdate},
	{"SDL_SetMainReady_REAL", (uintptr_t)&SDL_SetMainReady},
	{"SDL_SetMemoryFunctions_REAL", (uintptr_t)&SDL_SetMemoryFunctions},
	{"SDL_SetTextureScaleMode_REAL", (uintptr_t)&SDL_SetTextureScaleMode},
	{"SDL_SetWindowHitTest_REAL", (uintptr_t)&SDL_SetWindowHitTest},
	{"SDL_SetWindowInputFocus_REAL", (uintptr_t)&SDL_SetWindowInputFocus},
	{"SDL_SetWindowModalFor_REAL", (uintptr_t)&SDL_SetWindowModalFor},
	{"SDL_SetWindowOpacity_REAL", (uintptr_t)&SDL_SetWindowOpacity},
	{"SDL_SetWindowResizable_REAL", (uintptr_t)&SDL_SetWindowResizable},
	{"SDL_SetYUVConversionMode_REAL", (uintptr_t)&SDL_SetYUVConversionMode},
	{"SDL_TLSCreate_REAL", (uintptr_t)&SDL_TLSCreate},
	{"SDL_TLSGet_REAL", (uintptr_t)&SDL_TLSGet},
	{"SDL_TLSSet_REAL", (uintptr_t)&SDL_TLSSet},
	{"SDL_UnlockJoysticks_REAL", (uintptr_t)&SDL_UnlockJoysticks},
	{"SDL_UpdateYUVTexture_REAL", (uintptr_t)&SDL_UpdateYUVTexture},
	{"SDL_WarpMouseGlobal_REAL", (uintptr_t)&SDL_WarpMouseGlobal},
	{"SDL_acos_REAL", (uintptr_t)&SDL_acos},
	{"SDL_acosf_REAL", (uintptr_t)&SDL_acosf},
	{"SDL_asin_REAL", (uintptr_t)&SDL_asin},
	{"SDL_asinf_REAL", (uintptr_t)&SDL_asinf},
	{"SDL_atan_REAL", (uintptr_t)&SDL_atan},
	{"SDL_atan2_REAL", (uintptr_t)&SDL_atan2},
	{"SDL_atan2f_REAL", (uintptr_t)&SDL_atan2f},
	{"SDL_atanf_REAL", (uintptr_t)&SDL_atanf},
	{"SDL_ceilf_REAL", (uintptr_t)&SDL_ceilf},
	{"SDL_copysign_REAL", (uintptr_t)&SDL_copysign},
	{"SDL_copysignf_REAL", (uintptr_t)&SDL_copysignf},
	{"SDL_cos_REAL", (uintptr_t)&SDL_cos},
	{"SDL_exp_REAL", (uintptr_t)&SDL_exp},
	{"SDL_expf_REAL", (uintptr_t)&SDL_expf},
	{"SDL_fabs_REAL", (uintptr_t)&SDL_fabs},
	{"SDL_fabsf_REAL", (uintptr_t)&SDL_fabsf},
	{"SDL_floor_REAL", (uintptr_t)&SDL_floor},
	{"SDL_floorf_REAL", (uintptr_t)&SDL_floorf},
	{"SDL_fmod_REAL", (uintptr_t)&SDL_fmod},
	{"SDL_fmodf_REAL", (uintptr_t)&SDL_fmodf},
	{"SDL_islower_REAL", (uintptr_t)&SDL_islower},
	{"SDL_isupper_REAL", (uintptr_t)&SDL_isupper},
	{"SDL_log_REAL", (uintptr_t)&SDL_log},
	{"SDL_log10_REAL", (uintptr_t)&SDL_log10},
	{"SDL_log10f_REAL", (uintptr_t)&SDL_log10f},
	{"SDL_logf_REAL", (uintptr_t)&SDL_logf},
	{"SDL_ltoa_REAL", (uintptr_t)&SDL_ltoa},
	{"SDL_pow_REAL", (uintptr_t)&SDL_pow},
	{"SDL_powf_REAL", (uintptr_t)&SDL_powf},
	{"SDL_scalbn_REAL", (uintptr_t)&SDL_scalbn},
	{"SDL_scalbnf_REAL", (uintptr_t)&SDL_scalbnf},
	{"SDL_sin_REAL", (uintptr_t)&SDL_sin},
	{"SDL_sqrt_REAL", (uintptr_t)&SDL_sqrt},
	{"SDL_sqrtf_REAL", (uintptr_t)&SDL_sqrtf},
	{"SDL_strtokr_REAL", (uintptr_t)&SDL_strtokr},
	{"SDL_tan_REAL", (uintptr_t)&SDL_tan},
	{"SDL_tanf_REAL", (uintptr_t)&SDL_tanf},
	{"SDL_utf8strlen_REAL", (uintptr_t)&SDL_utf8strlen},
	{"SDL_vsnprintf_REAL", (uintptr_t)&SDL_vsnprintf},
	{"SDL_vsscanf_REAL", (uintptr_t)&SDL_vsscanf},
	{"SDL_wcscmp_REAL", (uintptr_t)&SDL_wcscmp},
	{"SDL_wcsdup_REAL", (uintptr_t)&SDL_wcsdup},
	{"SDL_wcsncmp_REAL", (uintptr_t)&SDL_wcsncmp},
	{"SDL_wcsstr_REAL", (uintptr_t)&SDL_wcsstr},
	{"SDL_wcsstr_REAL", (uintptr_t)&SDL_wcsstr},
};

static size_t numhooks = sizeof(default_dynlib) / sizeof(*default_dynlib);

int check_kubridge(void)
{
	int search_unk[2];
	return _vshKernelSearchModuleByName("kubridge", search_unk);
}

enum MethodIDs
{
	UNKNOWN = 0,
	INIT,
	GET_LANGUAGE
} MethodIDs;

typedef struct
{
	char *name;
	enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
	{"<init>", INIT},
	{"getLanguage", GET_LANGUAGE},
};

int GetMethodID(void *env, void *class, const char *name, const char *sig)
{
	printf("GetMethodID: %s\n", name);

	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++)
	{
		if (strcmp(name, name_to_method_ids[i].name) == 0)
		{
			return name_to_method_ids[i].id;
		}
	}

	return UNKNOWN;
}

int GetStaticMethodID(void *env, void *class, const char *name, const char *sig)
{
	printf("GetStaticMethodID: %s\n", name);

	for (int i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID); i++)
	{
		if (strcmp(name, name_to_method_ids[i].name) == 0)
			return name_to_method_ids[i].id;
	}

	return UNKNOWN;
}

void CallStaticVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
}

int CallStaticBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	switch (methodID)
	{
	default:
		return 0;
	}
}

int CallStaticIntMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	switch (methodID)
	{
	default:
		return 0;
	}
}

int64_t CallStaticLongMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	switch (methodID)
	{
	default:
		return 0;
	}
}

uint64_t CallLongMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	return -1;
}

void *FindClass(void)
{
	return (void *)0x41414141;
}

void *NewGlobalRef(void *env, char *str)
{
	return (void *)0x42424242;
}

void DeleteGlobalRef(void *env, char *str)
{
}

void *NewObjectV(void *env, void *clazz, int methodID, uintptr_t args)
{
	return (void *)0x43434343;
}

void *GetObjectClass(void *env, void *obj)
{
	return (void *)0x44444444;
}

char *NewStringUTF(void *env, char *bytes)
{
	return bytes;
}

char *GetStringUTFChars(void *env, char *string, int *isCopy)
{
	return string;
}

size_t GetStringUTFLength(void *env, char *string)
{
	return strlen(string);
}

int GetJavaVM(void *env, void **vm)
{
	*vm = fake_vm;
	return 0;
}

int GetFieldID(void *env, void *clazz, const char *name, const char *sig)
{
	return 0;
}

int GetBooleanField(void *env, void *obj, int fieldID)
{
	return 1;
}

void *GetObjectArrayElement(void *env, uint8_t *obj, int idx)
{
	return NULL;
}

int CallBooleanMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	switch (methodID)
	{
	default:
		return 0;
	}
}

char duration[32];
void *CallObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	int lang = -1;
	switch (methodID)
	{
	case GET_LANGUAGE:
		sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
		switch (lang)
		{
		case SCE_SYSTEM_PARAM_LANG_FRENCH:
			return "fr";
		case SCE_SYSTEM_PARAM_LANG_SPANISH:
			return "es";
		case SCE_SYSTEM_PARAM_LANG_GERMAN:
			return "de";
		case SCE_SYSTEM_PARAM_LANG_ITALIAN:
			return "it";
		case SCE_SYSTEM_PARAM_LANG_RUSSIAN:
			return "ru";
		default:
			return "en";
		}
	default:
		return 0x34343434;
	}
}

int CallIntMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	switch (methodID)
	{
	default:
		return 0;
	}
}

void CallVoidMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	switch (methodID)
	{
	default:
		break;
	}
}

int GetStaticFieldID(void *env, void *clazz, const char *name, const char *sig)
{
	return 0;
}

void *GetStaticObjectField(void *env, void *clazz, int fieldID)
{
	switch (fieldID)
	{
	default:
		return NULL;
	}
}

void GetStringUTFRegion(void *env, char *str, size_t start, size_t len, char *buf)
{
	sceClibMemcpy(buf, &str[start], len);
	buf[len] = 0;
}

void *CallStaticObjectMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	return NULL;
}

int GetIntField(void *env, void *obj, int fieldID) { return 0; }

float GetFloatField(void *env, void *obj, int fieldID)
{
	switch (fieldID)
	{
	default:
		return 0.0f;
	}
}

float CallStaticFloatMethodV(void *env, void *obj, int methodID, uintptr_t *args)
{
	switch (methodID)
	{
	default:
		if (methodID != UNKNOWN)
		{
			dlog("CallStaticDoubleMethodV(%d)\n", methodID);
		}
		return 0;
	}
}

int GetArrayLength(void *env, void *array)
{
	printf("GetArrayLength returned %d\n", *(int *)array);
	return *(int *)array;
}

/*int crasher(unsigned int argc, void *argv) {
	uint32_t *nullptr = NULL;
	for (;;) {
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		if (pad.buttons & SCE_CTRL_SELECT) *nullptr = 0;
		sceKernelDelayThread(100);
	}
}*/

void GGLog(const char *fmt, ...)
{
	va_list list;
	static char string[0x8000];

	va_start(list, fmt);
	vsprintf(string, fmt, list);
	va_end(list);

	printf("[GGLOG] %s\n", string);
}

void patch_game(void)
{
	// hook_addr(so_symbol(&diceydungeons_mod, "Android_JNI_GetEnv"), (uintptr_t)&Android_JNI_GetEnv);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AddEventWatch"), (uintptr_t)&SDL_AddEventWatch);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AddHintCallback"), (uintptr_t)&SDL_AddHintCallback);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AddTimer"), (uintptr_t)&SDL_AddTimer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AllocFormat"), (uintptr_t)&SDL_AllocFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AllocPalette"), (uintptr_t)&SDL_AllocPalette);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AllocRW"), (uintptr_t)&SDL_AllocRW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidBackButton"), (uintptr_t)&SDL_AndroidBackButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetActivity"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetExternalStoragePath"), (uintptr_t)&SDL_AndroidGetExternalStoragePath);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetExternalStorageState"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetInternalStoragePath"), (uintptr_t)&SDL_AndroidGetInternalStoragePath);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetJNIEnv"), (uintptr_t)&Android_JNI_GetEnv);
	// hook_addr(so_symbol(&diceydungeons_mod, "SDL_Android_Init"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicAdd"), (uintptr_t)&SDL_AtomicAdd);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicCAS"), (uintptr_t)&SDL_AtomicCAS);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicCASPtr"), (uintptr_t)&SDL_AtomicCASPtr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicGet"), (uintptr_t)&SDL_AtomicGet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicGetPtr"), (uintptr_t)&SDL_AtomicGetPtr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicLock"), (uintptr_t)&SDL_AtomicLock);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicSet"), (uintptr_t)&SDL_AtomicSet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicSetPtr"), (uintptr_t)&SDL_AtomicSetPtr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicTryLock"), (uintptr_t)&SDL_AtomicTryLock);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicUnlock"), (uintptr_t)&SDL_AtomicUnlock);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioInit"), (uintptr_t)&SDL_AudioInit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioQuit"), (uintptr_t)&SDL_AudioQuit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamAvailable"), (uintptr_t)&SDL_AudioStreamAvailable);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamClear"), (uintptr_t)&SDL_AudioStreamClear);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamFlush"), (uintptr_t)&SDL_AudioStreamFlush);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamGet"), (uintptr_t)&SDL_AudioStreamGet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamPut"), (uintptr_t)&SDL_AudioStreamPut);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_BuildAudioCVT"), (uintptr_t)&SDL_BuildAudioCVT);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CalculateGammaRamp"), (uintptr_t)&SDL_CalculateGammaRamp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CaptureMouse"), (uintptr_t)&SDL_CaptureMouse);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ClearError"), (uintptr_t)&SDL_ClearError);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ClearHints"), (uintptr_t)&SDL_ClearHints);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ClearQueuedAudio"), (uintptr_t)&SDL_ClearQueuedAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CloseAudio"), (uintptr_t)&SDL_CloseAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CloseAudioDevice"), (uintptr_t)&SDL_CloseAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ComposeCustomBlendMode"), (uintptr_t)&SDL_ComposeCustomBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CondBroadcast"), (uintptr_t)&SDL_CondBroadcast);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CondSignal"), (uintptr_t)&SDL_CondSignal);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CondWait"), (uintptr_t)&SDL_CondWait);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CondWaitTimeout"), (uintptr_t)&SDL_CondWaitTimeout);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ConvertAudio"), (uintptr_t)&SDL_ConvertAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ConvertPixels"), (uintptr_t)&SDL_ConvertPixels);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ConvertSurface"), (uintptr_t)&SDL_ConvertSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ConvertSurfaceFormat"), (uintptr_t)&SDL_ConvertSurfaceFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateColorCursor"), (uintptr_t)&SDL_CreateColorCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateCond"), (uintptr_t)&SDL_CreateCond);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateCursor"), (uintptr_t)&SDL_CreateCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateMutex"), (uintptr_t)&SDL_CreateMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRGBSurface"), (uintptr_t)&SDL_CreateRGBSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRGBSurfaceFrom"), (uintptr_t)&SDL_CreateRGBSurfaceFrom);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRGBSurfaceWithFormat"), (uintptr_t)&SDL_CreateRGBSurfaceWithFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRGBSurfaceWithFormatFrom"), (uintptr_t)&SDL_CreateRGBSurfaceWithFormatFrom);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRenderer"), (uintptr_t)&SDL_CreateRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateSemaphore"), (uintptr_t)&SDL_CreateSemaphore);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateShapedWindow"), (uintptr_t)&SDL_CreateShapedWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateSoftwareRenderer"), (uintptr_t)&SDL_CreateSoftwareRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateSystemCursor"), (uintptr_t)&SDL_CreateSystemCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateTexture"), (uintptr_t)&SDL_CreateTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateTextureFromSurface"), (uintptr_t)&SDL_CreateTextureFromSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateThread"), (uintptr_t)&SDL_CreateThread);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateThreadWithStackSize"), (uintptr_t)&SDL_CreateThreadWithStackSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateWindow"), (uintptr_t)&SDL_CreateWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateWindowAndRenderer"), (uintptr_t)&SDL_CreateWindowAndRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateWindowFrom"), (uintptr_t)&SDL_CreateWindowFrom);
	// hook_addr(so_symbol(&diceydungeons_mod, "SDL_DYNAPI_entry"), (uintptr_t)&SDL_DYNAPI_entry);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DelEventWatch"), (uintptr_t)&SDL_DelEventWatch);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DelHintCallback"), (uintptr_t)&SDL_DelHintCallback);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Delay"), (uintptr_t)&SDL_Delay);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DequeueAudio"), (uintptr_t)&SDL_DequeueAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyCond"), (uintptr_t)&SDL_DestroyCond);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyMutex"), (uintptr_t)&SDL_DestroyMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyRenderer"), (uintptr_t)&SDL_DestroyRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroySemaphore"), (uintptr_t)&SDL_DestroySemaphore);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyTexture"), (uintptr_t)&SDL_DestroyTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyWindow"), (uintptr_t)&SDL_DestroyWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DetachThread"), (uintptr_t)&SDL_DetachThread);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DisableScreenSaver"), (uintptr_t)&SDL_DisableScreenSaver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DuplicateSurface"), (uintptr_t)&SDL_DuplicateSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_EnableScreenSaver"), (uintptr_t)&SDL_EnableScreenSaver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_EnclosePoints"), (uintptr_t)&SDL_EnclosePoints);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Error"), (uintptr_t)&SDL_Error);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_EventState"), (uintptr_t)&SDL_EventState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FillRect"), (uintptr_t)&SDL_FillRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FillRects"), (uintptr_t)&SDL_FillRects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FilterEvents"), (uintptr_t)&SDL_FilterEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FlushEvent"), (uintptr_t)&SDL_FlushEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FlushEvents"), (uintptr_t)&SDL_FlushEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeAudioStream"), (uintptr_t)&SDL_FreeAudioStream);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeCursor"), (uintptr_t)&SDL_FreeCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeFormat"), (uintptr_t)&SDL_FreeFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreePalette"), (uintptr_t)&SDL_FreePalette);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeRW"), (uintptr_t)&SDL_FreeRW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeSurface"), (uintptr_t)&SDL_FreeSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeWAV"), (uintptr_t)&SDL_FreeWAV);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_BindTexture"), (uintptr_t)&SDL_GL_BindTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_CreateContext"), (uintptr_t)&SDL_GL_CreateContext);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_DeleteContext"), (uintptr_t)&SDL_GL_DeleteContext);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_ExtensionSupported"), (uintptr_t)&SDL_GL_ExtensionSupported);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetAttribute"), (uintptr_t)&SDL_GL_GetAttribute);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetCurrentContext"), (uintptr_t)&SDL_GL_GetCurrentContext);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetCurrentWindow"), (uintptr_t)&SDL_GL_GetCurrentWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetDrawableSize"), (uintptr_t)&SDL_GL_GetDrawableSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetProcAddress"), (uintptr_t)&SDL_GL_GetProcAddress);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetSwapInterval"), (uintptr_t)&SDL_GL_GetSwapInterval);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_LoadLibrary"), (uintptr_t)&SDL_GL_LoadLibrary);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_MakeCurrent"), (uintptr_t)&SDL_GL_MakeCurrent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_ResetAttributes"), (uintptr_t)&SDL_GL_ResetAttributes);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_SetAttribute"), (uintptr_t)&SDL_GL_SetAttribute);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_SetSwapInterval"), (uintptr_t)&SDL_GL_SetSwapInterval);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_SwapWindow"), (uintptr_t)&SDL_GL_SwapWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_UnbindTexture"), (uintptr_t)&SDL_GL_UnbindTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_UnloadLibrary"), (uintptr_t)&SDL_GL_UnloadLibrary);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerAddMapping"), (uintptr_t)&SDL_GameControllerAddMapping);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerAddMappingsFromRW"), (uintptr_t)&SDL_GameControllerAddMappingsFromRW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerClose"), (uintptr_t)&SDL_GameControllerClose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerEventState"), (uintptr_t)&SDL_GameControllerEventState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerFromInstanceID"), (uintptr_t)&SDL_GameControllerFromInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerFromPlayerIndex"), (uintptr_t)&SDL_GameControllerFromPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetAttached"), (uintptr_t)&SDL_GameControllerGetAttached);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetAxis"), (uintptr_t)&SDL_GameControllerGetAxis);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetAxisFromString"), (uintptr_t)&SDL_GameControllerGetAxisFromString);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetBindForAxis"), (uintptr_t)&SDL_GameControllerGetBindForAxis);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetBindForButton"), (uintptr_t)&SDL_GameControllerGetBindForButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetButton"), (uintptr_t)&SDL_GameControllerGetButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetButtonFromString"), (uintptr_t)&SDL_GameControllerGetButtonFromString);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetJoystick"), (uintptr_t)&SDL_GameControllerGetJoystick);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetPlayerIndex"), (uintptr_t)&SDL_GameControllerGetPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetProduct"), (uintptr_t)&SDL_GameControllerGetProduct);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetProductVersion"), (uintptr_t)&SDL_GameControllerGetProductVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetStringForAxis"), (uintptr_t)&SDL_GameControllerGetStringForAxis);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetStringForButton"), (uintptr_t)&SDL_GameControllerGetStringForButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetType"), (uintptr_t)&SDL_GameControllerGetType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetVendor"), (uintptr_t)&SDL_GameControllerGetVendor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerMapping"), (uintptr_t)&SDL_GameControllerMapping);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerMappingForDeviceIndex"), (uintptr_t)&SDL_GameControllerMappingForDeviceIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerMappingForGUID"), (uintptr_t)&SDL_GameControllerMappingForGUID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerMappingForIndex"), (uintptr_t)&SDL_GameControllerMappingForIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerName"), (uintptr_t)&SDL_GameControllerName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerNameForIndex"), (uintptr_t)&SDL_GameControllerNameForIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerNumMappings"), (uintptr_t)&SDL_GameControllerNumMappings);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerOpen"), (uintptr_t)&SDL_GameControllerOpen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerRumble"), (uintptr_t)&SDL_GameControllerRumble);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerSetPlayerIndex"), (uintptr_t)&SDL_GameControllerSetPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerTypeForIndex"), (uintptr_t)&SDL_GameControllerTypeForIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerUpdate"), (uintptr_t)&SDL_GameControllerUpdate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAndroidSDKVersion"), (uintptr_t)&SDL_GetAndroidSDKVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAssertionHandler"), (uintptr_t)&SDL_GetAssertionHandler);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAssertionReport"), (uintptr_t)&SDL_GetAssertionReport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAudioDeviceName"), (uintptr_t)&SDL_GetAudioDeviceName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAudioDeviceStatus"), (uintptr_t)&SDL_GetAudioDeviceStatus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAudioDriver"), (uintptr_t)&SDL_GetAudioDriver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAudioStatus"), (uintptr_t)&SDL_GetAudioStatus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetBasePath"), (uintptr_t)&SDL_GetBasePath);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCPUCacheLineSize"), (uintptr_t)&SDL_GetCPUCacheLineSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCPUCount"), (uintptr_t)&SDL_GetCPUCount);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetClipRect"), (uintptr_t)&SDL_GetClipRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetClipboardText"), (uintptr_t)&SDL_GetClipboardText);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetClosestDisplayMode"), (uintptr_t)&SDL_GetClosestDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetColorKey"), (uintptr_t)&SDL_GetColorKey);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCurrentAudioDriver"), (uintptr_t)&SDL_GetCurrentAudioDriver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCurrentDisplayMode"), (uintptr_t)&SDL_GetCurrentDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCurrentVideoDriver"), (uintptr_t)&SDL_GetCurrentVideoDriver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCursor"), (uintptr_t)&SDL_GetCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDefaultAssertionHandler"), (uintptr_t)&SDL_GetDefaultAssertionHandler);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDefaultCursor"), (uintptr_t)&SDL_GetDefaultCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDesktopDisplayMode"), (uintptr_t)&SDL_GetDesktopDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayBounds"), (uintptr_t)&SDL_GetDisplayBounds);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayDPI"), (uintptr_t)&SDL_GetDisplayDPI);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayMode"), (uintptr_t)&SDL_GetDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayName"), (uintptr_t)&SDL_GetDisplayName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayOrientation"), (uintptr_t)&SDL_GetDisplayOrientation);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayUsableBounds"), (uintptr_t)&SDL_GetDisplayUsableBounds);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetError"), (uintptr_t)&SDL_GetError);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetEventFilter"), (uintptr_t)&SDL_GetEventFilter);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetGlobalMouseState"), (uintptr_t)&SDL_GetGlobalMouseState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetGrabbedWindow"), (uintptr_t)&SDL_GetGrabbedWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetHint"), (uintptr_t)&SDL_GetHint);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetHintBoolean"), (uintptr_t)&SDL_GetHintBoolean);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyFromName"), (uintptr_t)&SDL_GetKeyFromName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyFromScancode"), (uintptr_t)&SDL_GetKeyFromScancode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyName"), (uintptr_t)&SDL_GetKeyName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyboardFocus"), (uintptr_t)&SDL_GetKeyboardFocus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyboardState"), (uintptr_t)&SDL_GetKeyboardState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetMemoryFunctions"), (uintptr_t)&SDL_GetMemoryFunctions);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetModState"), (uintptr_t)&SDL_GetModState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetMouseFocus"), (uintptr_t)&SDL_GetMouseFocus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetMouseState"), (uintptr_t)&SDL_GetMouseState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumAllocations"), (uintptr_t)&SDL_GetNumAllocations);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumAudioDevices"), (uintptr_t)&SDL_GetNumAudioDevices);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumAudioDrivers"), (uintptr_t)&SDL_GetNumAudioDrivers);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumDisplayModes"), (uintptr_t)&SDL_GetNumDisplayModes);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumRenderDrivers"), (uintptr_t)&SDL_GetNumRenderDrivers);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumTouchDevices"), (uintptr_t)&SDL_GetNumTouchDevices);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumTouchFingers"), (uintptr_t)&SDL_GetNumTouchFingers);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumVideoDisplays"), (uintptr_t)&SDL_GetNumVideoDisplays);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumVideoDrivers"), (uintptr_t)&SDL_GetNumVideoDrivers);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPerformanceCounter"), (uintptr_t)&SDL_GetPerformanceCounter);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPerformanceFrequency"), (uintptr_t)&SDL_GetPerformanceFrequency);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPixelFormatName"), (uintptr_t)&SDL_GetPixelFormatName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPlatform"), (uintptr_t)&SDL_GetPlatform);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPowerInfo"), (uintptr_t)&SDL_GetPowerInfo);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPrefPath"), (uintptr_t)&SDL_GetPrefPath_hook);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetQueuedAudioSize"), (uintptr_t)&SDL_GetQueuedAudioSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRGB"), (uintptr_t)&SDL_GetRGB);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRGBA"), (uintptr_t)&SDL_GetRGBA);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRelativeMouseMode"), (uintptr_t)&SDL_GetRelativeMouseMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRelativeMouseState"), (uintptr_t)&SDL_GetRelativeMouseState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderDrawBlendMode"), (uintptr_t)&SDL_GetRenderDrawBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderDrawColor"), (uintptr_t)&SDL_GetRenderDrawColor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderDriverInfo"), (uintptr_t)&SDL_GetRenderDriverInfo);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderTarget"), (uintptr_t)&SDL_GetRenderTarget);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderer"), (uintptr_t)&SDL_GetRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRendererInfo"), (uintptr_t)&SDL_GetRendererInfo);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRendererOutputSize"), (uintptr_t)&SDL_GetRendererOutputSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRevision"), (uintptr_t)&SDL_GetRevision);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetScancodeFromKey"), (uintptr_t)&SDL_GetScancodeFromKey);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetScancodeFromName"), (uintptr_t)&SDL_GetScancodeFromName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetScancodeName"), (uintptr_t)&SDL_GetScancodeName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetShapedWindowMode"), (uintptr_t)&SDL_GetShapedWindowMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetSurfaceAlphaMod"), (uintptr_t)&SDL_GetSurfaceAlphaMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetSurfaceBlendMode"), (uintptr_t)&SDL_GetSurfaceBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetSurfaceColorMod"), (uintptr_t)&SDL_GetSurfaceColorMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetSystemRAM"), (uintptr_t)&SDL_GetSystemRAM);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTextureAlphaMod"), (uintptr_t)&SDL_GetTextureAlphaMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTextureBlendMode"), (uintptr_t)&SDL_GetTextureBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTextureColorMod"), (uintptr_t)&SDL_GetTextureColorMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTextureScaleMode"), (uintptr_t)&SDL_GetTextureScaleMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetThreadID"), (uintptr_t)&SDL_GetThreadID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetThreadName"), (uintptr_t)&SDL_GetThreadName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTicks"), (uintptr_t)&SDL_GetTicks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTouchDevice"), (uintptr_t)&SDL_GetTouchDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTouchDeviceType"), (uintptr_t)&SDL_GetTouchDeviceType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTouchFinger"), (uintptr_t)&SDL_GetTouchFinger);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetVersion"), (uintptr_t)&SDL_GetVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetVideoDriver"), (uintptr_t)&SDL_GetVideoDriver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowBordersSize"), (uintptr_t)&SDL_GetWindowBordersSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowBrightness"), (uintptr_t)&SDL_GetWindowBrightness);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowData"), (uintptr_t)&SDL_GetWindowData);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowDisplayIndex"), (uintptr_t)&SDL_GetWindowDisplayIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowDisplayMode"), (uintptr_t)&SDL_GetWindowDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowFlags"), (uintptr_t)&SDL_GetWindowFlags);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowFromID"), (uintptr_t)&SDL_GetWindowFromID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowGammaRamp"), (uintptr_t)&SDL_GetWindowGammaRamp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowGrab"), (uintptr_t)&SDL_GetWindowGrab);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowID"), (uintptr_t)&SDL_GetWindowID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowMaximumSize"), (uintptr_t)&SDL_GetWindowMaximumSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowMinimumSize"), (uintptr_t)&SDL_GetWindowMinimumSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowOpacity"), (uintptr_t)&SDL_GetWindowOpacity);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowPixelFormat"), (uintptr_t)&SDL_GetWindowPixelFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowPosition"), (uintptr_t)&SDL_GetWindowPosition);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowSize"), (uintptr_t)&SDL_GetWindowSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowSurface"), (uintptr_t)&SDL_GetWindowSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowTitle"), (uintptr_t)&SDL_GetWindowTitle);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowWMInfo"), (uintptr_t)&SDL_GetWindowWMInfo);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetYUVConversionMode"), (uintptr_t)&SDL_GetYUVConversionMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetYUVConversionModeForResolution"), (uintptr_t)&SDL_GetYUVConversionModeForResolution);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticClose"), (uintptr_t)&SDL_HapticClose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticDestroyEffect"), (uintptr_t)&SDL_HapticDestroyEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticEffectSupported"), (uintptr_t)&SDL_HapticEffectSupported);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticGetEffectStatus"), (uintptr_t)&SDL_HapticGetEffectStatus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticIndex"), (uintptr_t)&SDL_HapticIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticName"), (uintptr_t)&SDL_HapticName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticNewEffect"), (uintptr_t)&SDL_HapticNewEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticNumAxes"), (uintptr_t)&SDL_HapticNumAxes);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticNumEffects"), (uintptr_t)&SDL_HapticNumEffects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticNumEffectsPlaying"), (uintptr_t)&SDL_HapticNumEffectsPlaying);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticOpen"), (uintptr_t)&SDL_HapticOpen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticOpenFromJoystick"), (uintptr_t)&SDL_HapticOpenFromJoystick);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticOpenFromMouse"), (uintptr_t)&SDL_HapticOpenFromMouse);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticOpened"), (uintptr_t)&SDL_HapticOpened);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticPause"), (uintptr_t)&SDL_HapticPause);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticQuery"), (uintptr_t)&SDL_HapticQuery);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRumbleInit"), (uintptr_t)&SDL_HapticRumbleInit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRumblePlay"), (uintptr_t)&SDL_HapticRumblePlay);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRumbleStop"), (uintptr_t)&SDL_HapticRumbleStop);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRumbleSupported"), (uintptr_t)&SDL_HapticRumbleSupported);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRunEffect"), (uintptr_t)&SDL_HapticRunEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticSetAutocenter"), (uintptr_t)&SDL_HapticSetAutocenter);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticSetGain"), (uintptr_t)&SDL_HapticSetGain);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticStopAll"), (uintptr_t)&SDL_HapticStopAll);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticStopEffect"), (uintptr_t)&SDL_HapticStopEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticUnpause"), (uintptr_t)&SDL_HapticUnpause);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticUpdateEffect"), (uintptr_t)&SDL_HapticUpdateEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Has3DNow"), (uintptr_t)&SDL_Has3DNow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasARMSIMD"), (uintptr_t)&SDL_HasARMSIMD);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasAVX"), (uintptr_t)&SDL_HasAVX);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasAVX2"), (uintptr_t)&SDL_HasAVX2);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasAVX512F"), (uintptr_t)&SDL_HasAVX512F);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasAltiVec"), (uintptr_t)&SDL_HasAltiVec);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasClipboardText"), (uintptr_t)&SDL_HasClipboardText);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasColorKey"), (uintptr_t)&SDL_HasColorKey);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasEvent"), (uintptr_t)&SDL_HasEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasEvents"), (uintptr_t)&SDL_HasEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasIntersection"), (uintptr_t)&SDL_HasIntersection);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasMMX"), (uintptr_t)&SDL_HasMMX);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasNEON"), (uintptr_t)&SDL_HasNEON);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasRDTSC"), (uintptr_t)&SDL_HasRDTSC);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE"), (uintptr_t)&SDL_HasSSE);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE2"), (uintptr_t)&SDL_HasSSE2);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE3"), (uintptr_t)&SDL_HasSSE3);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE41"), (uintptr_t)&SDL_HasSSE41);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE42"), (uintptr_t)&SDL_HasSSE42);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasScreenKeyboardSupport"), (uintptr_t)&SDL_HasScreenKeyboardSupport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HideWindow"), (uintptr_t)&SDL_HideWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Init"), (uintptr_t)&SDL_Init);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_InitSubSystem"), (uintptr_t)&SDL_InitSubSystem);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IntersectRect"), (uintptr_t)&SDL_IntersectRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IntersectRectAndLine"), (uintptr_t)&SDL_IntersectRectAndLine);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsGameController"), (uintptr_t)&SDL_IsGameController);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsScreenKeyboardShown"), (uintptr_t)&SDL_IsScreenKeyboardShown);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsScreenSaverEnabled"), (uintptr_t)&SDL_IsScreenSaverEnabled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsShapedWindow"), (uintptr_t)&SDL_IsShapedWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsAndroidTV"), (uintptr_t)&SDL_IsAndroidTV);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsChromebook"), (uintptr_t)&SDL_IsChromebook);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsDeXMode"), (uintptr_t)&SDL_IsDeXMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsGameController"), (uintptr_t)&SDL_IsGameController);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsScreenKeyboardShown"), (uintptr_t)&SDL_IsScreenKeyboardShown);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsScreenSaverEnabled"), (uintptr_t)&SDL_IsScreenSaverEnabled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsShapedWindow"), (uintptr_t)&SDL_IsShapedWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsTablet"), (uintptr_t)&SDL_IsTablet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsTextInputActive"), (uintptr_t)&SDL_IsTextInputActive);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickClose"), (uintptr_t)&SDL_JoystickClose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickCurrentPowerLevel"), (uintptr_t)&SDL_JoystickCurrentPowerLevel);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickEventState"), (uintptr_t)&SDL_JoystickEventState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickFromInstanceID"), (uintptr_t)&SDL_JoystickFromInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickFromPlayerIndex"), (uintptr_t)&SDL_JoystickFromPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetAttached"), (uintptr_t)&SDL_JoystickGetAttached);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetAxis"), (uintptr_t)&SDL_JoystickGetAxis);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetAxisInitialState"), (uintptr_t)&SDL_JoystickGetAxisInitialState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetBall"), (uintptr_t)&SDL_JoystickGetBall);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetButton"), (uintptr_t)&SDL_JoystickGetButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceGUID"), (uintptr_t)&SDL_JoystickGetDeviceGUID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceInstanceID"), (uintptr_t)&SDL_JoystickGetDeviceInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDevicePlayerIndex"), (uintptr_t)&SDL_JoystickGetDevicePlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceProduct"), (uintptr_t)&SDL_JoystickGetDeviceProduct);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceProductVersion"), (uintptr_t)&SDL_JoystickGetDeviceProductVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceType"), (uintptr_t)&SDL_JoystickGetDeviceType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceVendor"), (uintptr_t)&SDL_JoystickGetDeviceVendor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetGUID"), (uintptr_t)&SDL_JoystickGetGUID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetGUIDFromString"), (uintptr_t)&SDL_JoystickGetGUIDFromString);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetGUIDString"), (uintptr_t)&SDL_JoystickGetGUIDString);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetHat"), (uintptr_t)&SDL_JoystickGetHat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetPlayerIndex"), (uintptr_t)&SDL_JoystickGetPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetProduct"), (uintptr_t)&SDL_JoystickGetProduct);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetProductVersion"), (uintptr_t)&SDL_JoystickGetProductVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetType"), (uintptr_t)&SDL_JoystickGetType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetVendor"), (uintptr_t)&SDL_JoystickGetVendor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickInstanceID"), (uintptr_t)&SDL_JoystickInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickIsHaptic"), (uintptr_t)&SDL_JoystickIsHaptic);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickName"), (uintptr_t)&SDL_JoystickName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNameForIndex"), (uintptr_t)&SDL_JoystickNameForIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNumAxes"), (uintptr_t)&SDL_JoystickNumAxes);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNumBalls"), (uintptr_t)&SDL_JoystickNumBalls);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNumButtons"), (uintptr_t)&SDL_JoystickNumButtons);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNumHats"), (uintptr_t)&SDL_JoystickNumHats);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickOpen"), (uintptr_t)&SDL_JoystickOpen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickRumble"), (uintptr_t)&SDL_JoystickRumble);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickSetPlayerIndex"), (uintptr_t)&SDL_JoystickSetPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickUpdate"), (uintptr_t)&SDL_JoystickUpdate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadBMP_RW"), (uintptr_t)&SDL_LoadBMP_RW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadDollarTemplates"), (uintptr_t)&SDL_LoadDollarTemplates);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadFile"), (uintptr_t)&SDL_LoadFile);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadFile_RW"), (uintptr_t)&SDL_LoadFile_RW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadFunction"), (uintptr_t)&SDL_LoadFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadObject"), (uintptr_t)&SDL_LoadObject);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadWAV_RW"), (uintptr_t)&SDL_LoadWAV_RW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockAudio"), (uintptr_t)&SDL_LockAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockAudioDevice"), (uintptr_t)&SDL_LockAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockJoysticks"), (uintptr_t)&SDL_LockJoysticks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockMutex"), (uintptr_t)&SDL_LockMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockSurface"), (uintptr_t)&SDL_LockSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockTexture"), (uintptr_t)&SDL_LockTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockTextureToSurface"), (uintptr_t)&SDL_LockTextureToSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Log"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogCritical"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogDebug"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogError"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogGetOutputFunction"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogGetPriority"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogInfo"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogMessage"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogMessageV"), (uintptr_t)&ret0);			// MessageV);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogResetPriorities"), (uintptr_t)&ret0);	// ResetPriorities);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogSetAllPriority"), (uintptr_t)&ret0);	// SetAllPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogSetOutputFunction"), (uintptr_t)&ret0); // SetOutputFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogSetPriority"), (uintptr_t)&ret0);		// SetPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogVerbose"), (uintptr_t)&ret0);			// Verbose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogWarn"), (uintptr_t)&ret0);				// Warn);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LowerBlit"), (uintptr_t)&SDL_LowerBlit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LowerBlitScaled"), (uintptr_t)&SDL_LowerBlitScaled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MapRGB"), (uintptr_t)&SDL_MapRGB);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MapRGBA"), (uintptr_t)&SDL_MapRGBA);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MasksToPixelFormatEnum"), (uintptr_t)&SDL_MasksToPixelFormatEnum);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MaximizeWindow"), (uintptr_t)&SDL_MaximizeWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MemoryBarrierAcquireFunction"), (uintptr_t)&SDL_MemoryBarrierAcquireFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MemoryBarrierReleaseFunction"), (uintptr_t)&SDL_MemoryBarrierReleaseFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Metal_CreateView"), (uintptr_t)&SDL_Metal_CreateView);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Metal_DestroyView"), (uintptr_t)&SDL_Metal_DestroyView);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MinimizeWindow"), (uintptr_t)&SDL_MinimizeWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MixAudio"), (uintptr_t)&SDL_MixAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MixAudioFormat"), (uintptr_t)&SDL_MixAudioFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MouseIsHaptic"), (uintptr_t)&SDL_MouseIsHaptic);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_NewAudioStream"), (uintptr_t)&SDL_NewAudioStream);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_NumHaptics"), (uintptr_t)&SDL_NumHaptics);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_NumJoysticks"), (uintptr_t)&SDL_NumJoysticks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_NumSensors"), (uintptr_t)&SDL_NumSensors);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationDidBecomeActive"), (uintptr_t)&SDL_OnApplicationDidBecomeActive);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationDidEnterBackground"), (uintptr_t)&SDL_OnApplicationDidEnterBackground);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationDidReceiveMemoryWarning"), (uintptr_t)&SDL_OnApplicationDidReceiveMemoryWarning);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationWillEnterForeground"), (uintptr_t)&SDL_OnApplicationWillEnterForeground);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationWillResignActive"), (uintptr_t)&SDL_OnApplicationWillResignActive);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationWillTerminate"), (uintptr_t)&SDL_OnApplicationWillTerminate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OpenAudio"), (uintptr_t)&SDL_OpenAudio_fake);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OpenAudioDevice"), (uintptr_t)&SDL_OpenAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PauseAudio"), (uintptr_t)&SDL_PauseAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PauseAudioDevice"), (uintptr_t)&SDL_PauseAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PeepEvents"), (uintptr_t)&SDL_PeepEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PixelFormatEnumToMasks"), (uintptr_t)&SDL_PixelFormatEnumToMasks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PollEvent"), (uintptr_t)&SDL_PollEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PumpEvents"), (uintptr_t)&SDL_PumpEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PushEvent"), (uintptr_t)&SDL_PushEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_QueryTexture"), (uintptr_t)&SDL_QueryTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_QueueAudio"), (uintptr_t)&SDL_QueueAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Quit"), (uintptr_t)&SDL_Quit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_QuitSubSystem"), (uintptr_t)&SDL_QuitSubSystem);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWFromConstMem"), (uintptr_t)&SDL_RWFromConstMem);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWFromFP"), (uintptr_t)&SDL_RWFromFP);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWFromFile"), (uintptr_t)&SDL_RWFromFile_hook);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWFromMem"), (uintptr_t)&SDL_RWFromMem);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWclose"), (uintptr_t)&SDL_RWclose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWread"), (uintptr_t)&SDL_RWread);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWseek"), (uintptr_t)&SDL_RWseek);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWsize"), (uintptr_t)&SDL_RWsize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWtell"), (uintptr_t)&SDL_RWtell);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWwrite"), (uintptr_t)&SDL_RWwrite);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RaiseWindow"), (uintptr_t)&SDL_RaiseWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadBE16"), (uintptr_t)&SDL_ReadBE16);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadBE32"), (uintptr_t)&SDL_ReadBE32);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadBE64"), (uintptr_t)&SDL_ReadBE64);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadLE16"), (uintptr_t)&SDL_ReadLE16);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadLE32"), (uintptr_t)&SDL_ReadLE32);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadLE64"), (uintptr_t)&SDL_ReadLE64);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadU8"), (uintptr_t)&SDL_ReadU8);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RecordGesture"), (uintptr_t)&SDL_RecordGesture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RegisterEvents"), (uintptr_t)&SDL_RegisterEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RemoveTimer"), (uintptr_t)&SDL_RemoveTimer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderClear"), (uintptr_t)&SDL_RenderClear);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderCopy"), (uintptr_t)&SDL_RenderCopy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderCopyEx"), (uintptr_t)&SDL_RenderCopyEx);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderCopyExF"), (uintptr_t)&SDL_RenderCopyExF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderCopyF"), (uintptr_t)&SDL_RenderCopyF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawLine"), (uintptr_t)&SDL_RenderDrawLine);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawLineF"), (uintptr_t)&SDL_RenderDrawLineF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawLines"), (uintptr_t)&SDL_RenderDrawLines);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawLinesF"), (uintptr_t)&SDL_RenderDrawLinesF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawPoint"), (uintptr_t)&SDL_RenderDrawPoint);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawPointF"), (uintptr_t)&SDL_RenderDrawPointF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawPoints"), (uintptr_t)&SDL_RenderDrawPoints);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawPointsF"), (uintptr_t)&SDL_RenderDrawPointsF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawRect"), (uintptr_t)&SDL_RenderDrawRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawRectF"), (uintptr_t)&SDL_RenderDrawRectF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawRects"), (uintptr_t)&SDL_RenderDrawRects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawRectsF"), (uintptr_t)&SDL_RenderDrawRectsF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFillRect"), (uintptr_t)&SDL_RenderFillRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFillRectF"), (uintptr_t)&SDL_RenderFillRectF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFillRects"), (uintptr_t)&SDL_RenderFillRects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFillRectsF"), (uintptr_t)&SDL_RenderFillRectsF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFlush"), (uintptr_t)&SDL_RenderFlush);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetClipRect"), (uintptr_t)&SDL_RenderGetClipRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetIntegerScale"), (uintptr_t)&SDL_RenderGetIntegerScale);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetLogicalSize"), (uintptr_t)&SDL_RenderGetLogicalSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetMetalCommandEncoder"), (uintptr_t)&SDL_RenderGetMetalCommandEncoder);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetMetalLayer"), (uintptr_t)&SDL_RenderGetMetalLayer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetScale"), (uintptr_t)&SDL_RenderGetScale);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetViewport"), (uintptr_t)&SDL_RenderGetViewport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderIsClipEnabled"), (uintptr_t)&SDL_RenderIsClipEnabled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderPresent"), (uintptr_t)&SDL_RenderPresent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderReadPixels"), (uintptr_t)&SDL_RenderReadPixels);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetClipRect"), (uintptr_t)&SDL_RenderSetClipRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetIntegerScale"), (uintptr_t)&SDL_RenderSetIntegerScale);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetLogicalSize"), (uintptr_t)&SDL_RenderSetLogicalSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetScale"), (uintptr_t)&SDL_RenderSetScale);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetViewport"), (uintptr_t)&SDL_RenderSetViewport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderTargetSupported"), (uintptr_t)&SDL_RenderTargetSupported);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReportAssertion"), (uintptr_t)&SDL_ReportAssertion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ResetAssertionReport"), (uintptr_t)&SDL_ResetAssertionReport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RestoreWindow"), (uintptr_t)&SDL_RestoreWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SIMDAlloc"), (uintptr_t)&SDL_SIMDAlloc);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SIMDFree"), (uintptr_t)&SDL_SIMDFree);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SIMDGetAlignment"), (uintptr_t)&SDL_SIMDGetAlignment);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SaveAllDollarTemplates"), (uintptr_t)&SDL_SaveAllDollarTemplates);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SaveBMP_RW"), (uintptr_t)&SDL_SaveBMP_RW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SaveDollarTemplate"), (uintptr_t)&SDL_SaveDollarTemplate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemPost"), (uintptr_t)&SDL_SemPost);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemTryWait"), (uintptr_t)&SDL_SemTryWait);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemValue"), (uintptr_t)&SDL_SemValue);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemWait"), (uintptr_t)&SDL_SemWait);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemWaitTimeout"), (uintptr_t)&SDL_SemWaitTimeout);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorClose"), (uintptr_t)&SDL_SensorClose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorFromInstanceID"), (uintptr_t)&SDL_SensorFromInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetData"), (uintptr_t)&SDL_SensorGetData);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetDeviceInstanceID"), (uintptr_t)&SDL_SensorGetDeviceInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetDeviceName"), (uintptr_t)&SDL_SensorGetDeviceName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetDeviceNonPortableType"), (uintptr_t)&SDL_SensorGetDeviceNonPortableType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetDeviceType"), (uintptr_t)&SDL_SensorGetDeviceType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetInstanceID"), (uintptr_t)&SDL_SensorGetInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetName"), (uintptr_t)&SDL_SensorGetName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetNonPortableType"), (uintptr_t)&SDL_SensorGetNonPortableType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetType"), (uintptr_t)&SDL_SensorGetType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorOpen"), (uintptr_t)&SDL_SensorOpen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorUpdate"), (uintptr_t)&SDL_SensorUpdate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetAssertionHandler"), (uintptr_t)&SDL_SetAssertionHandler);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetClipRect"), (uintptr_t)&SDL_SetClipRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetClipboardText"), (uintptr_t)&SDL_SetClipboardText);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetColorKey"), (uintptr_t)&SDL_SetColorKey);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetCursor"), (uintptr_t)&SDL_SetCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetError"), (uintptr_t)&SDL_SetError);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetEventFilter"), (uintptr_t)&SDL_SetEventFilter);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetHint"), (uintptr_t)&SDL_SetHint);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetHintWithPriority"), (uintptr_t)&SDL_SetHintWithPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetMainReady"), (uintptr_t)&SDL_SetMainReady);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetMemoryFunctions"), (uintptr_t)&SDL_SetMemoryFunctions);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetModState"), (uintptr_t)&SDL_SetModState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetPaletteColors"), (uintptr_t)&SDL_SetPaletteColors);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetPixelFormatPalette"), (uintptr_t)&SDL_SetPixelFormatPalette);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetRelativeMouseMode"), (uintptr_t)&SDL_SetRelativeMouseMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetRenderDrawBlendMode"), (uintptr_t)&SDL_SetRenderDrawBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetRenderDrawColor"), (uintptr_t)&SDL_SetRenderDrawColor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetRenderTarget"), (uintptr_t)&SDL_SetRenderTarget);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfaceAlphaMod"), (uintptr_t)&SDL_SetSurfaceAlphaMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfaceBlendMode"), (uintptr_t)&SDL_SetSurfaceBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfaceColorMod"), (uintptr_t)&SDL_SetSurfaceColorMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfacePalette"), (uintptr_t)&SDL_SetSurfacePalette);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfaceRLE"), (uintptr_t)&SDL_SetSurfaceRLE);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextInputRect"), (uintptr_t)&SDL_SetTextInputRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextureAlphaMod"), (uintptr_t)&SDL_SetTextureAlphaMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextureBlendMode"), (uintptr_t)&SDL_SetTextureBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextureColorMod"), (uintptr_t)&SDL_SetTextureColorMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextureScaleMode"), (uintptr_t)&SDL_SetTextureScaleMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetThreadPriority"), (uintptr_t)&SDL_SetThreadPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowBordered"), (uintptr_t)&SDL_SetWindowBordered);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowBrightness"), (uintptr_t)&SDL_SetWindowBrightness);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowData"), (uintptr_t)&SDL_SetWindowData);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowDisplayMode"), (uintptr_t)&SDL_SetWindowDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowFullscreen"), (uintptr_t)&SDL_SetWindowFullscreen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowGammaRamp"), (uintptr_t)&SDL_SetWindowGammaRamp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowGrab"), (uintptr_t)&SDL_SetWindowGrab);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowHitTest"), (uintptr_t)&SDL_SetWindowHitTest);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowIcon"), (uintptr_t)&SDL_SetWindowIcon);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowInputFocus"), (uintptr_t)&SDL_SetWindowInputFocus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowMaximumSize"), (uintptr_t)&SDL_SetWindowMaximumSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowMinimumSize"), (uintptr_t)&SDL_SetWindowMinimumSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowModalFor"), (uintptr_t)&SDL_SetWindowModalFor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowOpacity"), (uintptr_t)&SDL_SetWindowOpacity);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowPosition"), (uintptr_t)&SDL_SetWindowPosition);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowResizable"), (uintptr_t)&SDL_SetWindowResizable);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowShape"), (uintptr_t)&SDL_SetWindowShape);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowSize"), (uintptr_t)&SDL_SetWindowSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowTitle"), (uintptr_t)&SDL_SetWindowTitle);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetYUVConversionMode"), (uintptr_t)&SDL_SetYUVConversionMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ShowCursor"), (uintptr_t)&SDL_ShowCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ShowMessageBox"), (uintptr_t)&SDL_ShowMessageBox);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ShowSimpleMessageBox"), (uintptr_t)&SDL_ShowSimpleMessageBox);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ShowWindow"), (uintptr_t)&SDL_ShowWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SoftStretch"), (uintptr_t)&SDL_SoftStretch);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_StartTextInput"), (uintptr_t)&SDL_StartTextInput);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_StopTextInput"), (uintptr_t)&SDL_StopTextInput);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_TLSCreate"), (uintptr_t)&SDL_TLSCreate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_TLSGet"), (uintptr_t)&SDL_TLSGet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_TLSSet"), (uintptr_t)&SDL_TLSSet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ThreadID"), (uintptr_t)&SDL_ThreadID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_TryLockMutex"), (uintptr_t)&SDL_TryLockMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnionRect"), (uintptr_t)&SDL_UnionRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnloadObject"), (uintptr_t)&SDL_UnloadObject);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockAudio"), (uintptr_t)&SDL_UnlockAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockAudioDevice"), (uintptr_t)&SDL_UnlockAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockJoysticks"), (uintptr_t)&SDL_UnlockJoysticks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockMutex"), (uintptr_t)&SDL_UnlockMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockSurface"), (uintptr_t)&SDL_UnlockSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockTexture"), (uintptr_t)&SDL_UnlockTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpdateWindowSurface"), (uintptr_t)&SDL_UpdateWindowSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpdateWindowSurfaceRects"), (uintptr_t)&SDL_UpdateWindowSurfaceRects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpdateYUVTexture"), (uintptr_t)&SDL_UpdateYUVTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpperBlit"), (uintptr_t)&SDL_UpperBlit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpperBlitScaled"), (uintptr_t)&SDL_UpperBlitScaled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_VideoInit"), (uintptr_t)&SDL_VideoInit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_VideoQuit"), (uintptr_t)&SDL_VideoQuit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_CreateSurface"), (uintptr_t)&SDL_Vulkan_CreateSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_GetDrawableSize"), (uintptr_t)&SDL_Vulkan_GetDrawableSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_GetInstanceExtensions"), (uintptr_t)&SDL_Vulkan_GetInstanceExtensions);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_GetVkGetInstanceProcAddr"), (uintptr_t)&SDL_Vulkan_GetVkGetInstanceProcAddr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_LoadLibrary"), (uintptr_t)&SDL_Vulkan_LoadLibrary);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_UnloadLibrary"), (uintptr_t)&SDL_Vulkan_UnloadLibrary);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WaitEvent"), (uintptr_t)&SDL_WaitEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WaitEventTimeout"), (uintptr_t)&SDL_WaitEventTimeout);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WaitThread"), (uintptr_t)&SDL_WaitThread);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WarpMouseGlobal"), (uintptr_t)&SDL_WarpMouseGlobal);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WarpMouseInWindow"), (uintptr_t)&SDL_WarpMouseInWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WasInit"), (uintptr_t)&SDL_WasInit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteBE16"), (uintptr_t)&SDL_WriteBE16);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteBE32"), (uintptr_t)&SDL_WriteBE32);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteBE64"), (uintptr_t)&SDL_WriteBE64);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteLE16"), (uintptr_t)&SDL_WriteLE16);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteLE32"), (uintptr_t)&SDL_WriteLE32);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteLE64"), (uintptr_t)&SDL_WriteLE64);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteU8"), (uintptr_t)&SDL_WriteU8);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_abs"), (uintptr_t)&SDL_abs);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_acos"), (uintptr_t)&SDL_acos);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_acosf"), (uintptr_t)&SDL_acosf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_asin"), (uintptr_t)&SDL_asin);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_asinf"), (uintptr_t)&SDL_asinf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atan"), (uintptr_t)&SDL_atan);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atan2"), (uintptr_t)&SDL_atan2);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atan2f"), (uintptr_t)&SDL_atan2f);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atanf"), (uintptr_t)&SDL_atanf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atof"), (uintptr_t)&SDL_atof);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atoi"), (uintptr_t)&SDL_atoi);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_calloc"), (uintptr_t)&SDL_calloc);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ceil"), (uintptr_t)&SDL_ceil);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ceilf"), (uintptr_t)&SDL_ceilf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_copysign"), (uintptr_t)&SDL_copysign);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_copysignf"), (uintptr_t)&SDL_copysignf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_cos"), (uintptr_t)&SDL_cos);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_cosf"), (uintptr_t)&SDL_cosf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_exp"), (uintptr_t)&SDL_exp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_expf"), (uintptr_t)&SDL_expf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_fabs"), (uintptr_t)&SDL_fabs);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_fabsf"), (uintptr_t)&SDL_fabsf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_floor"), (uintptr_t)&SDL_floor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_floorf"), (uintptr_t)&SDL_floorf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_fmod"), (uintptr_t)&SDL_fmod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_fmodf"), (uintptr_t)&SDL_fmodf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_free"), (uintptr_t)&SDL_free);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_getenv"), (uintptr_t)&SDL_getenv);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_iconv"), (uintptr_t)&SDL_iconv);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_iconv_close"), (uintptr_t)&SDL_iconv_close);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_iconv_open"), (uintptr_t)&SDL_iconv_open);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_iconv_string"), (uintptr_t)&SDL_iconv_string);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_isdigit"), (uintptr_t)&SDL_isdigit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_islower"), (uintptr_t)&SDL_islower);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_isspace"), (uintptr_t)&SDL_isspace);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_isupper"), (uintptr_t)&SDL_isupper);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_itoa"), (uintptr_t)&SDL_itoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_lltoa"), (uintptr_t)&SDL_lltoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_log"), (uintptr_t)&SDL_log);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_log10"), (uintptr_t)&SDL_log10);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_log10f"), (uintptr_t)&SDL_log10f);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_logf"), (uintptr_t)&SDL_logf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ltoa"), (uintptr_t)&SDL_ltoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ltoa"), (uintptr_t)&SDL_ltoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_malloc"), (uintptr_t)&SDL_malloc);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_memcmp"), (uintptr_t)&SDL_memcmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_memcpy"), (uintptr_t)&SDL_memcpy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_memmove"), (uintptr_t)&SDL_memmove);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_memset"), (uintptr_t)&SDL_memset);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_pow"), (uintptr_t)&SDL_pow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_powf"), (uintptr_t)&SDL_powf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_qsort"), (uintptr_t)&SDL_qsort);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_realloc"), (uintptr_t)&SDL_realloc);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_scalbn"), (uintptr_t)&SDL_scalbn);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_scalbnf"), (uintptr_t)&SDL_scalbnf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_setenv"), (uintptr_t)&SDL_setenv);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sin"), (uintptr_t)&SDL_sin);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sinf"), (uintptr_t)&SDL_sinf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_snprintf"), (uintptr_t)&SDL_snprintf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sqrt"), (uintptr_t)&SDL_sqrt);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sqrtf"), (uintptr_t)&SDL_sqrtf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sscanf"), (uintptr_t)&SDL_sscanf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strcasecmp"), (uintptr_t)&SDL_strcasecmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strchr"), (uintptr_t)&SDL_strchr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strcmp"), (uintptr_t)&SDL_strcmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strdup"), (uintptr_t)&SDL_strdup);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strlcat"), (uintptr_t)&SDL_strlcat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strlcpy"), (uintptr_t)&SDL_strlcpy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strlen"), (uintptr_t)&SDL_strlen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strlwr"), (uintptr_t)&SDL_strlwr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strncasecmp"), (uintptr_t)&SDL_strncasecmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strncmp"), (uintptr_t)&SDL_strncmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strrchr"), (uintptr_t)&SDL_strrchr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strrev"), (uintptr_t)&SDL_strrev);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strstr"), (uintptr_t)&SDL_strstr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtod"), (uintptr_t)&SDL_strtod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtokr"), (uintptr_t)&SDL_strtokr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtol"), (uintptr_t)&SDL_strtol);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtoll"), (uintptr_t)&SDL_strtoll);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtoul"), (uintptr_t)&SDL_strtoul);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtoull"), (uintptr_t)&SDL_strtoull);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strupr"), (uintptr_t)&SDL_strupr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_tan"), (uintptr_t)&SDL_tan);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_tanf"), (uintptr_t)&SDL_tanf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_tolower"), (uintptr_t)&SDL_tolower);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_toupper"), (uintptr_t)&SDL_toupper);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_uitoa"), (uintptr_t)&SDL_uitoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ulltoa"), (uintptr_t)&SDL_ulltoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ultoa"), (uintptr_t)&SDL_ultoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_utf8strlcpy"), (uintptr_t)&SDL_utf8strlcpy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_utf8strlen"), (uintptr_t)&SDL_utf8strlen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_vsnprintf"), (uintptr_t)&SDL_vsnprintf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_vsnprintf"), (uintptr_t)&SDL_vsnprintf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_vsscanf"), (uintptr_t)&SDL_vsscanf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcscmp"), (uintptr_t)&SDL_wcscmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcsdup"), (uintptr_t)&SDL_wcsdup);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcslcat"), (uintptr_t)&SDL_wcslcat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcslcpy"), (uintptr_t)&SDL_wcslcpy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcslen"), (uintptr_t)&SDL_wcslen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcsncmp"), (uintptr_t)&SDL_wcsncmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcsstr"), (uintptr_t)&SDL_wcsstr);

	// FO'SHIZZLE
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AddEventWatch_REAL"), (uintptr_t)&SDL_AddEventWatch);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AddHintCallback_REAL"), (uintptr_t)&SDL_AddHintCallback);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AddTimer_REAL"), (uintptr_t)&SDL_AddTimer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AllocFormat_REAL"), (uintptr_t)&SDL_AllocFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AllocPalette_REAL"), (uintptr_t)&SDL_AllocPalette);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AllocRW_REAL"), (uintptr_t)&SDL_AllocRW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidBackButton_REAL"), (uintptr_t)&SDL_AndroidBackButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetActivity_REAL"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetExternalStoragePath_REAL"), (uintptr_t)&SDL_AndroidGetExternalStoragePath);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetExternalStorageState_REAL"), (uintptr_t)&ret0);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetInternalStoragePath_REAL"), (uintptr_t)&SDL_AndroidGetInternalStoragePath);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AndroidGetJNIEnv_REAL"), (uintptr_t)&Android_JNI_GetEnv);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicAdd_REAL"), (uintptr_t)&SDL_AtomicAdd);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicCASPtr_REAL"), (uintptr_t)&SDL_AtomicCASPtr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicCAS_REAL"), (uintptr_t)&SDL_AtomicCAS);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicGet_REAL"), (uintptr_t)&SDL_AtomicGet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicGetPtr_REAL"), (uintptr_t)&SDL_AtomicGetPtr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicLock_REAL"), (uintptr_t)&SDL_AtomicLock);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicSet_REAL"), (uintptr_t)&SDL_AtomicSet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicSetPtr_REAL"), (uintptr_t)&SDL_AtomicSetPtr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicTryLock_REAL"), (uintptr_t)&SDL_AtomicTryLock);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AtomicUnlock_REAL"), (uintptr_t)&SDL_AtomicUnlock);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioInit_REAL"), (uintptr_t)&SDL_AudioInit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioQuit_REAL"), (uintptr_t)&SDL_AudioQuit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamAvailable_REAL"), (uintptr_t)&SDL_AudioStreamAvailable);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamClear_REAL"), (uintptr_t)&SDL_AudioStreamClear);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamFlush_REAL"), (uintptr_t)&SDL_AudioStreamFlush);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamGet_REAL"), (uintptr_t)&SDL_AudioStreamGet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_AudioStreamPut_REAL"), (uintptr_t)&SDL_AudioStreamPut);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_BuildAudioCVT_REAL"), (uintptr_t)&SDL_BuildAudioCVT);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CalculateGammaRamp_REAL"), (uintptr_t)&SDL_CalculateGammaRamp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CaptureMouse_REAL"), (uintptr_t)&SDL_CaptureMouse);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ClearError_REAL"), (uintptr_t)&SDL_ClearError);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ClearHints_REAL"), (uintptr_t)&SDL_ClearHints);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ClearQueuedAudio_REAL"), (uintptr_t)&SDL_ClearQueuedAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CloseAudioDevice_REAL"), (uintptr_t)&SDL_CloseAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CloseAudio_REAL"), (uintptr_t)&SDL_CloseAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ComposeCustomBlendMode_REAL"), (uintptr_t)&SDL_ComposeCustomBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CondBroadcast_REAL"), (uintptr_t)&SDL_CondBroadcast);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CondSignal_REAL"), (uintptr_t)&SDL_CondSignal);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CondWaitTimeout_REAL"), (uintptr_t)&SDL_CondWaitTimeout);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CondWait_REAL"), (uintptr_t)&SDL_CondWait);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ConvertAudio_REAL"), (uintptr_t)&SDL_ConvertAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ConvertPixels_REAL"), (uintptr_t)&SDL_ConvertPixels);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ConvertSurfaceFormat_REAL"), (uintptr_t)&SDL_ConvertSurfaceFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ConvertSurface_REAL"), (uintptr_t)&SDL_ConvertSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateColorCursor_REAL"), (uintptr_t)&SDL_CreateColorCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateCond_REAL"), (uintptr_t)&SDL_CreateCond);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateCursor_REAL"), (uintptr_t)&SDL_CreateCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateMutex_REAL"), (uintptr_t)&SDL_CreateMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRGBSurfaceFrom_REAL"), (uintptr_t)&SDL_CreateRGBSurfaceFrom);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRGBSurfaceWithFormatFrom_REAL"), (uintptr_t)&SDL_CreateRGBSurfaceWithFormatFrom);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRGBSurfaceWithFormat_REAL"), (uintptr_t)&SDL_CreateRGBSurfaceWithFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRGBSurface_REAL"), (uintptr_t)&SDL_CreateRGBSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateRenderer_REAL"), (uintptr_t)&SDL_CreateRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateSemaphore_REAL"), (uintptr_t)&SDL_CreateSemaphore);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateShapedWindow_REAL"), (uintptr_t)&SDL_CreateShapedWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateSoftwareRenderer_REAL"), (uintptr_t)&SDL_CreateSoftwareRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateSystemCursor_REAL"), (uintptr_t)&SDL_CreateSystemCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateTextureFromSurface_REAL"), (uintptr_t)&SDL_CreateTextureFromSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateTexture_REAL"), (uintptr_t)&SDL_CreateTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateThreadWithStackSize_REAL"), (uintptr_t)&SDL_CreateThreadWithStackSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateThread_REAL"), (uintptr_t)&SDL_CreateThread);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateWindowAndRenderer_REAL"), (uintptr_t)&SDL_CreateWindowAndRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateWindowFrom_REAL"), (uintptr_t)&SDL_CreateWindowFrom);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_CreateWindow_REAL"), (uintptr_t)&SDL_CreateWindow);
	// hook_addr(so_symbol(&diceydungeons_mod, "SDL_DYNAPI_entry_REAL"), (uintptr_t)&SDL_DYNAPI_entry);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DelEventWatch_REAL"), (uintptr_t)&SDL_DelEventWatch);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DelHintCallback_REAL"), (uintptr_t)&SDL_DelHintCallback);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Delay_REAL"), (uintptr_t)&SDL_Delay);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DequeueAudio_REAL"), (uintptr_t)&SDL_DequeueAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyCond_REAL"), (uintptr_t)&SDL_DestroyCond);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyMutex_REAL"), (uintptr_t)&SDL_DestroyMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyRenderer_REAL"), (uintptr_t)&SDL_DestroyRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroySemaphore_REAL"), (uintptr_t)&SDL_DestroySemaphore);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyTexture_REAL"), (uintptr_t)&SDL_DestroyTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DestroyWindow_REAL"), (uintptr_t)&SDL_DestroyWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DetachThread_REAL"), (uintptr_t)&SDL_DetachThread);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DisableScreenSaver_REAL"), (uintptr_t)&SDL_DisableScreenSaver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_DuplicateSurface_REAL"), (uintptr_t)&SDL_DuplicateSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_EnableScreenSaver_REAL"), (uintptr_t)&SDL_EnableScreenSaver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_EnclosePoints_REAL"), (uintptr_t)&SDL_EnclosePoints);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Error_REAL"), (uintptr_t)&SDL_Error);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_EventState_REAL"), (uintptr_t)&SDL_EventState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FillRect_REAL"), (uintptr_t)&SDL_FillRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FillRects_REAL"), (uintptr_t)&SDL_FillRects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FilterEvents_REAL"), (uintptr_t)&SDL_FilterEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FlushEvent_REAL"), (uintptr_t)&SDL_FlushEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FlushEvents_REAL"), (uintptr_t)&SDL_FlushEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeAudioStream_REAL"), (uintptr_t)&SDL_FreeAudioStream);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeCursor_REAL"), (uintptr_t)&SDL_FreeCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeFormat_REAL"), (uintptr_t)&SDL_FreeFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreePalette_REAL"), (uintptr_t)&SDL_FreePalette);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeRW_REAL"), (uintptr_t)&SDL_FreeRW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeSurface_REAL"), (uintptr_t)&SDL_FreeSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_FreeWAV_REAL"), (uintptr_t)&SDL_FreeWAV);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_BindTexture_REAL"), (uintptr_t)&SDL_GL_BindTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_CreateContext_REAL"), (uintptr_t)&SDL_GL_CreateContext);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_DeleteContext_REAL"), (uintptr_t)&SDL_GL_DeleteContext);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_ExtensionSupported_REAL"), (uintptr_t)&SDL_GL_ExtensionSupported);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetAttribute_REAL"), (uintptr_t)&SDL_GL_GetAttribute);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetCurrentContext_REAL"), (uintptr_t)&SDL_GL_GetCurrentContext);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetCurrentWindow_REAL"), (uintptr_t)&SDL_GL_GetCurrentWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetDrawableSize_REAL"), (uintptr_t)&SDL_GL_GetDrawableSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetProcAddress_REAL"), (uintptr_t)&SDL_GL_GetProcAddress);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_GetSwapInterval_REAL"), (uintptr_t)&SDL_GL_GetSwapInterval);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_LoadLibrary_REAL"), (uintptr_t)&SDL_GL_LoadLibrary);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_MakeCurrent_REAL"), (uintptr_t)&SDL_GL_MakeCurrent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_ResetAttributes_REAL"), (uintptr_t)&SDL_GL_ResetAttributes);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_SetAttribute_REAL"), (uintptr_t)&SDL_GL_SetAttribute);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_SetSwapInterval_REAL"), (uintptr_t)&SDL_GL_SetSwapInterval);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_SwapWindow_REAL"), (uintptr_t)&SDL_GL_SwapWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_UnbindTexture_REAL"), (uintptr_t)&SDL_GL_UnbindTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GL_UnloadLibrary_REAL"), (uintptr_t)&SDL_GL_UnloadLibrary);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerAddMapping_REAL"), (uintptr_t)&SDL_GameControllerAddMapping);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerAddMappingsFromRW_REAL"), (uintptr_t)&SDL_GameControllerAddMappingsFromRW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerClose_REAL"), (uintptr_t)&SDL_GameControllerClose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerEventState_REAL"), (uintptr_t)&SDL_GameControllerEventState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerFromInstanceID_REAL"), (uintptr_t)&SDL_GameControllerFromInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerFromPlayerIndex_REAL"), (uintptr_t)&SDL_GameControllerFromPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetAttached_REAL"), (uintptr_t)&SDL_GameControllerGetAttached);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetAxisFromString_REAL"), (uintptr_t)&SDL_GameControllerGetAxisFromString);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetAxis_REAL"), (uintptr_t)&SDL_GameControllerGetAxis);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetBindForAxis_REAL"), (uintptr_t)&SDL_GameControllerGetBindForAxis);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetBindForButton_REAL"), (uintptr_t)&SDL_GameControllerGetBindForButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetButtonFromString_REAL"), (uintptr_t)&SDL_GameControllerGetButtonFromString);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetButton_REAL"), (uintptr_t)&SDL_GameControllerGetButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetJoystick_REAL"), (uintptr_t)&SDL_GameControllerGetJoystick);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetPlayerIndex_REAL"), (uintptr_t)&SDL_GameControllerGetPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetProductVersion_REAL"), (uintptr_t)&SDL_GameControllerGetProductVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetProduct_REAL"), (uintptr_t)&SDL_GameControllerGetProduct);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetStringForAxis_REAL"), (uintptr_t)&SDL_GameControllerGetStringForAxis);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetStringForButton_REAL"), (uintptr_t)&SDL_GameControllerGetStringForButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetType_REAL"), (uintptr_t)&SDL_GameControllerGetType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerGetVendor_REAL"), (uintptr_t)&SDL_GameControllerGetVendor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerMappingForDeviceIndex_REAL"), (uintptr_t)&SDL_GameControllerMappingForDeviceIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerMappingForGUID_REAL"), (uintptr_t)&SDL_GameControllerMappingForGUID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerMappingForIndex_REAL"), (uintptr_t)&SDL_GameControllerMappingForIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerMapping_REAL"), (uintptr_t)&SDL_GameControllerMapping);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerNameForIndex_REAL"), (uintptr_t)&SDL_GameControllerNameForIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerName_REAL"), (uintptr_t)&SDL_GameControllerName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerNumMappings_REAL"), (uintptr_t)&SDL_GameControllerNumMappings);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerOpen_REAL"), (uintptr_t)&SDL_GameControllerOpen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerRumble_REAL"), (uintptr_t)&SDL_GameControllerRumble);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerSetPlayerIndex_REAL"), (uintptr_t)&SDL_GameControllerSetPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerTypeForIndex_REAL"), (uintptr_t)&SDL_GameControllerTypeForIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GameControllerUpdate_REAL"), (uintptr_t)&SDL_GameControllerUpdate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAndroidSDKVersion_REAL"), (uintptr_t)&SDL_GetAndroidSDKVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAssertionHandler_REAL"), (uintptr_t)&SDL_GetAssertionHandler);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAssertionReport_REAL"), (uintptr_t)&SDL_GetAssertionReport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAudioDeviceName_REAL"), (uintptr_t)&SDL_GetAudioDeviceName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAudioDeviceStatus_REAL"), (uintptr_t)&SDL_GetAudioDeviceStatus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAudioDriver_REAL"), (uintptr_t)&SDL_GetAudioDriver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetAudioStatus_REAL"), (uintptr_t)&SDL_GetAudioStatus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetBasePath_REAL"), (uintptr_t)&SDL_GetBasePath);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCPUCacheLineSize_REAL"), (uintptr_t)&SDL_GetCPUCacheLineSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCPUCount_REAL"), (uintptr_t)&SDL_GetCPUCount);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetClipRect_REAL"), (uintptr_t)&SDL_GetClipRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetClipboardText_REAL"), (uintptr_t)&SDL_GetClipboardText);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetClosestDisplayMode_REAL"), (uintptr_t)&SDL_GetClosestDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetColorKey_REAL"), (uintptr_t)&SDL_GetColorKey);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCurrentAudioDriver_REAL"), (uintptr_t)&SDL_GetCurrentAudioDriver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCurrentDisplayMode_REAL"), (uintptr_t)&SDL_GetCurrentDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCurrentVideoDriver_REAL"), (uintptr_t)&SDL_GetCurrentVideoDriver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetCursor_REAL"), (uintptr_t)&SDL_GetCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDefaultAssertionHandler_REAL"), (uintptr_t)&SDL_GetDefaultAssertionHandler);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDefaultCursor_REAL"), (uintptr_t)&SDL_GetDefaultCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDesktopDisplayMode_REAL"), (uintptr_t)&SDL_GetDesktopDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayBounds_REAL"), (uintptr_t)&SDL_GetDisplayBounds);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayDPI_REAL"), (uintptr_t)&SDL_GetDisplayDPI);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayMode_REAL"), (uintptr_t)&SDL_GetDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayName_REAL"), (uintptr_t)&SDL_GetDisplayName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayOrientation_REAL"), (uintptr_t)&SDL_GetDisplayOrientation);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetDisplayUsableBounds_REAL"), (uintptr_t)&SDL_GetDisplayUsableBounds);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetError_REAL"), (uintptr_t)&SDL_GetError);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetEventFilter_REAL"), (uintptr_t)&SDL_GetEventFilter);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetGlobalMouseState_REAL"), (uintptr_t)&SDL_GetGlobalMouseState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetGrabbedWindow_REAL"), (uintptr_t)&SDL_GetGrabbedWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetHintBoolean_REAL"), (uintptr_t)&SDL_GetHintBoolean);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetHint_REAL"), (uintptr_t)&SDL_GetHint);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyFromName_REAL"), (uintptr_t)&SDL_GetKeyFromName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyFromScancode_REAL"), (uintptr_t)&SDL_GetKeyFromScancode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyName_REAL"), (uintptr_t)&SDL_GetKeyName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyboardFocus_REAL"), (uintptr_t)&SDL_GetKeyboardFocus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetKeyboardState_REAL"), (uintptr_t)&SDL_GetKeyboardState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetMemoryFunctions_REAL"), (uintptr_t)&SDL_GetMemoryFunctions);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetModState_REAL"), (uintptr_t)&SDL_GetModState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetMouseFocus_REAL"), (uintptr_t)&SDL_GetMouseFocus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetMouseState_REAL"), (uintptr_t)&SDL_GetMouseState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumAllocations_REAL"), (uintptr_t)&SDL_GetNumAllocations);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumAudioDevices_REAL"), (uintptr_t)&SDL_GetNumAudioDevices);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumAudioDrivers_REAL"), (uintptr_t)&SDL_GetNumAudioDrivers);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumDisplayModes_REAL"), (uintptr_t)&SDL_GetNumDisplayModes);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumRenderDrivers_REAL"), (uintptr_t)&SDL_GetNumRenderDrivers);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumTouchDevices_REAL"), (uintptr_t)&SDL_GetNumTouchDevices);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumTouchFingers_REAL"), (uintptr_t)&SDL_GetNumTouchFingers);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumVideoDisplays_REAL"), (uintptr_t)&SDL_GetNumVideoDisplays);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetNumVideoDrivers_REAL"), (uintptr_t)&SDL_GetNumVideoDrivers);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPerformanceCounter_REAL"), (uintptr_t)&SDL_GetPerformanceCounter);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPerformanceFrequency_REAL"), (uintptr_t)&SDL_GetPerformanceFrequency);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPixelFormatName_REAL"), (uintptr_t)&SDL_GetPixelFormatName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPlatform_REAL"), (uintptr_t)&SDL_GetPlatform);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPowerInfo_REAL"), (uintptr_t)&SDL_GetPowerInfo);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetPrefPath_REAL"), (uintptr_t)&SDL_GetPrefPath_hook);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetQueuedAudioSize_REAL"), (uintptr_t)&SDL_GetQueuedAudioSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRGBA_REAL"), (uintptr_t)&SDL_GetRGBA);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRGB_REAL"), (uintptr_t)&SDL_GetRGB);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRelativeMouseMode_REAL"), (uintptr_t)&SDL_GetRelativeMouseMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRelativeMouseState_REAL"), (uintptr_t)&SDL_GetRelativeMouseState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderDrawBlendMode_REAL"), (uintptr_t)&SDL_GetRenderDrawBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderDrawColor_REAL"), (uintptr_t)&SDL_GetRenderDrawColor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderDriverInfo_REAL"), (uintptr_t)&SDL_GetRenderDriverInfo);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderTarget_REAL"), (uintptr_t)&SDL_GetRenderTarget);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRendererInfo_REAL"), (uintptr_t)&SDL_GetRendererInfo);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRendererOutputSize_REAL"), (uintptr_t)&SDL_GetRendererOutputSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRenderer_REAL"), (uintptr_t)&SDL_GetRenderer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetRevision_REAL"), (uintptr_t)&SDL_GetRevision);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetScancodeFromKey_REAL"), (uintptr_t)&SDL_GetScancodeFromKey);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetScancodeFromName_REAL"), (uintptr_t)&SDL_GetScancodeFromName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetScancodeName_REAL"), (uintptr_t)&SDL_GetScancodeName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetShapedWindowMode_REAL"), (uintptr_t)&SDL_GetShapedWindowMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetSurfaceAlphaMod_REAL"), (uintptr_t)&SDL_GetSurfaceAlphaMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetSurfaceBlendMode_REAL"), (uintptr_t)&SDL_GetSurfaceBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetSurfaceColorMod_REAL"), (uintptr_t)&SDL_GetSurfaceColorMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetSystemRAM_REAL"), (uintptr_t)&SDL_GetSystemRAM);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTextureAlphaMod_REAL"), (uintptr_t)&SDL_GetTextureAlphaMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTextureBlendMode_REAL"), (uintptr_t)&SDL_GetTextureBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTextureColorMod_REAL"), (uintptr_t)&SDL_GetTextureColorMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTextureScaleMode_REAL"), (uintptr_t)&SDL_GetTextureScaleMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetThreadID_REAL"), (uintptr_t)&SDL_GetThreadID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetThreadName_REAL"), (uintptr_t)&SDL_GetThreadName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTicks_REAL"), (uintptr_t)&SDL_GetTicks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTouchDeviceType_REAL"), (uintptr_t)&SDL_GetTouchDeviceType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTouchDevice_REAL"), (uintptr_t)&SDL_GetTouchDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetTouchFinger_REAL"), (uintptr_t)&SDL_GetTouchFinger);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetVersion_REAL"), (uintptr_t)&SDL_GetVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetVideoDriver_REAL"), (uintptr_t)&SDL_GetVideoDriver);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowBordersSize_REAL"), (uintptr_t)&SDL_GetWindowBordersSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowBrightness_REAL"), (uintptr_t)&SDL_GetWindowBrightness);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowData_REAL"), (uintptr_t)&SDL_GetWindowData);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowDisplayIndex_REAL"), (uintptr_t)&SDL_GetWindowDisplayIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowDisplayMode_REAL"), (uintptr_t)&SDL_GetWindowDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowFlags_REAL"), (uintptr_t)&SDL_GetWindowFlags);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowFromID_REAL"), (uintptr_t)&SDL_GetWindowFromID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowGammaRamp_REAL"), (uintptr_t)&SDL_GetWindowGammaRamp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowGrab_REAL"), (uintptr_t)&SDL_GetWindowGrab);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowID_REAL"), (uintptr_t)&SDL_GetWindowID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowMaximumSize_REAL"), (uintptr_t)&SDL_GetWindowMaximumSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowMinimumSize_REAL"), (uintptr_t)&SDL_GetWindowMinimumSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowOpacity_REAL"), (uintptr_t)&SDL_GetWindowOpacity);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowPixelFormat_REAL"), (uintptr_t)&SDL_GetWindowPixelFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowPosition_REAL"), (uintptr_t)&SDL_GetWindowPosition);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowSize_REAL"), (uintptr_t)&SDL_GetWindowSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowSurface_REAL"), (uintptr_t)&SDL_GetWindowSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetWindowTitle_REAL"), (uintptr_t)&SDL_GetWindowTitle);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetYUVConversionModeForResolution_REAL"), (uintptr_t)&SDL_GetYUVConversionModeForResolution);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_GetYUVConversionMode_REAL"), (uintptr_t)&SDL_GetYUVConversionMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticClose_REAL"), (uintptr_t)&SDL_HapticClose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticDestroyEffect_REAL"), (uintptr_t)&SDL_HapticDestroyEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticEffectSupported_REAL"), (uintptr_t)&SDL_HapticEffectSupported);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticGetEffectStatus_REAL"), (uintptr_t)&SDL_HapticGetEffectStatus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticIndex_REAL"), (uintptr_t)&SDL_HapticIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticName_REAL"), (uintptr_t)&SDL_HapticName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticNewEffect_REAL"), (uintptr_t)&SDL_HapticNewEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticNumAxes_REAL"), (uintptr_t)&SDL_HapticNumAxes);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticNumEffectsPlaying_REAL"), (uintptr_t)&SDL_HapticNumEffectsPlaying);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticNumEffects_REAL"), (uintptr_t)&SDL_HapticNumEffects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticOpenFromJoystick_REAL"), (uintptr_t)&SDL_HapticOpenFromJoystick);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticOpenFromMouse_REAL"), (uintptr_t)&SDL_HapticOpenFromMouse);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticOpen_REAL"), (uintptr_t)&SDL_HapticOpen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticOpened_REAL"), (uintptr_t)&SDL_HapticOpened);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticPause_REAL"), (uintptr_t)&SDL_HapticPause);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticQuery_REAL"), (uintptr_t)&SDL_HapticQuery);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRumbleInit_REAL"), (uintptr_t)&SDL_HapticRumbleInit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRumblePlay_REAL"), (uintptr_t)&SDL_HapticRumblePlay);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRumbleStop_REAL"), (uintptr_t)&SDL_HapticRumbleStop);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRumbleSupported_REAL"), (uintptr_t)&SDL_HapticRumbleSupported);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticRunEffect_REAL"), (uintptr_t)&SDL_HapticRunEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticSetAutocenter_REAL"), (uintptr_t)&SDL_HapticSetAutocenter);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticSetGain_REAL"), (uintptr_t)&SDL_HapticSetGain);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticStopAll_REAL"), (uintptr_t)&SDL_HapticStopAll);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticStopEffect_REAL"), (uintptr_t)&SDL_HapticStopEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticUnpause_REAL"), (uintptr_t)&SDL_HapticUnpause);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HapticUpdateEffect_REAL"), (uintptr_t)&SDL_HapticUpdateEffect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Has3DNow_REAL"), (uintptr_t)&SDL_Has3DNow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasARMSIMD_REAL"), (uintptr_t)&SDL_HasARMSIMD);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasAVX2_REAL"), (uintptr_t)&SDL_HasAVX2);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasAVX512F_REAL"), (uintptr_t)&SDL_HasAVX512F);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasAVX_REAL"), (uintptr_t)&SDL_HasAVX);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasAltiVec_REAL"), (uintptr_t)&SDL_HasAltiVec);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasClipboardText_REAL"), (uintptr_t)&SDL_HasClipboardText);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasColorKey_REAL"), (uintptr_t)&SDL_HasColorKey);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasEvent_REAL"), (uintptr_t)&SDL_HasEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasEvents_REAL"), (uintptr_t)&SDL_HasEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasIntersection_REAL"), (uintptr_t)&SDL_HasIntersection);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasMMX_REAL"), (uintptr_t)&SDL_HasMMX);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasNEON_REAL"), (uintptr_t)&SDL_HasNEON);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasRDTSC_REAL"), (uintptr_t)&SDL_HasRDTSC);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE2_REAL"), (uintptr_t)&SDL_HasSSE2);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE3_REAL"), (uintptr_t)&SDL_HasSSE3);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE41_REAL"), (uintptr_t)&SDL_HasSSE41);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE42_REAL"), (uintptr_t)&SDL_HasSSE42);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasSSE_REAL"), (uintptr_t)&SDL_HasSSE);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HasScreenKeyboardSupport_REAL"), (uintptr_t)&SDL_HasScreenKeyboardSupport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_HideWindow_REAL"), (uintptr_t)&SDL_HideWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_InitSubSystem_REAL"), (uintptr_t)&SDL_InitSubSystem);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Init_REAL"), (uintptr_t)&SDL_Init);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IntersectRectAndLine_REAL"), (uintptr_t)&SDL_IntersectRectAndLine);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IntersectRect_REAL"), (uintptr_t)&SDL_IntersectRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsGameController_REAL"), (uintptr_t)&SDL_IsGameController);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsScreenKeyboardShown_REAL"), (uintptr_t)&SDL_IsScreenKeyboardShown);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsScreenSaverEnabled_REAL"), (uintptr_t)&SDL_IsScreenSaverEnabled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsShapedWindow_REAL"), (uintptr_t)&SDL_IsShapedWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsAndroidTV_REAL"), (uintptr_t)&SDL_IsAndroidTV);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsChromebook_REAL"), (uintptr_t)&SDL_IsChromebook);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsDeXMode_REAL"), (uintptr_t)&SDL_IsDeXMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsGameController_REAL"), (uintptr_t)&SDL_IsGameController);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsScreenKeyboardShown_REAL"), (uintptr_t)&SDL_IsScreenKeyboardShown);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsScreenSaverEnabled_REAL"), (uintptr_t)&SDL_IsScreenSaverEnabled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsTablet_REAL"), (uintptr_t)&SDL_IsTablet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_IsTextInputActive_REAL"), (uintptr_t)&SDL_IsTextInputActive);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickClose_REAL"), (uintptr_t)&SDL_JoystickClose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickCurrentPowerLevel_REAL"), (uintptr_t)&SDL_JoystickCurrentPowerLevel);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickEventState_REAL"), (uintptr_t)&SDL_JoystickEventState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickFromInstanceID_REAL"), (uintptr_t)&SDL_JoystickFromInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickFromPlayerIndex_REAL"), (uintptr_t)&SDL_JoystickFromPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetAttached_REAL"), (uintptr_t)&SDL_JoystickGetAttached);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetAxisInitialState_REAL"), (uintptr_t)&SDL_JoystickGetAxisInitialState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetAxis_REAL"), (uintptr_t)&SDL_JoystickGetAxis);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetBall_REAL"), (uintptr_t)&SDL_JoystickGetBall);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetButton_REAL"), (uintptr_t)&SDL_JoystickGetButton);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceGUID_REAL"), (uintptr_t)&SDL_JoystickGetDeviceGUID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceInstanceID_REAL"), (uintptr_t)&SDL_JoystickGetDeviceInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDevicePlayerIndex_REAL"), (uintptr_t)&SDL_JoystickGetDevicePlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceProductVersion_REAL"), (uintptr_t)&SDL_JoystickGetDeviceProductVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceProduct_REAL"), (uintptr_t)&SDL_JoystickGetDeviceProduct);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceType_REAL"), (uintptr_t)&SDL_JoystickGetDeviceType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetDeviceVendor_REAL"), (uintptr_t)&SDL_JoystickGetDeviceVendor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetGUIDFromString_REAL"), (uintptr_t)&SDL_JoystickGetGUIDFromString);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetGUIDString_REAL"), (uintptr_t)&SDL_JoystickGetGUIDString);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetGUID_REAL"), (uintptr_t)&SDL_JoystickGetGUID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetHat_REAL"), (uintptr_t)&SDL_JoystickGetHat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetPlayerIndex_REAL"), (uintptr_t)&SDL_JoystickGetPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetProductVersion_REAL"), (uintptr_t)&SDL_JoystickGetProductVersion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetProduct_REAL"), (uintptr_t)&SDL_JoystickGetProduct);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetType_REAL"), (uintptr_t)&SDL_JoystickGetType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickGetVendor_REAL"), (uintptr_t)&SDL_JoystickGetVendor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickInstanceID_REAL"), (uintptr_t)&SDL_JoystickInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickIsHaptic_REAL"), (uintptr_t)&SDL_JoystickIsHaptic);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNameForIndex_REAL"), (uintptr_t)&SDL_JoystickNameForIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickName_REAL"), (uintptr_t)&SDL_JoystickName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNumAxes_REAL"), (uintptr_t)&SDL_JoystickNumAxes);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNumBalls_REAL"), (uintptr_t)&SDL_JoystickNumBalls);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNumButtons_REAL"), (uintptr_t)&SDL_JoystickNumButtons);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickNumHats_REAL"), (uintptr_t)&SDL_JoystickNumHats);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickOpen_REAL"), (uintptr_t)&SDL_JoystickOpen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickRumble_REAL"), (uintptr_t)&SDL_JoystickRumble);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickSetPlayerIndex_REAL"), (uintptr_t)&SDL_JoystickSetPlayerIndex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_JoystickUpdate_REAL"), (uintptr_t)&SDL_JoystickUpdate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadBMP_RW_REAL"), (uintptr_t)&SDL_LoadBMP_RW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadDollarTemplates_REAL"), (uintptr_t)&SDL_LoadDollarTemplates);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadFile_REAL"), (uintptr_t)&SDL_LoadFile);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadFile_RW_REAL"), (uintptr_t)&SDL_LoadFile_RW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadFunction_REAL"), (uintptr_t)&SDL_LoadFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadObject_REAL"), (uintptr_t)&SDL_LoadObject);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LoadWAV_RW_REAL"), (uintptr_t)&SDL_LoadWAV_RW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockAudioDevice_REAL"), (uintptr_t)&SDL_LockAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockAudio_REAL"), (uintptr_t)&SDL_LockAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockJoysticks_REAL"), (uintptr_t)&SDL_LockJoysticks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockMutex_REAL"), (uintptr_t)&SDL_LockMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockSurface_REAL"), (uintptr_t)&SDL_LockSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockTextureToSurface_REAL"), (uintptr_t)&SDL_LockTextureToSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LockTexture_REAL"), (uintptr_t)&SDL_LockTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogCritical_REAL"), (uintptr_t)&ret0);			 // Critical);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogDebug_REAL"), (uintptr_t)&ret0);			 // Debug);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogError_REAL"), (uintptr_t)&ret0);			 // Error);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogGetOutputFunction_REAL"), (uintptr_t)&ret0); // GetOutputFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogGetPriority_REAL"), (uintptr_t)&ret0);		 // GetPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogInfo_REAL"), (uintptr_t)&ret0);				 // Info);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogMessageV_REAL"), (uintptr_t)&ret0);			 // MessageV);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogMessage_REAL"), (uintptr_t)&ret0);			 // Message);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogResetPriorities_REAL"), (uintptr_t)&ret0);	 // ResetPriorities);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogSetAllPriority_REAL"), (uintptr_t)&ret0);	 // SetAllPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogSetOutputFunction_REAL"), (uintptr_t)&ret0); // SetOutputFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogSetPriority_REAL"), (uintptr_t)&ret0);		 // SetPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogVerbose_REAL"), (uintptr_t)&ret0);			 // Verbose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LogWarn_REAL"), (uintptr_t)&ret0);				 // Warn);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Log_REAL"), (uintptr_t)&ret0);					 //);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LowerBlitScaled_REAL"), (uintptr_t)&SDL_LowerBlitScaled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_LowerBlit_REAL"), (uintptr_t)&SDL_LowerBlit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MapRGBA_REAL"), (uintptr_t)&SDL_MapRGBA);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MapRGB_REAL"), (uintptr_t)&SDL_MapRGB);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MasksToPixelFormatEnum_REAL"), (uintptr_t)&SDL_MasksToPixelFormatEnum);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MaximizeWindow_REAL"), (uintptr_t)&SDL_MaximizeWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MemoryBarrierAcquireFunction_REAL"), (uintptr_t)&SDL_MemoryBarrierAcquireFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MemoryBarrierReleaseFunction_REAL"), (uintptr_t)&SDL_MemoryBarrierReleaseFunction);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Metal_CreateView_REAL"), (uintptr_t)&SDL_Metal_CreateView);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Metal_DestroyView_REAL"), (uintptr_t)&SDL_Metal_DestroyView);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MinimizeWindow_REAL"), (uintptr_t)&SDL_MinimizeWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MixAudioFormat_REAL"), (uintptr_t)&SDL_MixAudioFormat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MixAudio_REAL"), (uintptr_t)&SDL_MixAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_MouseIsHaptic_REAL"), (uintptr_t)&SDL_MouseIsHaptic);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_NewAudioStream_REAL"), (uintptr_t)&SDL_NewAudioStream);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_NumHaptics_REAL"), (uintptr_t)&SDL_NumHaptics);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_NumJoysticks_REAL"), (uintptr_t)&SDL_NumJoysticks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_NumSensors_REAL"), (uintptr_t)&SDL_NumSensors);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationDidBecomeActive_REAL"), (uintptr_t)&SDL_OnApplicationDidBecomeActive);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationDidEnterBackground_REAL"), (uintptr_t)&SDL_OnApplicationDidEnterBackground);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationDidReceiveMemoryWarning_REAL"), (uintptr_t)&SDL_OnApplicationDidReceiveMemoryWarning);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationWillEnterForeground_REAL"), (uintptr_t)&SDL_OnApplicationWillEnterForeground);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationWillResignActive_REAL"), (uintptr_t)&SDL_OnApplicationWillResignActive);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OnApplicationWillTerminate_REAL"), (uintptr_t)&SDL_OnApplicationWillTerminate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OpenAudioDevice_REAL"), (uintptr_t)&SDL_OpenAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_OpenAudio_REAL"), (uintptr_t)&SDL_OpenAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PauseAudioDevice_REAL"), (uintptr_t)&SDL_PauseAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PauseAudio_REAL"), (uintptr_t)&SDL_PauseAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PeepEvents_REAL"), (uintptr_t)&SDL_PeepEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PixelFormatEnumToMasks_REAL"), (uintptr_t)&SDL_PixelFormatEnumToMasks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PollEvent_REAL"), (uintptr_t)&SDL_PollEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PumpEvents_REAL"), (uintptr_t)&SDL_PumpEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_PushEvent_REAL"), (uintptr_t)&SDL_PushEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_QueryTexture_REAL"), (uintptr_t)&SDL_QueryTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_QueueAudio_REAL"), (uintptr_t)&SDL_QueueAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_QuitSubSystem_REAL"), (uintptr_t)&SDL_QuitSubSystem);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Quit_REAL"), (uintptr_t)&SDL_Quit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWFromConstMem_REAL"), (uintptr_t)&SDL_RWFromConstMem);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWFromFP_REAL"), (uintptr_t)&SDL_RWFromFP);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWFromFile_REAL"), (uintptr_t)&SDL_RWFromFile_hook);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWFromMem_REAL"), (uintptr_t)&SDL_RWFromMem);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWclose_REAL"), (uintptr_t)&SDL_RWclose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWread_REAL"), (uintptr_t)&SDL_RWread);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWseek_REAL"), (uintptr_t)&SDL_RWseek);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWsize_REAL"), (uintptr_t)&SDL_RWsize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWtell_REAL"), (uintptr_t)&SDL_RWtell);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RWwrite_REAL"), (uintptr_t)&SDL_RWwrite);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RaiseWindow_REAL"), (uintptr_t)&SDL_RaiseWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadBE16_REAL"), (uintptr_t)&SDL_ReadBE16);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadBE32_REAL"), (uintptr_t)&SDL_ReadBE32);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadBE64_REAL"), (uintptr_t)&SDL_ReadBE64);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadLE16_REAL"), (uintptr_t)&SDL_ReadLE16);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadLE32_REAL"), (uintptr_t)&SDL_ReadLE32);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadLE64_REAL"), (uintptr_t)&SDL_ReadLE64);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReadU8_REAL"), (uintptr_t)&SDL_ReadU8);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RecordGesture_REAL"), (uintptr_t)&SDL_RecordGesture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RegisterEvents_REAL"), (uintptr_t)&SDL_RegisterEvents);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RemoveTimer_REAL"), (uintptr_t)&SDL_RemoveTimer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderClear_REAL"), (uintptr_t)&SDL_RenderClear);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderCopyExF_REAL"), (uintptr_t)&SDL_RenderCopyExF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderCopyEx_REAL"), (uintptr_t)&SDL_RenderCopyEx);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderCopyF_REAL"), (uintptr_t)&SDL_RenderCopyF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderCopy_REAL"), (uintptr_t)&SDL_RenderCopy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawLineF_REAL"), (uintptr_t)&SDL_RenderDrawLineF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawLine_REAL"), (uintptr_t)&SDL_RenderDrawLine);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawLinesF_REAL"), (uintptr_t)&SDL_RenderDrawLinesF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawLines_REAL"), (uintptr_t)&SDL_RenderDrawLines);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawPointF_REAL"), (uintptr_t)&SDL_RenderDrawPointF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawPoint_REAL"), (uintptr_t)&SDL_RenderDrawPoint);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawPointsF_REAL"), (uintptr_t)&SDL_RenderDrawPointsF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawPoints_REAL"), (uintptr_t)&SDL_RenderDrawPoints);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawRectF_REAL"), (uintptr_t)&SDL_RenderDrawRectF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawRect_REAL"), (uintptr_t)&SDL_RenderDrawRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawRectsF_REAL"), (uintptr_t)&SDL_RenderDrawRectsF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderDrawRects_REAL"), (uintptr_t)&SDL_RenderDrawRects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFillRectF_REAL"), (uintptr_t)&SDL_RenderFillRectF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFillRect_REAL"), (uintptr_t)&SDL_RenderFillRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFillRectsF_REAL"), (uintptr_t)&SDL_RenderFillRectsF);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFillRects_REAL"), (uintptr_t)&SDL_RenderFillRects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderFlush_REAL"), (uintptr_t)&SDL_RenderFlush);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetClipRect_REAL"), (uintptr_t)&SDL_RenderGetClipRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetIntegerScale_REAL"), (uintptr_t)&SDL_RenderGetIntegerScale);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetLogicalSize_REAL"), (uintptr_t)&SDL_RenderGetLogicalSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetMetalCommandEncoder_REAL"), (uintptr_t)&SDL_RenderGetMetalCommandEncoder);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetMetalLayer_REAL"), (uintptr_t)&SDL_RenderGetMetalLayer);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetScale_REAL"), (uintptr_t)&SDL_RenderGetScale);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderGetViewport_REAL"), (uintptr_t)&SDL_RenderGetViewport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderIsClipEnabled_REAL"), (uintptr_t)&SDL_RenderIsClipEnabled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderPresent_REAL"), (uintptr_t)&SDL_RenderPresent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderReadPixels_REAL"), (uintptr_t)&SDL_RenderReadPixels);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetClipRect_REAL"), (uintptr_t)&SDL_RenderSetClipRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetIntegerScale_REAL"), (uintptr_t)&SDL_RenderSetIntegerScale);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetLogicalSize_REAL"), (uintptr_t)&SDL_RenderSetLogicalSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetScale_REAL"), (uintptr_t)&SDL_RenderSetScale);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderSetViewport_REAL"), (uintptr_t)&SDL_RenderSetViewport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RenderTargetSupported_REAL"), (uintptr_t)&SDL_RenderTargetSupported);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ReportAssertion_REAL"), (uintptr_t)&SDL_ReportAssertion);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ResetAssertionReport_REAL"), (uintptr_t)&SDL_ResetAssertionReport);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_RestoreWindow_REAL"), (uintptr_t)&SDL_RestoreWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SIMDAlloc_REAL"), (uintptr_t)&SDL_SIMDAlloc);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SIMDFree_REAL"), (uintptr_t)&SDL_SIMDFree);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SIMDGetAlignment_REAL"), (uintptr_t)&SDL_SIMDGetAlignment);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SaveAllDollarTemplates_REAL"), (uintptr_t)&SDL_SaveAllDollarTemplates);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SaveBMP_RW_REAL"), (uintptr_t)&SDL_SaveBMP_RW);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SaveDollarTemplate_REAL"), (uintptr_t)&SDL_SaveDollarTemplate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemPost_REAL"), (uintptr_t)&SDL_SemPost);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemTryWait_REAL"), (uintptr_t)&SDL_SemTryWait);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemValue_REAL"), (uintptr_t)&SDL_SemValue);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemWaitTimeout_REAL"), (uintptr_t)&SDL_SemWaitTimeout);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SemWait_REAL"), (uintptr_t)&SDL_SemWait);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorClose_REAL"), (uintptr_t)&SDL_SensorClose);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorFromInstanceID_REAL"), (uintptr_t)&SDL_SensorFromInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetData_REAL"), (uintptr_t)&SDL_SensorGetData);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetDeviceInstanceID_REAL"), (uintptr_t)&SDL_SensorGetDeviceInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetDeviceName_REAL"), (uintptr_t)&SDL_SensorGetDeviceName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetDeviceNonPortableType_REAL"), (uintptr_t)&SDL_SensorGetDeviceNonPortableType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetDeviceType_REAL"), (uintptr_t)&SDL_SensorGetDeviceType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetInstanceID_REAL"), (uintptr_t)&SDL_SensorGetInstanceID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetName_REAL"), (uintptr_t)&SDL_SensorGetName);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetNonPortableType_REAL"), (uintptr_t)&SDL_SensorGetNonPortableType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorGetType_REAL"), (uintptr_t)&SDL_SensorGetType);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorOpen_REAL"), (uintptr_t)&SDL_SensorOpen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SensorUpdate_REAL"), (uintptr_t)&SDL_SensorUpdate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetAssertionHandler_REAL"), (uintptr_t)&SDL_SetAssertionHandler);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetClipRect_REAL"), (uintptr_t)&SDL_SetClipRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetClipboardText_REAL"), (uintptr_t)&SDL_SetClipboardText);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetColorKey_REAL"), (uintptr_t)&SDL_SetColorKey);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetCursor_REAL"), (uintptr_t)&SDL_SetCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetError_REAL"), (uintptr_t)&SDL_SetError);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetEventFilter_REAL"), (uintptr_t)&SDL_SetEventFilter);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetHintWithPriority_REAL"), (uintptr_t)&SDL_SetHintWithPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetHint_REAL"), (uintptr_t)&SDL_SetHint);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetMainReady_REAL"), (uintptr_t)&SDL_SetMainReady);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetMemoryFunctions_REAL"), (uintptr_t)&SDL_SetMemoryFunctions);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetModState_REAL"), (uintptr_t)&SDL_SetModState);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetPaletteColors_REAL"), (uintptr_t)&SDL_SetPaletteColors);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetPixelFormatPalette_REAL"), (uintptr_t)&SDL_SetPixelFormatPalette);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetRelativeMouseMode_REAL"), (uintptr_t)&SDL_SetRelativeMouseMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetRenderDrawBlendMode_REAL"), (uintptr_t)&SDL_SetRenderDrawBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetRenderDrawColor_REAL"), (uintptr_t)&SDL_SetRenderDrawColor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetRenderTarget_REAL"), (uintptr_t)&SDL_SetRenderTarget);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfaceAlphaMod_REAL"), (uintptr_t)&SDL_SetSurfaceAlphaMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfaceBlendMode_REAL"), (uintptr_t)&SDL_SetSurfaceBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfaceColorMod_REAL"), (uintptr_t)&SDL_SetSurfaceColorMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfacePalette_REAL"), (uintptr_t)&SDL_SetSurfacePalette);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetSurfaceRLE_REAL"), (uintptr_t)&SDL_SetSurfaceRLE);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextInputRect_REAL"), (uintptr_t)&SDL_SetTextInputRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextureAlphaMod_REAL"), (uintptr_t)&SDL_SetTextureAlphaMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextureBlendMode_REAL"), (uintptr_t)&SDL_SetTextureBlendMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextureColorMod_REAL"), (uintptr_t)&SDL_SetTextureColorMod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetTextureScaleMode_REAL"), (uintptr_t)&SDL_SetTextureScaleMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetThreadPriority_REAL"), (uintptr_t)&SDL_SetThreadPriority);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowBordered_REAL"), (uintptr_t)&SDL_SetWindowBordered);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowBrightness_REAL"), (uintptr_t)&SDL_SetWindowBrightness);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowData_REAL"), (uintptr_t)&SDL_SetWindowData);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowDisplayMode_REAL"), (uintptr_t)&SDL_SetWindowDisplayMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowFullscreen_REAL"), (uintptr_t)&SDL_SetWindowFullscreen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowGammaRamp_REAL"), (uintptr_t)&SDL_SetWindowGammaRamp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowGrab_REAL"), (uintptr_t)&SDL_SetWindowGrab);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowHitTest_REAL"), (uintptr_t)&SDL_SetWindowHitTest);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowIcon_REAL"), (uintptr_t)&SDL_SetWindowIcon);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowInputFocus_REAL"), (uintptr_t)&SDL_SetWindowInputFocus);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowMaximumSize_REAL"), (uintptr_t)&SDL_SetWindowMaximumSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowMinimumSize_REAL"), (uintptr_t)&SDL_SetWindowMinimumSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowModalFor_REAL"), (uintptr_t)&SDL_SetWindowModalFor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowOpacity_REAL"), (uintptr_t)&SDL_SetWindowOpacity);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowPosition_REAL"), (uintptr_t)&SDL_SetWindowPosition);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowResizable_REAL"), (uintptr_t)&SDL_SetWindowResizable);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowShape_REAL"), (uintptr_t)&SDL_SetWindowShape);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowSize_REAL"), (uintptr_t)&SDL_SetWindowSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetWindowTitle_REAL"), (uintptr_t)&SDL_SetWindowTitle);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SetYUVConversionMode_REAL"), (uintptr_t)&SDL_SetYUVConversionMode);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ShowCursor_REAL"), (uintptr_t)&SDL_ShowCursor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ShowMessageBox_REAL"), (uintptr_t)&SDL_ShowMessageBox);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ShowSimpleMessageBox_REAL"), (uintptr_t)&SDL_ShowSimpleMessageBox);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ShowWindow_REAL"), (uintptr_t)&SDL_ShowWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_SoftStretch_REAL"), (uintptr_t)&SDL_SoftStretch);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_StartTextInput_REAL"), (uintptr_t)&SDL_StartTextInput);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_StopTextInput_REAL"), (uintptr_t)&SDL_StopTextInput);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_TLSCreate_REAL"), (uintptr_t)&SDL_TLSCreate);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_TLSGet_REAL"), (uintptr_t)&SDL_TLSGet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_TLSSet_REAL"), (uintptr_t)&SDL_TLSSet);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ThreadID_REAL"), (uintptr_t)&SDL_ThreadID);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_TryLockMutex_REAL"), (uintptr_t)&SDL_TryLockMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnionRect_REAL"), (uintptr_t)&SDL_UnionRect);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnloadObject_REAL"), (uintptr_t)&SDL_UnloadObject);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockAudioDevice_REAL"), (uintptr_t)&SDL_UnlockAudioDevice);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockAudio_REAL"), (uintptr_t)&SDL_UnlockAudio);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockJoysticks_REAL"), (uintptr_t)&SDL_UnlockJoysticks);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockMutex_REAL"), (uintptr_t)&SDL_UnlockMutex);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockSurface_REAL"), (uintptr_t)&SDL_UnlockSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UnlockTexture_REAL"), (uintptr_t)&SDL_UnlockTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpdateWindowSurfaceRects_REAL"), (uintptr_t)&SDL_UpdateWindowSurfaceRects);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpdateWindowSurface_REAL"), (uintptr_t)&SDL_UpdateWindowSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpdateYUVTexture_REAL"), (uintptr_t)&SDL_UpdateYUVTexture);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpperBlitScaled_REAL"), (uintptr_t)&SDL_UpperBlitScaled);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_UpperBlit_REAL"), (uintptr_t)&SDL_UpperBlit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_VideoInit_REAL"), (uintptr_t)&SDL_VideoInit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_VideoQuit_REAL"), (uintptr_t)&SDL_VideoQuit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_CreateSurface_REAL"), (uintptr_t)&SDL_Vulkan_CreateSurface);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_GetDrawableSize_REAL"), (uintptr_t)&SDL_Vulkan_GetDrawableSize);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_GetInstanceExtensions_REAL"), (uintptr_t)&SDL_Vulkan_GetInstanceExtensions);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_GetVkGetInstanceProcAddr_REAL"), (uintptr_t)&SDL_Vulkan_GetVkGetInstanceProcAddr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_LoadLibrary_REAL"), (uintptr_t)&SDL_Vulkan_LoadLibrary);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_Vulkan_UnloadLibrary_REAL"), (uintptr_t)&SDL_Vulkan_UnloadLibrary);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WaitEventTimeout_REAL"), (uintptr_t)&SDL_WaitEventTimeout);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WaitEvent_REAL"), (uintptr_t)&SDL_WaitEvent);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WaitThread_REAL"), (uintptr_t)&SDL_WaitThread);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WarpMouseGlobal_REAL"), (uintptr_t)&SDL_WarpMouseGlobal);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WarpMouseInWindow_REAL"), (uintptr_t)&SDL_WarpMouseInWindow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WasInit_REAL"), (uintptr_t)&SDL_WasInit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteBE16_REAL"), (uintptr_t)&SDL_WriteBE16);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteBE32_REAL"), (uintptr_t)&SDL_WriteBE32);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteBE64_REAL"), (uintptr_t)&SDL_WriteBE64);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteLE16_REAL"), (uintptr_t)&SDL_WriteLE16);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteLE32_REAL"), (uintptr_t)&SDL_WriteLE32);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteLE64_REAL"), (uintptr_t)&SDL_WriteLE64);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_WriteU8_REAL"), (uintptr_t)&SDL_WriteU8);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_abs_REAL"), (uintptr_t)&SDL_abs);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_acos_REAL"), (uintptr_t)&SDL_acos);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_acosf_REAL"), (uintptr_t)&SDL_acosf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_asin_REAL"), (uintptr_t)&SDL_asin);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_asinf_REAL"), (uintptr_t)&SDL_asinf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atan2_REAL"), (uintptr_t)&SDL_atan2);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atan2f_REAL"), (uintptr_t)&SDL_atan2f);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atan_REAL"), (uintptr_t)&SDL_atan);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atanf_REAL"), (uintptr_t)&SDL_atanf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atof_REAL"), (uintptr_t)&SDL_atof);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_atoi_REAL"), (uintptr_t)&SDL_atoi);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_calloc_REAL"), (uintptr_t)&SDL_calloc);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ceil_REAL"), (uintptr_t)&SDL_ceil);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ceilf_REAL"), (uintptr_t)&SDL_ceilf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_copysign_REAL"), (uintptr_t)&SDL_copysign);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_copysignf_REAL"), (uintptr_t)&SDL_copysignf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_cos_REAL"), (uintptr_t)&SDL_cos);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_cosf_REAL"), (uintptr_t)&SDL_cosf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_exp_REAL"), (uintptr_t)&SDL_exp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_expf_REAL"), (uintptr_t)&SDL_expf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_fabs_REAL"), (uintptr_t)&SDL_fabs);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_fabsf_REAL"), (uintptr_t)&SDL_fabsf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_floor_REAL"), (uintptr_t)&SDL_floor);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_floorf_REAL"), (uintptr_t)&SDL_floorf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_fmod_REAL"), (uintptr_t)&SDL_fmod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_fmodf_REAL"), (uintptr_t)&SDL_fmodf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_free_REAL"), (uintptr_t)&SDL_free);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_getenv_REAL"), (uintptr_t)&SDL_getenv);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_iconv_REAL"), (uintptr_t)&SDL_iconv);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_iconv_close_REAL"), (uintptr_t)&SDL_iconv_close);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_iconv_open_REAL"), (uintptr_t)&SDL_iconv_open);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_iconv_string_REAL"), (uintptr_t)&SDL_iconv_string);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_isdigit_REAL"), (uintptr_t)&SDL_isdigit);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_islower_REAL"), (uintptr_t)&SDL_islower);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_isspace_REAL"), (uintptr_t)&SDL_isspace);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_isupper_REAL"), (uintptr_t)&SDL_isupper);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_itoa_REAL"), (uintptr_t)&SDL_itoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_lltoa_REAL"), (uintptr_t)&SDL_lltoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_log10_REAL"), (uintptr_t)&SDL_log10);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_log10f_REAL"), (uintptr_t)&SDL_log10f);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_log_REAL"), (uintptr_t)&SDL_log);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_logf_REAL"), (uintptr_t)&SDL_logf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ltoa_REAL"), (uintptr_t)&SDL_ltoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_malloc_REAL"), (uintptr_t)&SDL_malloc);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_memcmp_REAL"), (uintptr_t)&SDL_memcmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_memcpy_REAL"), (uintptr_t)&SDL_memcpy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_memmove_REAL"), (uintptr_t)&SDL_memmove);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_memset_REAL"), (uintptr_t)&SDL_memset);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_pow_REAL"), (uintptr_t)&SDL_pow);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_powf_REAL"), (uintptr_t)&SDL_powf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_qsort_REAL"), (uintptr_t)&SDL_qsort);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_realloc_REAL"), (uintptr_t)&SDL_realloc);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_scalbn_REAL"), (uintptr_t)&SDL_scalbn);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_scalbnf_REAL"), (uintptr_t)&SDL_scalbnf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_setenv_REAL"), (uintptr_t)&SDL_setenv);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sin_REAL"), (uintptr_t)&SDL_sin);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sinf_REAL"), (uintptr_t)&SDL_sinf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_snprintf_REAL"), (uintptr_t)&SDL_snprintf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sqrt_REAL"), (uintptr_t)&SDL_sqrt);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sqrtf_REAL"), (uintptr_t)&SDL_sqrtf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_sscanf_REAL"), (uintptr_t)&SDL_sscanf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strcasecmp_REAL"), (uintptr_t)&SDL_strcasecmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strchr_REAL"), (uintptr_t)&SDL_strchr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strcmp_REAL"), (uintptr_t)&SDL_strcmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strdup_REAL"), (uintptr_t)&SDL_strdup);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strlcat_REAL"), (uintptr_t)&SDL_strlcat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strlcpy_REAL"), (uintptr_t)&SDL_strlcpy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strlen_REAL"), (uintptr_t)&SDL_strlen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strlwr_REAL"), (uintptr_t)&SDL_strlwr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strncasecmp_REAL"), (uintptr_t)&SDL_strncasecmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strncmp_REAL"), (uintptr_t)&SDL_strncmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strrchr_REAL"), (uintptr_t)&SDL_strrchr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strrev_REAL"), (uintptr_t)&SDL_strrev);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strstr_REAL"), (uintptr_t)&SDL_strstr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtod_REAL"), (uintptr_t)&SDL_strtod);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtokr_REAL"), (uintptr_t)&SDL_strtokr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtol_REAL"), (uintptr_t)&SDL_strtol);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtoll_REAL"), (uintptr_t)&SDL_strtoll);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtoul_REAL"), (uintptr_t)&SDL_strtoul);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strtoull_REAL"), (uintptr_t)&SDL_strtoull);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_strupr_REAL"), (uintptr_t)&SDL_strupr);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_tan_REAL"), (uintptr_t)&SDL_tan);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_tanf_REAL"), (uintptr_t)&SDL_tanf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_tolower_REAL"), (uintptr_t)&SDL_tolower);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_toupper_REAL"), (uintptr_t)&SDL_toupper);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_uitoa_REAL"), (uintptr_t)&SDL_uitoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ulltoa_REAL"), (uintptr_t)&SDL_ulltoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_ultoa_REAL"), (uintptr_t)&SDL_ultoa);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_utf8strlcpy_REAL"), (uintptr_t)&SDL_utf8strlcpy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_utf8strlen_REAL"), (uintptr_t)&SDL_utf8strlen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_vsnprintf_REAL"), (uintptr_t)&SDL_vsnprintf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_vsscanf_REAL"), (uintptr_t)&SDL_vsscanf);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcscmp_REAL"), (uintptr_t)&SDL_wcscmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcsdup_REAL"), (uintptr_t)&SDL_wcsdup);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcslcat_REAL"), (uintptr_t)&SDL_wcslcat);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcslcpy_REAL"), (uintptr_t)&SDL_wcslcpy);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcslen_REAL"), (uintptr_t)&SDL_wcslen);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcsncmp_REAL"), (uintptr_t)&SDL_wcsncmp);
	hook_addr(so_symbol(&diceydungeons_mod, "SDL_wcsstr_REAL"), (uintptr_t)&SDL_wcsstr);
}

void *pthread_main(void *arg)
{
	// Disabling rearpad
	SDL_setenv("VITA_DISABLE_TOUCH_BACK", "1", 1);

	int (*SDL_main)(int argc, char *argv[]) = (void *)so_symbol(&diceydungeons_mod, "SDL_main");

	char *args[2];
	args[0] = "ux0:data/diceydungeons";
	SDL_main(1, args);
}

int main(int argc, char *argv[])
{
	// sceSysmoduleLoadModule(SCE_SYSMODULE_RAZOR_CAPTURE);
	// SceUID crasher_thread = sceKernelCreateThread("crasher", crasher, 0x40, 0x1000, 0, 0, NULL);
	// sceKernelStartThread(crasher_thread, 0, NULL);

	SceAppUtilInitParam init_param = {0};
	SceAppUtilBootParam boot_param = {0};
	sceAppUtilInit(&init_param, &boot_param);
	SceAppUtilAppEventParam eventParam;
	sceClibMemset(&eventParam, 0, sizeof(SceAppUtilAppEventParam));
	sceAppUtilReceiveAppEvent(&eventParam);
	if (eventParam.type == 0x05)
	{
		char buffer[2048];
		sceAppUtilAppEventParseLiveArea(&eventParam, buffer);
		if (strstr(buffer, "custom"))
			framecap = 1;
	}

	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
	sceTouchSetSamplingState(SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_START);

	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	scePowerSetGpuXbarClockFrequency(166);

	if (check_kubridge() < 0)
		fatal_error("Error kubridge.skprx is not installed.");

	if (!file_exists("ur0:/data/libshacccg.suprx") && !file_exists("ur0:/data/external/libshacccg.suprx"))
		fatal_error("Error libshacccg.suprx is not installed.");

	char fname[256];
	sprintf(data_path, "ux0:data/diceydungeons");

	printf("Loading liblime\n");
	sprintf(fname, "%s/liblime.so", data_path);
	if (so_file_load(&diceydungeons_mod, fname, LOAD_ADDRESS) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&diceydungeons_mod);
	so_resolve(&diceydungeons_mod, default_dynlib, sizeof(default_dynlib), 0);
	so_flush_caches(&diceydungeons_mod);
	so_initialize(&diceydungeons_mod);

	printf("Loading libApplicationMain\n");
	sprintf(fname, "%s/libApplicationMain.so", data_path);
	if (so_file_load(&diceydungeonsmain_mod, fname, LOAD_ADDRESS + 0x1000000) < 0)
		fatal_error("Error could not load %s.", fname);
	so_relocate(&diceydungeonsmain_mod);
	so_resolve(&diceydungeonsmain_mod, default_dynlib, sizeof(default_dynlib), 0);

	vglUseTripleBuffering(GL_FALSE);
	vglSetParamBufferSize(3 * 1024 * 1024);
	vglInitWithCustomThreshold(0, SCREEN_W, SCREEN_H, MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, 0, 0, 0, SCE_GXM_MULTISAMPLE_NONE);

	patch_game();
	so_flush_caches(&diceydungeonsmain_mod);
	so_initialize(&diceydungeonsmain_mod);

	memset(fake_vm, 'A', sizeof(fake_vm));
	*(uintptr_t *)(fake_vm + 0x00) = (uintptr_t)fake_vm; // just point to itself...
	*(uintptr_t *)(fake_vm + 0x10) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x14) = (uintptr_t)ret0;
	*(uintptr_t *)(fake_vm + 0x18) = (uintptr_t)GetEnv;

	memset(fake_env, 'A', sizeof(fake_env));
	*(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env; // just point to itself...
	*(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
	*(uintptr_t *)(fake_env + 0x4C) = (uintptr_t)ret0; // PushLocalFrame
	*(uintptr_t *)(fake_env + 0x50) = (uintptr_t)ret0; // PopLocalFrame
	*(uintptr_t *)(fake_env + 0x54) = (uintptr_t)NewGlobalRef;
	*(uintptr_t *)(fake_env + 0x58) = (uintptr_t)DeleteGlobalRef;
	*(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)ret0; // DeleteLocalRef
	*(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
	*(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
	*(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
	*(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
	*(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
	*(uintptr_t *)(fake_env + 0xC8) = (uintptr_t)CallIntMethodV;
	*(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
	*(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
	*(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
	*(uintptr_t *)(fake_env + 0x17C) = (uintptr_t)GetBooleanField;
	*(uintptr_t *)(fake_env + 0x190) = (uintptr_t)GetIntField;
	*(uintptr_t *)(fake_env + 0x198) = (uintptr_t)GetFloatField;
	*(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
	*(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
	*(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
	*(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
	*(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticLongMethodV;
	*(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
	*(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
	*(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
	*(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
	*(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
	*(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
	*(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
	*(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0; // ReleaseStringUTFChars
	*(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
	*(uintptr_t *)(fake_env + 0x2B4) = (uintptr_t)GetObjectArrayElement;
	*(uintptr_t *)(fake_env + 0x35C) = (uintptr_t)ret0; // RegisterNatives
	*(uintptr_t *)(fake_env + 0x36C) = (uintptr_t)GetJavaVM;
	*(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;

	pthread_t t2;
	pthread_attr_t attr2;

	pthread_attr_init(&attr2);
	pthread_attr_setstacksize(&attr2, 512 * 1024);
	pthread_create(&t2, &attr2, pthread_main, NULL);

	pthread_join(t2, NULL);

	return 0;
}
