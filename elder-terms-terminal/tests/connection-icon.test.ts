import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import type { GtkApp, GtkWidgetElement } from 'gestament';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { capturePixel, expectElementKind } from './test-helpers';
import { runGtkTest, withTemporaryDirectory } from './gtk-test-helpers';

const clickWidget = async (
  app: GtkApp,
  widget: GtkWidgetElement
): Promise<void> => {
  const { bounds } = await widget.capture();
  await app.input.moveMouseTo(
    Math.trunc(bounds.x + bounds.width / 2),
    Math.trunc(bounds.y + bounds.height / 2)
  );
  await app.input.setMouseButton('left', true);
  await app.input.setMouseButton('left', false);
};

const openSettings = async (app: GtkApp): Promise<void> => {
  await clickWidget(app, await app.getById('application_menu_button'));
  await expectElementKind(
    await app.getById('settings_menu_item'),
    'menuItem'
  ).click();
  await app.getById('settings_dialog');
};

it('shows a saved connection icon at the left of the terminal title bar', async (context) => {
  await withTemporaryDirectory(async (directory) => {
    const icon = join(directory, 'icon.png');
    const config = join(directory, 'connection.ini');
    const image = new PNG({ width: 2, height: 1 });
    image.data.set([255, 0, 0, 255, 255, 0, 0, 255]);
    await writeFile(icon, PNG.sync.write(image));
    await writeFile(config, `[general]\nicon=file:${icon}\n`);
    await runGtkTest(context, ['--test-fixture', '-c', config], async (app) => {
      const titleIcon = expectElementKind(
        await app.getById('header_connection_icon'),
        'image'
      );
      await waitForResult(async () => {
        const [red, green, blue] = capturePixel(
          await titleIcon.capture(),
          0.5,
          0.5
        );
        expect(red).toBeGreaterThan(220);
        expect(green).toBeLessThan(35);
        expect(blue).toBeLessThan(35);
      });
      await openSettings(app);
      await expectElementKind(
        await app.getById('settings_general_icon_reset'),
        'button'
      ).click();
      await expectElementKind(
        await app.getById('settings_cancel_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await app.findById('settings_dialog')).toBeUndefined();
        const [red, green, blue] = capturePixel(
          await titleIcon.capture(),
          0.5,
          0.5
        );
        expect(red).toBeGreaterThan(220);
        expect(green).toBeLessThan(35);
        expect(blue).toBeLessThan(35);
      });
      await openSettings(app);
      await expectElementKind(
        await app.getById('settings_general_icon_reset'),
        'button'
      ).click();
      await expectElementKind(
        await app.getById('settings_apply_button'),
        'button'
      ).click();
      await waitForResult(async () => {
        expect(await app.findById('settings_dialog')).toBeUndefined();
        const [red, green, blue] = capturePixel(
          await titleIcon.capture(),
          0.5,
          0.5
        );
        expect(red < 220 || green > 35 || blue > 35).toBe(true);
      });
    });
  });
});

for (const preset of [
  {
    name: 'Noto Color Emoji',
    specification: 'noto:emoji_u1f44d',
    expected: ([red, green, blue]: ReturnType<typeof capturePixel>) => {
      expect(red).toBeGreaterThan(180);
      expect(green).toBeGreaterThan(100);
      expect(blue).toBeLessThan(100);
    },
    sample: [0.4, 0.5] as const,
    badge: false,
  },
  {
    name: 'Material Icons',
    specification: 'material:action/home',
    expected: ([red, green, blue]: ReturnType<typeof capturePixel>) => {
      expect(red).toBeLessThan(80);
      expect(green).toBeLessThan(80);
      expect(blue).toBeLessThan(80);
    },
    sample: [0.5, 0.5] as const,
    badge: true,
  },
]) {
  it(`shows saved ${preset.name} artwork in the terminal title bar`, async (context) => {
    await withTemporaryDirectory(async (directory) => {
      const config = join(directory, 'connection.ini');
      await writeFile(config, `[general]\nicon=${preset.specification}\n`);
      await runGtkTest(
        context,
        ['--test-fixture', '-c', config],
        async (app) => {
          const titleIcon = expectElementKind(
            await app.getById('header_connection_icon'),
            'image'
          );
          await waitForResult(async () => {
            const captured = await titleIcon.capture();
            preset.expected(
              capturePixel(captured, preset.sample[0], preset.sample[1])
            );
            if (preset.badge) {
              const [red, green, blue] = capturePixel(captured, 0.15, 0.5);
              expect(red).toBeGreaterThan(170);
              expect(green).toBeGreaterThan(170);
              expect(blue).toBeGreaterThan(170);
            }
          });
        }
      );
    });
  });
}
