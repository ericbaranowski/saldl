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

#ifndef SALDL_WRITE_MODE_H
#define SALDL_WRITE_MODE_H
#else
#error redefining SALDL_WRITE_MODE_H
#endif

#include "common.h"

void set_modes(info_s *info_ptr);
void set_write_opts(CURL* handle, void* storage, saldl_params *params_ptr, bool no_body);

/* vim: set filetype=c ts=2 sw=2 et spell foldmethod=syntax: */
