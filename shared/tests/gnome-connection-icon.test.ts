// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { randomUUID } from 'node:crypto';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';

const execute = promisify(execFile);
const repository = fileURLToPath(new URL('../..', import.meta.url));
const driver = join(repository, '.build/shared/connection-desktop-driver');
const terminal = join(
  repository,
  '.build/elder-terms-terminal/elder-terms-terminal'
);
const enabled = process.env.ELDER_TERMS_GNOME_ICON_TEST === '1';

const isolatedEnvironment = (): NodeJS.ProcessEnv => {
  const names = [
    'DISPLAY',
    'DBUS_SESSION_BUS_ADDRESS',
    'XDG_CONFIG_HOME',
    'XDG_DATA_HOME',
    'XDG_RUNTIME_DIR',
    'XDG_CACHE_HOME',
  ] as const;
  const values = Object.fromEntries(
    names.map((name) => [name, process.env[`ELDER_TERMS_GNOME_ICON_${name}`]])
  );
  for (const name of names) {
    if (!values[name])
      throw new Error(`Missing isolated GNOME environment: ${name}`);
  }
  if (
    values.DISPLAY === process.env.DISPLAY ||
    values.DBUS_SESSION_BUS_ADDRESS === process.env.DBUS_SESSION_BUS_ADDRESS
  ) {
    throw new Error('Refusing to connect this test to the login desktop');
  }
  for (const name of [
    'XDG_CONFIG_HOME',
    'XDG_DATA_HOME',
    'XDG_RUNTIME_DIR',
    'XDG_CACHE_HOME',
  ] as const) {
    const value = values[name]!;
    if (!value.startsWith('/') || resolve(value) === resolve(repository))
      throw new Error(`Invalid isolated GNOME directory: ${name}`);
    if (value === process.env[name])
      throw new Error(`Refusing to use the login directory: ${name}`);
  }
  return {
    ...process.env,
    ...values,
    GDK_BACKEND: 'x11',
    GSETTINGS_BACKEND: 'memory',
    XDG_SESSION_TYPE: 'x11',
    WAYLAND_DISPLAY: undefined,
  };
};

const findWindow = async (
  appId: string,
  env: NodeJS.ProcessEnv
): Promise<string | undefined> => {
  try {
    const { stdout } = await execute(
      'xdotool',
      ['search', '--onlyvisible', '--class', appId],
      { env }
    );
    return stdout.trim().split('\n')[0];
  } catch {
    return undefined;
  }
};

const captureDock = async (
  output: string,
  label: string,
  env: NodeJS.ProcessEnv
): Promise<PNG> => {
  const xwd = join(output, `${label}.xwd`);
  const png = join(output, `${label}.png`);
  try {
    await execute('xwd', ['-root', '-silent', '-out', xwd], { env });
    await execute('convert', [xwd, png], { env });
    return PNG.sync.read(await readFile(png));
  } finally {
    await rm(xwd, { force: true });
  }
};

const dockColorPixels = (image: PNG, color: readonly number[]): number => {
  let pixels = 0;
  for (let y = 30; y < image.height; y += 1) {
    for (let x = 0; x < Math.min(65, image.width); x += 1) {
      const offset = (y * image.width + x) * 4;
      if (
        color.every((value, channel) => image.data[offset + channel] === value)
      )
        pixels += 1;
    }
  }
  return pixels;
};

const waitForDockColor = async (
  appId: string,
  windowId: string,
  color: readonly number[],
  output: string,
  stage: string,
  env: NodeJS.ProcessEnv
): Promise<void> => {
  const deadline = Date.now() + 20_000;
  let frame = 0;
  let lastCount = 0;
  while (Date.now() < deadline) {
    expect(await findWindow(appId, env)).toBe(windowId);
    const image = await captureDock(output, `${stage}-${frame}`, env);
    lastCount = dockColorPixels(image, color);
    if (lastCount > 900) return;
    frame += 1;
    await new Promise((done) => setTimeout(done, 250));
  }
  throw new Error(
    `${stage}: dock kept the previous icon with the same window alive ` +
      `(matching pixels: ${lastCount}; frames: ${frame})`
  );
};

