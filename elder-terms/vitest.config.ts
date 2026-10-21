import { defineConfig } from 'vitest/config';
import { fileURLToPath } from 'node:url';
import prettierMax from 'prettier-max';
import { getTestFontEnvironment } from '../scripts/test-font-environment.mjs';

const testResultRunId =
  process.env.ELDER_TERMS_TEST_RESULT_RUN_ID ??
  new Date().toISOString().replace(/[:.]/gu, '-');

export default defineConfig({
  plugins: [prettierMax()],
  test: {
    env: {
      ...getTestFontEnvironment(),
      XDG_DATA_HOME: fileURLToPath(
        new URL(
          `../test-results/${testResultRunId}/launcher-xdg-data`,
          import.meta.url
        )
      ),
    },
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
