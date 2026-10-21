/*
 * modbus_error.c
 *
 * 错误分类、记录与诊断。
 */

#include "modbus_error.h"

static mb_err_hook_t s_errorHook = NULL;

void mb_err_set_hook(mb_err_hook_t hook)
{
    s_errorHook = hook;
}

static void mb_err_notify(mb_err_t err, const char *where)
{
    if (s_errorHook != NULL)
    {
        s_errorHook(err, where);
    }
}

mb_err_t mb_err_fail(mb_err_t err, const char *where)
{
    mb_err_notify(err, where);
    return err;
}

mb_err_t mb_err_report(mb_err_t *last, uint16_t *count, mb_err_t err, const char *where)
{
    if (last != NULL)
    {
        *last = err;
    }
    if (count != NULL)
    {
        (*count)++;
    }
    mb_err_notify(err, where);
    return err;
}

void mb_err_clear(mb_err_t *last)
{
    if (last != NULL)
    {
        *last = MB_OK;
    }
}

mb_err_class_t mb_err_classify(mb_err_t err)
{
    switch (err)
    {
    case MB_OK:
        return MB_CLASS_OK;

    /* 代码 1..4 是 Modbus 规范定义的异常代码，
     * 正是从站必须放入异常响应中的内容。 */
    case MB_EXC_FUNC_CODE:
    case MB_EXC_ADDR_RANGE:
    case MB_EXC_REGS_QUANT:
    case MB_EXC_EXECUTE:
        return MB_CLASS_PROTO_EXC;

    case MB_ERR_BAD_CONFIG:
    case MB_ERR_BAD_ROLE_ID:
        return MB_CLASS_CONFIG;

    case MB_ERR_NOT_MASTER:
    case MB_ERR_POLLING:
    case MB_ERR_NO_DATA:
    case MB_ERR_BAD_SLAVE_ID:
    case MB_ERR_BAD_FC:
        return MB_CLASS_USAGE;

    case MB_ERR_BUFF_OVERFLOW:
    case MB_ERR_BAD_CRC:
    case MB_ERR_EXCEPTION:
    case MB_ERR_BAD_SIZE:
    case MB_ERR_TIME_OUT:
    case MB_ERR_TRANSPORT:
    default:
        return MB_CLASS_COMM;
    }
}

uint8_t mb_err_exception_byte(mb_err_t err)
{
    if (mb_err_classify(err) != MB_CLASS_PROTO_EXC)
    {
        return 0;
    }
    return (uint8_t)err;
}

const char *mb_err_str(mb_err_t err)
{
    switch (err)
    {
    case MB_OK:                 return "OK";
    case MB_EXC_FUNC_CODE:      return "illegal function";
    case MB_EXC_ADDR_RANGE:     return "illegal data address";
    case MB_EXC_REGS_QUANT:     return "illegal data value";
    case MB_EXC_EXECUTE:        return "slave device failure";
    case MB_ERR_NOT_MASTER:     return "instance is not a master";
    case MB_ERR_POLLING:        return "previous query not consumed yet";
    case MB_ERR_BAD_CONFIG:     return "invalid configuration";
    case MB_ERR_NO_DATA:        return "missing data buffer";
    case MB_ERR_BAD_ROLE_ID:    return "slave id must be 1..247";
    case MB_ERR_BAD_SLAVE_ID:   return "query target outside 1..247";
    case MB_ERR_BAD_FC:         return "unsupported function code";
    case MB_ERR_BUFF_OVERFLOW:  return "receive buffer overflow";
    case MB_ERR_BAD_CRC:        return "CRC mismatch";
    case MB_ERR_EXCEPTION:      return "exception response received";
    case MB_ERR_BAD_SIZE:       return "frame too short or inconsistent";
    case MB_ERR_TIME_OUT:       return "query timed out";
    case MB_ERR_TRANSPORT:      return "transport failed to send the frame";
    default:                    return "unknown error";
    }
}
