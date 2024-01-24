/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 * Copyright (c) 2023 MariaDB plc, Finnish Branch
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2027-11-30
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */

export const LOGO = `
.___  ___.      ___      ___   ___      _______.  ______      ___       __       _______
|   \\/   |     /   \\     \\  \\ /  /     /       | /      |    /   \\     |  |     |   ____|
|  \\  /  |    /  ^  \\     \\  V  /     |   (----'|  ,----'   /  ^  \\    |  |     |  |__
|  |\\/|  |   /  /_\\  \\     >   <       \\   \\    |  |       /  /_\\  \\   |  |     |   __|
|  |  |  |  /  _____  \\   /  .  \\  .----)   |   |  '----. /  _____  \\  |  '----.|  |____
|__|  |__| /__/     \\__\\ /__/ \\__\\ |_______/     \\______|/__/     \\__\\ |_______||_______|
`

export const ICON_SHEETS = {
    monitors: {
        frames: ['$vuetify.icons.mxs_stopped', '$vuetify.icons.mxs_good'],
        colorClasses: ['text-grayed-out', 'text-success'],
    },
    services: {
        frames: [
            '$vuetify.icons.mxs_critical',
            '$vuetify.icons.mxs_good',
            '$vuetify.icons.mxs_stopped',
        ],
        colorClasses: ['text-error', 'text-success', 'text-grayed-out'],
    },
    listeners: {
        frames: [
            '$vuetify.icons.mxs_critical',
            '$vuetify.icons.mxs_good',
            '$vuetify.icons.mxs_stopped',
        ],
        colorClasses: ['text-error', 'text-success', 'text-grayed-out'],
    },
    servers: {
        frames: [
            '$vuetify.icons.mxs_criticalServer',
            '$vuetify.icons.mxs_goodServer',
            '$vuetify.icons.mxs_maintenance',
        ],
        colorClasses: ['text-error', 'text-success', 'text-grayed-out'],
    },
    replication: {
        frames: [
            '$vuetify.icons.mxs_stopped',
            '$vuetify.icons.mxs_good',
            '$vuetify.icons.mxs_statusWarning',
        ],
        colorClasses: ['text-grayed-out', 'text-success', 'text-warning'],
    },
    logPriorities: {
        frames: {
            alert: '$vuetify.icons.mxs_alertWarning',
            error: '$vuetify.icons.mxs_critical',
            warning: '$vuetify.icons.mxs_statusInfo',
            notice: '$vuetify.icons.mxs_reports',
            info: '$vuetify.icons.mxs_statusInfo',
            debug: 'mdi-bug',
        },
        colorClasses: {
            alert: 'text-error',
            error: 'text-error',
            warning: 'text-warning',
            notice: 'text-info',
            info: 'text-info',
            debug: 'text-accent',
        },
    },
    config_sync: {
        frames: ['mdi-sync-alert', 'mdi-cog-sync-outline', 'mdi-cog-sync-outline'],
        colorClasses: ['text-error', 'text-success', 'text-primary'],
    },
}

export const ROUTING_TARGET_RELATIONSHIP_TYPES = Object.freeze(['servers', 'services', 'monitors'])

// routes having children routes
export const ROUTE_GROUP = Object.freeze({
    DASHBOARD: 'dashboard',
    VISUALIZATION: 'visualization',
    CLUSTER: 'cluster',
    DETAIL: 'detail',
})

// key names must be taken from ROUTE_GROUP values
export const DEF_REFRESH_RATE_BY_GROUP = Object.freeze({
    dashboard: 10,
    visualization: 60,
    cluster: 60,
    detail: 10,
})

// Do not alter the order of the keys as it's used for generating steps for the Config Wizard
export const MXS_OBJ_TYPES = Object.freeze({
    SERVERS: 'servers',
    MONITORS: 'monitors',
    FILTERS: 'filters',
    SERVICES: 'services',
    LISTENERS: 'listeners',
})

export const LOG_PRIORITIES = ['alert', 'error', 'warning', 'notice', 'info', 'debug']

export const SERVER_OP_TYPES = Object.freeze({
    MAINTAIN: 'maintain',
    CLEAR: 'clear',
    DRAIN: 'drain',
    DELETE: 'delete',
})

export const MONITOR_OP_TYPES = Object.freeze({
    STOP: 'stop',
    START: 'start',
    DESTROY: 'destroy',
    SWITCHOVER: 'async-switchover',
    RESET_REP: 'async-reset-replication',
    RELEASE_LOCKS: 'async-release-locks',
    FAILOVER: 'async-failover',
    REJOIN: 'async-rejoin',
    CS_GET_STATUS: 'async-cs-get-status',
    CS_STOP_CLUSTER: 'async-cs-stop-cluster',
    CS_START_CLUSTER: 'async-cs-start-cluster',
    CS_SET_READONLY: 'async-cs-set-readonly',
    CS_SET_READWRITE: 'async-cs-set-readwrite',
    CS_ADD_NODE: 'async-cs-add-node',
    CS_REMOVE_NODE: 'async-cs-remove-node',
})

export const USER_ROLES = Object.freeze({ ADMIN: 'admin', BASIC: 'basic' })

export const USER_ADMIN_ACTIONS = Object.freeze({ DELETE: 'delete', UPDATE: 'update', ADD: 'add' })

export const DURATION_SUFFIXES = Object.freeze(['ms', 's', 'm', 'h'])

export const MRDB_MON = 'mariadbmon'

export const MRDB_PROTOCOL = 'MariaDBProtocol'

const TIME_REF_POINT_KEYS = [
    'NOW',
    'START_OF_TODAY',
    'END_OF_YESTERDAY',
    'START_OF_YESTERDAY',
    'NOW_MINUS_2_DAYS',
    'NOW_MINUS_LAST_WEEK',
    'NOW_MINUS_LAST_2_WEEKS',
    'NOW_MINUS_LAST_MONTH',
]

export const TIME_REF_POINTS = Object.freeze(
    TIME_REF_POINT_KEYS.reduce((obj, key) => ({ ...obj, [key]: key }), {})
)
