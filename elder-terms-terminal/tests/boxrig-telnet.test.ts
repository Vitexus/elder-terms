// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { once } from 'node:events';
import { createServer, type Socket } from 'node:net';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { expect, it } from 'vitest';

const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

it.each(['EOF', 'cancel'])(
  'preserves a trailing NVT carriage return at %s',
  async (ending) => {
    const sockets: Socket[] = [];
    const server = createServer((socket) => {
      sockets.push(socket);
      socket.on('data', () => {});
      const bytes = Buffer.concat([
        Buffer.from([255, 254, 0, 255, 252, 0]),
        Buffer.from('LAST\r'),
      ]);
      if (ending === 'EOF') socket.end(bytes);
      else socket.write(bytes);
    });
    server.listen(0, '127.0.0.1');
    await once(server, 'listening');
    const address = server.address();
    if (!address || typeof address === 'string')
      throw new Error('Missing server port');
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: ['--telnet', '127.0.0.1', String(address.port)],
    });
    try {
      const app = await launcher.launch();
      if (ending === 'cancel') {
        await toPass(async () => {
          const output = (await app.output()).stdout;
          const raw = Buffer.concat(
            [...output.matchAll(/^raw=([^\n]*)/gm)].map((match) =>
              Buffer.from(match[1], 'base64')
            )
          );
          expect(raw.toString()).toBe('LAST');
        });
        const disconnect = await app.getById('driver_disconnect');
        if (disconnect.kind !== 'button') throw new Error('Missing disconnect');
        await disconnect.click();
      }
      await toPass(async () =>
        expect((await app.output()).stdout).toMatch(
          /connection=closed .*error=0/
        )
      );
      const output = (await app.output()).stdout;
      const raw = Buffer.concat(
        [...output.matchAll(/^raw=([^\n]*)/gm)].map((match) =>
          Buffer.from(match[1], 'base64')
        )
      );
      expect(raw.toString()).toBe('LAST\r');
    } finally {
      await launcher.release();
      for (const socket of sockets) socket.destroy();
      const closed = once(server, 'close');
      server.close();
      await closed;
    }
  }
);

it('runs TELNET negotiation, terminal replies, resize and reconnect through boxrig', async () => {
  const sockets: Socket[] = [];
  const received: Buffer[][] = [];
  const server = createServer((socket) => {
    sockets.push(socket);
    const chunks: Buffer[] = [];
    received.push(chunks);
    socket.on('data', (bytes: Buffer) => chunks.push(Buffer.from(bytes)));
    socket.write(
      Buffer.from([
        255, 253, 0, 255, 251, 0, 255, 253, 31, 255, 253, 24, 255, 250, 24, 1,
        255, 240,
      ])
    );
    socket.write('\x1b[6nTELNET:日本語\r\n');
  });
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const address = server.address();
  if (!address || typeof address === 'string')
    throw new Error('Missing server port');
  const launcher = createGtkAppLauncher({
    appPath: driverPath,
    args: ['--telnet', '127.0.0.1', String(address.port)],
  });
  try {
    const app = await launcher.launch();
    const visible = async (): Promise<string> => {
      const line =
        (await app.output()).stdout
          .split('\n')
          .filter((value) => value.includes(' text='))
          .pop() ?? '';
      return Buffer.from(
        / text=(.*)/.exec(line)?.[1] ?? '',
        'base64'
      ).toString();
    };
    await toPass(async () => {
      expect(await visible()).toContain('TELNET:日本語');
      const bytes = Buffer.concat(received[0] ?? []);
      expect(bytes.includes(Buffer.from('\x1b[1;1R'))).toBe(true);
      expect(
        bytes.includes(
          Buffer.concat([
            Buffer.from([255, 250, 24, 0]),
            Buffer.from('xterm-256color'),
            Buffer.from([255, 240]),
          ])
        )
      ).toBe(true);
    });
    const sendBreak = await app.getById('driver_break');
    if (sendBreak.kind !== 'button') throw new Error('Missing BREAK control');
    await sendBreak.click();
    await toPass(async () => {
      expect((await app.output()).stdout).toContain('break_finished=1');
      expect(Buffer.concat(received[0]).includes(Buffer.from([255, 243]))).toBe(
        true
      );
    });
    const window = await app.getById('driver_window');
    const status = await app.getById('driver_status');
    if (window.kind !== 'window' || status.kind !== 'label')
      throw new Error('Missing driver window');
    await window.activate();
    sockets[0].write('\x1b[8;28;100t');
    await toPass(async () => {
      expect(await status.text()).toContain('grid=100x28');
      expect(
        Buffer.concat(received[0]).includes(
          Buffer.from([255, 250, 31, 0, 100, 0, 28, 255, 240])
        )
      ).toBe(true);
    });
    const bounds = await window.bounds();
    await window.resizeTo(bounds.width + 120, bounds.height + 80);
    await toPass(async () => {
      const grid = /grid=(\d+)x(\d+)/.exec(await status.text());
      expect(grid).not.toBeNull();
      const columns = Number(grid?.[1]);
      const rows = Number(grid?.[2]);
      expect(columns).toBeGreaterThan(80);
      expect(
        Buffer.concat(received[0]).includes(
          Buffer.from([
            255,
            250,
            31,
            columns >> 8,
            columns & 255,
            rows >> 8,
            rows & 255,
            255,
            240,
          ])
        )
      ).toBe(true);
    });
    const terminal = await app.getById('driver_terminal');
    const terminalBounds = (await terminal.capture()).bounds;
    await app.input.moveMouseTo(terminalBounds.x + 10, terminalBounds.y + 10);
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    await app.input.pressKey('a');
    await app.input.pressKey('Return');
    await toPass(async () =>
      expect(Buffer.concat(received[0]).includes(Buffer.from('a\r'))).toBe(true)
    );
    sockets[0].end(Buffer.from([0xe2, 0x82]));
    await toPass(async () => {
      expect((await app.output()).stdout).toMatch(
        /connection=closed generation=1 .*error=0/
      );
      expect(await visible()).toContain('�');
    });
    const reconnect = await app.getById('driver_reconnect');
    if (reconnect.kind !== 'button') throw new Error('Missing reconnect');
    await reconnect.click();
    await toPass(async () => {
      expect((await app.output()).stdout).toContain(
        'connection=connected generation=2'
      );
      expect((await visible()).match(/TELNET:日本語/g)).toHaveLength(2);
    });
    const output = (await app.output()).stdout;
    const raw = Buffer.concat(
      [...output.matchAll(/^raw=([^\n]*)/gm)].map((match) =>
        Buffer.from(match[1], 'base64')
      )
    );
    expect(raw.includes(255)).toBe(false);
  } finally {
    await launcher.release();
    for (const socket of sockets) socket.destroy();
    const closed = once(server, 'close');
    server.close();
    await closed;
  }
});
