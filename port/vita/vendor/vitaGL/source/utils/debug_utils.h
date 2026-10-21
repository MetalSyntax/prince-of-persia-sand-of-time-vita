/*
 * This file is part of vitaGL
 * Copyright 2017, 2018, 2019, 2020 Rinnegatamante
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation, version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* 
 * gpu_utils.h:
 * Header file for the debug utilities.
 */

#ifndef _DEBUG_UTILS_H_
#define _DEBUG_UTILS_H_

#ifdef __cplusplus
extern "C" {
#endif

// Debugging tool
char *get_gxm_error_literal(uint32_t code);
#ifdef LOG_ERRORS
// pop-sot: el port puede recoger los mensajes en su log (vgl_log_hook); si no, sceClibPrintf
extern void vgl_log_hook(const char *fmt, ...) __attribute__((weak));
#define vgl_log(...) do { if (vgl_log_hook) vgl_log_hook(__VA_ARGS__); else sceClibPrintf(__VA_ARGS__); } while (0)
#else
#define vgl_log(...)
#endif

#ifdef __cplusplus
}
#endif

#endif
