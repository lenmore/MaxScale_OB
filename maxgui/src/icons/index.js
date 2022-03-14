/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-03-08
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
import { camelCase } from 'utils/helpers'
let icons = {}
const req = require.context(
    // The relative path of the components folder
    './',
    // Whether or not to look in subfolders
    false,
    /\.(vue)$/i
)
req.keys().forEach(fileName => {
    const name = camelCase(
        fileName
            .split('/')
            .pop()
            .replace(/\.\w+$/, '')
    )
    icons[name] = { component: req(fileName).default }
})

export default icons
