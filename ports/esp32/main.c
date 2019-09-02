/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * Development of the code in this file was sponsored by Microbric Pty Ltd
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016 Damien P. George
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_task.h"
#include "soc/cpu.h"
#include "esp_log.h"
#include "esp_spiram.h"
#include "esp_timer.h"		// add by zkh
#include "driver/timer.h"

#include "startup/oled.h"

#include "py/stackctrl.h"
#include "py/nlr.h"
#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mphal.h"
#include "lib/mp-readline/readline.h"
#include "lib/utils/pyexec.h"
#include "uart.h"
#include "modmachine.h"
#include "modnetwork.h"
#include "mpthreadport.h"

// MicroPython runs as a task under FreeRTOS
#define MP_TASK_PRIORITY        (ESP_TASK_PRIO_MIN + 1)
#define MP_TASK_STACK_SIZE      (16 * 1024)
#define MP_TASK_STACK_LEN       (MP_TASK_STACK_SIZE / sizeof(StackType_t))

int vprintf_null(const char *format, va_list ap) {
    // do nothing: this is used as a log target during raw repl mode
    return 0;
}

volatile uint32_t ticker_ticks_ms = 0;
extern void mpython_music_tick(void);
extern void mpython_display_tick(void);
static void timer_1ms_ticker(void *args)
{
    ticker_ticks_ms += 1;
    mpython_music_tick();
    mpython_display_tick();
}

void mpython_display_exception(mp_obj_t exc_in)
{
    mp_uint_t n, *values;
    mp_obj_exception_get_traceback(exc_in, &n, &values);
    if (1) {
        vstr_t vstr;
        mp_print_t print;
        vstr_init_print(&vstr, 50, &print);
        #if MICROPY_ENABLE_SOURCE_LINE
        if (n >= 3) {
            mp_printf(&print, "line %u\n", values[1]);
        }
        #endif
        if (mp_obj_is_native_exception_instance(exc_in)) {
            mp_obj_exception_t *exc = (mp_obj_exception_t*)MP_OBJ_TO_PTR(exc_in);
            mp_printf(&print, "%q:\n  ", exc->base.type->name);
            if (exc->args != NULL && exc->args->len != 0) {
                mp_obj_print_helper(&print, exc->args->items[0], PRINT_STR);
            }
        }
        oled_init();
        oled_clear();
        oled_print(vstr_null_terminated_str(&vstr), 0, 0);
        oled_show();
        vstr_clear(&vstr);
        oled_deinit();
    }
}

void mpython_stop_timer(void) {
    // disable all timer and thread created by main.py
    for (timer_group_t g = TIMER_GROUP_0; g < TIMER_GROUP_MAX; g++) {
        for (timer_idx_t i = TIMER_0; i < TIMER_MAX; i++) {
            timer_pause(g, i);
        }
    }
}

void mpython_stop_thread(void) {
    #if MICROPY_PY_THREAD
    mp_thread_deinit();
    #endif  
}

void mp_task(void *pvParameter) {
    volatile uint32_t sp = (uint32_t)get_sp();
    #if MICROPY_PY_THREAD
    mp_thread_init(pxTaskGetStackStart(NULL), MP_TASK_STACK_LEN);
    #endif
    esp_log_level_set("*", ESP_LOG_ERROR);    // only error msg for mpython
    // esp_log_level_set("*", ESP_LOG_INFO);

    uart_init();

    #if CONFIG_SPIRAM_SUPPORT
    // Try to use the entire external SPIRAM directly for the heap
    size_t mp_task_heap_size;
    void *mp_task_heap = (void*)0x3f800000;
    switch (esp_spiram_get_chip_size()) {
        case ESP_SPIRAM_SIZE_16MBITS:
            mp_task_heap_size = 2 * 1024 * 1024;
            break;
        case ESP_SPIRAM_SIZE_32MBITS:
        case ESP_SPIRAM_SIZE_64MBITS:
            mp_task_heap_size = 4 * 1024 * 1024;
            break;
        default:
            // No SPIRAM, fallback to normal allocation
            mp_task_heap_size = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
            mp_task_heap = malloc(mp_task_heap_size);
            break;
    }
    #else
    // Allocate the uPy heap using malloc and get the largest available region
    size_t mp_task_heap_size = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    void *mp_task_heap = malloc(mp_task_heap_size);
    #endif

soft_reset:
    // startup
    oled_init();
    oled_drawImg(ani_startup[24]);
    //oled_drawAnimation(ani_startup, 25, 50);
    //oled_clear();
    oled_show();
    oled_deinit();

    // initialise the stack pointer for the main thread
    mp_stack_set_top((void *)sp);
    mp_stack_set_limit(MP_TASK_STACK_SIZE - 1024);
    gc_init(mp_task_heap, mp_task_heap + mp_task_heap_size);
    mp_init();
    mp_obj_list_init(mp_sys_path, 0);
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR_));
    mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(MP_QSTR__slash_lib));
    mp_obj_list_init(mp_sys_argv, 0);
    readline_init0();

    // initialise peripherals
    machine_pins_init();
	// add by zhang kaihua
	// for music function
	const esp_timer_create_args_t periodic_timer_args = {
		.callback = &timer_1ms_ticker,
		.name = "music tick timer"
	};
	esp_timer_handle_t periodic_timer;
    ticker_ticks_ms = 0;
	ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
	ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, 1000));

    // run boot-up scripts
    pyexec_frozen_module("_boot.py");
    pyexec_file_if_exists("boot.py");
    if (pyexec_mode_kind == PYEXEC_MODE_FRIENDLY_REPL) {
        pyexec_file_if_exists("main.py");
    }

    for (;;) {
        if (pyexec_mode_kind == PYEXEC_MODE_RAW_REPL) {
            vprintf_like_t vprintf_log = esp_log_set_vprintf(vprintf_null);
            if (pyexec_raw_repl() != 0) {
                break;
            }
            esp_log_set_vprintf(vprintf_log);
        } else {
            if (pyexec_friendly_repl() != 0) {
                break;
            }
        }
    }

    machine_timer_deinit_all();

    #if MICROPY_PY_THREAD
    mp_thread_deinit();
    #endif

    gc_sweep_all();

    mp_hal_stdout_tx_str("mpython: soft reboot\r\n");

    // deinitialise peripherals
    machine_pins_deinit();
    usocket_events_deinit();

    esp_timer_stop(periodic_timer);
    esp_timer_delete(periodic_timer);
    MP_STATE_PORT(music_data) = NULL;

    mp_deinit();
    fflush(stdout);
    goto soft_reset;
}

void app_main(void) {
    nvs_flash_init();
    xTaskCreate(mp_task, "mp_task", MP_TASK_STACK_LEN, NULL, MP_TASK_PRIORITY, &mp_main_task_handle);
}

void nlr_jump_fail(void *val) {
    printf("NLR jump failed, val=%p\n", val);
    esp_restart();
}

// modussl_mbedtls uses this function but it's not enabled in ESP IDF
void mbedtls_debug_set_threshold(int threshold) {
    (void)threshold;
}
