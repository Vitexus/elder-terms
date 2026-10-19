import { chmod, writeFile } from 'node:fs/promises';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { describe, expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import {
  runGtkTest,
  withTemporaryDirectory,
  readTerminalGridLayout,
} from './gtk-test-helpers';
import { expectElementKind } from './test-helpers';

const exec = promisify(execFile);
const luminance = (rgb: number): number => {
  const linear = (byte: number): number => {
    const channel = byte / 255;
    return channel <= 0.04045
      ? channel / 12.92
      : ((channel + 0.055) / 1.055) ** 2.4;
  };
  return (
    0.2126 * linear((rgb >>> 16) & 255) +
    0.7152 * linear((rgb >>> 8) & 255) +
    0.0722 * linear(rgb & 255)
  );
};

describe('terminal appearance', () => {
  for (const background of ['none', '#101010', '#efefef', '#613583']) {
    it(`keeps the scrollbar visible and usable on ${background}`, async (context) => {
      await withTemporaryDirectory(async (directory) => {
        const config = join(directory, 'terminal.ini');
        const shell = join(directory, 'shell.sh');
        const release = join(directory, 'release');
        await exec('mkfifo', [release]);
        await writeFile(
          shell,
          `#!/bin/sh\ni=0\nwhile [ "$i" -lt 100 ]; do printf 'line %s\\r\\n' "$i"; i=$((i+1)); done\nread -r done < '${release}'\n`
        );
        await chmod(shell, 0o755);
        await writeFile(
          config,
          `[general]\nbackground=${background}\n[terminal]\n`
        );
        await runGtkTest(
          context,
          ['-c', config],
          async (app, evidence) => {
            const bar = expectElementKind(
              await app.getById('terminal_scrollbar'),
              'scrollbar'
            );
            await waitForResult(async () =>
              expect(await bar.value()).toBeGreaterThan(50)
            );
            const before = await readTerminalGridLayout(app);
            const capture = await waitForResult(async () => {
              const shot = await bar.capture();
              const png = PNG.sync.read(shot.image);
              const colors = new Map<number, number>();
              for (let y = 10; y < png.height - 10; y++) {
                const at = (y * png.width + Math.floor(png.width / 2)) * 4;
                const rgb =
                  png.data[at] * 65536 +
                  png.data[at + 1] * 256 +
                  png.data[at + 2];
                colors.set(rgb, (colors.get(rgb) ?? 0) + 1);
              }
              const dominant = [...colors]
                .sort((a, b) => b[1] - a[1])
                .slice(0, 2);
              expect(dominant).toHaveLength(2);
              expect(dominant[1][1]).toBeGreaterThan(5);
              const levels = dominant
                .map(([rgb]) => luminance(rgb))
                .sort((a, b) => a - b);
              expect(
                (levels[1] + 0.05) / (levels[0] + 0.05)
              ).toBeGreaterThanOrEqual(3);
              return shot;
            });
            await evidence.captureEvidence('scrollbar-visible', async () =>
              app.capture()
            );
            const initial = await bar.value();
            await app.input.moveMouseTo(
              Math.round(before.terminalCapture.bounds.x + 40),
              Math.round(before.terminalCapture.bounds.y + 40)
            );
            await app.input.scrollWheel(0, -5);
            await waitForResult(async () =>
              expect(await bar.value()).toBeLessThan(initial)
            );
            await bar.setValue(initial);
            const bounds = capture.bounds;
            const x = Math.round(bounds.x + bounds.width / 2);
            await app.input.moveMouseTo(
              x,
              Math.round(bounds.y + bounds.height * 0.9)
            );
            await app.input.setMouseButton('left', true);
            await app.input.moveMouseTo(
              x,
              Math.round(bounds.y + bounds.height * 0.3)
            );
            await app.input.setMouseButton('left', false);
            await waitForResult(async () =>
              expect(await bar.value()).toBeLessThan(initial)
            );
            const after = await readTerminalGridLayout(app);
            expect(after.terminalCapture.bounds).toEqual(
              before.terminalCapture.bounds
            );
          },
          { env: { SHELL: shell } }
        );
      });
    });
  }
});
