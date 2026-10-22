import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import {
  createGtkAppLauncher,
  type GtkApp,
  type GtkWidgetElement,
} from 'gestament';
import { waitForResult } from 'gestament/testing';
import { describe, expect, it } from 'vitest';
import { createTestEvidence, expectElementKind } from './test-helpers';

describe.concurrent('file transfer compact title bar', () => {
  for (const { type, tlsMode, theme, scale } of [
    { type: 'ftp', tlsMode: 'none', theme: 'Adwaita', scale: 1 },
    { type: 'ftp', tlsMode: 'explicit', theme: 'Adwaita:dark', scale: 1 },
    { type: 'sftp', tlsMode: 'none', theme: 'Adwaita', scale: 2 },
    { type: 'ftp', tlsMode: 'none', theme: 'Yaru', scale: 1 },
    { type: 'sftp', tlsMode: 'none', theme: 'Yaru-dark', scale: 1 },
    { type: 'ftp', tlsMode: 'implicit', theme: 'Yaru-dark', scale: 2 },
  ]) {
    it(`shrinks at startup and restores the themed height when toggled (${type}, ${tlsMode}, ${theme}, scale ${scale})`, async (context) => {
      const directory = await mkdtemp(join(tmpdir(), 'elder-terms-header-'));
      const configPath = join(directory, 'connection.ini');
      const evidence = createTestEvidence(context);
      const apps: GtkApp[] = [];
      const launcher = createGtkAppLauncher({
        appPath: fileURLToPath(
          new URL(
            '../../.build/elder-terms-terminal/elder-terms-file-transfer',
            import.meta.url
          )
        ),
        env: {
          LANGUAGE: 'en',
          LC_ALL: 'C.UTF-8',
          GTK_THEME: theme,
          GDK_SCALE: String(scale),
          XDG_CONFIG_HOME: join(directory, 'config'),
          XDG_DATA_HOME: join(directory, 'data'),
        },
        xvfbScreen: '2560x1600x24',
        onSystemOutput: evidence.recordSystemOutputEvent,
      });
      try {
        const settings = [
          `name=Compact ${type}`,
          `type=${type}`,
          '[ssh]',
          'address=fixture.example',
          '[sftp]',
          `local_directory=${directory}`,
          'remote_directory=/remote',
          '[ftp]',
          'address=fixture.example',
          'username=fixture-user',
          `tls_mode=${tlsMode}`,
          `local_directory=${directory}`,
          'remote_directory=/remote',
          '',
        ].join('\n');
        let regularHeight = 0;
        let regularConnectionIconSize = 0;
        for (const compact of [false, true]) {
          await writeFile(
            configPath,
            `[general]\ncompact_mode=${compact}\n${settings}`
          );
          const app = await launcher.launch(
            ['--test-fixture', '-c', configPath],
            {
              onOutput: evidence.recordAppOutputEvent,
            }
          );
          apps.push(app);
          const header = await app.getById('file_transfer_header_bar');
          const capture = await evidence.captureEvidence(
            compact ? 'compact-header' : 'regular-header',
            async () => header.capture()
          );
          if (!compact) {
            regularHeight = capture.bounds.height;
            regularConnectionIconSize = (
              await (
                await app.getById('file_transfer_connection_icon')
              ).capture()
            ).bounds.width;
            expect(regularHeight).toBeGreaterThan(0);
            await app.release();
            continue;
          }

          const compactHeight = Math.round(regularHeight * 0.75);
          expect(capture.bounds.height).toBe(compactHeight);
          const compactConnectionIconSize = (
            await (await app.getById('file_transfer_connection_icon')).capture()
          ).bounds.width;
          expect(compactConnectionIconSize).toBeLessThan(
            regularConnectionIconSize
          );
          expect(capture.clipped).toBe(false);
          for (const id of [
            'file_transfer_connection_icon',
            'file_transfer_title_label',
            'conn_indicator_image',
            'conn_indicator_label',
            'sd_indicator_image',
            'sd_indicator_label',
            'rd_indicator_image',
            'rd_indicator_label',
          ]) {
            const child = await (await app.getById(id)).capture();
            expect(child.clipped, id).toBe(false);
            expect(child.bounds.height, id).toBeGreaterThan(0);
            expect(child.bounds.y, id).toBeGreaterThanOrEqual(capture.bounds.y);
            expect(
              child.bounds.y + child.bounds.height,
              id
            ).toBeLessThanOrEqual(capture.bounds.y + compactHeight);
          }
          const buttons = [await app.getById('application_menu_button')];
          const pending: GtkWidgetElement[] = [header];
          let hasCloseButton = false;
          while (pending.length !== 0) {
            const widget = pending.shift()!;
            if (widget.kind === 'button') {
              buttons.push(widget);
              if ((await widget.info()).name === 'Close') hasCloseButton = true;
            }
            if ('getChildCount' in widget) {
              for (
                let index = 0;
                index < (await widget.getChildCount());
                ++index
              ) {
                const child = await widget.childAt(index);
                if (child !== undefined) pending.push(child);
              }
            }
          }
          expect(hasCloseButton).toBe(true);
          for (const button of buttons) {
            const { bounds } = await button.capture();
            expect(bounds.width).toBe(bounds.height);
            expect(bounds.height).toBeGreaterThanOrEqual(24);
            expect(bounds.y - capture.bounds.y).toBeGreaterThanOrEqual(2);
            expect(
              capture.bounds.y + compactHeight - bounds.y - bounds.height
            ).toBeGreaterThanOrEqual(2);
          }

          if (type === 'ftp') {
            await expectElementKind(
              await app.getById('file_transfer_prompt_accept_button'),
              'button'
            ).click();
            await waitForResult(async () => {
              expect(
                await expectElementKind(
                  await app.getById('file_transfer_prompt_message_label'),
                  'label'
                ).text()
              ).toBe('Password:');
            });
            await expectElementKind(
              await app.getById('file_transfer_prompt_entry'),
              'entry'
            ).setText('fixture-secret');
            await expectElementKind(
              await app.getById('file_transfer_prompt_accept_button'),
              'button'
            ).click();
          }
          await waitForResult(async () => {
            expect(
              await expectElementKind(
                await app.getById('file_transfer_status_label'),
                'label'
              ).text()
            ).toBe('Ready');
          });

          // Reapplying Enabled must not shrink an already compact header.
          for (const enabled of [true, false, true, true, false]) {
            await expectElementKind(
              await app.getById('application_menu_button'),
              'toggleButton'
            ).click();
            await expectElementKind(
              await app.getById('settings_menu_item'),
              'menuItem'
            ).click();
            await expectElementKind(
              await app.getById('settings_general_compact_mode_combo'),
              'comboBox'
            ).selectChildAt(enabled ? 1 : 2);
            await expectElementKind(
              await app.getById('settings_apply_button'),
              'button'
            ).click();
            await waitForResult(async () => {
              expect(await app.getWindowCount()).toBe(1);
              expect((await header.capture()).bounds.height).toBe(
                enabled ? compactHeight : regularHeight
              );
              expect(
                (
                  await (
                    await app.getById('file_transfer_connection_icon')
                  ).capture()
                ).bounds.width
              ).toBe(
                enabled ? compactConnectionIconSize : regularConnectionIconSize
              );
              const status = await (
                await app.getById('file_transfer_status_bar')
              ).info();
              expect(status.states.includes('showing')).toBe(!enabled);
            });
          }
        }
      } finally {
        try {
          await evidence.flushOutputs(apps, launcher);
        } finally {
          await launcher.release();
          await evidence.release();
          await rm(directory, { recursive: true, force: true });
        }
      }
    });
  }
});
