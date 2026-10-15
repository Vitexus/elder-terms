import { fileURLToPath } from 'node:url';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/boxrig-driver', import.meta.url)
);

describe('boxrig communication services', () => {
  it.each(['raw', 'cooked'] as const)(
    'records %s output once, including immediate output, EOF and reconnection',
    async (mode) => {
      const directory = await mkdtemp(join(tmpdir(), 'boxrig-log-'));
      const path = join(directory, 'received.log');
      const expected =
        mode === 'raw' ? Buffer.from([0x93, 0xfa, 0x82]) : Buffer.from('日�');
      const launcher = createGtkAppLauncher({
        appPath: driverPath,
        args: [
          '--local',
          '--encoding',
          'CP932',
          `--log-${mode}`,
          path,
          '--',
          '/bin/sh',
          '-c',
          "printf '\\223\\372\\202'",
        ],
      });
      try {
        const app = await launcher.launch();
        await toPass(async () => {
          expect((await app.output()).stdout).toMatch(
            /connection=closed generation=1 .*exit=0 .*error=0/
          );
          expect(await readFile(path)).toEqual(expected);
        });
        const reconnect = await app.getById('driver_reconnect');
        if (reconnect.kind !== 'button')
          throw new Error('Reconnect control is missing');
        await reconnect.click();
        await toPass(async () => {
          expect((await app.output()).stdout).toMatch(
            /connection=closed generation=2 .*exit=0 .*error=0/
          );
          expect(await readFile(path)).toEqual(
            Buffer.concat([expected, expected])
          );
        });
      } finally {
        await launcher.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  );
});
