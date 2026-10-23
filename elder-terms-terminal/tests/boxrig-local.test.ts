// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { access, mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);
const quote = (value: string): string => `'${value.split("'").join("'\\''")}'`;

describe('boxrig local PTY', () => {
  it('exchanges input and DSR replies, applies the grid, and drains EOF once', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'boxrig-local-'));
    const reply = join(directory, 'reply');
    const size = join(directory, 'size');
    const command = `stty raw -echo; printf '\\033[6n'; dd bs=1 count=6 of=${quote(reply)} 2>/dev/null; stty cooked -echo; printf 'PTY_READY\\r\\n'; while IFS= read -r line; do stty size > ${quote(size)}; printf 'RECEIVED:%s\\r\\n' "$line"; [ "$line" = abc ] && break; done; printf 'EOF:日本語\\342\\202'; exit 127`;
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: ['--local', '--', '/bin/sh', '-c', command],
      env: { LIBGL_ALWAYS_SOFTWARE: '1' },
    });
    try {
      const app = await launcher.launch();
      const visible = async (): Promise<string> => {
        const lines = (await app.output()).stdout.split('\n');
        const line =
          lines.filter((value) => value.includes(' text=')).pop() ?? '';
        return Buffer.from(
          / text=(.*)/.exec(line)?.[1] ?? '',
          'base64'
        ).toString();
      };
      await toPass(async () => {
        expect(await visible()).toContain('PTY_READY');
        expect(await readFile(reply, 'utf8')).toBe('\x1b[1;1R');
      });
      const window = await app.getById('driver_window');
      const status = await app.getById('driver_status');
      if (window.kind !== 'window' || status.kind !== 'label')
        throw new Error('Missing driver window');
      const bounds = await window.bounds();
      let grid: RegExpExecArray | null = null;
      const terminal = await app.getById('driver_terminal');
      await window.activate();
      const boundsTerminal = (await terminal.capture()).bounds;
      await app.input.moveMouseTo(boundsTerminal.x + 10, boundsTerminal.y + 10);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      for (const offset of [120, 220, 60]) {
        const previous = /grid=(\d+)x(\d+)/.exec(await status.text())?.[0];
        await window.resizeTo(
          bounds.width + offset,
          bounds.height + offset / 2
        );
        await toPass(async () => {
          grid = /grid=(\d+)x(\d+)/.exec(await status.text());
          expect(grid?.[0]).not.toBe(previous);
          expect(grid).not.toBeNull();
          expect(
            (await app.output()).stdout
              .split('\n')
              .filter((line) => line.includes(' text='))
              .pop()
          ).toContain('ready=1');
        });
        for (const key of ['r', 'Return']) await app.input.pressKey(key);
        await toPass(async () =>
          expect((await readFile(size, 'utf8')).trim()).toBe(
            `${grid![2]} ${grid![1]}`
          )
        );
      }
      for (const key of ['a', 'b', 'c', 'Return'])
        await app.input.pressKey(key);
      await toPass(async () => {
        expect((await app.output()).stdout).toMatch(
          /connection=closed .*spawn=1 .*exit=127 .*reaped=1/
        );
        expect(await visible()).toContain('RECEIVED:abc');
        expect(await visible()).toContain('EOF:日本語�');
      });
      expect((await readFile(size, 'utf8')).trim()).toBe(
        `${grid![2]} ${grid![1]}`
      );
      const output = (await app.output()).stdout;
      const received = (field: string): Buffer =>
        Buffer.concat(
          [...output.matchAll(new RegExp(`^${field}=([^\\n]*)`, 'gm'))].map(
            (match) => Buffer.from(match[1], 'base64')
          )
        );
      expect(received('cooked')).toEqual(
        Buffer.from(received('raw').toString('utf8'))
      );
      expect(received('raw').toString()).toContain('RECEIVED:abc');
      expect(received('raw').toString().split('EOF:').length).toBe(2);
      const pid = Number(/connection=closed .*pid=(\d+)/.exec(output)?.[1]);
      await expect(access(`/proc/${pid}`)).rejects.toThrow();
    } finally {
      await launcher.release();
      await rm(directory, { recursive: true, force: true });
    }
  });

  for (const scenario of [
    {
      name: 'exec failure',
      command: ['/nonexistent/boxrig-command'],
      result:
        /connection=closed .*spawn=2 stage=11 errno=2 .*exit=127 .*reaped=1/,
    },
    {
      name: 'normal exit 127',
      command: ['/bin/sh', '-c', 'exit 127'],
      result:
        /connection=closed .*spawn=1 stage=0 errno=0 .*exit=127 .*reaped=1/,
    },
  ]) {
    it(`distinguishes ${scenario.name} through spawn diagnostics`, async () => {
      const launcher = createGtkAppLauncher({
        appPath: driverPath,
        args: ['--local', '--', ...scenario.command],
      });
      try {
        const app = await launcher.launch();
        await toPass(async () =>
          expect((await app.output()).stdout).toMatch(scenario.result)
        );
      } finally {
        await launcher.release();
      }
    });
  }

  it('cancels, reaps, and reconnects without sending old input', async () => {
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: [
        '--local',
        '--',
        '/bin/sh',
        '-c',
        'stty -echo; printf "CONNECTED\\r\\n"; IFS= read -r value; printf "NEW:%s\\r\\n" "$value"',
      ],
    });
    try {
      const app = await launcher.launch();
      await toPass(async () =>
        expect((await app.output()).stdout).toContain(
          'connection=connected generation=1'
        )
      );
      const stop = await app.getById('driver_disconnect');
      const reconnect = await app.getById('driver_reconnect');
      if (stop.kind !== 'button' || reconnect.kind !== 'button')
        throw new Error('Missing connection controls');
      await stop.click();
      await toPass(async () =>
        expect((await app.output()).stdout).toMatch(
          /connection=closed generation=1 .*cancelled=1 reaped=1/
        )
      );
      const first = (await app.output()).stdout;
      const pid = Number(/connection=closed .*pid=(\d+)/.exec(first)?.[1]);
      await expect(access(`/proc/${pid}`)).rejects.toThrow();
      await reconnect.click();
      await toPass(async () =>
        expect((await app.output()).stdout).toContain(
          'connection=connected generation=2'
        )
      );
      const terminal = await app.getById('driver_terminal');
      const boundsTerminal = (await terminal.capture()).bounds;
      await app.input.moveMouseTo(boundsTerminal.x + 10, boundsTerminal.y + 10);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      for (const key of ['x', 'Return']) await app.input.pressKey(key);
      await toPass(async () =>
        expect((await app.output()).stdout).toMatch(
          /connection=closed generation=2 .*exit=0 .*reaped=1/
        )
      );
      const output = (await app.output()).stdout;
      const raw = Buffer.concat(
        [...output.matchAll(/^raw=([^\n]*)/gm)].map((match) =>
          Buffer.from(match[1], 'base64')
        )
      ).toString();
      expect(raw.match(/CONNECTED/g)).toHaveLength(2);
      expect(raw).toContain('NEW:x');
    } finally {
      await launcher.release();
    }
  });

  it('recovers its active child before the window finishes closing', async () => {
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: ['--local', '--', '/bin/sh', '-c', 'trap "" HUP TERM; read value'],
    });
    try {
      const app = await launcher.launch();
      let pid = 0;
      await toPass(async () => {
        pid = Number(
          /connection=connected .*pid=(\d+)/.exec(
            (await app.output()).stdout
          )?.[1]
        );
        expect(pid).toBeGreaterThan(0);
      });
      await launcher.release();
      await expect(access(`/proc/${pid}`)).rejects.toThrow();
    } finally {
      await launcher.release();
    }
  });
});
