import { execFileSync, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, rm, symlink, unlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
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
    args: [
      '--serial',
      path,
      '--carrier',
      'cd',
      '--return',
      'lf',
      '--backspace',
      'bs',
    ],
  });
  try {
    const app = await launcher.launch();
    await toPass(async () =>
      expect((await app.output()).stdout).toContain(
        'connection=connected generation=1'
      )
    );
    expect((await app.output()).stderr).toContain(
      'serial carrier detection unavailable'
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
    expect(
      execFileSync('stty', ['-F', path, 'speed'], { encoding: 'utf8' }).trim()
    ).toBe('9600');
    const baud = await app.getById('driver_baud_115200');
    if (baud.kind !== 'button')
      throw new Error('Missing serial settings control');
    await baud.click();
    await toPass(async () => {
      expect(
        execFileSync('stty', ['-F', path, 'speed'], { encoding: 'utf8' }).trim()
      ).toBe('115200');
      expect((await app.output()).stdout).toContain(
        'serial_baud=115200 generation=1'
      );
      expect(await visible()).toContain('SERIAL:日本語');
    });
    const sendBreak = await app.getById('driver_break');
    if (sendBreak.kind !== 'button') throw new Error('Missing BREAK control');
    await sendBreak.click();
    await toPass(async () =>
      expect((await app.output()).stdout).toContain('break_finished=1')
    );
    expect(received().toString()).toBe('\x1b[1;1R');
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
    expect(
      execFileSync('stty', ['-F', path, 'speed'], { encoding: 'utf8' }).trim()
    ).toBe('115200');
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
      const state = [
        ...(await app.output()).stdout.matchAll(
          /connection=(\w+) generation=(\d+)/g
        ),
      ].pop();
      expect(state?.[1]).toBe('closed');
      expect(Number(state?.[2])).toBeGreaterThanOrEqual(2);
      expect(await visible()).toContain('�');
    });
  } finally {
    await launcher.release();
    if (peer.exitCode === null && peer.signalCode === null) peer.kill();
    await completion;
  }
});

it('recovers an initially absent serial device and a replacement without losing terminal history', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'boxrig-serial-recovery-'));
  const device = join(directory, 'device');
  const peers: ReturnType<typeof spawn>[] = [];
  const completions: Promise<unknown>[] = [];
  const launcher = createGtkAppLauncher({
    appPath: driverPath,
    args: ['--serial', device],
  });
  try {
    const app = await launcher.launch();
    await toPass(async () =>
      expect((await app.output()).stdout).toMatch(
        /connection=closed generation=1 .*error=2/
      )
    );
    let priorGeneration = 1;
    for (const marker of ['FIRST_DEVICE', 'SECOND_DEVICE']) {
      const row = marker === 'FIRST_DEVICE' ? 1 : 3;
      const peer = spawn(helperPath, [], { stdio: ['pipe', 'pipe', 'pipe'] });
      peers.push(peer);
      const completion = once(peer, 'close');
      completions.push(completion);
      let peerOutput = '';
      const path = await new Promise<string>((resolve, reject) => {
        peer.once('error', reject);
        peer.once('exit', () =>
          reject(new Error('Serial peer exited before readiness'))
        );
        peer.stdout!.on('data', (chunk: Buffer) => {
          peerOutput += chunk.toString();
          const match = /^READY ([^\n]+)\n/.exec(peerOutput);
          if (match) resolve(match[1]);
        });
      });
      await symlink(path, device);
      let connectedGeneration = 0;
      await toPass(async () => {
        const output = (await app.output()).stdout;
        const connected = [
          ...output.matchAll(/connection=connected generation=(\d+)/g),
        ].pop();
        connectedGeneration = Number(connected?.[1]);
        expect(connectedGeneration).toBeGreaterThan(priorGeneration);
      });
      if (marker === 'FIRST_DEVICE') {
        const baud = await app.getById('driver_baud_115200');
        if (baud.kind !== 'button')
          throw new Error('Missing serial settings control');
        await baud.click();
      }
      await toPass(async () =>
        expect(
          execFileSync('stty', ['-F', path, 'speed'], {
            encoding: 'utf8',
          }).trim()
        ).toBe('115200')
      );
      peer.stdin!.write(`TX \x1b[${row};1H\x1b[6n${marker}\x1bE\n`);
      await toPass(async () => {
        const output = (await app.output()).stdout;
        const cooked = Buffer.concat(
          [...output.matchAll(/^cooked=(.*)$/gm)].map((match) =>
            Buffer.from(match[1], 'base64')
          )
        ).toString();
        expect(cooked).toContain(marker);
        const received = Buffer.concat(
          [...peerOutput.matchAll(/^RX (.*)$/gm)].map((match) =>
            Buffer.from(match[1], 'hex')
          )
        );
        expect(received.toString()).toBe(`\x1b[${row};1R`);
      });
      peer.stdin!.write(Buffer.from([84, 88, 32, 0xe2, 0x82, 10]));
      await toPass(async () => {
        const raw = Buffer.concat(
          [...(await app.output()).stdout.matchAll(/^raw=(.*)$/gm)].map(
            (match) => Buffer.from(match[1], 'base64')
          )
        );
        expect(raw.subarray(-2)).toEqual(Buffer.from([0xe2, 0x82]));
      });
      await unlink(device);
      peer.stdin!.write('QUIT\n');
      await completion;
      await toPass(async () => {
        const state = [
          ...(await app.output()).stdout.matchAll(
            /connection=(\w+) generation=(\d+)/g
          ),
        ].pop();
        expect(state?.[1]).toBe('closed');
        expect(Number(state?.[2])).toBeGreaterThanOrEqual(connectedGeneration);
      });
      priorGeneration = connectedGeneration;
    }
    const output = (await app.output()).stdout;
    const cooked = Buffer.concat(
      [...output.matchAll(/^cooked=(.*)$/gm)].map((match) =>
        Buffer.from(match[1], 'base64')
      )
    ).toString();
    expect(cooked).toBe(
      '\x1b[1;1H\x1b[6nFIRST_DEVICE\x1bE�\x1b[3;1H\x1b[6nSECOND_DEVICE\x1bE�'
    );
    const lastText = [...output.matchAll(/ text=(.*)$/gm)].pop()?.[1] ?? '';
    expect(Buffer.from(lastText, 'base64').toString()).toContain(
      'SECOND_DEVICE'
    );
    expect(Buffer.from(lastText, 'base64').toString()).toContain(
      'FIRST_DEVICE'
    );
  } finally {
    await launcher.release();
    for (const peer of peers)
      if (peer.exitCode === null && peer.signalCode === null) peer.kill();
    await Promise.all(completions);
    await rm(directory, { recursive: true, force: true });
  }
});
