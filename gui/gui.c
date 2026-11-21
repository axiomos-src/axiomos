/*
 * AxiomOS - GUI lifecycle.
 *
 * `gui_init` takes the framebuffer description from the boot information
 * block, sets up the compositor and opens the default set of windows.  The
 * main loop in kmain calls `gui_frame` once per idle cycle; each frame polls
 * input, lets the focused window handle keys and then composites.
 */

#include <gui.h>

static bool_t g_active = false;
static char g_splash_stage[48] = "starting";
static i32  g_splash_percent = 0;

bool_t hvci_active(void);
bool_t gui_active(void) { return g_active; }

bool_t gui_init(void)
{
    if (!g_boot_info || !g_boot_info->fb.base) {
        printk("gui: no framebuffer from the loader\n");
        return false;
    }

    struct abp_framebuffer *fb = &g_boot_info->fb;
    printk("gui: framebuffer %ux%u bpp=%u stride=%u base=%p\n",
           fb->width, fb->height, fb->bpp, fb->stride, (void *)fb->base);

    if (!gui_compositor_init((u32)fb->base, fb->width, fb->height, fb->stride,
                             fb->bpp, fb->red_mask, fb->green_mask,
                             fb->blue_mask)) {
        printk("gui: compositor initialisation failed\n");
        return false;
    }

    gui_wm_init();
    gui_apps_init();

    /* Default desktop: terminal, integrity monitor and system monitor. */
    gui_app_terminal_open();
    gui_app_hvci_open();
    gui_app_taskmanager_open();

    g_active = true;

    /* Push the first frame out immediately so that the screen never stays on
     * the loader's splash. */
    gui_compositor_frame();
    printk("gui: desktop ready\n");
    return true;
}

void gui_frame(void)
{
    if (!g_active)
        return;
    gui_input_poll();
    gui_compositor_frame();
}

void gui_handle_input(void)
{
    gui_input_poll();
}

void gui_shutdown(void)
{
    g_active = false;
}

void gui_boot_splash(const char *stage, i32 percent)
{
    i32 i = 0;
    while (stage && stage[i] && i < 47) {
        g_splash_stage[i] = stage[i];
        i++;
    }
    g_splash_stage[i] = 0;
    g_splash_percent = percent;
    printk("gui: %s %d%%\n", g_splash_stage, percent);
}

/* Panic screen.  The kernel reaches this through a weak symbol while the
 * system is in an unknown state, so it deliberately touches as little of the
 * GUI as possible: the serial log already carries the full panic report. */
void gui_panic_screen(const char *message)
{
    if (!g_active)
        return;
    printk("gui: panic screen: %s\n", message ? message : "(no message)");
    g_active = false;
}
