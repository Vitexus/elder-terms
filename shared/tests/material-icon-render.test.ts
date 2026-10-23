// elder-terms: A terminal application, inspired by personal computing in the 1990s.
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/elder-terms/

import { execFile } from 'node:child_process';
import {
  copyFile,
  mkdir,
  mkdtemp,
  readFile,
  rm,
  writeFile,
} from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { promisify } from 'node:util';
import { PNG } from 'pngjs';
import { expect, it } from 'vitest';

const execute = promisify(execFile);

it('renders transparent black and white glyphs with an eight-pixel contrasting outline', async () => {
  const directory = await mkdtemp(join(tmpdir(), 'elder-material-render-'));
  try {
    const source = join(directory, 'source');
    const rendered = join(directory, 'rendered');
    await mkdir(join(source, 'action'), { recursive: true });
    await copyFile(
      fileURLToPath(
        new URL(
          '../../data/connection-icons/material-svg/action/barcode_reader.svg',
          import.meta.url
        )
      ),
      join(source, 'action/barcode_reader.svg')
    );
    await writeFile(
      join(source, 'action/square.svg'),
      '<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24">' +
        '<path fill="none" d="M0 0h24v24H0z"/>' +
        '<path d="M8 8h8v8H8z"/></svg>'
    );
    const resources = join(directory, 'icons.xml');
    await writeFile(
      resources,
      '<gresources><gresource prefix="/test">' +
        '<file>material/action/square.png</file>' +
        '</gresource></gresources>'
    );
    await execute(process.execPath, [
      fileURLToPath(
        new URL(
          '../../scripts/render-material-connection-icons.mts',
          import.meta.url
        )
      ),
      source,
      resources,
      join(directory, 'icons.c'),
      rendered,
      'convert',
      'glib-compile-resources',
      'gdk-pixbuf-thumbnailer',
    ]);
    const black = PNG.sync.read(
      await readFile(join(rendered, 'material/action/square.png'))
    );
    const pixel = (image: PNG, x: number, y: number): number[] => [
      ...image.data.subarray(
        (y * image.width + x) * 4,
        (y * image.width + x) * 4 + 4
      ),
    ];
    expect([black.width, black.height]).toEqual([256, 256]);
    expect(pixel(black, 32, 128)[3]).toBe(0);
    expect(pixel(black, 128, 128)).toEqual([0, 0, 0, 255]);
    // The centered square begins at x=98.67; an 8px outline reaches x=90.67.
    expect(pixel(black, 92, 128)).toEqual([255, 255, 255, 255]);
    expect(pixel(black, 89, 128)[3]).toBe(0);
    const white = PNG.sync.read(
      await readFile(join(rendered, 'material-white/action/square.png'))
    );
    expect([white.width, white.height]).toEqual([256, 256]);
    expect(pixel(white, 128, 128)).toEqual([255, 255, 255, 255]);
    expect(pixel(white, 92, 128)).toEqual([0, 0, 0, 255]);
    for (let offset = 0; offset < black.data.length; offset += 4) {
      expect(white.data[offset + 3]).toBe(black.data[offset + 3]);
      if (black.data[offset + 3] > 0)
        expect(
          Math.abs(black.data[offset] + white.data[offset] - 255)
        ).toBeLessThanOrEqual(1);
    }
    const barcode = PNG.sync.read(
      await readFile(join(rendered, 'material/action/barcode_reader.png'))
    );
    let opaquePixels = 0;
    for (let offset = 3; offset < barcode.data.length; offset += 4)
      if (barcode.data[offset] === 255) ++opaquePixels;
    expect(opaquePixels).toBeGreaterThan(1000);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
