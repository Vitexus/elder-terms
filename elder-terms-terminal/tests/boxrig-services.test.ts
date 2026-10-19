import { fileURLToPath } from 'node:url';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

describe('boxrig communication services', () => {
  it.each(['buffer', 'file'] as const)(
    'sends encoded text from a %s with normalized newlines and resumes keyboard input',
    async (source) => {
      const directory = await mkdtemp(join(tmpdir(), 'boxrig-text-send-'));
      const path = join(directory, 'received');
      const next = join(directory, 'next');
      const sourcePath = join(directory, 'source.txt');
      await writeFile(sourcePath, '日本\n語', 'utf8');
      const launcher = createGtkAppLauncher({
        appPath: driverPath,
        args: [
          '--local',
          '--encoding',
          'CP932',
          '--return',
          'crlf',
          ...(source === 'buffer'
            ? ['--text', '日本\n語']
            : ['--text-uri', `file://${sourcePath}`]),
          '--rate',
          '32',
          '--',
          '/bin/sh',
          '-c',
          `stty raw -echo; printf READY; dd bs=1 count=8 iflag=fullblock of='${path}' 2>/dev/null; dd bs=1 count=1 of='${next}' 2>/dev/null`,
        ],
      });
      try {
        const app = await launcher.launch();
        await toPass(async () =>
          expect((await app.output()).stdout).toContain('connection=connected')
        );
        const send = await app.getById('driver_send_text');
        if (send.kind !== 'button')
          throw new Error('Text send control is missing');
        await send.click();
        await toPass(async () => {
          expect((await app.output()).stdout).toContain('text_send_finished=1');
          expect(await readFile(path)).toEqual(
            Buffer.from([0x93, 0xfa, 0x96, 0x7b, 0x0d, 0x0a, 0x8c, 0xea])
          );
          const report = (await app.output()).stdout
            .split('\n')
            .filter((line) => line.includes(' text='))
            .pop();
          expect(report).toContain('input=0');
        });
        const window = await app.getById('driver_window');
        if (window.kind !== 'window')
          throw new Error('Driver window is missing');
        await window.activate();
        const terminal = await app.getById('driver_terminal');
        const bounds = (await terminal.capture()).bounds;
        await app.input.moveMouseTo(bounds.x + 10, bounds.y + 10);
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await app.input.pressKey('x');
        await toPass(async () => {
          expect(await readFile(next, 'utf8')).toBe('x');
          expect((await app.output()).stdout).toMatch(
            /connection=closed generation=1 .*exit=0 .*error=0/
          );
        });
      } finally {
        await launcher.release();
        await rm(directory, { recursive: true, force: true });
      }
    }
  );

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
