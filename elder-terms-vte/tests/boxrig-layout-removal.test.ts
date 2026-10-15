import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createGtkAppLauncher } from 'gestament';
import { toPass } from 'gestament/testing';
import { expect, it } from 'vitest';

const appPath = fileURLToPath(
  new URL('../../.build/elder-terms-vte/elder-terms-vte', import.meta.url)
);

for (const backend of ['opengl', 'cairo']) {
  it(`uses configured DIP size and visual zoom in the main window with ${backend}`, async () => {
    const directory = await mkdtemp(join(tmpdir(), 'boxrig-main-'));
    const config = join(directory, 'terminal.ini');
    await writeFile(
      config,
      '[general]\ntype=local\nname=Main display\nauto_close=false\n[terminal]\nwidth=80\nheight=15\nfont_size_dip=14.5\nscrollback_lines=1234\n'
    );
    const launcher = createGtkAppLauncher({
      appPath,
      args: ['--test-fixture', '-c', config],
      env: {
        XDG_CONFIG_HOME: directory,
        LIBGL_ALWAYS_SOFTWARE: '1',
        GTK_IM_MODULE: 'gtk-im-context-simple',
        ...(backend === 'cairo' ? { GDK_GL: 'disable' } : {}),
      },
    });
    try {
      const app = await launcher.launch();
      const status = await app.getById('fixture_terminal_state_label');
      if (status.kind !== 'label')
        throw new Error('Missing terminal presentation state');
      await toPass(async () => {
        const report = await status.text();
        expect(report).toContain(`backend=${backend}`);
        expect(report).toContain('grid=80x15');
        expect(report).toContain('font=14.50');
        expect(report).toContain('history=1234');
        expect(report).toContain('ready=1');
      });
      const window = await app.getById('main_window');
      if (window.kind !== 'window') throw new Error('Missing terminal window');
      await window.activate();
      const terminal = await app.getById('terminal_view');
      expect((await terminal.info()).interfaces).toContain('Text');
      const bounds = (await terminal.capture()).bounds;
      await app.input.moveMouseTo(bounds.x + 20, bounds.y + 20);
      await app.input.setMouseButton('left', true);
      await app.input.setMouseButton('left', false);
      await app.input.setModifier('control', true);
      await app.input.pressKey('equal');
      await app.input.setModifier('control', false);
      await toPass(async () => {
        const report = await status.text();
        expect(report).toContain('ready=1');
        expect(report).toContain('grid=80x15');
        expect(Number(/font=([\d.]+)/.exec(report)?.[1])).toBeGreaterThan(14.5);
      });
    } finally {
      await launcher.release();
      await rm(directory, { recursive: true, force: true });
    }
  });
}
