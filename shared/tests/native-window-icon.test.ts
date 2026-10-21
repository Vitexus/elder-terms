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
  protocol: 'icons' | 'icons-scale' | 'none',
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
    ['icon-test', protocol],
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
  const output = await probe('wayland-fallback', 'none', undefined);
  expect(output).not.toContain('SET ');
});

const supportedGtk = process.env.ELDER_TERMS_NATIVE_ICON_GTK_LIBRARY_PATH;

it.skipIf(process.env.ELDER_TERMS_NATIVE_ICON_EXPECT_LEGACY_GTK !== '1')(
  'loads without the optional GTK API even when the compositor offers icons',
  async () => {
    const output = await probe('wayland-unsupported', 'icons', undefined);
    expect(output).not.toContain('SET ');
  }
);

it.skipIf(!supportedGtk)(
  'detects a compositor without the icon protocol at runtime',
  async () => {
    const output = await probe('wayland-no-protocol', 'none', supportedGtk);
    expect(output).not.toContain('SET ');
  }
);

it.skipIf(!supportedGtk)(
  'sends, replaces and resets icon pixels using the same binary',
  async () => {
    const output = await probe('wayland', 'icons', supportedGtk);
    const buffers = output
      .split('\n')
      .filter((line) => line.startsWith('BUFFER '));
    const ids = [...new Set(buffers.map((line) => line.split(' ')[1]))];
    expect(ids).toHaveLength(3);
    for (const id of ids) {
      const sizes = buffers
        .filter((line) => line.split(' ')[1] === id)
        .map((line) => line.split(' ').slice(2, 4));
      expect(sizes).toEqual([
        ['16', '1'],
        ['32', '1'],
        ['256', '1'],
      ]);
    }
    expect(buffers[0].split(' ').slice(2)).toEqual([
      '16',
      '1',
      String(0xffdd2233),
      String(0xffdd2233 * 256),
    ]);
    expect(
      buffers
        .find((line) => line.startsWith(`BUFFER ${ids[1]} 16 1 `))
        ?.split(' ')
        .slice(2)
    ).toEqual(['16', '1', '0', String(0x80092277 * 128)]);
    expect(
      buffers
        .find((line) => line.startsWith(`BUFFER ${ids[2]} 16 1 `))
        ?.split(' ')
        .slice(2)
    ).toEqual(buffers[0].split(' ').slice(2));
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

it.skipIf(!supportedGtk)(
  'recreates immutable buffers at the new output scale',
  async () => {
    const output = await probe('wayland-scale', 'icons-scale', supportedGtk);
    const buffers = output
      .split('\n')
      .filter((line) => line.startsWith('BUFFER '));
    const byIcon = new Map<string, string[]>();
    for (const line of buffers) {
      const [, id, size, scale] = line.split(' ');
      byIcon.set(id, [...(byIcon.get(id) ?? []), `${size}:${scale}`]);
    }
    const icons = [...byIcon.values()];
    expect(icons.length).toBeGreaterThanOrEqual(2);
    expect(icons[0]).toEqual(['16:1', '32:1', '256:1']);
    expect(icons[1]).toEqual(['32:2', '64:2', '512:2']);
    for (const sizes of icons) expect(new Set(sizes).size).toBe(sizes.length);
    expect(output).toContain('COMMIT ');
  }
);

it.skipIf(!supportedGtk)(
  'downscales the smallest suitable source image for intermediate sizes',
  async () => {
    const output = await probe('wayland-multisize', 'icons', supportedGtk);
    const pixels = output
      .split('\n')
      .filter((line) => line.startsWith('BUFFER '))
      .map((line) => line.split(' '))
      .map((parts) => ({
        size: Number(parts[2]),
        scale: Number(parts[3]),
        pixel: Number(parts[4]),
      }));
    expect(pixels).toEqual([
      { size: 16, scale: 1, pixel: 0xffdd2233 },
      { size: 32, scale: 1, pixel: 0xff1144ee },
      { size: 256, scale: 1, pixel: 0xff1144ee },
    ]);
  }
);
