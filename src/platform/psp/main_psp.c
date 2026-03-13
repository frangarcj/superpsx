/**
 * main_psp.c — PSP entry point for SuperPSX
 */
#include <pspkernel.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <stdlib.h>
#include <string.h>

#include "superpsx.h"
#include "config.h"
#include "platform.h"

PSP_MODULE_INFO("SuperPSX", 0, 1, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);

char psx_exe_filename_buf[512] = "";
const char *psx_exe_filename = NULL;
int psx_boot_mode = 0;
char disc_image_path[512] = "";
const char **psx_host_args = NULL;
int psx_host_argc = 0;

static int exit_callback(int arg1, int arg2, void *common) {
    (void)arg1; (void)arg2; (void)common;
    sceKernelExitGame();
    return 0;
}

static int callback_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void setup_callbacks(void) {
    int thid = sceKernelCreateThread("update_thread", callback_thread,
                                     0x11, 0xFA0, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
}

int main(int argc, char *argv[]) {
    /* Redirect stdout/stderr for PPSSPP console capture */
    freopen("superpsx.log", "w", stdout);
    freopen("superpsx.log", "w", stderr);
    printf("MAIN: SuperPSX PSP started\n");

    setup_callbacks();
    Platform_Init();

    psx_exe_filename = psx_exe_filename_buf;
    load_config_file();

    /* Parse arguments (from PPSSPP or PSP loader) */
    if (argc > 1) {
        strncpy(psx_exe_filename_buf, argv[1], sizeof(psx_exe_filename_buf) - 1);
        psx_exe_filename_buf[sizeof(psx_exe_filename_buf) - 1] = '\0';
    }

    /* Host arguments for PSX EXE loading */
    if (argc > 1) {
        psx_host_args = (const char **)argv + 1;
        psx_host_argc = argc - 1;
    }

    /* Fallback: boot BIOS shell */
    if (psx_exe_filename_buf[0] == '\0') {
        strcpy(psx_exe_filename_buf, "bios/SCPH1001.BIN");
    }

    Init_SuperPSX();

    Platform_Halt();
    return 0;
}
