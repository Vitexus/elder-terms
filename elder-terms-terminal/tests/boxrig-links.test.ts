import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const appPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

describe('boxrig link actions', () => {
  for (const backend of ['opengl', 'cairo'] as const) {
    it(`uses current Unicode and OSC 8 captures on Ctrl+click with ${backend}`, async () => {
      const fixture =
        'https://first.test/　https://second.test/\r\n' +
        '\x1b]8;;vscode://file/tmp/a%20b.cpp:12:3\x1b\\Open file\x1b]8;;\x1b\\\r\n' +
        '\x1b[?1000h\x1b[?1006h';
      const launcher = createGtkAppLauncher({
        appPath,
        env: {
          LIBGL_ALWAYS_SOFTWARE: '1',
          GTK_IM_MODULE: 'gtk-im-context-simple',
          ...(backend === 'cairo' ? { GDK_GL: 'disable' } : {}),
        },
      });
      try {
        const app = await launcher.launch([
          '--links',
          '--local',
          '--',
          '/bin/sh',
          '-c',
          `stty raw -echo; printf '%s' '${fixture}'; exec cat`,
        ]);
        const window = await app.getById('driver_window');
        if (window.kind !== 'window') throw new Error('Missing driver window');
        await window.activate();
        const terminal = await app.getById('driver_terminal');
        await toPass(async () => {
          const lines = (await app.output()).stdout.trimEnd().split('\n');
          const state =
            lines.reverse().find((line) => line.startsWith('backend=')) ?? '';
          expect(state).toContain('ready=1');
          expect(
            Buffer.from(/text=(.*)/.exec(state)?.[1] ?? '', 'base64').toString()
          ).toContain('Open file');
          const cells = /cell=(\d+)x(\d+)/.exec(state);
          expect(cells).not.toBeNull();
        });
        const state =
          (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend=')) ?? '';
        const cells = /cell=(\d+)x(\d+)/.exec(state)!;
        const { bounds } = await terminal.capture();
        const click = async (row: number, column: number) => {
          await app.input.moveMouseTo(
            Math.trunc(bounds.x + (column + 0.5) * Number(cells[1])),
            Math.trunc(bounds.y + (row + 0.5) * Number(cells[2]))
          );
          await app.input.setModifier('control', true);
          try {
            await app.input.setMouseButton('left', true);
            await app.input.setMouseButton('left', false);
          } finally {
            await app.input.setModifier('control', false);
          }
        };
        const actions = async () =>
          (await app.output()).stdout
            .split('\n')
            .filter((line) => line.startsWith('link='))
            .map((line) =>
              Buffer.from(line.slice(5), 'base64').toString().split('\0')
            );
        await click(0, 5);
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=1');
        });
        await toPass(async () =>
          expect(await actions()).toEqual([['xdg-open', 'https://first.test/']])
        );
        await click(0, 25);
        await toPass(async () => expect(await actions()).toHaveLength(2));
        expect((await actions())[1]).toEqual([
          'xdg-open',
          'https://second.test/',
        ]);
        await click(1, 3);
        await toPass(async () => expect(await actions()).toHaveLength(3));
        expect((await actions())[2]).toEqual([
          'code',
          '--reuse-window',
          '--goto',
          '/tmp/a b.cpp:12:3',
        ]);
        const toggle = await app.getById('driver_links_toggle');
        if (toggle.kind !== 'button') throw new Error('Missing link toggle');
        await toggle.click();
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=0');
        });
        await toggle.click();
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=1');
          expect(status).toContain('ready=1');
        });
        await click(0, 5);
        await toPass(async () => expect(await actions()).toHaveLength(4));
        await app.input.moveMouseTo(
          bounds.x + 3,
          bounds.y + Number(cells[2]) * 3
        );
        await toPass(async () => {
          const status = (await app.output()).stdout
            .split('\n')
            .reverse()
            .find((line) => line.startsWith('backend='));
          expect(status).toContain('link_hover=0');
        });
        await app.input.pressKey('q');
        await toPass(async () =>
          expect((await app.output()).stdout).toContain('cooked=cQ==')
        );
        const output = (await app.output()).stdout
          .split('\n')
          .filter((line) => line.startsWith('cooked='))
          .map((line) => Buffer.from(line.slice(7), 'base64').toString())
          .join('');
        expect(output).not.toContain('\x1b[<');
      } finally {
        await launcher.release();
      }
    });
  }
});