it.skipIf(!enabled)(
  'updates the GNOME dock icon while the same connection window stays open',
  async () => {
    const env = isolatedEnvironment();
    const id = randomUUID();
    const appId = `net.kekyo.elder-terms.connection.${id}`;
    const output = join(
      repository,
      '.build/test-results/gnome-connection-icon',
      id
    );
    const config = join(env.XDG_CONFIG_HOME!, `gnome-icon-${id}.ini`);
    await mkdir(output, { recursive: true });
    await execute(
      'gdbus',
      [
        'call',
        '--session',
        '--dest',
        'org.gnome.Shell',
        '--object-path',
        '/org/gnome/Shell',
        '--method',
        'org.freedesktop.DBus.Peer.Ping',
      ],
      { env }
    );

    let terminalProcess: ReturnType<typeof spawn> | undefined;
    let recorder: ReturnType<typeof spawn> | undefined;
    let recorderError = '';
    const video = join(output, 'dock-change.mp4');
    const publish = async (rgba: string): Promise<void> => {
      const { stdout } = await execute(
        driver,
        [id, 'GNOME icon test', rgba, terminal, '--test-fixture', '-c', config],
        { env }
      );
      const icon = stdout.trimEnd().split('\n')[1];
      if (!icon) throw new Error('Desktop icon publication returned no image');
      await writeFile(
        config,
        `[general]\nname=GNOME icon test\nconnection_id=${id}\nicon=file:${icon}\n`
      );
    };
    try {
      recorder = spawn(
        'ffmpeg',
        [
          '-hide_banner',
          '-loglevel',
          'error',
          '-y',
          '-f',
          'x11grab',
          '-draw_mouse',
          '0',
          '-video_size',
          '1600x900',
          '-framerate',
          '5',
          '-i',
          env.DISPLAY!,
          '-c:v',
          'libx264',
          '-preset',
          'ultrafast',
          '-crf',
          '18',
          video,
        ],
        { env, stdio: ['ignore', 'ignore', 'pipe'] }
      );
      recorder.stderr?.on('data', (chunk: Buffer) => {
        recorderError += chunk.toString();
      });

      await publish('ff0000ff');
      // GNOME 46 delays application catalog rescans for five seconds. This
      // setup pause lets the first window bind to the new desktop entry;
      // each tested icon update below waits for observed dock pixels.
      await new Promise((done) => setTimeout(done, 6_000));
      terminalProcess = spawn(terminal, ['--test-fixture', '-c', config], {
        env,
        stdio: 'ignore',
      });
      let windowId: string | undefined;
      const startupDeadline = Date.now() + 10_000;
      while (!windowId && Date.now() < startupDeadline) {
        windowId = await findWindow(appId, env);
        if (!windowId) await new Promise((done) => setTimeout(done, 100));
      }
      expect(windowId).toBeDefined();
      await waitForDockColor(
        appId,
        windowId!,
        [255, 0, 0],
        output,
        'initial-red',
        env
      );
      await publish('0000ffff');
      await waitForDockColor(
        appId,
        windowId!,
        [0, 0, 255],
        output,
        'running-blue',
        env
      );
      expect(await findWindow(appId, env)).toBe(windowId);

      await execute('xdotool', ['windowclose', windowId!], { env });
      if (
        terminalProcess.exitCode === null &&
        terminalProcess.signalCode === null
      )
        await once(terminalProcess, 'exit');
      await publish('00ff00ff');
      terminalProcess = spawn(terminal, ['--test-fixture', '-c', config], {
        env,
        stdio: 'ignore',
      });
      windowId = undefined;
      const reopenedDeadline = Date.now() + 10_000;
      while (!windowId && Date.now() < reopenedDeadline) {
        windowId = await findWindow(appId, env);
        if (!windowId) await new Promise((done) => setTimeout(done, 100));
      }
      expect(windowId).toBeDefined();
      await waitForDockColor(
        appId,
        windowId!,
        [0, 255, 0],
        output,
        'first-launch-green',
        env
      );
      expect(await findWindow(appId, env)).toBe(windowId);
      await writeFile(
        join(output, 'result.txt'),
        'Dock changed red to blue with one window open, then green on the first launch after another edit.\n'
      );
    } finally {
      if (terminalProcess?.pid) {
        terminalProcess.kill('SIGTERM');
        if (
          terminalProcess.exitCode === null &&
          terminalProcess.signalCode === null
        )
          await once(terminalProcess, 'exit');
      }
      if (recorder?.pid) {
        recorder.kill('SIGINT');
        if (recorder.exitCode === null && recorder.signalCode === null)
          await once(recorder, 'exit');
      }
      await writeFile(join(output, 'ffmpeg.log'), recorderError);
      await rm(config, { force: true });
    }
    const { stdout: duration } = await execute(
      'ffprobe',
      [
        '-v',
        'error',
        '-show_entries',
        'format=duration',
        '-of',
        'default=noprint_wrappers=1:nokey=1',
        video,
      ],
      { env }
    );
    expect(Number(duration.trim())).toBeGreaterThan(0);
    expect(recorderError).toBe('');
  },
  90_000
);
