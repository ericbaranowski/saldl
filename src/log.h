/*
    This file is a part of saldl.

    Copyright (C) 2014-2015 Mohammad AlSaleh <CE.Mohammad.AlSaleh at gmail.com>
    https://saldl.github.io

    saldl is free software: you can redistribute it and/or modify
    it under the terms of the Affero GNU General Public License as
    published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    Affero GNU General Public License for more details.

    You should have received a copy of the Affero GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _SALDL_LOG_H
#define _SALDL_LOG_H
#else
#error redefining _SALDL_LOG_H
#endif

#include <assert.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h> /* va stuff */

#define FN __func__
#define LN __LINE__

#define MAX_VERBOSITY 7
#define MAX_NO_COLOR 2

#ifdef __MINGW32__
#define SALDL_PRINTF_FORMAT __MINGW_PRINTF_FORMAT
#else
#define SALDL_PRINTF_FORMAT printf
#endif

const char* ret_char;

const char *bold;
const char *end;
const char *up;
const char *erase_before;
const char *erase_after;
const char *erase_screen_before;
const char *erase_screen_after;

const char *ok_color;
const char *debug_event_color;
const char *debug_color;
const char *info_color;
const char *warn_color;
const char *error_color;
const char *fatal_color;
const char *finish_color;

char debug_event_msg_prefix[256];
char debug_msg_prefix[256];
char info_msg_prefix[256];
char warn_msg_prefix[256];
char error_msg_prefix[256];
char fatal_msg_prefix[256];
char finish_msg[256];

typedef void(log_func)(const char*, const char*, ...) __attribute__(( format(SALDL_PRINTF_FORMAT,2,3) ));

log_func *debug_event_msg;
log_func *debug_msg;
log_func *info_msg;
log_func *warn_msg;
log_func *err_msg;

log_func def_debug_event_msg;
log_func def_debug_msg;
log_func def_info_msg;
log_func def_warn_msg;
log_func def_err_msg;
log_func fatal;
void null_msg();

int tty_width();
void set_color(size_t *no_color);
void set_verbosity(size_t *verbosity, bool *libcurl_verbosity);