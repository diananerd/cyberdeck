#include "deck_sdi.h"

const char *deck_sdi_strerror(deck_sdi_err_t err)
{
    switch (err) {
        case DECK_SDI_OK:                  return "ok";
        case DECK_SDI_ERR_NOT_FOUND:       return "not_found";
        case DECK_SDI_ERR_INVALID_ARG:     return "invalid_arg";
        case DECK_SDI_ERR_NOT_SUPPORTED:   return "not_supported";
        case DECK_SDI_ERR_NO_MEMORY:       return "no_memory";
        case DECK_SDI_ERR_TIMEOUT:         return "timeout";
        case DECK_SDI_ERR_IO:              return "io";
        case DECK_SDI_ERR_ALREADY_EXISTS:  return "already_exists";
        case DECK_SDI_ERR_BUSY:            return "busy";
        case DECK_SDI_ERR_FAIL:            return "fail";
        default:                           return "unknown";
    }
}
