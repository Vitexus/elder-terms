import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { expectElementKind, runSharedGtkTest } from './test-helpers';

for (const [profile, reference] of [
  ['protan', 'd05cba'],
  ['deutan', 'd36a50'],
  ['tritan', 'dd6152'],
]) {
  it(`previews the terminal's ${profile} colors before applying them`, async (context) => {
    await runSharedGtkTest(
      context,
      [
        '--page=terminal',
        `--connection=terminal.color_vision=${profile}`,
        '--connection=terminal.color_filter=oklch',
      ],
      async ({ app, directory }) => {
        const scroll = expectElementKind(
          await app.getById('settings_terminal_page_scrollbar'),
          'scrollbar'
        );
        await scroll.setValue((await scroll.valueInfo()).maximum);
        const expander = expectElementKind(
          await app.getById('settings_terminal_color_preview'),
          'expander'
        );
        await expander.expand();
        const before = expectElementKind(
          await app.getById('settings_terminal_color_preview_before'),
          'label'
        );
        const after = expectElementKind(
          await app.getById('settings_terminal_color_preview_after'),
          'label'
        );
        // Expansion changes the scroll range on GTK's next allocation.
        // Follow that range until both samples are fully inside the viewport.
        const page = await app.getById('settings_terminal_page');
        await waitForResult(async () => {
          await scroll.setValue((await scroll.valueInfo()).maximum);
          const viewport = (await page.capture()).bounds;
          for (const sample of [before, after]) {
            const bounds = (await sample.capture()).bounds;
            expect(bounds.y).toBeGreaterThanOrEqual(viewport.y);
            expect(bounds.y + bounds.height).toBeLessThanOrEqual(
              viewport.y + viewport.height
            );
          }
        });
        expect(await before.text()).toContain('- removed');
        expect(await before.text()).toContain('256');
        expect(await before.text()).toContain('RGB');
        expect(await after.text()).toBe(await before.text());
        const count = (image: Buffer, color: string): number => {
          const frame = PNG.sync.read(image);
          let found = 0;
          for (let i = 0; i < frame.data.length; i += 4)
            if (frame.data.subarray(i, i + 3).toString('hex') === color)
              found++;
          return found;
        };
        await waitForResult(async () =>
          expect(
            count((await after.capture()).image, reference)
          ).toBeGreaterThan(5)
        );
        expect(count((await before.capture()).image, '800000')).toBeGreaterThan(
          5
        );
        const amount = expectElementKind(
          await app.getById('settings_terminal_color_vision_strength_entry'),
          'entry'
        );
        await amount.setText('0');
        // Oklch stays active independently when recoloring is zero.
        await waitForResult(async () =>
          expect(
            count((await after.capture()).image, 'db6354')
          ).toBeGreaterThan(5)
        );
        await amount.setText('100');
        await waitForResult(async () =>
          expect(
            count((await after.capture()).image, reference)
          ).toBeGreaterThan(5)
        );
        await writeFile(
          join(directory, 'preview-dark.png'),
          (await app.capture()).image
        );
        const background = expectElementKind(
          await app.getById('settings_terminal_color_preview_background'),
          'comboBox'
        );
        await background.selectChildAt(1);
        await waitForResult(async () =>
          expect(
            count((await before.capture()).image, 'ffffff')
          ).toBeGreaterThan(1000)
        );
        await writeFile(
          join(directory, 'preview-light.png'),
          (await app.capture()).image
        );
        const filter = expectElementKind(
          await app.getById('settings_terminal_color_filter_combo'),
          'comboBox'
        );
        const mode = expectElementKind(
          await app.getById('settings_terminal_color_vision_combo'),
          'comboBox'
        );
        await mode.selectChildAt(1);
        await filter.selectChildAt(1);
        await waitForResult(async () => {
          const a = PNG.sync.read((await before.capture()).image);
          const b = PNG.sync.read((await after.capture()).image);
          // GTK can allocate the odd remaining pixel to one grid column.
          // Compare their shared area, including every sample and text glyph.
          expect(a.height).toBe(b.height);
          const width = Math.min(a.width, b.width);
          for (let row = 0; row < a.height; row++)
            expect(
              a.data
                .subarray(row * a.width * 4, (row * a.width + width) * 4)
                .equals(
                  b.data.subarray(
                    row * b.width * 4,
                    (row * b.width + width) * 4
                  )
                )
            ).toBe(true);
        });
      }
    );
  });
}
