// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile, spawn } from 'node:child_process';
import { once } from 'node:events';
import { readFileSync, watch } from 'node:fs';
import { mkdir, mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { createRequire } from 'node:module';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { expect, it } from 'vitest';

const execute = promisify(execFile);
const require = createRequire(import.meta.url);
const { PNG } = require('pngjs') as typeof import('pngjs');
const build = fileURLToPath(
  new URL('../../.build/elder-terms-terminal/', import.meta.url)
);

for (const [route, backend, snooper] of [
  ['gtk', 'cairo', false],
  ['ibus', 'cairo', false],
  ['fcitx5', 'cairo', false],
  ['ibus', 'context-loss', false],
  ['gtk', 'cairo', true],
] as const) {
  it(`uses AUTO ${route} with ${backend}${snooper ? ' and IBus key snooping' : ''} for composition, commit, settings and input inhibition`, async () => {
    const root = await mkdtemp(join(tmpdir(), 'elder-boxrig-ime-'));
    const config = join(root, 'config');
    await mkdir(join(config, 'gtk-3.0'), { recursive: true });
    await writeFile(
      join(config, 'gtk-3.0/settings.ini'),
      '[Settings]\ngtk-im-module=gtk-im-context-simple\n'
    );
    const address = `unix:path=${root}/ibus.sock`;
    const addressFile = join(root, 'ibus.address');
    const launcher = createGtkAppLauncher({
      appPath: join(build, 'boxrig-driver'),
      args: [
        ...(backend === 'context-loss' ? ['--context-loss'] : []),
        '--local',
        '--',
        '/bin/sh',
        '-c',
        'stty raw -echo; printf READY; exec cat',
      ],
      accessibilitySession: 'minimal',
      env: {
        XDG_CONFIG_HOME: config,
        XDG_CACHE_HOME: join(root, 'cache'),
        XDG_DATA_HOME: join(root, 'data'),
        GTK_IM_MODULE: route === 'gtk' ? 'ibus' : undefined,
        IBUS_DISABLE_SNOOPER: snooper ? '0' : '1',
        XMODIFIERS: route === 'fcitx5' ? '@im=fcitx' : '@im=ibus',
        IBUS_ADDRESS: route === 'fcitx5' ? '' : address,
        IBUS_ADDRESS_FILE: addressFile,
        IBUS_USE_PORTAL: undefined,
        FCITX_DBUS_ADDRESS: undefined,
        GDK_GL: backend === 'cairo' ? 'disable' : undefined,
        LIBGL_ALWAYS_SOFTWARE: '1',
      },
    });
    const children: ReturnType<typeof spawn>[] = [];
    const completions: Promise<unknown>[] = [];
    let recorder: ReturnType<typeof spawn> | undefined;
    let recording: Promise<unknown> | undefined;
    try {
      const env = await launcher.environment();
      if (route !== 'fcitx5') {
        let daemon: ReturnType<typeof spawn>;
        const listener = watch(root);
        const ready = new Promise<void>((resolve, reject) => {
          listener.on('error', reject);
          listener.on('change', () => {
            try {
              const record = readFileSync(addressFile, 'utf8');
              if (
                record.includes(`IBUS_DAEMON_PID=${daemon.pid}\n`) &&
                record.includes(`IBUS_ADDRESS=${address}`)
              )
                resolve();
            } catch (error) {
              if ((error as NodeJS.ErrnoException).code !== 'ENOENT')
                reject(error);
            }
          });
        });
        try {
          daemon = spawn(
            'ibus-daemon',
            [
              '--single',
              '--panel=disable',
              '--config=disable',
              '--emoji-extension=disable',
              `--address=${address}`,
            ],
            { env, stdio: ['ignore', 'pipe', 'pipe'] }
          );
          children.push(daemon);
          completions.push(once(daemon, 'close'));
          await Promise.race([
            ready,
            (async () => {
              await once(daemon, 'exit');
              throw new Error('IBus exited before readiness');
            })(),
          ]);
        } finally {
          listener.close();
        }
      }
      const engine = spawn(
        join(
          build,
          route === 'fcitx5' ? 'boxrig-fcitx5-server' : 'boxrig-ibus-engine'
        ),
        [],
        { env, stdio: ['ignore', 'pipe', 'pipe'] }
      );
      children.push(engine);
      completions.push(once(engine, 'close'));
      await new Promise<void>((resolve, reject) => {
        let output = '';
        engine.once('error', reject);
        engine.once('exit', () =>
          reject(new Error(`IME engine exited before readiness: ${output}`))
        );
        engine.stdout.on('data', (bytes: Buffer) => {
          output += bytes.toString();
          if (output.includes('READY\n')) resolve();
        });
        engine.stderr.on('data', (bytes: Buffer) => {
          output += bytes.toString();
        });
      });
      const app = await launcher.launch();
      const report = async (): Promise<string> =>
        (await app.output()).stdout
          .split('\n')
          .filter((line) => line.includes(' ime_policy='))
          .pop() ?? '';
      const cooked = async (): Promise<string> =>
        Buffer.concat(
          [...(await app.output()).stdout.matchAll(/^cooked=([^\n]*)/gm)].map(
            (match) => Buffer.from(match[1], 'base64')
          )
        ).toString();
      const preedit = async (): Promise<string> =>
        Buffer.from(
          / preedit=([^ ]*)/.exec(await report())?.[1] ?? '',
          'base64'
        ).toString();
      const window = await app.getById('driver_window');
      if (window.kind !== 'window') throw new Error('Missing driver window');
      await window.activate();
      const bounds = (await (await app.getById('driver_terminal')).capture())
        .bounds;
      await app.input.moveMouseTo(bounds.x + 10, bounds.y + 10);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await toPass(async () => {
        expect(await report()).toContain(
          `ime_policy=0 ime_route=${route === 'gtk' ? 1 : route === 'ibus' ? 2 : 3}`
        );
        expect(await cooked()).toBe('READY');
        expect(await report()).toContain(
          `backend=${backend === 'cairo' ? 'cairo' : 'opengl'}`
        );
        if (route !== 'gtk') {
          expect(await report()).toContain('ime_pending=0 ime_contexts=1');
        }
      });
      if (route !== 'fcitx5')
        await execute(
          'gdbus',
          [
            'call',
            '--address',
            address,
            '--dest',
            'org.boxrig.IMEProbe',
            '--object-path',
            '/org/boxrig/IMEProbe',
            '--method',
            'org.boxrig.IMEProbe.WaitForClient',
            route === 'gtk' ? 'gtk3-im:' : 'boxrig-gtk3',
          ],
          { env }
        );
      const evidence = fileURLToPath(
        new URL(
          `../../test-results/boxrig/ime-${route}-${backend}${snooper ? '-snooper' : ''}/`,
          import.meta.url
        )
      );
      await mkdir(evidence, { recursive: true });
      const screen = PNG.sync.read((await app.capture()).image);
      const frameSize = screen.width * screen.height * 4;
      let pending = Buffer.alloc(0);
      let frame = Buffer.alloc(0);
      let frameNumber = 0;
      let recordingError = '';
      recorder = spawn(
        'ffmpeg',
        [
          '-y',
          '-hide_banner',
          '-loglevel',
          'error',
          '-f',
          'x11grab',
          '-framerate',
          '30',
          '-draw_mouse',
          '0',
          '-video_size',
          `${screen.width}x${screen.height}`,
          '-i',
          env.DISPLAY ?? '',
          '-map',
          '0:v',
          '-c:v',
          'ffv1',
          '-pix_fmt',
          'bgr0',
          '-fps_mode',
          'passthrough',
          join(evidence, 'composition.mkv'),
          '-map',
          '0:v',
          '-c:v',
          'rawvideo',
          '-pix_fmt',
          'bgr0',
          '-fps_mode',
          'passthrough',
          '-f',
          'rawvideo',
          'pipe:1',
        ],
        { env, stdio: ['pipe', 'pipe', 'pipe'] }
      );
      recording = once(recorder, 'close');
      recorder.stderr!.on('data', (bytes: Buffer) => {
        recordingError += bytes.toString();
      });
      recorder.stdout!.on('data', (bytes: Buffer) => {
        pending = Buffer.concat([pending, bytes]);
        while (pending.length >= frameSize) {
          frame = Buffer.from(pending.subarray(0, frameSize));
          pending = pending.subarray(frameSize);
          frameNumber++;
        }
      });
      const states: { label: string; frame: number }[] = [];
      const observeVideo = async (label: string): Promise<Buffer> => {
        const previous = frameNumber;
        let pixels = Buffer.alloc(0);
        await toPass(async () => {
          expect(recorder!.exitCode, recordingError).toBeNull();
          expect(frameNumber).toBeGreaterThan(previous);
          const captured = PNG.sync.read(
            (await (await app.getById('driver_terminal')).capture()).image
          );
          const width = Math.min(captured.width, 480);
          const height = Math.min(captured.height, 44);
          pixels = Buffer.alloc(width * height * 3);
          let different = 0;
          for (let y = 0; y < height; y++)
            for (let x = 0; x < width; x++) {
              const source = (y * captured.width + x) * 4;
              const recorded =
                ((bounds.y + y) * screen.width + bounds.x + x) * 4;
              let changed = false;
              for (let channel = 0; channel < 3; channel++) {
                pixels[(y * width + x) * 3 + channel] =
                  captured.data[source + channel];
                if (
                  Math.abs(
                    frame[recorded + 2 - channel] -
                      captured.data[source + channel]
                  ) > 8
                )
                  changed = true;
              }
              if (changed) different++;
            }
          expect(different / (width * height)).toBeLessThan(0.005);
        });
        states.push({ label, frame: frameNumber });
        await writeFile(
          join(evidence, `${label}.png`),
          (await (await app.getById('driver_terminal')).capture()).image
        );
        return pixels;
      };
      const metrics = /cell=(\d+)x(\d+)/.exec(await report());
      if (!metrics) throw new Error('Missing selection metrics');
      await app.input.moveMouseTo(
        bounds.x + 1,
        bounds.y + Math.trunc(Number(metrics[2]) / 2)
      );
      await app.input.setMouseButton('left', true);
      await app.input.moveMouseTo(
        bounds.x + Number(metrics[1]) * 5 - 1,
        bounds.y + Math.trunc(Number(metrics[2]) / 2)
      );
      await app.input.setMouseButton('left', false);
      await toPass(async () => expect(await report()).toContain('selected=1'));
      const beforeComposition = await observeVideo('before');
      const composeKey = route === 'fcitx5' ? 'a' : 'F1';
      const composition =
        route === 'fcitx5' ? 'きしゃ' : '記者の汽車が記者で記者した';
      const committed = route === 'fcitx5' ? '汽車🙂' : composition;
      await app.input.pressKey(composeKey);
      await toPass(async () => {
        expect(await preedit()).toBe(composition);
        expect(await cooked()).toBe('READY');
      });
      if (backend === 'context-loss') {
        const loseContext = await app.getById('driver_lose_gl_context');
        if (loseContext.kind !== 'button')
          throw new Error('Context-loss control is missing');
        await loseContext.click();
        await toPass(async () => {
          expect(await report()).toContain('backend=cairo');
          expect(await report()).toContain('reason=3');
          expect(await preedit()).toBe(composition);
          expect(await cooked()).toBe('READY');
        });
      }
      const composingPixels = await observeVideo('preedit');
      expect(
        composingPixels.filter(
          (value, index) => Math.abs(value - beforeComposition[index]) > 8
        ).length
      ).toBeGreaterThan(150);
      const settings = await app.getById('driver_trs80');
      if (settings.kind !== 'button')
        throw new Error('Missing input settings control');
      await settings.click();
      await toPass(async () =>
        expect(await report()).toContain('input_pending=1')
      );
      await app.input.pressKey('Escape');
      await toPass(async () => {
        expect(await preedit()).toBe('');
        expect(await report()).toContain('input_pending=0');
        expect(await report()).toContain('selected=1');
        expect(await cooked()).toBe('READY');
      });
      const cancelledPixels = await observeVideo('cancelled');
      expect(
        cancelledPixels.filter(
          (value, index) => Math.abs(value - composingPixels[index]) > 8
        ).length
      ).toBeGreaterThan(150);
      const keyCounts = async () => {
        const { stdout } = await execute(
          'gdbus',
          [
            'call',
            '--address',
            address,
            '--dest',
            'org.boxrig.IMEProbe',
            '--object-path',
            '/org/boxrig/IMEProbe',
            '--method',
            'org.boxrig.IMEProbe.GetState',
          ],
          { env }
        );
        const counts = [...stdout.matchAll(/uint32 (\d+)/g)].map((match) =>
          Number(match[1])
        );
        return counts.slice(3, 5);
      };
      const beforeEscape = snooper ? await keyCounts() : [];
      await app.input.pressKey('Escape');
      await toPass(async () => {
        // The probe consumes Escape even without preedit. IBus's optional
        // GTK snooper handles it before either boxrig or the application.
        if (snooper)
          expect(await keyCounts()).toEqual(
            beforeEscape.map((count) => count + 1)
          );
        expect(await report()).toContain(`selected=${snooper ? 1 : 0}`);
        expect(await preedit()).toBe('');
        expect(await cooked()).toBe('READY');
      });
      await observeVideo(snooper ? 'ime-consumed-escape' : 'selection-cleared');
      await app.input.pressKey(composeKey);
      await toPass(async () => expect(await preedit()).toBe(composition));
      await app.input.pressKey(route === 'fcitx5' ? 'b' : 'Return');
      await toPass(async () => {
        expect(await cooked()).toBe(`READY${committed}`);
        expect(await preedit()).toBe('');
      });
      await observeVideo('committed');
      await app.input.pressKey(composeKey);
      await toPass(async () => expect(await preedit()).toBe(composition));
      const pause = await app.getById('driver_pause_input');
      const resume = await app.getById('driver_resume_input');
      if (pause.kind !== 'button' || resume.kind !== 'button')
        throw new Error('Missing input controls');
      await pause.click();
      await toPass(async () => {
        expect(await report()).toContain(' input=2 ');
        expect(await preedit()).toBe('');
      });
      await observeVideo('inhibited');
      await resume.click();
      await toPass(async () => expect(await report()).toContain(' input=0 '));
      await app.input.pressKey(composeKey);
      await toPass(async () => expect(await preedit()).toBe(composition));
      await app.input.pressKey(route === 'fcitx5' ? 'b' : 'Return');
      await toPass(async () =>
        expect(await cooked()).toBe(`READY${committed}${committed}`)
      );
      await observeVideo('resumed');
      await writeFile(
        join(evidence, 'states.json'),
        JSON.stringify(states, null, 2)
      );
    } finally {
      if (recorder?.exitCode === null && recorder.signalCode === null)
        recorder.stdin!.write('q\n');
      if (recording) await recording;
      await launcher.release();
      for (const child of children.reverse())
        if (child.exitCode === null && child.signalCode === null) child.kill();
      await Promise.all(completions);
      await rm(root, { recursive: true, force: true });
    }
  });
}
