// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { defineConfig } from 'vitest/config';
import prettierMax from 'prettier-max';
import { getTestFontEnvironment } from '../scripts/test-font-environment.mjs';

export default defineConfig({
  plugins: [prettierMax()],
  test: {
    env: getTestFontEnvironment(),
    fileParallelism: false,
    globals: true,
    environment: 'node',
    include: ['tests/**/*.test.ts'],
    testTimeout: 30_000,
    coverage: {
      enabled: false,
    },
  },
});
