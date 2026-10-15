import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { describe, expect, it } from 'vitest';

const driverPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/boxrig-driver', import.meta.url)
);
const libraryPeerPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/xyzm-pause-peer', import.meta.url)
);
const quote = (value: string): string => `'${value.split("'").join("'\\''")}'`;

describe('boxrig binary transfers', () => {
  for (const protocol of ['xmodem', 'ymodem', 'zmodem'] as const) {
    for (const direction of ['send', 'receive'] as const) {
      for (const peerKind of ['library', 'lrzsz'] as const) {
        it(`${protocol} ${direction} with ${peerKind} excludes binary data from display and logs, then resumes the stream`, async () => {
          const directory = await mkdtemp(join(tmpdir(), 'boxrig-transfer-'));
          const remote = await mkdtemp(join(directory, 'remote-'));
          const destination = await mkdtemp(join(directory, 'destination-'));
          const source = join(directory, 'payload.bin');
          const payload = Buffer.from(
            Array.from({ length: 8192 }, (_, index) => (index * 37 + 19) % 256)
          );
          await writeFile(source, payload);
          const gate = join(directory, 'start');
          const end = join(directory, 'end');
          const postTransfer = join(directory, 'post-transfer');
          const creating = spawn('/usr/bin/mkfifo', [gate, end, postTransfer]);
          const [code] = await once(creating, 'close');
          expect(code).toBe(0);
          const lrzszPeer =
            direction === 'send'
              ? protocol === 'xmodem'
                ? '/usr/bin/rx -b received.bin'
                : protocol === 'ymodem'
                  ? '/usr/bin/rb -b'
                  : '/usr/bin/rz -b'
              : protocol === 'xmodem'
                ? `/usr/bin/sx -b ${quote(source)}`
                : protocol === 'ymodem'
                  ? `/usr/bin/sb --ymodem -b -q ${quote(source)}`
                  : `/usr/bin/sz -b ${quote(source)}`;
          const peer =
            peerKind === 'lrzsz'
              ? lrzszPeer
              : [
                  quote(libraryPeerPath),
                  '--protocol',
                  protocol,
                  '--direction',
                  direction === 'send' ? 'receive' : 'send',
                  '--source',
                  quote(source),
                  '--output-dir',
                  quote(remote),
                  '--fallback-name',
                  'received.bin',
                  '--pause-request-file',
                  quote(join(directory, 'pause-request')),
                  '--paused-file',
                  quote(join(directory, 'paused')),
                  '--resume-file',
                  quote(join(directory, 'resume')),
                ].join(' ');
          const errors = join(directory, 'peer.stderr');
          const log = join(directory, 'received.log');
          // lrzsz can flush its final ACK/OO while restoring the tty. Its tests
          // wait for both sides to finish; the library peer exercises an immediate
          // prompt after the complete protocol trailer without that tty race.
          const handoff =
            peerKind === 'lrzsz' ? `read token < ${quote(postTransfer)}; ` : '';
          const command = `cd ${quote(remote)}; stty raw -echo; printf READY; read token < ${quote(gate)}; ${peer} 2>${quote(errors)}; status=$?; ${handoff}printf AFTER; read token < ${quote(end)}; exit "$status"`;
          const launcher = createGtkAppLauncher({
            appPath: driverPath,
            args: [
              '--local',
              '--encoding',
              'CP932',
              '--log-raw',
              log,
              '--transfer',
              protocol,
              direction,
              '--transfer-uri',
              `file://${source}`,
              '--transfer-directory',
              destination,
              '--',
              '/bin/sh',
              '-c',
              command,
            ],
          });
          try {
            const app = await launcher.launch();
            await toPass(async () =>
              expect(await readFile(log, 'utf8')).toBe('READY')
            );
            const transfer = await app.getById('driver_transfer');
            if (transfer.kind !== 'button')
              throw new Error('Transfer control is missing');
            await transfer.click();
            await toPass(async () =>
              expect((await app.output()).stdout).toContain('transfer_active=1')
            );
            await writeFile(gate, 'start\n');
            if (peerKind === 'lrzsz') {
              await toPass(
                async () => {
                  const output = await app.output();
                  expect(
                    output.stdout,
                    output.stderr + (await readFile(errors, 'utf8'))
                  ).toContain('transfer_finished=1');
                },
                { timeoutMs: 30000 }
              );
              await writeFile(postTransfer, 'continue\n');
            }
            await toPass(
              async () => {
                const output = await app.output();
                expect(
                  output.stdout,
                  output.stderr + (await readFile(errors, 'utf8'))
                ).toContain('transfer_finished=1');
                const received =
                  direction === 'send'
                    ? join(
                        remote,
                        protocol === 'xmodem' ? 'received.bin' : 'payload.bin'
                      )
                    : join(
                        destination,
                        protocol === 'xmodem' ? 'received.bin' : 'payload.bin'
                      );
                expect(await readFile(received)).toEqual(payload);
                expect(await readFile(log, 'utf8'), output.stderr).toBe(
                  'READYAFTER'
                );
              },
              { timeoutMs: 30000 }
            );
            await writeFile(end, 'end\n');
            await toPass(async () =>
              expect((await app.output()).stdout).toMatch(
                /connection=closed generation=1 .*exit=0 .*error=0/
              )
            );
            const output = (await app.output()).stdout;
            const cooked = Buffer.concat(
              [...output.matchAll(/^cooked=([^\n]*)/gm)].map((match) =>
                Buffer.from(match[1], 'base64')
              )
            ).toString();
            expect(cooked).toBe('READYAFTER');
          } finally {
            await launcher.release();
            await rm(directory, { recursive: true, force: true });
          }
        });
      }
    }
  }
});
