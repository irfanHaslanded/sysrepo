/**
 * @file error_format.h
 * @author Michal Vasko <mvasko@cesnet.cz>
 * @brief Functions for simplified manipulation with callback errors.
 *
 * @copyright
 * Copyright (c) 2018 - 2021 Deutsche Telekom AG.
 * Copyright (c) 2018 - 2021 CESNET, z.s.p.o.
 *
 * This source code is licensed under BSD 3-Clause License (the "License").
 * You may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     https://opensource.org/licenses/BSD-3-Clause
 */

#ifndef SYSREPO_ERROR_FORMAT_H_
#define SYSREPO_ERROR_FORMAT_H_

#include <libyang/libyang.h>

#include "../sysrepo_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup utils_error_format Error Format Handling Utilities
 * @{
 *
 * Generally, if an application wants to communicate an error from a sysrepo callback, it must return an
 * ::sr_error_t error value and may optionally set error message using ::sr_session_set_error_message().
 * Additionally, if more information needs to be communicated, arbitrary chunks of data can be written by
 * ::sr_session_push_error_data(), which can then be decoded based on the error format set by
 * ::sr_session_set_error_format().
 *
 * To make writing specific errors easier, following is a list of well-known error formats, for which
 * utility functions are provided that allow setting the whole error by a single function.
 *
 * Well-known error formats:
 * - NETCONF
 */

/**
 * @brief Set NETCONF callback error.
 *
 * Meaning of each value corresponds to the definiition of NETCONF
 * [rpc-error](https://tools.ietf.org/html/rfc6241#section-4.3) element and value restrictions of @p error_type
 * and @p error_tag are checked.
 *
 * @param[in] session Implicit session provided in a callback.
 * @param[in] error_type Error type.
 * @param[in] error_tag Error tag.
 * @param[in] error_app_tag Optional error app tag.
 * @param[in] error_path Optional error path.
 * @param[in] error_message Error message.
 * @param[in] error_info_count Optional count of elements in error info.
 * @param[in] ... Optional error info elements. There must be 2x @p error_info_count parameters. They create pairs
 * of element-name and element-value to be set for the error.
 * @return Error code (::SR_ERR_OK on success).
 */
int sr_session_set_netconf_error(sr_session_ctx_t *session, const char *error_type, const char *error_tag,
        const char *error_app_tag, const char *error_path, const char *error_message, uint32_t error_info_count, ...);

/**
 * @brief Get NETCONF callback error.
 *
 * @param[in] err NETCONF error to read.
 * @param[out] error_type Error type.
 * @param[out] error_tag Error tag.
 * @param[out] error_app_tag Error app tag, set to NULL if none.
 * @param[out] error_path Error path, set to NULL if none.
 * @param[out] error_message Error message.
 * @param[out] error_info_elements Array of error info elements, set to NULL if none.
 * @param[out] error_info_values Array of error info values, set to NULL if none.
 * @param[out] error_info_count Error info count of both @p error_info_elements and @p error_info_values.
 * @return Error code (::SR_ERR_OK on success).
 */
int sr_err_get_netconf_error(const sr_error_info_err_t *err, const char **error_type, const char **error_tag,
        const char **error_app_tag, const char **error_path, const char **error_message,
        const char ***error_info_elements, const char ***error_info_values, uint32_t *error_info_count);

/** @} utils_error_format */

#ifdef __cplusplus
}
#endif

#endif /* SYSREPO_ERROR_FORMAT_H_ */