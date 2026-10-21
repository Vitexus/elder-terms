import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { expect, it } from 'vitest';

const binary = (name: string): string =>
  fileURLToPath(new URL(`../../.build/shared/${name}`, import.meta.url));

const probe = async (
  mode: string,
  protocol: boolean,
  gtkLibraryPath: string | undefined
): Promise<string> => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-terms-native-icon-'));
  const env = {
    ...process.env,
    XDG_RUNTIME_DIR: directory,
    WAYLAND_DISPLAY: 'icon-test',
    GDK_BACKEND: 'wayland',
    GSETTINGS_BACKEND: 'memory',
    NO_AT_BRIDGE: '1',
    GTK_MODULES: '',
    LD_LIBRARY_PATH: gtkLibraryPath ?? process.env.LD_LIBRARY_PATH,
  };
  const server = spawn(
    binary('native-window-icon-compositor'),
    ['icon-test', protocol ? 'icons' : 'none'],
    { env }
  );
  const serverClosed = once(server, 'close');
  let output = '';
  let errors = '';
  server.stderr.on('data', (data: Buffer) => {
    errors += data.toString();
  });
  try {
    await new Promise<void>((resolve, reject) => {
      const timeout = setTimeout(
        () => reject(new Error(`Compositor did not start: ${errors}`)),
        10_000
      );
      server.stdout.on('data', (data: Buffer) => {
        output += data.toString();
        if (output.includes('READY\n')) {
          clearTimeout(timeout);
          resolve();
        }
      });
      server.once('close', () => {
        clearTimeout(timeout);
        reject(new Error(`Compositor exited: ${errors}`));
      });
    });
    const client = spawn(binary('native-window-icon-test'), [mode], { env });
    let clientOutput = '';
    client.stdout.on('data', (data: Buffer) => {
      clientOutput += data.toString();
    });
    client.stderr.on('data', (data: Buffer) => {
      clientOutput += data.toString();
    });
    const timeout = setTimeout(() => client.kill('SIGKILL'), 15_000);
    const [code, signal] = await once(client, 'close');
    clearTimeout(timeout);
    expect({ code, signal, clientOutput, errors }).toEqual({
      code: 0,
      signal: null,
      clientOutput: '',
      errors: '',
    });
  } finally {
    server.kill('SIGTERM');
    const [code] = await serverClosed;
    const evidence = fileURLToPath(
      new URL('../../test-results/plan6/native-icons/', import.meta.url)
    );
    await mkdir(evidence, { recursive: true });
    await writeFile(join(evidence, `${mode}.log`), output + errors);
    await rm(directory, { recursive: true, force: true });
    expect({ code, errors, output }).toMatchObject({ code: 0, errors: '' });
  }
  return output;
};

it('keeps running when the icon API or protocol is unavailable', async () => {
  const output = await probe('wayland-fallback', false, undefined);
  expect(output).not.toContain('SET ');
});

const supportedGtk = process.env.ELDER_TERMS_NATIVE_ICON_GTK_LIBRARY_PATH;

it.skipIf(process.env.ELDER_TERMS_NATIVE_ICON_EXPECT_LEGACY_GTK !== '1')(
  'loads without the optional GTK API even when the compositor offers icons',
  async () => {
    const output = await probe('wayland-unsupported', true, undefined);
    expect(output).not.toContain('SET ');
  }
);

it.skipIf(!supportedGtk)(
  'detects a compositor without the icon protocol at runtime',
  async () => {
    const output = await probe('wayland-no-protocol', false, supportedGtk);
    expect(output).not.toContain('SET ');
  }
);

it.skipIf(!supportedGtk)(
  'sends, replaces and resets icon pixels using the same binary',
  async () => {
    const output = await probe('wayland', true, supportedGtk);
    const buffers = output
      .split('\n')
      .filter((line) => line.startsWith('BUFFER '));
    expect(buffers).toHaveLength(3);
    expect(buffers[0].split(' ').slice(2)).toEqual([
      '16',
      '1',
      String(0xffdd2233),
      String(0xffdd2233 * 256),
    ]);
    expect(buffers[1].split(' ').slice(2)).toEqual([
      '16',
      '1',
      '0',
      String(0x80092277 * 128),
    ]);
    const ids = buffers.map((line) => line.split(' ')[1]);
    expect(buffers[2].split(' ').slice(2)).toEqual(
      buffers[0].split(' ').slice(2)
    );
    const commits = output
      .split('\n')
      .filter((line) => line.startsWith('COMMIT '));
    expect(commits).toEqual([
      `COMMIT ${ids[0]}`,
      `COMMIT ${ids[1]}`,
      'COMMIT 0',
      'COMMIT 0',
      `COMMIT ${ids[2]}`,
    ]);
    for (const id of ids) expect(output).toContain(`DESTROY ${id}\n`);
  }
);
