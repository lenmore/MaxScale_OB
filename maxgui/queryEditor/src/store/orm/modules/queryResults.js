/*
 * Copyright (c) 2020 MariaDB Corporation Ab
 *
 * Use of this software is governed by the Business Source License included
 * in the LICENSE.TXT file and at www.mariadb.com/bsl11.
 *
 * Change Date: 2026-11-16
 *
 * On the date above, in accordance with the Business Source License, use
 * of this software will be governed by version 2 or later of the General
 * Public License.
 */
import Worksheet from '@queryEditorSrc/store/orm/models/Worksheet'
import QueryTabMem from '@queryEditorSrc/store/orm/models/QueryTabMem'
import QueryConn from '@queryEditorSrc/store/orm/models/QueryConn'
import QueryResult from '@queryEditorSrc/store/orm/models/QueryResult'

export default {
    namespaced: true,
    actions: {
        /**
         * @param {String} param.qualified_name - Table id (database_name.table_name).
         * @param {String} param.query_mode - a key in QUERY_MODES. Either PRVW_DATA or PRVW_DATA_DETAILS
         */
        async fetchPrvw({ rootState, dispatch }, { qualified_name, query_mode }) {
            const activeQueryTabConn = QueryConn.getters('getActiveQueryTabConn')
            const activeQueryTabId = Worksheet.getters('getActiveQueryTabId')
            const request_sent_time = new Date().valueOf()
            let field, sql, queryName
            const escapedQN = this.vue.$helpers.escapeIdentifiers(qualified_name)
            switch (query_mode) {
                case rootState.queryEditorConfig.config.QUERY_MODES.PRVW_DATA:
                    sql = `SELECT * FROM ${escapedQN} LIMIT 1000;`
                    queryName = `Preview ${escapedQN} data`
                    field = 'prvw_data'
                    break
                case rootState.queryEditorConfig.config.QUERY_MODES.PRVW_DATA_DETAILS:
                    sql = `DESCRIBE ${escapedQN};`
                    queryName = `View ${escapedQN} details`
                    field = 'prvw_data_details'
                    break
            }
            QueryTabMem.update({
                where: activeQueryTabId,
                data(obj) {
                    obj[field].request_sent_time = request_sent_time
                    obj[field].total_duration = 0
                    obj[field].is_loading = true
                },
            })
            const [e, res] = await this.vue.$helpers.asyncTryCatch(
                this.vue.$queryHttp.post(`/sql/${activeQueryTabConn.id}/queries`, {
                    sql,
                    max_rows: rootState.queryPersisted.query_row_limit,
                })
            )
            if (e)
                QueryTabMem.update({
                    where: activeQueryTabId,
                    data(obj) {
                        obj[field].is_loading = false
                    },
                })
            else {
                const now = new Date().valueOf()
                const total_duration = ((now - request_sent_time) / 1000).toFixed(4)
                QueryTabMem.update({
                    where: activeQueryTabId,
                    data(obj) {
                        obj[field].data = Object.freeze(res.data.data)
                        obj[field].total_duration = parseFloat(total_duration)
                        obj[field].is_loading = false
                    },
                })
                dispatch(
                    'queryPersisted/pushQueryLog',
                    {
                        startTime: now,
                        name: queryName,
                        sql,
                        res,
                        connection_name: activeQueryTabConn.name,
                        queryType: rootState.queryEditorConfig.config.QUERY_LOG_TYPES.ACTION_LOGS,
                    },
                    { root: true }
                )
            }
        },
        /**
         * @param {String} query - SQL query string
         */
        async fetchUserQuery({ dispatch, getters, rootState }, query) {
            const activeQueryTabConn = QueryConn.getters('getActiveQueryTabConn')
            const request_sent_time = new Date().valueOf()
            const activeQueryTabId = Worksheet.getters('getActiveQueryTabId')
            const abort_controller = new AbortController()
            const config = rootState.queryEditorConfig.config

            QueryTabMem.update({
                where: activeQueryTabId,
                data: {
                    query_results: {
                        request_sent_time,
                        total_duration: 0,
                        is_loading: true,
                        data: {},
                        abort_controller,
                    },
                },
            })

            let [e, res] = await this.vue.$helpers.asyncTryCatch(
                this.vue.$queryHttp.post(
                    `/sql/${activeQueryTabConn.id}/queries`,
                    {
                        sql: query,
                        max_rows: rootState.queryPersisted.query_row_limit,
                    },
                    { signal: abort_controller.signal }
                )
            )

            if (e)
                QueryTabMem.update({
                    where: activeQueryTabId,
                    data(obj) {
                        obj.query_results.is_loading = false
                    },
                })
            else {
                const now = new Date().valueOf()
                const total_duration = ((now - request_sent_time) / 1000).toFixed(4)
                // If the KILL command was sent for the query is being run, the query request is aborted/canceled
                if (getters.getHasKillFlagMapByQueryTabId(activeQueryTabId)) {
                    QueryTabMem.update({
                        where: activeQueryTabId,
                        data(obj) {
                            obj.has_kill_flag = false
                            /**
                             * This is done automatically in queryHttp.interceptors.response.
                             * However, because the request is aborted, is_conn_busy needs to be set manually.
                             */
                            obj.is_conn_busy = false
                        },
                    })
                    res = {
                        data: {
                            data: {
                                attributes: {
                                    results: [{ message: config.QUERY_CANCELED }],
                                    sql: query,
                                },
                            },
                        },
                    }
                } else if (query.match(/(use|drop database)\s/i))
                    await QueryConn.dispatch('updateActiveDb', {})

                QueryTabMem.update({
                    where: activeQueryTabId,
                    data(obj) {
                        obj.query_results = {
                            ...obj.query_results,
                            data: Object.freeze(res.data.data),
                            total_duration: parseFloat(total_duration),
                            is_loading: false,
                        }
                    },
                })

                dispatch(
                    'queryPersisted/pushQueryLog',
                    {
                        startTime: now,
                        sql: query,
                        res,
                        connection_name: activeQueryTabConn.name,
                        queryType: rootState.queryEditorConfig.config.QUERY_LOG_TYPES.USER_LOGS,
                    },
                    { root: true }
                )
            }
        },
        /**
         * This action uses the current active worksheet connection to send
         * KILL QUERY thread_id
         */
        async stopUserQuery({ commit }) {
            const activeQueryTabConn = QueryConn.getters('getActiveQueryTabConn')
            const activeQueryTabId = Worksheet.getters('getActiveQueryTabId')
            const wkeConn = QueryConn.getters('getActiveWkeConn')
            const [e, res] = await this.vue.$helpers.asyncTryCatch(
                this.vue.$queryHttp.post(`/sql/${wkeConn.id}/queries`, {
                    sql: `KILL QUERY ${activeQueryTabConn.attributes.thread_id}`,
                })
            )
            if (e) this.vue.$logger.error(e)
            else {
                QueryTabMem.update({
                    where: activeQueryTabId,
                    data(obj) {
                        obj.has_kill_flag = true
                    },
                })
                const results = this.vue.$typy(res, 'data.data.attributes.results').safeArray
                if (this.vue.$typy(results, '[0].errno').isDefined)
                    commit(
                        'mxsApp/SET_SNACK_BAR_MESSAGE',
                        {
                            text: [
                                'Failed to stop the query',
                                ...Object.keys(results[0]).map(key => `${key}: ${results[0][key]}`),
                            ],
                            type: 'error',
                        },
                        { root: true }
                    )
                else
                    this.vue
                        .$typy(QueryTabMem.find(activeQueryTabId), 'abort_controller.abort')
                        .safeFunction() // abort the running query
            }
        },
        /**
         * This action clears prvw_data and prvw_data_details to empty object.
         * Call this action when user selects option in the sidebar.
         * This ensure sub-tabs in Data Preview tab are generated with fresh data
         */
        clearDataPreview() {
            QueryTabMem.update({
                where: Worksheet.getters('getActiveQueryTabId'),
                data(obj) {
                    obj.prvw_data = {}
                    obj.prvw_data_details = {}
                },
            })
        },
    },
    getters: {
        getQueryResult: () => QueryResult.find(Worksheet.getters('getActiveQueryTabId')) || {},
        getCurrQueryMode: (state, getters) => getters.getQueryResult.curr_query_mode || '',
        getIsVisSidebarShown: (state, getters) =>
            getters.getQueryResult.is_vis_sidebar_shown || false,

        // Getters for accessing query data stored in memory
        getQueryTabMem: () => QueryTabMem.find(Worksheet.getters('getActiveQueryTabId')) || {},
        getUserQueryRes: (state, getters) => getters.getQueryTabMem.query_results || {},
        getPrvwData: (state, getters, rootState) => mode => {
            const { PRVW_DATA, PRVW_DATA_DETAILS } = rootState.queryEditorConfig.config.QUERY_MODES
            switch (mode) {
                case PRVW_DATA:
                    return getters.getQueryTabMem.prvw_data || {}
                case PRVW_DATA_DETAILS:
                    return getters.getQueryTabMem.prvw_data_details || {}
                default:
                    return {}
            }
        },
        // Getters by query_tab_id
        getUserQueryResByQueryTabId: () => query_tab_id => {
            const { query_results = {} } = QueryTabMem.find(query_tab_id) || {}
            return query_results
        },
        getLoadingQueryResultByQueryTabId: (state, getters) => query_tab_id =>
            getters.getUserQueryResByQueryTabId(query_tab_id).is_loading || false,

        getHasKillFlagMapByQueryTabId: (state, getters) => query_tab_id =>
            getters.getUserQueryResByQueryTabId(query_tab_id).has_kill_flag || false,
    },
}
