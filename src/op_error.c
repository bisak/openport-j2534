/*
 * op_error.c — last-error text. See op_error.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "op_error.h"
#include "j2534/j2534.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static _Thread_local char g_text[OP_ERR_TEXT_MAX];

void op_err_clear(void) { g_text[0] = '\0'; }

void op_err_set(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_text, sizeof g_text, fmt, ap);
    va_end(ap);
}

const char *op_err_get(void)
{
    return g_text[0] != '\0' ? g_text : "No error";
}

const char *op_err_name(long code)
{
    switch ((unsigned long)code) {
    case STATUS_NOERROR:            return "STATUS_NOERROR";
    case ERR_NOT_SUPPORTED:         return "ERR_NOT_SUPPORTED";
    case ERR_INVALID_CHANNEL_ID:    return "ERR_INVALID_CHANNEL_ID";
    case ERR_INVALID_PROTOCOL_ID:   return "ERR_INVALID_PROTOCOL_ID";
    case ERR_NULL_PARAMETER:        return "ERR_NULL_PARAMETER";
    case ERR_INVALID_IOCTL_VALUE:   return "ERR_INVALID_IOCTL_VALUE";
    case ERR_INVALID_FLAGS:         return "ERR_INVALID_FLAGS";
    case ERR_FAILED:                return "ERR_FAILED";
    case ERR_DEVICE_NOT_CONNECTED:  return "ERR_DEVICE_NOT_CONNECTED";
    case ERR_TIMEOUT:               return "ERR_TIMEOUT";
    case ERR_INVALID_MSG:           return "ERR_INVALID_MSG";
    case ERR_INVALID_TIME_INTERVAL: return "ERR_INVALID_TIME_INTERVAL";
    case ERR_EXCEEDED_LIMIT:        return "ERR_EXCEEDED_LIMIT";
    case ERR_INVALID_MSG_ID:        return "ERR_INVALID_MSG_ID";
    case ERR_DEVICE_IN_USE:         return "ERR_DEVICE_IN_USE";
    case ERR_INVALID_IOCTL_ID:      return "ERR_INVALID_IOCTL_ID";
    case ERR_BUFFER_EMPTY:          return "ERR_BUFFER_EMPTY";
    case ERR_BUFFER_FULL:           return "ERR_BUFFER_FULL";
    case ERR_BUFFER_OVERFLOW:       return "ERR_BUFFER_OVERFLOW";
    case ERR_PIN_INVALID:           return "ERR_PIN_INVALID";
    case ERR_CHANNEL_IN_USE:        return "ERR_CHANNEL_IN_USE";
    case ERR_MSG_PROTOCOL_ID:       return "ERR_MSG_PROTOCOL_ID";
    case ERR_INVALID_FILTER_ID:     return "ERR_INVALID_FILTER_ID";
    case ERR_NO_FLOW_CONTROL:       return "ERR_NO_FLOW_CONTROL";
    case ERR_NOT_UNIQUE:            return "ERR_NOT_UNIQUE";
    case ERR_INVALID_BAUDRATE:      return "ERR_INVALID_BAUDRATE";
    case ERR_INVALID_DEVICE_ID:     return "ERR_INVALID_DEVICE_ID";
    case ERR_INIT_FAILED:           return "ERR_INIT_FAILED";
    default:                        return "ERR_UNKNOWN";
    }
}
