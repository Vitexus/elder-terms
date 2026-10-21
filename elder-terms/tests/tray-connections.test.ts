import { execFile } from 'node:child_process';
import {
  chmod,
  mkdtemp,
  readFile,
  rename,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { promisify } from 'node:util';
import type { GtkApp } from 'gestament';
import { waitForResult } from 'gestament/testing';
import { expect, it } from 'vitest';
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

const execute = promisify(execFile);
const callBus = async (
  app: GtkApp,
  destination: string,
  path: string,
  method: string,
  args: string[]
): Promise<string> => {
  const { stdout } = await execute(
    'gdbus',
    [
      'call',
      '--session',
      '--dest',
      destination,
      '--object-path',
      path,
      '--method',
      method,
      '--',
      ...args,
    ],
    { env: { ...process.env, ...(await app.environment()) } }
  );
  return stdout;
};
const menuItems = (layout: string): { id: string; label: string }[] =>
  Array.from(
    layout.matchAll(/\((\d+), \{[^}]*?'label': <'([^']*)'>/gu),
    (match) => ({ id: match[1]!, label: match[2]! })
  );

it('shows all saved connections in the tray and follows changes while the launcher is hidden', async (context) => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-tray-connections-'));
  const capture = join(directory, 'launched.json');
  for (const kind of ['terminal', 'file-transfer']) {
    const child = join(directory, `${kind}.mjs`);
    await writeFile(
      child,
      `#!/usr/bin/env node\nimport fs from 'node:fs';fs.writeFileSync(${JSON.stringify(capture)}, JSON.stringify({kind:${JSON.stringify(kind)},args:process.argv.slice(2)}));\n`
    );
    await chmod(child, 0o700);
  }
  try {
    await runLauncherGtkTest(
      context,
      async (connections) => {
        await writeFile(
          join(connections, '..', 'global.ini'),
          '[general]\nstartup_mode=tray\nopen_application=\n'
        );
        await writeFile(
          join(connections, 'Alpha_One.ini'),
          '[general]\nicon=noto:emoji_u1f44d\nopen_connection=Ctrl+Alt+X\n'
        );
        await writeFile(
          join(connections, 'Beta transfer.ini'),
          '[general]\ntype=sftp\nicon=material:action/home\n[ssh]\naddress=fixture.example\n'
        );
        await writeFile(
          join(connections, 'Conflict.ini'),
          '[general]\nopen_connection=Ctrl+Alt+X\n'
        );
      },
      async ({ app, connections }) => {
        expect(await app.getWindowCount()).toBe(0);
        await app.getTrayItem({ id: 'elder-terms' });
        const registered = await callBus(
          app,
          'org.kde.StatusNotifierWatcher',
          '/StatusNotifierWatcher',
          'org.freedesktop.DBus.Properties.Get',
          ['org.kde.StatusNotifierWatcher', 'RegisteredStatusNotifierItems']
        );
        const bus = registered.match(
          /['"]([^'"]+)\/StatusNotifierItem['"]/u
        )?.[1];
        expect(bus).toBeDefined();
        const menu = async (method: string, args: string[]): Promise<string> =>
          await callBus(
            app,
            bus!,
            '/StatusNotifierMenu',
            `com.canonical.dbusmenu.${method}`,
            args
          );
        const layout = async (): Promise<string> =>
          await menu('GetLayout', [
            '0',
            '-1',
            "['label', 'children-display', 'enabled']",
          ]);
        const first = await waitForResult(async () => {
          const value = await layout();
          expect(menuItems(value).map((item) => item.label)).toEqual([
            'Open elder-terms',
            'Connections',
            'Alpha__One (Ctrl+Alt+X)',
            'Beta transfer',
            'Conflict (Ctrl+Alt+X)',
            'Application settings',
            'About elder-terms',
            'Quit',
          ]);
          return value;
        });
        const connectionsItem = menuItems(first).find(
          (item) => item.label === 'Connections'
        )!;
        expect(
          await menu('GetLayout', [
            connectionsItem.id,
            '1',
            "['label', 'children-display']",
          ])
        ).toContain('Beta transfer');
        await waitForResult(async () => {
          const icons = await menu('GetLayout', [
            connectionsItem.id,
            '1',
            "['icon-data']",
          ]);
          expect(icons.match(/'icon-data'/gu)?.length).toBe(3);
        });
        const open = async (label: string): Promise<void> => {
          const item = menuItems(await layout()).find(
            (entry) => entry.label === label
          );
          expect(item).toBeDefined();
          await menu('Event', [item!.id, 'clicked', '<0>', '0']);
        };
        await open('Beta transfer');
        await waitForResult(async () => {
          expect(JSON.parse(await readFile(capture, 'utf8'))).toEqual({
            kind: 'file-transfer',
            args: ['-c', join(connections, 'Beta transfer.ini')],
          });
        });
        expect(await app.getWindowCount()).toBe(0);
        await rm(capture);
        await rename(
          join(connections, 'Beta transfer.ini'),
          join(connections, 'Renamed transfer.ini')
        );
        await writeFile(
          join(connections, 'Alpha_One.ini'),
          '[general]\nicon=material:action/home\nopen_connection=Ctrl+Alt+Z\n'
        );
        await rm(join(connections, 'Conflict.ini'));
        await writeFile(join(connections, 'Added.ini'), '[general]\n');
        await waitForResult(async () => {
          const value = await layout();
          expect(menuItems(value).map((item) => item.label)).toEqual([
            'Open elder-terms',
            'Connections',
            'Added',
            'Alpha__One (Ctrl+Alt+Z)',
            'Renamed transfer',
            'Application settings',
            'About elder-terms',
            'Quit',
          ]);
          expect(value).not.toBe(first);
        });
        await open('Added');
        await waitForResult(async () => {
          expect(JSON.parse(await readFile(capture, 'utf8'))).toEqual({
            kind: 'terminal',
            args: ['-c', join(connections, 'Added.ini')],
          });
        });
        await (await app.getTrayItem({ id: 'elder-terms' })).click();
        const list = await app.getById('connection_list');
        if (list.kind === 'tree') await list.selectChildAt(1);
        else await expectElementKind(list, 'table').selectRow(1);
        await expectElementKind(
          await app.getById('settings_general_icon_reset'),
          'button'
        ).click();
        expect(
          await readFile(join(connections, 'Alpha_One.ini'), 'utf8')
        ).toContain('icon=material:action/home');
        await rm(capture);
        await open('Renamed transfer');
        await waitForResult(async () => {
          expect(JSON.parse(await readFile(capture, 'utf8'))).toEqual({
            kind: 'file-transfer',
            args: ['-c', join(connections, 'Renamed transfer.ini')],
          });
        });
        for (const name of ['Added', 'Alpha_One', 'Renamed transfer'])
          await rm(join(connections, `${name}.ini`));
        await waitForResult(async () => {
          const value = await layout();
          expect(value).toContain('No saved connections');
          expect(value).toContain("'enabled': <false>");
        });
      },
      {
        args: [],
        env: {
          ELDER_TERMS_TERMINAL_PATH: join(directory, 'terminal.mjs'),
          ELDER_TERMS_FILE_TRANSFER_PATH: join(directory, 'file-transfer.mjs'),
        },
      }
    );
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
}, 60_000);
