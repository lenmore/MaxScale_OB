/*
 * Copyright (c) 2023 MariaDB plc
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
require('@rushstack/eslint-patch/modern-module-resolution')

module.exports = {
  root: true,
  env: {
    node: true,
  },
  extends: [
    'plugin:vue/vue3-essential',
    'eslint:recommended',
    '@vue/eslint-config-prettier/skip-formatting',
    'plugin:vitest-globals/recommended',
  ],
  overrides: [
    {
      files: ['src/assets/icons/*.vue'],
      rules: {
        'vue/multi-word-component-names': 0,
      },
    },
    {
      files: ['**/__tests__/**/*.js'],
      env: {
        'vitest-globals/env': true,
      },
    },
  ],
  parserOptions: {
    ecmaVersion: 'latest',
  },
}
