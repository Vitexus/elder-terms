import { writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';
import { waitForResult } from 'gestament/testing';
import { expectElementKind, runLauncherGtkTest } from './test-helpers';

const solidImage = (color: readonly number[]): Buffer => {
  const png = new PNG({ width: 32, height: 16 });
  for (let offset = 0; offset < png.data.length; offset += 4) {
    png.data.set([...color, 255], offset);
  }
  return PNG.sync.write(png);
};

const colorPixels = (image: Buffer, color: readonly number[]): number => {
  const png = PNG.sync.read(image);
  let count = 0;
  // Icons occupy the left edge before the connection name.
  for (let y = 0; y < png.height; y += 1) {
    for (let x = 0; x < Math.min(32, png.width); x += 1) {
      const offset = (y * png.width + x) * 4;
      if (color.every((value, channel) => png.data[offset + channel] === value))
        count += 1;
    }
  }
  return count;
};

it('shows distinct saved connection icons and reloads a changed selection', async (context) => {
  await runLauncherGtkTest(
    context,
    async (connections) => {
      const red = join(connections, '..', 'red icon.png');
      const blue = join(connections, '..', 'blue icon.png');
      await writeFile(red, solidImage([255, 0, 0]));
      await writeFile(blue, solidImage([0, 0, 255]));
      await writeFile(
        join(connections, 'Alpha.ini'),
        `[general]\nicon=file:${red}\n`
      );
      await writeFile(
        join(connections, 'Beta.ini'),
        `[general]\nicon=file:${blue}\n`
      );
      await writeFile(
        join(connections, 'Default.ini'),
        '[general]\ntype=local\n'
      );
      await writeFile(
        join(connections, 'Missing.ini'),
        '[general]\nicon=file:/no-such-connection-icon.png\n'
      );
    },
    async ({ app, connections }) => {
      const list = await app.getById('connection_list');
      await waitForResult(async () => {
        const capture = await list.capture();
        expect(colorPixels(capture.image, [255, 0, 0])).toBeGreaterThan(40);
        expect(colorPixels(capture.image, [0, 0, 255])).toBeGreaterThan(40);
      });
      const table = expectElementKind(list, 'table');
      const defaultCell = await table.cellAt(2, 0);
      const missingCell = await table.cellAt(3, 0);
      expect(defaultCell).toBeDefined();
      expect(missingCell).toBeDefined();
      if (!defaultCell || !missingCell)
        throw new Error('Default icon rows are missing');
      const defaultImage = PNG.sync.read((await defaultCell.capture()).image);
      const missingImage = PNG.sync.read((await missingCell.capture()).image);
      expect(missingImage.height).toBe(defaultImage.height);
      let nonWhite = 0;
      for (let y = 0; y < defaultImage.height; y += 1) {
        for (let x = 0; x < 30; x += 1) {
          const from = (y * defaultImage.width + x) * 4;
          const to = (y * missingImage.width + x) * 4;
          const pixels = [...defaultImage.data.subarray(from, from + 4)];
          expect([...missingImage.data.subarray(to, to + 4)]).toEqual(pixels);
          if (pixels[0] < 220 || pixels[1] < 220 || pixels[2] < 220)
            nonWhite += 1;
        }
      }
      expect(nonWhite).toBeGreaterThan(40);
      await writeFile(
        join(connections, 'Beta.ini'),
        `[general]\nicon=file:${join(connections, '..', 'red icon.png')}\n`
      );
      await waitForResult(async () => {
        const capture = await list.capture();
        expect(colorPixels(capture.image, [255, 0, 0])).toBeGreaterThan(100);
        expect(colorPixels(capture.image, [0, 0, 255])).toBe(0);
      });
    }
  );
});
