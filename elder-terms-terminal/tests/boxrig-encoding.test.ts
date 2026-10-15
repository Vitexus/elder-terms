import { fileURLToPath } from 'node:url';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { startClipboardTextProvider } from './clipboard-test-helpers';

const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/boxrig-driver', import.meta.url)
);

describe('boxrig stream conversion', () => {
  it('changes encoding on the same connection and finalizes the old partial character once', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'boxrig-live-encoding-'));
    const destination = join(directory, 'received');
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: [
        '--local',
        '--encoding',
        'CP932',
        '--',
        '/bin/sh',
        '-c',
        `stty raw -echo; printf '\\223\\372\\202'; dd bs=1 count=3 iflag=fullblock of='${destination}' 2>/dev/null; printf '語'`,
      ],
    });
    try {
      const app = await launcher.launch();
      await toPass(async () => {
        const output = (await app.output()).stdout;
        const raw = Buffer.concat(
          [...output.matchAll(/^raw=([^\n]*)/gm)].map((match) =>
            Buffer.from(match[1], 'base64')
          )
        );
        expect(raw).toEqual(Buffer.from([0x93, 0xfa, 0x82]));
        expect(output).toContain('connection=connected generation=1');
      });
      const change = await app.getById('driver_utf8');
      if (change.kind !== 'button')
        throw new Error('Encoding control is missing');
      await change.click();
      await toPass(async () =>
        expect((await app.output()).stdout).toContain(
          'encoding=UTF-8 pending=0'
        )
      );
      const provider = await startClipboardTextProvider(app, '語');
      try {
        const window = await app.getById('driver_window');
        if (window.kind !== 'window')
          throw new Error('Driver window is missing');
        await window.activate();
        const paste = await app.getById('driver_paste');
        const bounds = (await paste.capture()).bounds;
        await app.input.moveMouseTo(
          Math.trunc(bounds.x + bounds.width / 2),
          Math.trunc(bounds.y + bounds.height / 2)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await toPass(async () => {
          expect(await readFile(destination)).toEqual(Buffer.from('語'));
          const output = (await app.output()).stdout;
          expect(output).toMatch(
            /connection=closed generation=1 .*exit=0 .*error=0/
          );
          const cooked = Buffer.concat(
            [...output.matchAll(/^cooked=([^\n]*)/gm)].map((match) =>
              Buffer.from(match[1], 'base64')
            )
          ).toString();
          expect(cooked).toBe('日�語');
          const snapshot =
            output
              .split('\n')
              .filter((line) => line.includes(' text='))
              .pop() ?? '';
          expect(
            Buffer.from(
              / text=(.*)/.exec(snapshot)?.[1] ?? '',
              'base64'
            ).toString()
          ).toContain('日�語');
        });
      } finally {
        await provider.close();
      }
    } finally {
      await launcher.release();
      await rm(directory, { recursive: true, force: true });
    }
  });

  it('decodes CP932 and reports the raw stream and finalized UTF-8 once', async () => {
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: [
        '--local',
        '--encoding',
        'CP932',
        '--',
        '/bin/sh',
        '-c',
        "printf '\\223\\372\\226\\173\\214\\352\\202'",
      ],
    });
    try {
      const app = await launcher.launch();
      await toPass(async () => {
        const output = (await app.output()).stdout;
        expect(output).toMatch(/connection=closed .*exit=0 .*reaped=1/);
        const snapshot =
          output
            .split('\n')
            .filter((line) => line.includes(' text='))
            .pop() ?? '';
        expect(
          Buffer.from(
            / text=(.*)/.exec(snapshot)?.[1] ?? '',
            'base64'
          ).toString()
        ).toContain('日本語�');
      });
      const output = (await app.output()).stdout;
      const raw = Buffer.concat(
        [...output.matchAll(/^raw=([^\n]*)/gm)].map((match) =>
          Buffer.from(match[1], 'base64')
        )
      );
      const cooked = Buffer.concat(
        [...output.matchAll(/^cooked=([^\n]*)/gm)].map((match) =>
          Buffer.from(match[1], 'base64')
        )
      );
      expect(raw).toEqual(
        Buffer.from([0x93, 0xfa, 0x96, 0x7b, 0x8c, 0xea, 0x82])
      );
      expect(cooked).toEqual(Buffer.from('日本語�'));
    } finally {
      await launcher.release();
    }
  });

  it('encodes pasted UTF-8 once at the peer boundary', async () => {
    const directory = await mkdtemp(join(tmpdir(), 'boxrig-encode-'));
    const destination = join(directory, 'received');
    const launcher = createGtkAppLauncher({
      appPath: driverPath,
      args: [
        '--local',
        '--encoding',
        'CP932',
        '--',
        '/bin/sh',
        '-c',
        `stty raw -echo; printf 'ENCODE_READY\\r\\n'; dd bs=1 count=6 iflag=fullblock of='${destination}' 2>/dev/null`,
      ],
    });
    try {
      const app = await launcher.launch();
      await toPass(async () =>
        expect((await app.output()).stdout).toContain('connection=connected')
      );
      const provider = await startClipboardTextProvider(app, '日本語');
      try {
        const paste = await app.getById('driver_paste');
        if (paste.kind !== 'button')
          throw new Error('Paste control is missing');
        const window = await app.getById('driver_window');
        if (window.kind !== 'window')
          throw new Error('Driver window is missing');
        await window.activate();
        const bounds = (await paste.capture()).bounds;
        await app.input.moveMouseTo(
          Math.trunc(bounds.x + bounds.width / 2),
          Math.trunc(bounds.y + bounds.height / 2)
        );
        await app.input.setMouseButton('left', true);
        await app.input.setMouseButton('left', false);
        await toPass(async () =>
          expect(await readFile(destination)).toEqual(
            Buffer.from([0x93, 0xfa, 0x96, 0x7b, 0x8c, 0xea])
          )
        );
      } finally {
        await provider.close();
      }
    } finally {
      await launcher.release();
      await rm(directory, { recursive: true, force: true });
    }
  });
});
