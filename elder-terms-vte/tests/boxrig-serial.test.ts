import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { expect, it } from 'vitest';

const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/boxrig-driver', import.meta.url)
);
const helperPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/serial-pty-helper', import.meta.url)
);

it('exchanges serial bytes, reconnects without clearing the display, and finalizes unplugged input', async () => {
  const peer = spawn(helperPath, [], { stdio: ['pipe', 'pipe', 'pipe'] });
  const completion = once(peer, 'close');
  let output = '';
  const path = await new Promise<string>((resolve, reject) => {
    peer.once('error', reject);
    peer.once('exit', () =>
      reject(new Error('Serial peer exited before readiness'))
    );
    peer.stdout.on('data', (chunk: Buffer) => {
      output += chunk.toString();
      const ready = /^READY ([^\n]+)\n/.exec(output);
      if (ready) resolve(ready[1]);
    });
  });
  const launcher = createGtkAppLauncher({
    appPath: driverPath,
    args: ['--serial', path, '--return', 'lf', '--backspace', 'bs'],
  });
  try {
    const app = await launcher.launch();
    await toPass(async () =>
      expect((await app.output()).stdout).toContain(
        'connection=connected generation=1'
      )
    );
    peer.stdin.write('TX \x1b[6nSERIAL:日本語\x1bE\n');
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
    const received = (): Buffer =>
      Buffer.concat(
        [...output.matchAll(/^RX ([^\n]*)/gm)].map((match) =>
          Buffer.from(match[1], 'hex')
        )
      );
    await toPass(async () => {
      expect(await visible()).toContain('SERIAL:日本語');
      expect(received().toString()).toBe('\x1b[1;1R');
    });
    const window = await app.getById('driver_window');
    if (window.kind !== 'window') throw new Error('Missing driver window');
    await window.activate();
    const bounds = (await (await app.getById('driver_terminal')).capture())
      .bounds;
    await app.input.moveMouseTo(bounds.x + 10, bounds.y + 10);
    await app.input.setMouseButton('left', true);
    await app.input.setMouseButton('left', false);
    for (const key of ['x', 'Return', 'BackSpace'])
      await app.input.pressKey(key);
    await toPass(async () =>
      expect(received().toString()).toBe('\x1b[1;1Rx\n\b')
    );
    const disconnect = await app.getById('driver_disconnect');
    const reconnect = await app.getById('driver_reconnect');
    if (disconnect.kind !== 'button' || reconnect.kind !== 'button')
      throw new Error('Missing connection controls');
    await disconnect.click();
    await toPass(async () =>
      expect((await app.output()).stdout).toContain(
        'connection=closed generation=1'
      )
    );
    await reconnect.click();
    await toPass(async () =>
      expect((await app.output()).stdout).toContain(
        'connection=connected generation=2'
      )
    );
    peer.stdin.write('TX RECONNECTED\x1bE\n');
    await toPass(async () => {
      expect(await visible()).toContain('RECONNECTED');
      expect(await visible()).toContain('SERIAL:日本語');
    });
    peer.stdin.write(Buffer.from([84, 88, 32, 0xe2, 0x82, 10]));
    await toPass(async () => {
      const raw = Buffer.concat(
        [...(await app.output()).stdout.matchAll(/^raw=([^\n]*)/gm)].map(
          (match) => Buffer.from(match[1], 'base64')
        )
      );
      expect(raw.subarray(-2)).toEqual(Buffer.from([0xe2, 0x82]));
    });
    peer.stdin.write('QUIT\n');
    await completion;
    await toPass(async () => {
      expect((await app.output()).stdout).toMatch(
        /connection=closed generation=2 .*error=0/
      );
      expect(await visible()).toContain('�');
    });
  } finally {
    await launcher.release();
    if (peer.exitCode === null && peer.signalCode === null) peer.kill();
    await completion;
  }
});
